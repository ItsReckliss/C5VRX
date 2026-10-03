/* C5VRX by Twotoz and contributors. Experimental LUT16 access lab.
 * Register source: ESP-IDF v6.0.2 ESP32-C5 bitscrambler_ll/struct headers.
 * This never uses load_lut (which changes the live width) or RUN/HALT/reset.
 * Live RAM arbitration and FIFO continuity still require physical acceptance.
 */
#include "cvbs_level_hw.h"
#include "cvbs_level.h"
#include "c5vrx4.h"
#include "soc/bitscrambler_struct.h"
#include "hal/bitscrambler_ll.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
static SemaphoreHandle_t access_lock;
void c5v4_level_hw_lock(void)
{
    if (!access_lock) access_lock = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(access_lock ? ESP_OK : ESP_ERR_NO_MEM);
    xSemaphoreTakeRecursive(access_lock, portMAX_DELAY);
}
void c5v4_level_hw_unlock(void) { xSemaphoreGiveRecursive(access_lock); }
static c5v4_level_t servo;
static uint16_t original[1024];
static bool ready;
static uint32_t writes, faults;
static uint16_t read_entry(unsigned index)
{
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg0.lut_idx = index;
    return (uint16_t)BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg1.lut;
}
static void write_entry(unsigned index, uint16_t value)
{
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg0.lut_idx = index;
    BITSCRAMBLER.lut_cfg[BITSCRAMBLER_DIR_TX].cfg1.lut = value;
    __asm__ volatile ("fence iorw, iorw" ::: "memory");
}
void c5v4_level_hw_stop(void)
{
    c5v4_level_hw_lock(); ready = false; c5v4_level_hw_unlock();
}
void c5v4_level_hw_prepare(void)
{
    ready = false;
    c5v4_level_init(&servo);
    if (!c5vrx4_level_enabled() || c5vrx4_cvbs_legacy_enabled()) return;
    if (bitscrambler_ll_get_lut_width(&BITSCRAMBLER, BITSCRAMBLER_DIR_TX) != 1) return;
    /* The headers disagree on whether host address units follow LUT width.
     * A stopped-engine differential probe verifies all 1024 halfword views,
     * including decoder neighbours, before any live update is allowed. */
    for (unsigned i = 0; i < 1024; ++i) original[i] = read_entry(i);
    unsigned probe = 32;
    uint16_t changed = original[probe] ^ 1u;
    write_entry(probe, changed);
    bool ok = true;
    for (unsigned i = 0; i < 1024; ++i)
        if (read_entry(i) != (i == probe ? changed : original[i])) ok = false;
    write_entry(probe, original[probe]);
    for (unsigned i = 0; i < 1024; ++i)
        if (read_entry(i) != original[i]) ok = false;
    ready = ok;
    if (!ok) ++faults;
    printf("C5V4_LEVEL startup_probe=%s live_arbitration=unproven\n", ok ? "pass" : "refused");
}
bool c5v4_level_hw_ready(void) { return ready; }
void c5v4_level_hw_observe(const c5v4_cvbs_stats_t *stats, bool fresh,
                         uint32_t context, uint64_t now)
{
    c5v4_level_hw_lock();
    if (!ready || !c5vrx4_level_enabled()) goto done;
    if (bitscrambler_ll_get_lut_width(&BITSCRAMBLER, BITSCRAMBLER_DIR_TX) != 1) {
        ready = false; ++faults; goto done;
    }
    if (!c5v4_level_observe(&servo, stats, fresh, context, now)) goto done;
    for (unsigned bank = 0; bank < 4; bank += 2) {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned index = bank * 256 + i;
            uint16_t next = c5v4_level_word(original[index], servo.codes[i]);
            if (next != original[index]) {
                write_entry(index, next);
                if (read_entry(index) != next) {
                    ready = false; ++faults; goto done;
                }
                original[index] = next; ++writes;
            }
        }
    }
done:
    c5v4_level_hw_unlock();
}
void c5v4_level_hw_invalidate(void)
{
    c5v4_level_hw_lock(); servo.good = 0; c5v4_level_hw_unlock();
}
void c5v4_level_hw_print(void)
{
    c5v4_level_hw_lock();
    printf("C5V4_LEVEL requested=%u ready=%u experimental=1 updates=%lu writes=%lu faults=%lu "
           "good=%u refused=%u span_bins=%d blank_bins=%d target_sync_mv=0 target_blank_mv=300 "
           "loss=hold sync_regeneration=0 atomic_update=0\n",
           c5vrx4_level_enabled(), ready, (unsigned long)servo.updates,
           (unsigned long)writes, (unsigned long)faults, servo.good, servo.refusals,
           servo.span, servo.blank);
    c5v4_level_hw_unlock();
}
