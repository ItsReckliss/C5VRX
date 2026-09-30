#include "sync_flywheel.h"

#include <string.h>

/* Geometry at 20 MS/s output pairs (ITU-R BT.470): H sync 4.7 us = 94,
 * burst from ~5.3/5.6 us to ~7.8 us (106..157), back porch to >= 9.2 us. */
#define SFW_SYNC_PAIRS      94
#define SFW_MIN_RUN         8     /* consecutive low codes that start a pulse */
#define SFW_WIN_LOCKED      16
#define SFW_WIN_UNLOCKED    40
#define SFW_CLEAN_ERR       3
#define SFW_BURST_FROM      100
#define SFW_BURST_TO        170
#define SFW_SYNC_LEVEL_FROM 15
#define SFW_SYNC_LEVEL_TO   75
#define SFW_BLANK_FROM      165
#define SFW_BLANK_TO        180
#define SFW_SPAN            210   /* pairs after a line start the analysis reads */
#define SFW_KILL_ON_Q16     (65536u / 4u)    /* >25 % of lines repaired */
#define SFW_KILL_OFF_Q16    (65536u / 20u)   /* <5 % */
/* A crystal line clock: with +-1 pair edge noise the 1/64 frequency loop
 * leaves ~0.016 pair/line period error, so coasting 650 lines drifts ~10
 * pairs (0.5 us). Lock needs 32 clean lines since acquisition. The field
 * counter coasts too (up to 50 fields = 1 s), so a fade that also hides the
 * vertical sync keeps both repair and vertical-interval protection. */
#define SFW_FIELD_COAST     50u
#define SFW_COAST_LINES     650u   /* ~2 fields, ~10 pairs worst drift */
#define SFW_LOCK_CLEAN      32u
#define SFW_LOST_LINES      1200u

static uint8_t s_cell_for_phase[256];
static const uint8_t *s_cell_lut_phase;

/* Strong, well-defined cells (bucket-centre radius 4..6.5 steps) chosen per
 * target phase, so synthetic endpoints land within ~1 code of the target. */
static void build_cells(const uint8_t *phase)
{
    if (s_cell_lut_phase == phase) return;
    int best_err[256];
    for (int t = 0; t < 256; ++t) best_err[t] = 1 << 30;
    for (int raw = 0; raw < 256; ++raw) {
        int q = (int8_t)((raw & 15) << 4) >> 4;
        int i = (int8_t)(raw & 240) >> 4;
        int r2x4 = (2 * i + 1) * (2 * i + 1) + (2 * q + 1) * (2 * q + 1);
        if (r2x4 < 64 || r2x4 > 169) continue;          /* r 4..6.5 */
        for (int t = 0; t < 256; ++t) {
            int e = ((phase[raw] - t + 128) & 255) - 128;
            e = e < 0 ? -e : e;
            /* Prefer smaller phase error, then the radius closest to 5. */
            int score = e * 1024 + (r2x4 > 100 ? r2x4 - 100 : 100 - r2x4);
            if (score < best_err[t]) { best_err[t] = score; s_cell_for_phase[t] = (uint8_t)raw; }
        }
    }
    s_cell_lut_phase = phase;
}

static inline uint32_t idx(const sfw_ring_t *r, uint64_t k)
{
    return (uint32_t)k & (r->ring_pairs - 1u);
}

static inline uint8_t endpoint(const sfw_ring_t *r, uint64_t k)
{
    return r->ring[idx(r, k) * 2u + 1u];
}

/* Forward code cursor mirroring the live demodulator. Phase8 codes are
 * stateless; HC codes depend on the retained state, so a window is opened
 * 8 endpoints early and the recursion warms up (the decoder only differs
 * from its bank-independent value on near-origin cells). Out-of-range or
 * backward access reopens the window. */
#define CUR_MAX 1024u
static struct {
    const sfw_ring_t *r;
    uint64_t base;
    uint32_t n;
    uint8_t prev_state;
    uint8_t code[CUR_MAX];
    uint8_t state[CUR_MAX];
} s_cur;

static void cur_open(const sfw_ring_t *r, uint64_t from)
{
    s_cur.r = r;
    s_cur.base = from;
    s_cur.n = 0;
    if (r->hc_dec) {
        uint8_t st = r->hc_dec[endpoint(r, from - 9u)];
        for (uint64_t k = from - 8u; k < from; ++k)
            st = r->hc_dec[((st >> 3) << 8) | endpoint(r, k)];
        s_cur.prev_state = st;
    }
}

