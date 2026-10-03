/* C5VRX by Twotoz and contributors; pinned C5 PHY investigation #150-153.
 * No production overrides. Polling cannot prove absence of short gate pulses. */
#include "phy_rx_lab.h"
#include "rf.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <stdio.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define PHYBIT(n) (UINT32_C(1) << (n))
#define EVENT_CAP 64u
#define PROFILE_US 10000000LL
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* Task ownership is recursive for nested channel/bandwidth restoration. The
 * short spinlock below protects observations; it must never cover vendor I2C. */
static StaticSemaphore_t s_transaction_storage;
static SemaphoreHandle_t s_transaction;
static void transaction_init(void)
{
    portENTER_CRITICAL(&s_mux);
    if (!s_transaction)
        s_transaction=xSemaphoreCreateRecursiveMutexStatic(&s_transaction_storage);
    portEXIT_CRITICAL(&s_mux);
    configASSERT(s_transaction);
}
static void transaction_take(void)
{
    transaction_init();
    int taken=xSemaphoreTakeRecursive(s_transaction, portMAX_DELAY);
    configASSERT(taken==pdTRUE);
    (void)taken;
}
static void transaction_give(void)
{
    int given=xSemaphoreGiveRecursive(s_transaction);
    configASSERT(given==pdTRUE);
    (void)given;
}

typedef struct {
    uint32_t addr, baseline, current, changes;
    int64_t changed_us;
} invariant_t;
static invariant_t s_regs[] = {
    { .addr=0x600A0890 }, { .addr=0x600A9C18 },
    { .addr=0x600A7C3C }, { .addr=0x600A7C40 }, { .addr=0x600A7C08 },
    { .addr=0x600A0800 }, { .addr=0x600A0400 }, { .addr=0x600A7C80 },
    { .addr=0x600A7010 }, { .addr=0x600A7014 }, { .addr=0x600A7044 },
    { .addr=0x600A7108 }, { .addr=0x600A0814 }, { .addr=0x600A0874 },
    { .addr=0x600A0430 }, { .addr=0x600A0448 },
    { .addr=0x600A702C }, { .addr=0x600A7030 },
    { .addr=0x600A4400 }, { .addr=0x600A7CE0 }, { .addr=0x600A7CE4 },
    { .addr=0x600A7904 }, { .addr=0x600A7074 },
    { .addr=0x600A0438 }, { .addr=0x600A0C0C }, { .addr=0x600A043C },
    { .addr=0x600A0C38 }, { .addr=0x600A0C3C }, { .addr=0x600A7068 },
    { .addr=0x600A7018 }, { .addr=0x600A701C },
    { .addr=0x600A7C44 }, { .addr=0x600A7C50 }, { .addr=0x600A4C5C },
    /* Only slot zero is decoded here; do not infer arbitrary slot bounds. */
    { .addr=0x600A7C14 }, { .addr=0x600A7810 }, { .addr=0x600A7814 },
    { .addr=0x600A7830 },
};
#define NREG (sizeof(s_regs) / sizeof(s_regs[0]))
typedef struct {
    int64_t time_us;
    uint32_t generation, addr, old_value, new_value;
} event_t;
static event_t s_events[EVENT_CAP];
static uint32_t s_event_total, s_generation, s_depth;
static bool s_ready, s_monitor;
static uint32_t s_vendor[NREG];
static int64_t s_last_poll;
/* OSI fields are shared with Wi-Fi tasks. Protected by the same short lock. */
static uint32_t s_enables, s_disables;
static int64_t s_enable_us, s_disable_us;
static const char *s_reason = "boot";

typedef struct { uint32_t addr, mask, value; } patch_t;
typedef struct { const char *name; unsigned count; patch_t patches[3]; } profile_t;
/* All masks recovered from the pinned binary. Names describe writes, not
 * alleged bypass/sensitivity semantics. Never change ADC/filter tuples here. */
