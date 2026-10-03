/* C5VRX by Twotoz and contributors. No RF gain or sample-paced CPU work. */
#include "cvbs_level.h"
#include "cvbs_tables.h"
#include <string.h>
static int magnitude(int v) { return v < 0 ? -v : v; }
static int delta(unsigned i)
{
    int d = (int)(i & 63) * 4 + 2 - 128;
    if ((i >> 6) == 1 && d < 0) d += 256;
    if ((i >> 6) == 2 && d >= 0) d -= 256;
    return d;
}
static uint8_t nearest(int64_t uv)
{
    unsigned best = 0;
    int64_t distance = INT64_MAX;
    for (unsigned c = 0; c < 64; ++c) {
        int64_t e = (int64_t)c5v4_dac_uv[c] - uv;
        if (e < 0) e = -e;
        if (e < distance) { distance = e; best = c; }
    }
    return (uint8_t)best;
}
void c5v4_level_init(c5v4_level_t *s)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->codes, c5v4_dac_codes, sizeof(s->codes));
}
uint16_t c5v4_level_word(uint16_t original, unsigned code)
{
    return (uint16_t)((original & ~63u) | (code & 63u));
}
bool c5v4_level_observe(c5v4_level_t *s, const c5v4_cvbs_stats_t *v,
                      bool fresh, uint32_t context, uint64_t now)
{
    if (!s->context_valid || s->context != context) {
        s->context = context; s->context_valid = true; s->good = 0;
    }
    /* Phase level evidence must agree across three fresh consecutive windows.
     * Reject origin collapse, folding/overload and distorted pulse plateaus.
     * 12 bins is the existing observer floor; never chase arbitrarily small sync.
     * Nominal gain range is about 0.32..3.2x the initial 0.15 V/MHz transfer. */
    bool valid = fresh && v && v->levels_valid && v->repeated &&
        v->span_bins >= 12 && v->span_bins <= 120 &&
        v->sync_mad_bins <= 4 && v->blank_mad_bins <= 4 &&
        v->origin_pm <= 350 && v->ambiguous_pm <= 100 && v->clip_pm <= 200 &&
        v->period_raw >= 2529 && v->period_raw <= 2574;
    if (!valid) { s->good = 0; ++s->refusals; return false; }
    if (s->good && (magnitude(v->blank_bins - s->blank) > 4 ||
                   magnitude(v->span_bins - s->span) > 4)) s->good = 0;
    s->blank = v->blank_bins; s->span = v->span_bins;
    if (s->good < 3) ++s->good;
    if (s->good < 3 || (s->updates && now - s->last_us < 100000)) return false;
    bool changed = false;
    for (unsigned i = 0; i < 256; ++i) {
        /* Fixed black and sync separation, BEFORE clipping to the loaded DAC.
         * Ambiguous trajectories become the black reference, never a rail. */
        int64_t uv = (i >> 6) == 3 ? 300000 :
            300000 + (int64_t)(delta(i) - s->blank) * 300000 / s->span;
        uint8_t target = nearest(uv), current = s->codes[i];
        /* Limit each intermediate table change to one DAC code per 100 ms.
         * This is a gradual sequential update, not an atomic bank switch. */
        if (target > current) ++current;
        else if (target < current) --current;
        if (s->codes[i] != current) { s->codes[i] = current; changed = true; }
    }
    if (changed) { ++s->updates; s->last_us = now; }
    return changed;
}
