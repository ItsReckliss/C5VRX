/* C5VRX by Twotoz and contributors: integrated range policy regressions. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rf.h"
#include "direct_gain_v3.h"
unsigned char phy_param[0x800];
static bool fixed;
static rf_iq_lane_stats_t lane_stats;
bool c5vrx4_ultrafine_forced(void) { return fixed; }
bool c5vrx4_history_enabled(void) { return false; }
uint8_t rf_get_iq_lanes(void) { return lane_stats.last_to; }
void rf_get_iq_lane_stats(rf_iq_lane_stats_t *s) { *s = lane_stats; }
#include "../lanes.c"

int main(void)
{
    uint8_t fit[256], outside[256];
    memset(fit, 0x11, sizeof(fit)); memset(outside, 0x33, sizeof(outside));
    assert(c5vrx4_lane_target(0, 2, fit, sizeof(fit), 1000) == 0);
    assert(c5vrx4_lane_target(0, 2, outside, sizeof(outside), 1200) == 0);
    assert(c5vrx4_lane_target(0, 2, fit, sizeof(fit), 1400) == 0);
    assert(c5vrx4_lane_target(0, 2, fit, sizeof(fit), 1600) == 1);
    assert(c5vrx4_lane_target(2, 0, NULL, 0, 1800) == 0);
    lane_stats.switches = 1; lane_stats.last_switch_us = 2000;
    assert(!c5vrx4_lane_window_ready(1999));
    assert(!c5vrx4_lane_window_ready(2209));
    assert(c5vrx4_lane_window_ready(2210));

    arc_gain_table_t table; arc_gain_table_from_bytes(&table, NULL, 81);
    direct_gain_v3_t v;
    dg3_observation_t severe = {.p50=65, .p95=113, .clip_pm=738,
                               .coherence=0, .observed_us=10000};
    direct_gain_v3_reset(&v, &table, 81, 62);
    direct_gain_v3_enable_lanes(&v, 2);
    assert(direct_gain_v3_tick(&v, &severe) == 20 && v.writes == 1);
    severe.observed_us += 100; /* Old saturated IQ after the drop: no hunt. */
    assert(direct_gain_v3_tick(&v, &severe) == 20 && v.writes == 1);
    direct_gain_v3_reset(&v, &table, 81, 62);
    direct_gain_v3_enable_lanes(&v, 2); v.lane = 2;
    severe.observed_us = 20000;
    assert(direct_gain_v3_tick(&v, &severe) == 81 && v.lane == 0);
    severe.observed_us += 1000;
    assert(direct_gain_v3_tick(&v, &severe) == 20);
    direct_gain_v3_reset(&v, &table, 81, 62);
    direct_gain_v3_enable_lanes(&v, 2);
    severe.clip_pm = 200; severe.observed_us = 30000;
    assert(direct_gain_v3_tick(&v, &severe) > 20); /* Moderate: physical stage cut. */
    direct_gain_v3_reset(&v, &table, 35, 62);
    dg3_observation_t good = {.p50=22, .p95=40, .coherence=99};
    for (unsigned n=0;n<100;n++) {
        good.observed_us = 40000 + n*1000;
        assert(direct_gain_v3_tick(&v, &good) == 35);
    }
    assert(v.writes == 0);
    puts("PASS: protected lanes, freshness, severe coarse overload floor, stale drop refusal and zero-write clean tracking");
}
