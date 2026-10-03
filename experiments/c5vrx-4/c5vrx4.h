#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Optional native tracking gate; entirely inert under Direct Gain V5.
 * Startup gain ownership comes from the separate c5vrx4 NVS namespace. */
void c5vrx4_start(void);
void c5vrx4_suspend(void);
void c5vrx4_resume(void);
bool c5vrx4_console(int key);
bool c5vrx4_history_enabled(void);
/* Fixed-lane comparison: ultrafine is opt-in; protected V5 lanes are default. */
bool c5vrx4_ultrafine_forced(void);
/* M: output transfer, NVS c5vrx4/cvbs_legacy (1 keeps LEGACY_FULL). */
enum { C5VRX4_CVBS_STD150 = 0, C5VRX4_CVBS_LEGACY = 1, C5VRX4_CVBS_150 = 2 };
unsigned c5vrx4_cvbs_mode(void);
const char *c5vrx4_cvbs_mode_name(void);
bool c5vrx4_cvbs_legacy_enabled(void);

bool c5vrx4_lane_window_ready(uint64_t now_us);
uint8_t c5vrx4_lane_target(uint8_t current, uint8_t requested, const uint8_t *sample, size_t bytes, uint64_t now_us);
void c5vrx4_lane_print(void);

/* u: experimental automatic CVBS level servo, opt-in/reboot. */
bool c5vrx4_level_enabled(void);