static const profile_t s_profiles[] = {
    {"STOCK", 0, {{0}}},
    {"PKDADC_BIT31_CLEAR", 1, {{0x600A0C38, PHYBIT(31), 0}}},
    {"NF_AUTO_FREEZE", 3, {{0x600A7018, PHYBIT(23)|PHYBIT(28), 0},
                           {0x600A7C44, PHYBIT(0), 0}, {0x600A7C50, PHYBIT(0), 0}}},
    {"BB_CCA_BIT30_CLEAR", 1, {{0x600A7018, PHYBIT(30), 0}}},
    /* phy_wifi_fbw_sel(0): multiple fields, not just one bit. */
    {"FRONT_BW_ARG0", 1, {{0x600A0874, 0x003F0000u, 0}}},
    /* phy_bb_bss_cbw40_dig(0) clears BOTH bits 2 and 3. */
    {"DIG_BW_ARG0", 1, {{0x600A9C18, PHYBIT(2)|PHYBIT(3), 0}}},
    {"CHAN_A_ARG0", 1, {{0x600A7904, PHYBIT(22)|7u, PHYBIT(22)}}},
    {"CHAN_A_ARG1", 1, {{0x600A7904, PHYBIT(22), 0}}},
    {"CHAN_B_ARG0", 1, {{0x600A7074, PHYBIT(13), PHYBIT(13)}}},
    {"CHAN_B_ARG1", 1, {{0x600A7074, PHYBIT(13), 0}}},
    {"SPUR_SLOT0_BIT13_CLEAR", 1, {{0x600A7C14, PHYBIT(13), 0}}},
};
static unsigned s_profile;
#ifdef C5VRX_PHY_RX_LAB_PINNED
static unsigned s_next_profile = 1;
#endif
static uint32_t s_saved[3];
static int64_t s_deadline;

static void record_locked(int64_t now, uint32_t addr, uint32_t old, uint32_t value)
{
    s_events[s_event_total % EVENT_CAP] = (event_t){now, s_generation, addr, old, value};
    ++s_event_total;
}

static void capture_locked(void)
{
    for (unsigned i=0; i<NREG; ++i) {
        s_regs[i].baseline = s_regs[i].current = REG(s_regs[i].addr);
    }
}

static void stock_locked(void)
{
    const profile_t *p = &s_profiles[s_profile];
    for (unsigned i=0; i<p->count; ++i) {
        patch_t x = p->patches[i];
        /* Restore exactly the owned fields. Do not replay unrelated volatile
         * status or a vendor update in another field of the same register. */
        REG(x.addr) = (REG(x.addr) & ~x.mask) | (s_saved[i] & x.mask);
    }
    s_profile = 0;
    s_deadline = 0;
}

