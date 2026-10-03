/* C5VRX by Twotoz and contributors. Bounded video level servo. */
#pragma once
#include "cvbs_monitor.h"
typedef struct {
    unsigned good, updates, refusals;
    int blank, span;
    uint32_t context;
    uint64_t last_us;
    bool context_valid;
    uint8_t codes[256];
} c5v4_level_t;
void c5v4_level_init(c5v4_level_t *state);
/* False freezes the last applied mapping, including through signal loss. */
bool c5v4_level_observe(c5v4_level_t *state, const c5v4_cvbs_stats_t *stats,
                      bool fresh, uint32_t context, uint64_t now_us);
uint16_t c5v4_level_word(uint16_t original, unsigned code);
