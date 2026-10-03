#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Snapshot estimator, never a source of live phase/PHY/output writes. */
typedef struct {
    unsigned pairs, ambiguous_pm, origin_pm, clip_pm;
    int mean_i_mcell, mean_q_mcell;
    unsigned pulses, repeated, period_raw;
    int sync_bins, blank_bins, span_bins, sync_mad_bins, blank_mad_bins;
    int sync_mv, blank_mv, sync_depth_mv;
    unsigned suggested_scale_q10;
    bool levels_valid;
} c5v4_cvbs_stats_t;
void c5v4_cvbs_analyze(const uint8_t *raw, size_t n, bool history, bool legacy,
                       c5v4_cvbs_stats_t *out);