void phy_rx_lab_osi_event(bool enable)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (enable) { ++s_enables; s_enable_us=now; }
    else { ++s_disables; s_disable_us=now; }
    if (s_ready) record_locked(now, enable ? 1u : 2u, s_depth,
                              enable ? s_enables : s_disables);
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_begin(const char *reason)
{
    transaction_take();
    portENTER_CRITICAL(&s_mux);
    if (s_depth++ == 0) {
        stock_locked(); /* Never carry a previous-channel snapshot into a tune. */
        s_reason = reason;
        if (s_ready) record_locked(esp_timer_get_time(), 3, 0, 1);
    }
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_capture_vendor(void)
{
    portENTER_CRITICAL(&s_mux);
    for (unsigned i=0; i<NREG; ++i) s_vendor[i]=REG(s_regs[i].addr);
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_end(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_depth && --s_depth == 0) {
        ++s_generation;
        capture_locked();
        s_ready = true;
        record_locked(esp_timer_get_time(), 3, 1, 0);
    }
    portEXIT_CRITICAL(&s_mux);
    transaction_give();
}

uint32_t phy_rx_lab_generation(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t generation=s_generation;
    portEXIT_CRITICAL(&s_mux);
    return generation;
}

bool phy_rx_lab_busy(void)
{
    portENTER_CRITICAL(&s_mux);
    bool busy=s_depth != 0;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}

/* Gain/lane writes share the RF mutex without cancelling an A/B profile or
 * publishing a new tune generation. Never wait with a stale control decision. */
bool phy_rx_lab_try_actuator(uint32_t expected_generation)
{
    transaction_init();
    if (xSemaphoreTakeRecursive(s_transaction, 0) != pdTRUE) return false;
    portENTER_CRITICAL(&s_mux);
    bool valid = !s_depth && s_generation == expected_generation;
    portEXIT_CRITICAL(&s_mux);
    if (!valid) transaction_give();
    return valid;
}
void phy_rx_lab_end_actuator(void) { transaction_give(); }

void phy_rx_lab_poll(void)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (s_ready && !s_depth && s_profile && now >= s_deadline) {
        stock_locked();
        ++s_generation;
        capture_locked();
        record_locked(now, 4, 1, 0); /* timeout, no UART in polling */
    }
    if (!s_ready || s_depth || !s_monitor || now-s_last_poll < 50000) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    s_last_poll=now;
    for (unsigned i=0; i<NREG; ++i) {
        invariant_t *r=&s_regs[i];
        uint32_t value=REG(r->addr);
        if (value != r->current) {
            record_locked(now, r->addr, r->current, value);
            r->current=value;
            ++r->changes;
            r->changed_us=now;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

void phy_rx_lab_toggle_monitor(void)
{
    portENTER_CRITICAL(&s_mux);
    s_monitor=!s_monitor;
    bool enabled=s_monitor;
    if (s_ready && !s_depth) capture_locked();
    portEXIT_CRITICAL(&s_mux);
    printf("PHYINV monitor=%u poll_us=50000 ring=%u\n", enabled, EVENT_CAP);
}

void phy_rx_lab_stock(void)
{
    portENTER_CRITICAL(&s_mux);
    bool active=s_profile != 0;
    stock_locked();
    if (active) { ++s_generation; capture_locked(); }
    portEXIT_CRITICAL(&s_mux);
    printf("PHYLAB profile=STOCK restored=%u\n", active);
}

bool phy_rx_lab_profile_active(void)
{
    portENTER_CRITICAL(&s_mux);
    bool active=s_profile != 0;
    portEXIT_CRITICAL(&s_mux);
    return active;
}

void phy_rx_lab_next_profile(void)
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    printf("PHYLAB refused=unverified_PHY_binary read_only=1\n");
    return;
#else
    if (rf_native_agc_active()) {
        printf("PHYLAB refused=native_AGC_owner\n");
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (!s_ready || s_depth) {
        portEXIT_CRITICAL(&s_mux);
        printf("PHYLAB refused=transaction_active\n");
        return;
    }
    stock_locked();
    s_profile=s_next_profile++;
    if (s_next_profile >= sizeof(s_profiles)/sizeof(s_profiles[0])) s_next_profile=1;
    const profile_t *p=&s_profiles[s_profile];
    for (unsigned i=0; i<p->count; ++i) {
        patch_t x=p->patches[i];
        s_saved[i]=REG(x.addr);
        REG(x.addr)=(s_saved[i] & ~x.mask) | x.value;
    }
    __sync_synchronize();
    bool verified=true;
    for (unsigned i=0; i<p->count; ++i) {
        patch_t x=p->patches[i];
        if ((REG(x.addr)&x.mask) != x.value) verified=false;
    }
    const char *name=p->name;
    if (!verified) stock_locked();
    s_deadline=s_profile ? esp_timer_get_time()+PROFILE_US : 0;
    ++s_generation;
    capture_locked();
    record_locked(esp_timer_get_time(), 4, 0, s_profile);
    portEXIT_CRITICAL(&s_mux);
    printf("PHYLAB profile=%s verified=%u timeout_ms=10000 persistent=0\n", name, verified);
#endif
}

void phy_rx_lab_mark(void)
{
    phy_rx_lab_poll();
    portENTER_CRITICAL(&s_mux);
    int64_t now=esp_timer_get_time();
    record_locked(now, 5, 0, 0);
    uint32_t total=s_event_total;
    portEXIT_CRITICAL(&s_mux);
    printf("PHYMARK t=%" PRId64 " events=%" PRIu32 "\n", now, total);
    uint32_t start=total>EVENT_CAP ? total-EVENT_CAP : 0;
    for (uint32_t i=start; i<total; ++i) {
        portENTER_CRITICAL(&s_mux);
        event_t e=s_events[i%EVENT_CAP];
        portEXIT_CRITICAL(&s_mux);
        printf("PHYEV t=%" PRId64 " gen=%" PRIu32 " reg=%08" PRIx32
               " old=%08" PRIx32 " new=%08" PRIx32 "\n",
               e.time_us,e.generation,e.addr,e.old_value,e.new_value);
    }
}

extern unsigned char phy_param[];
extern uint8_t phy_i2c_readReg(uint8_t block, uint8_t host, uint8_t reg);
void phy_rx_lab_dump(bool analog_i2c)
{
    /* Copy under lock; print outside it. No logging in the RF timing path. */
    invariant_t snapshot[NREG];
    uint32_t vendor[NREG];
    portENTER_CRITICAL(&s_mux);
    bool ready=s_ready && !s_depth;
    if (ready) {
        for (unsigned i=0; i<NREG; ++i) {
            snapshot[i]=s_regs[i];
            vendor[i]=s_vendor[i];
            snapshot[i].current=REG(snapshot[i].addr);
        }
    }
    uint32_t generation=s_generation, en=s_enables, dis=s_disables;
    int64_t en_us=s_enable_us, dis_us=s_disable_us;
    const char *reason=s_reason, *profile=s_profiles[s_profile].name;
    portEXIT_CRITICAL(&s_mux);
    if (!ready) { printf("PHYINV unavailable=transaction_or_startup\n"); return; }
    printf("PHYINV gen=%" PRIu32 " reason=%s profile=%s native=%u bw=%u freq=%u offset=%d"
           " enable=%" PRIu32 " enable_us=%" PRId64 " disable=%" PRIu32 " disable_us=%" PRId64 "\n",
           generation,reason,profile,rf_native_agc_active(),rf_get_analog_bandwidth()?40:20,
           rf_get_frequency_mhz(),rf_get_frequency_offset_khz(),en,en_us,dis,dis_us);
    printf("PHYOWN direct_agc_disabled=%u expected=%u\n",
           (unsigned)((REG(0x600A7030) >> 29) & 1u), !rf_native_agc_active());
    for (unsigned i=0; i<NREG; ++i) {
        invariant_t r=snapshot[i];
        printf("PHYREG reg=%08" PRIx32 " vendor=%08" PRIx32 " lock=%08" PRIx32 " now=%08" PRIx32
               " changes=%" PRIu32 " last_us=%" PRId64 "\n",
               r.addr,vendor[i],r.baseline,r.current,r.changes,r.changed_us);
    }
#ifdef C5VRX_PHY_RX_LAB_PINNED
    if (analog_i2c) {
        /* Explicit lab-only vendor I2C reads; never poll these in LOCK. */
        for (unsigned i=0xF5; i<=0xFC; ++i)
            printf("PHYPARAM offset=%03x value=%02x\n",i,phy_param[i]);
        for (uint8_t reg=6; reg<=21; ++reg)
            printf("PHYI2C block=67 reg=%02x value=%02x\n",reg,
                   phy_i2c_readReg(0x67,1,reg));
    }
#else
    (void)analog_i2c;
#endif
}

/* Issue #155: scoped vendor experiment, never a production/NVS setting.
 * phy_11p_set(0,0) installs defaults, so it is NOT an exact rollback. */
#ifdef C5VRX_PHY_RX_LAB_PINNED
extern void phy_11p_set(uint8_t enable, uint8_t mode);
extern void phy_i2c_writeReg(uint8_t block, uint8_t host, uint8_t reg, uint8_t value);
static const patch_t s_11p_fields[] = {
    {0x600A7CE4, 0x1cu, 0x10u}, {0x600A7030, PHYBIT(5), 0},
    {0x600A7048, 0x7f00u, 0x4000u}, {0x600A71C4, 0x00fe0000u, 0x00440000u},
};
static void snapshot_11p(const char *stage)
{
    printf("PHY11P_STATE stage=%s flags=%02x/%02x\n",stage,phy_param[0x26],phy_param[0x27]);
    for (unsigned i=0; i<4; ++i)
        printf("PHY11P_REG reg=%08" PRIx32 " value=%08" PRIx32 " mask=%08" PRIx32 "\n",
               s_11p_fields[i].addr,REG(s_11p_fields[i].addr),s_11p_fields[i].mask);
    for (uint8_t i=6; i<=13; ++i)
        printf("PHY11P_I2C reg=%u value=%02x\n",i,phy_i2c_readReg(0x67,1,i));
}
static bool verify_11p(const uint32_t *values, const uint8_t *analog)
{
    for (unsigned i=0; i<4; ++i)
        if ((REG(s_11p_fields[i].addr)&s_11p_fields[i].mask) != values[i]) return false;
    for (uint8_t i=0; i<8; ++i)
        if (phy_i2c_readReg(0x67,1,6+i)!=analog[i]) return false;
    return true;
}
#endif
esp_err_t phy_rx_lab_run_11p_probe(void (*observe)(const char *stage))
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)observe;
    printf("PHY11P refused=unverified_PHY_binary\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!observe || rf_native_agc_active()) return ESP_ERR_INVALID_STATE;
    phy_rx_lab_begin("11p_AB");
    uint32_t saved[4], enabled[4];
    uint8_t analog[8], expected[8];
    uint8_t flags[2]={phy_param[0x26],phy_param[0x27]};
    for (unsigned i=0; i<4; ++i) {
        saved[i]=REG(s_11p_fields[i].addr)&s_11p_fields[i].mask;
        enabled[i]=s_11p_fields[i].value;
    }
    for (uint8_t i=0; i<8; ++i) {
        analog[i]=phy_i2c_readReg(0x67,1,6+i);
        expected[i]=60;
    }
    snapshot_11p("BASELINE");
    observe("BASELINE");
    phy_11p_set(1,0);
    bool applied=phy_param[0x26]==1 && phy_param[0x27]==0 && verify_11p(enabled,expected);
    printf("PHY11P apply_verified=%u persistent=0\n",applied);
    if (applied) { snapshot_11p("ENABLED"); observe("ENABLED"); }
    /* Clear the persistent vendor flags before another channel operation can
     * replay 11p. Restore saved analog bytes, then only the owned MMIO masks. */
    phy_param[0x26]=flags[0]; phy_param[0x27]=flags[1];
    for (uint8_t i=0; i<8; ++i) phy_i2c_writeReg(0x67,1,6+i,analog[i]);
    for (unsigned i=0; i<4; ++i) {
        patch_t x=s_11p_fields[i];
        REG(x.addr)=(REG(x.addr)&~x.mask)|saved[i];
    }
    __sync_synchronize();
    bool restored=phy_param[0x26]==flags[0] && phy_param[0x27]==flags[1] && verify_11p(saved,analog);
    printf("PHY11P restore_verified=%u\n",restored);
    if (restored) { snapshot_11p("RESTORED"); observe("RESTORED"); }
    phy_rx_lab_end();
    return restored ? (applied ? ESP_OK : ESP_ERR_INVALID_RESPONSE) : ESP_FAIL;
#endif
}

/* Native analog patch prototype (#139): retain the hardware-selected tuple,
 * stop only BB acquisition, then use the vendor resume strobe. This bounded
 * console experiment does not infer acquisition-complete from an unknown FSM
 * byte and does not establish automatic analog tracking. */
#ifdef C5VRX_PHY_RX_LAB_PINNED
extern void phy_disable_agc(void);
extern void phy_enable_agc(void);
#endif
esp_err_t phy_rx_lab_run_native_hold(unsigned cycles,
    void (*observe)(const char *stage, unsigned cycle))
{
#ifndef C5VRX_PHY_RX_LAB_PINNED
    (void)cycles; (void)observe;
    printf("NATIVEHOLD refused=unverified_PHY_binary\n");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!rf_native_agc_active() || !observe || (cycles!=1 && cycles!=100))
        return ESP_ERR_INVALID_STATE;
    phy_rx_lab_begin("native_hold_lab");
    /* 702C is force/control configuration, NOT live gain status. Bit23 is
     * also the vendor resume strobe. Refuse any pre-existing forced mode. */
    if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) {
        phy_rx_lab_end();
        printf("NATIVEHOLD refused=already_held_or_forced\n");
        return ESP_ERR_INVALID_STATE;
    }
    unsigned completed=0;
    bool verified=true;
    observe("FREE_BASELINE",0);
    if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) verified=false;
    for (unsigned n=1; verified && n<=cycles; ++n) {
        phy_disable_agc(); /* Exactly 7030 bit29; no RF disable, no gain write. */
        if (!(REG(0x600A7030)&PHYBIT(29))) { verified=false; break; }
        observe("HOLD",n);
        /* An asynchronous writer reopening BB acquisition taints this trial. */
        if (!(REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) {
            verified=false; break;
        }
        phy_enable_agc(); /* Clear29 and pulse702C[23]; leaves RF AGC intact. */
        if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) {
            verified=false; break;
        }
        ++completed;
        observe("FREE_RESTORED",n);
        if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23))) verified=false;
    }
    /* Always recover baseline native ownership, even after failed hold. */
    if ((REG(0x600A7030)&PHYBIT(29)) || (REG(0x600A702C)&PHYBIT(23)))
        phy_enable_agc();
    bool restored=!(REG(0x600A7030)&PHYBIT(29)) && !(REG(0x600A702C)&PHYBIT(23));
    phy_rx_lab_end();
    printf("NATIVEHOLD completed=%u requested=%u gate_verified=%u restored=%u "
           "explicit_gain_index_writes=0 rf_disable_calls=0 hardware_acceptance=pending\n",
           completed,cycles,verified,restored);
    return restored ? (verified ? ESP_OK : ESP_ERR_INVALID_RESPONSE) : ESP_FAIL;
#endif
}
