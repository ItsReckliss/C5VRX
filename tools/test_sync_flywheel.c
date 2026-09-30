/* Host test: synthetic PAL CVBS -> FM phase -> Q4/I4 endpoint cells in the
 * live ring layout, RX/TX lag as on hardware, decoded with the exact Phase8
 * BitScrambler formula. Verifies acquisition, lock, repair of broken H sync,
 * untouched clean and vertical-interval lines, the TX write floor, colour
 * killer hysteresis and PAL detection. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phase8_gain_lut.h"
#include "sync_flywheel.h"

#define RING_PAIRS 16384u
#define LINE 1280u
#define FIELD_LINES 312u
#define SYNC 12
#define BLANK 22
#define CHUNK 2046u
#define TX_LAG 8192u
#define TOTAL_LINES 6000u

static uint8_t ring[RING_PAIRS * 2u];
static uint8_t original[TOTAL_LINES * LINE * 2u + 8u];
static uint8_t cell_for[256];
static const uint8_t *ph = c5vrx_phase8_gain_lut;

static uint8_t broken_line(unsigned line)
{
    /* Break every 3rd line between 400 and 1400, and every line in
     * 1800..2300 (heavy fade), except field starts. */
    unsigned in_field = line % FIELD_LINES;
    if (in_field < 20u || in_field + 10u > FIELD_LINES) return 0;
    if (line >= 400u && line < 1400u && line % 3u == 0u) return 1;
    if (line >= 1800u && line < 2300u) return 1;
    return 0;
}

static int desired_code(unsigned line, unsigned x)
{
    unsigned in_field = line % FIELD_LINES;
    if (in_field < 3u)                      /* broad pulses */
        return (x < 540u || (x >= 640u && x < 1180u)) ? SYNC : BLANK;
    if (in_field < 6u)                      /* equalizing pulses */
        return (x < 47u || (x >= 640u && x < 687u)) ? SYNC : BLANK;
    if (x < 94u) return SYNC;
    if (x >= 112u && x < 157u) return BLANK + ((x & 1u) ? 3 : -3);  /* burst */
    if (x < 210u || x >= 1250u) return BLANK;
    return BLANK + (int)((x * 7u + line * 3u) % 29u);               /* picture */
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Nearest strong cell per phase (independent of the module's table). */
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
    /* Generate the whole stream (pair k: bytes 2k middle, 2k+1 endpoint). */
    srand(1);
    int phase = 0;
    size_t pairs = (size_t)TOTAL_LINES * LINE;
    for (size_t k = 0; k < pairs; ++k) {
        unsigned line = (unsigned)(k / LINE), x = (unsigned)(k % LINE);
        uint8_t e, m;
        if (broken_line(line) && x < 120u) {
            e = (uint8_t)rand();            /* noise: pulse destroyed */
            m = (uint8_t)rand();
        } else {
            int d = 4 * desired_code(line, x) - 126;
            m = cell_for[(phase + d / 2) & 255];
            e = cell_for[(phase + d) & 255];
        }
        phase = ph[e];
        original[2 * k] = m;
        original[2 * k + 1] = e;
    }

    sync_flywheel_t f;
    sfw_init(&f);
    sfw_ring_t r = { ring, RING_PAIRS, ph };
    size_t rx = 0, tx = 0;
    unsigned repaired_ok = 0, repaired_bad = 0, clean_touched = 0, vwin_touched = 0;
    int prev_out_phase = 0;
    bool locked_seen = false;
    while (rx + CHUNK <= pairs) {
        /* RX completes one descriptor. */
        for (size_t k = rx; k < rx + CHUNK; ++k) {
            uint32_t at = (uint32_t)(k & (RING_PAIRS - 1u)) * 2u;
            ring[at] = original[2 * k];
            ring[at + 1] = original[2 * k + 1];
        }
        rx += CHUNK;
        size_t tx_now = rx > TX_LAG ? rx - TX_LAG : 0;
        sfw_run(&f, &r, rx, tx_now + 256u, true, true, 4096u);
        locked_seen |= sfw_locked(&f);
        /* TX reads [tx, tx_now): decode with the BitScrambler formula. */
        for (size_t k = tx; k < tx_now; ++k) {
            uint32_t at = (uint32_t)(k & (RING_PAIRS - 1u)) * 2u;
            uint8_t e = ring[at + 1];
            int code = ((128 + ph[e] - prev_out_phase) & 255) >> 2;
            prev_out_phase = ph[e];
            unsigned line = (unsigned)(k / LINE), x = (unsigned)(k % LINE);
            bool modified = ring[at] != original[2 * k] ||
                            ring[at + 1] != original[2 * k + 1];
            unsigned in_field = line % FIELD_LINES;
            if (in_field < 12u || in_field + 6u >= FIELD_LINES) {
                if (modified) ++vwin_touched;
                continue;
            }
            if (!broken_line(line) && modified && x < 100u) ++clean_touched;
            if (broken_line(line) && line > 600u && x >= 2u && x < 92u) {
                if (code >= SYNC - 1 && code <= SYNC + 1) ++repaired_ok;
                else ++repaired_bad;
            }
        }
        tx = tx_now;
    }
    printf("lines=%u clean=%u repaired=%u missed=%u vsyncs=%u acq=%u "
           "sync_q4=%u blank_q4=%u period_q8=%ld kill_events=%u floor_skips=%u\n",
           f.lines, f.clean, f.repaired, f.missed, f.vsyncs, f.acquisitions,
           f.sync_q4, f.blank_q4, (long)f.period_q8, f.kill_on_events,
           f.skipped_floor);
    printf("repaired_ok=%u repaired_bad=%u clean_touched=%u vwin_touched=%u\n",
           repaired_ok, repaired_bad, clean_touched, vwin_touched);
    assert(locked_seen);
    assert(sfw_standard(&f) == 1);
    assert(f.vsyncs >= 5u);
    assert(f.sync_q4 >= (SYNC - 1) * 16 && f.sync_q4 <= (SYNC + 1) * 16);
    assert(f.blank_q4 >= (BLANK - 2) * 16 && f.blank_q4 <= (BLANK + 2) * 16);
    assert(repaired_ok > 0u && repaired_bad * 50u <= repaired_ok);
    assert(clean_touched == 0u);
    assert(vwin_touched == 0u);
    assert(f.kill_on_events >= 1u);      /* heavy fade 1800..2300 */
    assert(!f.colour_kill);              /* released after the fade */
    puts("sync flywheel: OK");
    return 0;
}