static uint8_t code_at(const sfw_ring_t *r, uint64_t k)
{
    if (s_cur.r != r || k < s_cur.base || k >= s_cur.base + CUR_MAX)
        cur_open(r, k);
    while (s_cur.base + s_cur.n <= k) {
        uint64_t j = s_cur.base + s_cur.n;
        uint8_t c;
        if (r->hc_dec) {
            uint8_t prev = s_cur.n ? s_cur.state[s_cur.n - 1u] : s_cur.prev_state;
            uint8_t st = r->hc_dec[((prev >> 3) << 8) | endpoint(r, j)];
            c = r->hc_pair[(prev << 5) | st];
            s_cur.state[s_cur.n] = st;
        } else {
            c = (uint8_t)(((128 + r->phase[endpoint(r, j)] -
                            r->phase[endpoint(r, j - 1u)]) & 255) >> 2);
        }
        s_cur.code[s_cur.n++] = c;
    }
    return s_cur.code[k - s_cur.base];
}

/* HC synthesis cells per (decoder bank, state): the bank the hardware uses
 * is the previous state's quadrant, which synthesis knows. Radius 3..7.5
 * steps (preferring ~5) covers all 32 states in every bank. */
static uint8_t s_cell_for_state[4][32];
static const uint8_t *s_cell_lut_hc;

static void build_hc_cells(const uint8_t *dec)
{
    if (!dec || s_cell_lut_hc == dec) return;
    for (int bank = 0; bank < 4; ++bank) {
        int best[32];
        for (int t = 0; t < 32; ++t) best[t] = 1 << 30;
        for (int raw = 0; raw < 256; ++raw) {
            int q = (int8_t)((raw & 15) << 4) >> 4;
            int i = (int8_t)(raw & 240) >> 4;
            int r2x4 = (2 * i + 1) * (2 * i + 1) + (2 * q + 1) * (2 * q + 1);
            if (r2x4 < 36 || r2x4 > 225) continue;
            uint8_t st = dec[(bank << 8) | raw];
            int score = r2x4 > 100 ? r2x4 - 100 : 100 - r2x4;
            if (score < best[st]) {
                best[st] = score;
                s_cell_for_state[bank][st] = (uint8_t)raw;
            }
        }
    }
    s_cell_lut_hc = dec;
}

void sfw_init(sync_flywheel_t *f)
{
    memset(f, 0, sizeof(*f));
    f->state = SFW_ACQUIRE;
}

bool sfw_locked(const sync_flywheel_t *f)
{
    return f->state == SFW_TRACK && f->clean_since_acq >= SFW_LOCK_CLEAN &&
           f->lines_since_clean <= SFW_COAST_LINES;
}

int sfw_standard(const sync_flywheel_t *f)
{
    if (f->state != SFW_TRACK) return 0;
    return f->nominal_q8 == SFW_PAL_PERIOD_Q8 ? 1 : 2;
}

/* Emit a constant code over [a, b), starting from the real preceding
 * phase/state. Phase8: each endpoint rotates by the phase step Phase8 maps to
 * `code`. HC: each endpoint advances the 5-bit state by the step whose
 * nominal pair code is closest (code = 20 + 6 * step). The middle byte splits
 * the step so the 40 MS/s trajectory stays smooth. */
static void synth(sync_flywheel_t *f, const sfw_ring_t *r, uint64_t a,
                  uint64_t b, uint64_t floor, int code)
{
    bool hc = r->hc_dec != NULL;
    int d, prev;
    /* HC codes step by 6 per state (20 + 6 * step): a fractional
     * accumulator alternates steps so the mean hits `code`, like a real
     * signal between two state steps does. */
    int32_t d_q8 = 0, acc = 0;
    if (hc) {
        d_q8 = (int32_t)(code - 20) * 256 / 6;
        d = 0;
        cur_open(r, a);                       /* reads the ring as patched */
        prev = s_cur.prev_state;
    } else {
        d = 4 * code - 126;
        prev = r->phase[endpoint(r, a - 1u)];
    }
    bool skipped = false;
    for (uint64_t k = a; k < b; ++k) {
        if (k < floor) {
            prev = hc ? r->hc_dec[((prev >> 3) << 8) | endpoint(r, k)]
                      : r->phase[endpoint(r, k)];
            skipped = true;
            continue;
        }
        uint8_t e, m;
        if (hc) {
            acc += d_q8;
            d = acc >= 0 ? (acc + 128) >> 8 : -((-acc + 128) >> 8);
            acc -= d * 256;
            e = s_cell_for_state[prev >> 3][(prev + d) & 31];
            m = s_cell_for_state[prev >> 3][(prev + d / 2) & 31];
        } else {
            e = s_cell_for_phase[(prev + d) & 255];
            m = s_cell_for_phase[(prev + d / 2) & 255];
        }
        uint32_t at = idx(r, k) * 2u;
        r->ring[at] = m;
        r->ring[at + 1u] = e;
        prev = hc ? (prev + d) & 31 : r->phase[e];
    }
    if (skipped) ++f->skipped_floor;
    s_cur.r = NULL;                           /* ring changed: drop cache */
}

