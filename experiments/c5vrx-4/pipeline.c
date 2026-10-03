/* C5VRX by Twotoz and contributors: span75 transport and opt-in native gate. */
#include "c5vrx4.h"
#include "sdkconfig.h"
#if !CONFIG_C5VRX_DIRECT_GAIN_V3_EXPERIMENT
#error "C5VRX-4 requires CONFIG_C5VRX_DIRECT_GAIN_V3_EXPERIMENT=y for Direct Gain V5"
#endif
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rf.h"
#include "cvbs_tables.h"

#define AGC_CTRL (*(volatile uint32_t *)0x600a7030u)
#define AGC_HOLD (1u << 29)
#define PERIOD_US 1000u
#define WINDOW_US 20u
static gptimer_handle_t s_timer;
static SemaphoreHandle_t s_transition_lock;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_requested = true, s_running, s_open;
static unsigned s_suspend_depth;
static uint64_t s_next_open, s_opened_at;
static uint32_t s_opens, s_faults, s_late_max, s_open_max;
static bool s_history_loaded, s_history = false;
static bool s_cvbs_loaded;
static unsigned s_cvbs_mode;
static bool s_level_loaded, s_level;

bool c5vrx4_level_enabled(void)
{
    if (!s_level_loaded) {
        nvs_handle_t h; uint8_t enabled = 1;
        if (nvs_open("c5vrx4", NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_u8(h, "level_lab", &enabled); nvs_close(h);
        }
        s_level = enabled == 1; s_level_loaded = true;
    }
    return s_level;
}

static bool s_ultrafine_loaded, s_ultrafine = false;

unsigned c5vrx4_cvbs_mode(void)
{
    if (!s_cvbs_loaded) {
        nvs_handle_t handle;
        uint8_t mode = C5VRX4_CVBS_STD150;
        if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
            /* Historical key: 1 still means LEGACY_FULL. */
            (void)nvs_get_u8(handle, "cvbs_legacy", &mode);
            nvs_close(handle);
        }
        s_cvbs_mode = mode <= C5VRX4_CVBS_150 ? mode : C5VRX4_CVBS_STD150;
        s_cvbs_loaded = true;
    }
    return s_cvbs_mode;
}

bool c5vrx4_cvbs_legacy_enabled(void)
{
    return c5vrx4_cvbs_mode() == C5VRX4_CVBS_LEGACY;
}

const char *c5vrx4_cvbs_mode_name(void)
{
    static const char *const names[] = {"STD150", "LEGACY_FULL", "CVBS150"};
    return names[c5vrx4_cvbs_mode()];
}

bool c5vrx4_ultrafine_forced(void)
{
    if (!s_ultrafine_loaded) {
        nvs_handle_t handle;
        uint8_t enabled = 0;
        if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
            (void)nvs_get_u8(handle, "force_ultra_v2", &enabled);
            nvs_close(handle);
        }
        s_ultrafine = enabled != 0;
        s_ultrafine_loaded = true;
    }
    return s_ultrafine;
}

bool c5vrx4_history_enabled(void)
{
    if (!s_history_loaded) {
        nvs_handle_t handle;
        uint8_t enabled = 0;
        if (nvs_open("c5vrx4", NVS_READONLY, &handle) == ESP_OK) {
            (void)nvs_get_u8(handle, "unwrap_hc", &enabled);
            nvs_close(handle);
        }
        s_history = enabled != 0;
        s_history_loaded = true;
    }
    return s_history;
}

static bool IRAM_ATTR gate_alarm(gptimer_handle_t timer,
                                const gptimer_alarm_event_data_t *event,
                                void *context)
{
    (void)context;
    portENTER_CRITICAL_ISR(&s_lock);
    if (!s_running) {
        AGC_CTRL &= ~AGC_HOLD;
        portEXIT_CRITICAL_ISR(&s_lock);
        return false;
    }
    uint64_t now = event->count_value;
    uint64_t delay = now > event->alarm_value ? now - event->alarm_value : 0;
    if (delay > s_late_max) s_late_max = (uint32_t)delay;
    uint64_t next;
    if (s_open) {
        AGC_CTRL |= AGC_HOLD;
        uint64_t duration = now - s_opened_at;
        if (duration > s_open_max) s_open_max = (uint32_t)duration;
        s_open = false;
        /* A late callback skips missed periods, never emits a catch-up burst. */
        if (s_next_open <= now)
            s_next_open += ((now - s_next_open) / PERIOD_US + 1) * PERIOD_US;
        next = s_next_open;
    } else {
        AGC_CTRL &= ~AGC_HOLD;
        s_open = true;
        s_opened_at = now;
        ++s_opens;
        s_next_open += PERIOD_US;
        next = now + WINDOW_US;
    }
    gptimer_alarm_config_t alarm = {.alarm_count = next};
    if (gptimer_set_alarm_action(timer, &alarm) != ESP_OK) {
        ++s_faults;
        s_running = false;
        AGC_CTRL &= ~AGC_HOLD;
    }
    portEXIT_CRITICAL_ISR(&s_lock);
    return false;
}

