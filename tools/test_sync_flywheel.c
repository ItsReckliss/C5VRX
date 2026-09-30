/* Host test: synthetic PAL CVBS -> FM phase -> Q4/I4 endpoint cells in the
 * live ring layout, RX/TX lag as on hardware, decoded exactly like the live
 * demodulator (Phase8 endpoint formula, or the history-conditioned fm_hc
 * recursion). Verifies acquisition, lock, repair of broken H sync, untouched
 * clean and vertical-interval lines, the TX write floor, colour killer
 * hysteresis and PAL detection, in both demodulator modes. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fm_hc_lut.h"
#include "phase8_gain_lut.h"
#include "sync_flywheel.h"

#define RING_PAIRS 16384u
#define LINE 1280u
#define FIELD_LINES 312u
#define CHUNK 2046u
#define TX_LAG 8192u
#define TOTAL_LINES 10000u

static uint8_t ring[RING_PAIRS * 2u];
static uint8_t original[TOTAL_LINES * LINE * 2u + 8u];
static uint8_t cell_for[256];
static const uint8_t *ph = c5vrx_phase8_gain_lut;

/* Levels as Phase8 bins per 50 ns endpoint step. Phase8 FULL maps +-128
 * bins; Golden/HC map about -27..+57, so the HC case uses a signal inside
 * Golden's window (as a real VTX did in the Golden era). */
typedef struct { int sync, blank, white, jitter; } levels_t;

/* Deep fade: whole lines are noise, vertical sync included (~1.9 fields). */
static bool full_fade(unsigned line)
{
    return line >= 6000u && line < 6600u;
}

static bool broken_line(unsigned line)
{
    unsigned in_field = line % FIELD_LINES;
    if (in_field < 20u || in_field + 10u > FIELD_LINES) return false;
    if (line >= 400u && line < 1400u && line % 3u == 0u) return true;
    return line >= 1800u && line < 2300u;
}

static int desired_bins(const levels_t *lv, unsigned line, unsigned x)
{
    unsigned in_field = line % FIELD_LINES;
    int s = lv->sync, b = lv->blank;
    if (in_field < 3u)                      /* broad pulses */
        return (x < 540u || (x >= 640u && x < 1180u)) ? s : b;
    if (in_field < 6u)                      /* equalizing pulses */
        return (x < 47u || (x >= 640u && x < 687u)) ? s : b;
    if (x < 94u) return s;
    if (x >= 112u && x < 157u) return b + ((x & 1u) ? 6 : -6);     /* burst */
    if (x < 210u || x >= 1250u) return b;
    return b + (int)((x * 7u + line * 3u) % (unsigned)(lv->white - b));
}