static void acquire(sync_flywheel_t *f, const sfw_ring_t *r,
                    uint64_t avail_end, uint32_t max_scan)
{
    if (avail_end < 4096u) return;
    uint64_t oldest = avail_end > r->ring_pairs / 2u ?
                      avail_end - r->ring_pairs / 2u : 1u;
    if (f->scan_pos < oldest) f->scan_pos = oldest;
    uint64_t end = avail_end - SFW_SPAN;
    if (end > f->scan_pos + max_scan) end = f->scan_pos + max_scan;
    if (end < f->scan_pos + 2600u) return;          /* need two lines */

    uint16_t hist[64] = {0};
    uint32_t n = 0;
    for (uint64_t k = f->scan_pos; k < end; ++k, ++n) ++hist[code_at(r, k)];
    unsigned c = 0, p2 = 0, p50 = 0;
    bool got2 = false;
    for (unsigned v = 0; v < 64u; ++v) {
        c += hist[v];
        if (!got2 && c * 50u >= n) { p2 = v; got2 = true; }
        if (c * 2u >= n) { p50 = v; break; }
    }
    if (p50 < p2 + 4u) { f->scan_pos = end; return; }
    f->thr = (uint8_t)(p2 + (p50 - p2) / 4u);

    uint64_t starts[8];
    unsigned count = 0, run = 0;
    for (uint64_t k = f->scan_pos; k < end; ++k) {
        if (code_at(r, k) <= f->thr) { ++run; continue; }
        if (run >= 80u && run <= 110u && count < 8u) starts[count++] = k - run;
        run = 0;
    }
    for (unsigned a = 0; a + 1u < count; ++a) {
        uint64_t p = starts[a + 1u] - starts[a];
        int32_t nominal;
        if (p >= 1275u && p <= 1285u) nominal = SFW_PAL_PERIOD_Q8;
        else if (p >= 1266u && p <= 1276u) nominal = SFW_NTSC_PERIOD_Q8;
        else continue;
        f->state = SFW_TRACK;
        f->nominal_q8 = f->period_q8 = nominal;
        f->field_lines = nominal == SFW_PAL_PERIOD_Q8 ? 313u : 263u;
        f->next_q8 = ((starts[a + 1u]) << 8) + (uint64_t)nominal;
        f->hit_q8 = 128u;
        f->clean_since_acq = 0;
        f->lines_since_clean = 0;
        f->field_valid = false;
        f->vertical_run = 0;
        ++f->acquisitions;
        return;
    }
    f->scan_pos = end;
}

static unsigned mean_code(const sfw_ring_t *r, uint64_t a, uint64_t b)
{
    unsigned sum = 0;
    for (uint64_t k = a; k < b; ++k) sum += code_at(r, k);
    return (sum << 4) / (unsigned)(b - a);          /* Q4 */
}

/* Low-code run length from `start`, tolerating single noisy samples;
 * `inside` (optional) counts the tolerated high samples within the run. */
static unsigned pulse_width(const sync_flywheel_t *f, const sfw_ring_t *r,
                            uint64_t start, unsigned *inside)
{
    unsigned width = 0, highs = 0, total = 0;
    while (width < 600u && highs < 3u) {
        if (code_at(r, start + width) <= f->thr) highs = 0;
        else { ++highs; ++total; }
        ++width;
    }
    if (inside) *inside = total - highs;
    return width - highs;
}

/* Normal H sync ~94 and broad ~540 pairs with few gaps (a noisy real pulse
 * still reads mostly low); equalizing ~47 must be nearly solid. Gappy runs
 * of the same length are noise (HC maps ~half of random cells low). */
static bool plausible_pulse(unsigned width, unsigned inside)
{
    if (width >= 70u && width <= 130u) return inside <= width / 8u;
    if (width >= 200u) return inside <= width / 8u;
    if (width >= 40u && width <= 55u) return inside <= 2u;
    return false;
}

