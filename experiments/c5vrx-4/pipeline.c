#include "c5vrx4.h"
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rf.h"

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
    if (!s_transition_lock) return;
    portENTER_CRITICAL(&s_lock);
    if (s_suspend_depth) --s_suspend_depth;
    bool start = s_timer && s_requested && !s_suspend_depth && !s_running;
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
    portENTER_CRITICAL(&s_lock);
    bool running = s_running, open = s_open;
    uint32_t opens = s_opens, faults = s_faults;
    uint32_t late = s_late_max, duration = s_open_max;
    uint32_t control = AGC_CTRL;
    portEXIT_CRITICAL(&s_lock);
    printf("C5VRX4 pipeline=span75 phase_bits=6 iq_bits=4+4 "
           "iq_hz=40000000 dac_hz=40000000 unique_hz=13333333 "
           "pace=%d acquiring=%d period_us=%u window_us=%u "
           "opens=%" PRIu32 " late_max_us=%" PRIu32 " open_max_us=%" PRIu32
           " faults=%" PRIu32 " ctrl=0x%08" PRIx32 "\n",
           running, open, PERIOD_US, WINDOW_US, opens, late, duration, faults,
           control);
}

void c5vrx4_start(void)
{
    ESP_ERROR_CHECK(rf_native_agc_active() ? ESP_OK : ESP_ERR_INVALID_STATE);
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
    if (key == 'T') print_state();
    return false;
}