void c5vrx4_suspend(void)
{
    /* Direct Gain owns the hold state. No timer/register writes in this mode. */
    if (!rf_native_agc_active()) return;
    if (!s_transition_lock) return;
    xSemaphoreTakeRecursive(s_transition_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_lock);
    ++s_suspend_depth;
    s_running = false;
    AGC_CTRL &= ~AGC_HOLD;
    portEXIT_CRITICAL(&s_lock);
    if (s_timer) (void)gptimer_stop(s_timer);
}

void c5vrx4_resume(void)
{
    /* Direct Gain owns the hold state. No timer/register writes in this mode. */
    if (!rf_native_agc_active()) return;
    if (!s_transition_lock) return;
    portENTER_CRITICAL(&s_lock);
    if (s_suspend_depth) --s_suspend_depth;
    bool start = s_timer && s_requested && rf_native_agc_active() &&
                 !s_suspend_depth && !s_running;
    portEXIT_CRITICAL(&s_lock);
    if (!start) {
        xSemaphoreGiveRecursive(s_transition_lock);
        return;
    }
    /* Operator-selected native acquisition profile 127, no gain index write. */
    volatile uint32_t *speed = (volatile uint32_t *)0x600a7034u;
    *speed = (*speed & ~(0x7fu << 24)) | (127u << 24);
    ESP_ERROR_CHECK(gptimer_set_raw_count(s_timer, 0));
    gptimer_alarm_config_t alarm = {.alarm_count = WINDOW_US};
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_timer, &alarm));
    portENTER_CRITICAL(&s_lock);
    AGC_CTRL &= ~AGC_HOLD;
    s_running = s_open = true;
    s_opened_at = 0;
    s_next_open = PERIOD_US;
    ++s_opens;
    portEXIT_CRITICAL(&s_lock);
    ESP_ERROR_CHECK(gptimer_start(s_timer));
    xSemaphoreGiveRecursive(s_transition_lock);
}

static void print_state(void)
{
    c5vrx4_lane_print();
    portENTER_CRITICAL(&s_lock);
    bool running = s_running, open = s_open;
    uint32_t opens = s_opens, faults = s_faults;
    uint32_t late = s_late_max, duration = s_open_max;
    uint32_t control = AGC_CTRL;
    portEXIT_CRITICAL(&s_lock);
    printf("C5VRX4 pipeline=unwrap75_%s span_ns=75 phase_bits=8 winding=quadrant3 bound_step_bins=63 dac_delta_bits=6 iq_bits=4+4 "
           "iq_hz=40000000 dac_hz=40000000 unique_hz=13333333 "
           "gain_owner=%s pace=%d acquiring=%d period_us=%u window_us=%u "
           "opens=%" PRIu32 " late_max_us=%" PRIu32 " open_max_us=%" PRIu32
           " faults=%" PRIu32 " ctrl=0x%08" PRIx32 "\n",
           c5vrx4_history_enabled() ? "history" : "static",
           rf_native_agc_active() ? "native" : "direct_gain_v5",
           running, open, PERIOD_US, WINDOW_US, opens, late, duration, faults,
            control);
    static const char *const slopes[] = {"0.150", "full_span", "0.150"};
    static const unsigned blanks[] = {310u, 0u, 300u};
    unsigned mode = c5vrx4_cvbs_mode();
    printf("C5VRX4_CVBS transfer=%s reference_mv=%u volts_per_mhz=%s "
           "calibration=%s load_ohms=75 level_lab=%u "
           "sync_repair=0 keys=M_cycle_reboot,J_snapshot\n",
           c5vrx4_cvbs_mode_name(),
           mode == C5VRX4_CVBS_LEGACY ? (unsigned)(c5v4_dac_uv[c5v4_dac_legacy_codes[32]] / 1000u) :
                                        blanks[mode],
           slopes[mode],
           C5V4_DAC_MEASURED ? "measured" : "nominal", c5vrx4_level_enabled());
    printf("C5VRX4_LANES policy=%s lane=%u adc_step=%u window_codes=%u "
           "fold_guard=%s\n", c5vrx4_ultrafine_forced() ? "fixed_ultrafine" : "protected_v5",
           rf_get_iq_lanes(), 64u >> rf_get_iq_lanes(),
           512u >> rf_get_iq_lanes(),
           c5vrx4_ultrafine_forced() ? "disabled_for_fixed_lane_test" : "baseline");
}