unsigned sfw_run(sync_flywheel_t *f, const sfw_ring_t *r, uint64_t avail_end,
                 uint64_t write_floor, bool allow_repair, bool allow_colour_kill,
                 uint32_t max_scan)
{
    if (!f || !r || !r->ring || !r->phase || !r->ring_pairs ||
        (r->ring_pairs & (r->ring_pairs - 1u))) return 0;
    build_cells(r->phase);
    build_hc_cells(r->hc_dec);
    if (r->hc_dec && !r->hc_pair) return 0;
    if (f->state == SFW_ACQUIRE) {
        acquire(f, r, avail_end, max_scan);
        if (f->state == SFW_ACQUIRE) return 0;
    }
    unsigned processed = 0;
    while (f->state == SFW_TRACK) {
        uint64_t pred = (f->next_q8 + 128u) >> 8;
        if (pred + SFW_WIN_UNLOCKED + SFW_SPAN + 600u > avail_end) break;
        if (pred + r->ring_pairs / 2u < avail_end) {
            /* Fell behind the ring: data is gone. Re-acquire. */
            f->state = SFW_ACQUIRE;
            f->scan_pos = avail_end > 4096u ? avail_end - 4096u : 0u;
            break;
        }
        bool locked = sfw_locked(f);
        /* The window widens while coasting (drift grows with coasted lines)
         * so a real pulse is found again and never mistaken for a missing
         * one after a long fade. */
        int win = SFW_WIN_UNLOCKED;
        if (locked) {
            win = SFW_WIN_LOCKED + (int)(f->lines_since_clean / 25u);
            if (win > SFW_WIN_UNLOCKED) win = SFW_WIN_UNLOCKED;
        }

        /* Leading edge: first run of SFW_MIN_RUN low codes in the window. */
        /* `found` is the edge offset from the prediction and may be
         * negative (early pulse); `have` says whether a pulse was found.
         * First pass: a leading edge (high -> SFW_MIN_RUN lows) that forms a
         * plausible pulse; implausible noise runs are skipped, not taken. */
        int found = 0;
        bool have = false;
        unsigned width = 0;
        uint64_t start = pred;
        for (int e = -win; e <= win && !have; ++e) {
            uint64_t k = pred + (uint64_t)(int64_t)e;
            if (code_at(r, k - 1u) <= f->thr) continue;
            unsigned run = 0;
            while (run < SFW_MIN_RUN && code_at(r, k + run) <= f->thr) ++run;
            if (run < SFW_MIN_RUN) continue;
            unsigned inside = 0;
            unsigned w = pulse_width(f, r, k, &inside);
            if (plausible_pulse(w, inside)) {
                found = e; have = true; start = k; width = w;
            }
        }
        if (!have) {
            /* Second pass without the leading-edge test: after a fade the
             * sample before a real pulse can itself be a low noise code.
             * Nearest-first from the prediction; only a solid pulse of
             * H-sync width counts (a real pulse has no gaps). */
            for (int e = 0; e <= win && !have; e = e > 0 ? -e : 1 - e) {
                uint64_t k = pred + (uint64_t)(int64_t)e;
                unsigned run = 0;
                while (run < SFW_MIN_RUN && code_at(r, k + run) <= f->thr) ++run;
                if (run < SFW_MIN_RUN) continue;
                unsigned inside = 0;
                unsigned w = pulse_width(f, r, k, &inside);
                if (w >= 88u && w <= 100u && inside <= 2u) {
                    found = e; have = true; start = k; width = w;
                }
            }
        }
        bool normal = have && width >= 70u && width <= 130u;
        bool broad = have && width >= 200u;
        bool equalizing = have && width >= 40u && width <= 55u;
        bool vertical = broad || equalizing;
        bool clean = normal && (!locked ||
                     (found >= -SFW_CLEAN_ERR && found <= SFW_CLEAN_ERR));

        /* PLL: phase gain 1/4, frequency gain 1/64, period within 0.3 %. */
        uint64_t base = f->next_q8;
        if (normal || broad) {
            int32_t err_q8 = found * 256;
            f->period_q8 += err_q8 / 64;
            int32_t lim = f->nominal_q8 / 333;
            if (f->period_q8 > f->nominal_q8 + lim) f->period_q8 = f->nominal_q8 + lim;
            if (f->period_q8 < f->nominal_q8 - lim) f->period_q8 = f->nominal_q8 - lim;
            f->next_q8 = base + (uint64_t)(int64_t)(f->period_q8 + err_q8 / 4);
        } else {
            f->next_q8 = base + (uint64_t)(int64_t)f->period_q8;
        }
        f->hit_q8 = (uint16_t)(f->hit_q8 + ((clean ? 256 : 0) - (int)f->hit_q8) / 16);

        /* Field tracking from runs of broad/equalizing pulses. */
        if (vertical) {
            /* Accept a V sync where the field counter expects it (after a
             * coasted wrap it sits near the field start). */
            if (++f->vertical_run == 3u &&
                (!f->field_valid || f->line_in_field > f->field_lines / 2u ||
                 f->line_in_field < 12u)) {
                f->line_in_field = 3u;
                f->field_valid = true;
                f->fields_coasted = 0;
                ++f->vsyncs;
            }
        } else {
            /* A real vertical interval is a run of consecutive broad /
             * equalizing lines; anything else ends it (a lone noise run
             * that looked like an equalizing pulse must not stick). */
            f->vertical_run = 0;
        }
        ++f->line_in_field;
        if (f->field_valid) {
            /* Predicted field end: wrap to the next field's start (same +2
             * offset a detected V sync gives), alternating the half line. */
            uint16_t len = (uint16_t)(f->field_lines - (f->field_parity ? 1u : 0u));
            if (f->line_in_field >= len + 2u) {
                f->line_in_field = (uint16_t)(f->line_in_field - len);
                f->field_parity = !f->field_parity;
                if (++f->fields_coasted > SFW_FIELD_COAST) f->field_valid = false;
            }
        }
        bool in_vwin = !f->field_valid || f->vertical_run ||
                       f->line_in_field < 20u ||
                       f->line_in_field + 8u >= f->field_lines;

        ++f->lines;
        ++processed;
        if (clean) {
            ++f->clean;
            if (f->clean_since_acq < 0xFFFFu) ++f->clean_since_acq;
            f->lines_since_clean = 0;
            if ((f->clean & 3u) == 0u) {
                unsigned s = mean_code(r, start + SFW_SYNC_LEVEL_FROM,
                                       start + SFW_SYNC_LEVEL_TO);
                unsigned b = mean_code(r, start + SFW_BLANK_FROM,
                                       start + SFW_BLANK_TO);
                f->sync_q4 = f->sync_q4 ? (uint16_t)((7u * f->sync_q4 + s) / 8u) : (uint16_t)s;
                f->blank_q4 = f->blank_q4 ? (uint16_t)((7u * f->blank_q4 + b) / 8u) : (uint16_t)b;
            }
        } else {
            if (!have) ++f->missed;
            if (f->lines_since_clean < 0xFFFFu) ++f->lines_since_clean;
        }

        bool levels = f->sync_q4 && f->blank_q4 > f->sync_q4 + 32u;
        /* Only missing or malformed pulses are rewritten. A real pulse
         * that merely sits off the prediction (e.g. after coasting) is
         * left alone; it steers the PLL back instead. */
        bool repair = !normal && !vertical && locked && allow_repair &&
                      !in_vwin && levels;
        uint64_t line = (base + 128u) >> 8;
        if (repair) {
            synth(f, r, line, line + SFW_SYNC_PAIRS, write_floor,
                  (f->sync_q4 + 8u) >> 4);
            ++f->repaired;
        }
        int32_t repair_delta = (repair ? 65536 : 0) - (int32_t)f->repair_q16;
        f->repair_q16 = (uint32_t)((int32_t)f->repair_q16 + repair_delta / 1024);  /* ~65 ms */
        if (!f->colour_kill && f->repair_q16 > SFW_KILL_ON_Q16) {
            f->colour_kill = true;
            ++f->kill_on_events;
        } else if (f->colour_kill && f->repair_q16 < SFW_KILL_OFF_Q16) {
            f->colour_kill = false;
        }
        /* Anchor the burst window on the real pulse when one was found, so
         * a coasted prediction never overlaps the end of the actual sync. */
        uint64_t anchor = normal ? start : line;
        if (f->colour_kill && allow_colour_kill && locked && levels && !in_vwin)
            synth(f, r, anchor + SFW_BURST_FROM, anchor + SFW_BURST_TO,
                  write_floor, (f->blank_q4 + 8u) >> 4);

        if (f->lines_since_clean > SFW_LOST_LINES) {
            /* Nothing recognisable for a long time: re-acquire. */
            f->state = SFW_ACQUIRE;
            f->scan_pos = pred;
            f->colour_kill = false;
        }
    }
    return processed;
}
