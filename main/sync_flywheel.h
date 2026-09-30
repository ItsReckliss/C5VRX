#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Sync flywheel: keeps the goggle's sync separator locked when RF noise
 * breaks individual H-sync pulses near the range edge.
 *
 * The live Phase8 BitScrambler turns each 16-bit ring pair into one DAC code
 * from the endpoint byte (byte 2k+1): code = ((128 + p(e_k) - p(e_k-1)) & 255)
 * >> 2. The flywheel mirrors that on the CPU for a small window around each
 * predicted line start, tracks line phase/period with a PLL, and for lines
 * whose pulse is missing or malformed it rewrites the raw IQ bytes of the
 * pulse *before TX reads them* so Phase8 itself emits a clean sync at the
 * learned sync level. Clean lines are never touched and nothing is written in
 * the vertical interval. Optionally the burst window is blanked (colour
 * killer) while a large share of lines needs repair. */

typedef struct {
    uint8_t *ring;          /* raw IQ ring shared by RX and TX */
    uint32_t ring_pairs;    /* ring bytes / 2 */
    const uint8_t *phase;   /* Phase8 phase per raw byte (256 entries) */
    /* History-conditioned demodulator (fm_hc.bsasm) when non-NULL:
     * decoder[(prev_quadrant << 8) | raw] -> 5-bit state,
     * pair[(prev << 5) | cur] -> DAC code. NULL = Phase8 endpoint codes. */
    const uint8_t *hc_dec, *hc_pair;
} sfw_ring_t;

typedef enum { SFW_ACQUIRE = 0, SFW_TRACK = 1 } sfw_state_t;

typedef struct {
    sfw_state_t state;
    uint64_t scan_pos;        /* acquisition: next pair to scan */
    uint64_t next_q8;         /* predicted next line start, pair index Q8 */
    int32_t period_q8;        /* line period in pairs, Q8 */
    int32_t nominal_q8;
    uint8_t thr;              /* sync/not-sync code threshold */
    uint16_t sync_q4, blank_q4;   /* learned levels, code Q4 */
    uint16_t hit_q8;          /* EMA of clean-line rate, Q8 (256 = all) */
    uint16_t clean_since_acq, lines_since_clean;
    uint32_t repair_q16;      /* EMA of repaired-line rate, Q16 */
    uint16_t line_in_field;
    uint16_t field_lines;     /* 313 PAL / 263 NTSC (half line rounded up) */
    uint16_t vertical_run;
    bool field_valid;
    bool field_parity;        /* alternates 313/312 (PAL), 263/262 (NTSC) */
    uint8_t fields_coasted;   /* fields since the last detected V sync */
    bool colour_kill;
    /* counters */
    uint32_t lines, clean, repaired, missed, vsyncs, skipped_floor, acquisitions;
    uint32_t kill_on_events;
} sync_flywheel_t;

/* 1280 pairs/line: PAL 64 us at 20 MS/s. 1271.1: NTSC 63.556 us. */
#define SFW_PAL_PERIOD_Q8  (1280 * 256)
#define SFW_NTSC_PERIOD_Q8 325402      /* 1271.1 * 256 */

void sfw_init(sync_flywheel_t *f);

/* Process predicted lines whose analysis window lies before avail_end
 * (absolute pair index, exclusive: data RX has completed), at most
 * max_lines per call (the rest carries over). Writes only to pairs >=
 * write_floor (ahead of the TX read position). Returns lines processed.
 * max_scan bounds acquisition work per call; the caller also rate-limits
 * acquisition calls so a missing signal never starves the CPU. */
unsigned sfw_run(sync_flywheel_t *f, const sfw_ring_t *r, uint64_t avail_end,
                 uint64_t write_floor, bool allow_repair, bool allow_colour_kill,
                 uint32_t max_scan, unsigned max_lines);

bool sfw_locked(const sync_flywheel_t *f);
/* 1 = PAL, 2 = NTSC, 0 = unknown (from the tracked line period). */
int sfw_standard(const sync_flywheel_t *f);