static void run_case(bool hc, const levels_t *lv)
{
    srand(1);
    int phase = 0;
    size_t pairs = (size_t)TOTAL_LINES * LINE;
    for (size_t k = 0; k < pairs; ++k) {
        unsigned line = (unsigned)(k / LINE), x = (unsigned)(k % LINE);
        uint8_t e, m;
        if (full_fade(line) || (broken_line(line) && x < 120u)) {
            e = (uint8_t)rand();
            m = (uint8_t)rand();
        } else {
            int d = desired_bins(lv, line, x);
            if (lv->jitter)
                d += rand() % (2 * lv->jitter + 1) - lv->jitter;
            m = cell_for[(phase + d / 2) & 255];
            e = cell_for[(phase + d) & 255];
        }
        phase = ph[e];
        original[2 * k] = m;
        original[2 * k + 1] = e;
    }

    sync_flywheel_t f;
    sfw_init(&f);
    sfw_ring_t r = { ring, RING_PAIRS, ph,
                     hc ? c5vrx_hc_decoder : NULL,
                     hc ? c5vrx_hc_pair_code : NULL };
    size_t rx = 0, tx = 0;
    unsigned repaired_ok = 0, repaired_bad = 0, clean_touched = 0, vwin_touched = 0;
    int line_sum = 0, line_n = 0;
    unsigned fade_lines = 0, fade_patched = 0;
    int prev_phase = 0;
    uint8_t prev_state = 0;
    bool locked_seen = false;
    while (rx + CHUNK <= pairs) {
        for (size_t k = rx; k < rx + CHUNK; ++k) {
            uint32_t at = (uint32_t)(k & (RING_PAIRS - 1u)) * 2u;
            ring[at] = original[2 * k];
            ring[at + 1] = original[2 * k + 1];
        }
        rx += CHUNK;
        size_t tx_now = rx > TX_LAG ? rx - TX_LAG : 0;
        sfw_run(&f, &r, rx, tx_now + 256u, true, true, 4096u, 1000u);
        locked_seen |= sfw_locked(&f);
        int sync_code = (f.sync_q4 + 8) >> 4;
        for (size_t k = tx; k < tx_now; ++k) {
            uint32_t at = (uint32_t)(k & (RING_PAIRS - 1u)) * 2u;
            uint8_t e = ring[at + 1];
            int code;
            if (hc) {
                uint8_t st = c5vrx_hc_decoder[((prev_state >> 3) << 8) | e];
                code = c5vrx_hc_pair_code[(prev_state << 5) | st];
                prev_state = st;
            } else {
                code = ((128 + ph[e] - prev_phase) & 255) >> 2;
                prev_phase = ph[e];
            }
            unsigned line = (unsigned)(k / LINE), x = (unsigned)(k % LINE);
            bool modified = ring[at] != original[2 * k] ||
                            ring[at + 1] != original[2 * k + 1];
            unsigned in_field = line % FIELD_LINES;
            if (in_field < 12u || in_field + 6u >= FIELD_LINES) {
                if (modified) ++vwin_touched;
                continue;
            }
            bool faded = full_fade(line) && in_field >= 20u &&
                         in_field + 10u <= FIELD_LINES;
            if (!broken_line(line) && !full_fade(line) && modified && x < 100u) {
                ++clean_touched;
            }
            if (faded && x == 50u) ++fade_lines;
            if (faded && x >= 2u && x < 92u && modified) ++fade_patched;
            if ((broken_line(line) || faded) && line > 600u && x >= 2u && x < 92u) {
                if (!hc) {
                    if (code >= sync_code - 1 && code <= sync_code + 1) ++repaired_ok;
                    else ++repaired_bad;
                } else {
                    /* HC alternates 6-code steps: judge the pulse mean
                     * (what the goggle's sync separator sees). */
                    line_sum += code;
                    if (++line_n == 90) {
                        int mean16 = line_sum * 16 / 90;
                        if (mean16 >= f.sync_q4 - 24 && mean16 <= f.sync_q4 + 24)
                            repaired_ok += 90u;
                        else
                            repaired_bad += 90u;
                        line_sum = line_n = 0;
                    }
                }
            }
        }
        tx = tx_now;
    }
    printf("[%s] lines=%u clean=%u repaired=%u missed=%u vsyncs=%u acq=%u "
           "thr=%u sync_q4=%u blank_q4=%u kill_events=%u | ok=%u bad=%u "
           "clean_touched=%u vwin_touched=%u\n",
           hc ? "HC" : "Phase8", f.lines, f.clean, f.repaired, f.missed,
           f.vsyncs, f.acquisitions, f.thr, f.sync_q4, f.blank_q4,
           f.kill_on_events, repaired_ok, repaired_bad, clean_touched,
           vwin_touched);
    assert(locked_seen);
    assert(sfw_standard(&f) == 1);
    assert(f.vsyncs >= 5u);
    assert(f.blank_q4 > f.sync_q4 + 32u);
    assert(repaired_ok > 0u && repaired_bad * 50u <= repaired_ok);
    assert(clean_touched == 0u);
    /* The deep fade hides V sync for ~2 fields: repair and vertical
     * protection must both continue (field counter coasts). */
    printf("  deep fade: %u lines in picture region, %u pulse samples patched\n",
           fade_lines, fade_patched);
    assert(fade_patched >= fade_lines * 85u);
    assert(vwin_touched == 0u);
    assert(f.kill_on_events >= 1u);
    assert(!f.colour_kill);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int t = 0; t < 256; ++t) {
        int best = 999;
        for (int raw = 0; raw < 256; ++raw) {
            int q = (int8_t)((raw & 15) << 4) >> 4;
            int i = (int8_t)(raw & 240) >> 4;
            int r2 = (2 * i + 1) * (2 * i + 1) + (2 * q + 1) * (2 * q + 1);
            if (r2 < 64 || r2 > 169) continue;
            int e = ((ph[raw] - t + 128) & 255) - 128;
            if (e < 0) e = -e;
            if (e < best) { best = e; cell_for[t] = (uint8_t)raw; }
        }
    }
    /* Phase8: sync code 12, blank 22 (bins = 4 * code - 126). */
    const levels_t phase8 = { 4 * 12 - 126, 4 * 22 - 126, 4 * 50 - 126, 0 };
    /* HC/Golden window: sync -20 bins, blank 0, white +50. */
    const levels_t golden = { -20, 0, 50, 0 };
    /* Hardware (this VTX on Phase8 FULL): sync ~24.5, blanking ~29 codes,
     * i.e. only ~18 bins deep, with per-sample noise of +-1..2 codes. */
    const levels_t shallow = { 4 * 24 - 126, 4 * 29 - 126, 4 * 38 - 126, 6 };
    run_case(false, &phase8);
    run_case(true, &golden);
    run_case(false, &shallow);
    puts("sync flywheel: OK");
    return 0;
}