void c5vrx4_start(void)
{
    /* V5 owns gain in the default build: do not allocate/start a native gate
     * or touch its control/profile registers in this mode. */
    if (!rf_native_agc_active()) {
        print_state();
        return;
    }
    gptimer_config_t config = {.clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000};
    ESP_ERROR_CHECK(gptimer_new_timer(&config, &s_timer));
    gptimer_event_callbacks_t callbacks = {.on_alarm = gate_alarm};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &callbacks, NULL));
    ESP_ERROR_CHECK(gptimer_enable(s_timer));
    s_transition_lock = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(s_transition_lock ? ESP_OK : ESP_ERR_NO_MEM);
    c5vrx4_suspend();
    c5vrx4_resume();
    print_state();
}

bool c5vrx4_console(int key)
{
    if (key == 'u') {
        nvs_handle_t handle;
        bool enabled = !c5vrx4_level_enabled();
        esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, "level_lab", enabled ? 1 : 0);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        printf("C5VRX4 level_lab_next=%u err=%s action=%s\n", enabled,
               esp_err_to_name(err), err == ESP_OK ? "reboot" : "unchanged");
        if (err == ESP_OK) { fflush(stdout); vTaskDelay(pdMS_TO_TICKS(120)); esp_restart(); }
        return true;
    }
    if (key == 'M') {
        /* STD150 -> CVBS150 -> LEGACY_FULL -> STD150. */
        static const uint8_t next_mode[] = {C5VRX4_CVBS_150, C5VRX4_CVBS_STD150,
                                            C5VRX4_CVBS_LEGACY};
        static const char *const next_name[] = {"CVBS150", "STD150", "LEGACY_FULL"};
        nvs_handle_t handle;
        unsigned mode = c5vrx4_cvbs_mode();
        esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, "cvbs_legacy", next_mode[mode]);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        printf("C5VRX4 cvbs_next=%s err=%s action=%s\n",
               next_name[mode], esp_err_to_name(err),
               err == ESP_OK ? "reboot" : "unchanged");
        if (err == ESP_OK) { fflush(stdout); vTaskDelay(pdMS_TO_TICKS(120)); esp_restart(); }
        return true;
    }
    if (key == 'Z') {
        nvs_handle_t handle;
        bool enabled = !c5vrx4_ultrafine_forced();
        esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, "force_ultra_v2", enabled ? 1 : 0);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        printf("C5VRX4 ultrafine_next=%u err=%s action=%s\n", enabled,
               esp_err_to_name(err), err == ESP_OK ? "reboot" : "unchanged");
        if (err == ESP_OK) {
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(120));
            esp_restart();
        }
        return true;
    }
    if (key == 'h') {
        nvs_handle_t handle;
        bool enabled = !c5vrx4_history_enabled();
        esp_err_t err = nvs_open("c5vrx4", NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, "unwrap_hc", enabled ? 1 : 0);
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        printf("C5VRX4 history_next=%d err=%s action=%s\n", enabled,
               esp_err_to_name(err), err == ESP_OK ? "reboot" : "unchanged");
        if (err == ESP_OK) {
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(120));
            esp_restart();
        }
        return true;
    }
    if (key == 'T') {
        print_state();
        return false; /* Also print the ordinary receiver diagnostics. */
    }
    if (key == '~' && !rf_native_agc_active()) {
        printf("C5VRX4 pace_toggle=ignored gain_owner=direct_gain_v5 "
               "hint=N_selects_native_on_reboot\n");
        return true;
    }
    if (!s_transition_lock) return false;
    if (key == '~') {
        c5vrx4_suspend();
        if (s_requested) {
            s_requested = false;
        } else {
            s_requested = true;
        }
        c5vrx4_resume();
        print_state();
        return true;
    }
    return false;
}
