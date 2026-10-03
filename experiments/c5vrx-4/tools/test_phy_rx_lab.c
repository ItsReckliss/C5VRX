#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
static int64_t clock_us;
static bool native;
unsigned char phy_param[0x500];
int64_t esp_timer_get_time(void) { return clock_us; }
bool rf_native_agc_active(void) { return native; }
bool rf_get_analog_bandwidth(void) { return true; }
uint16_t rf_get_frequency_mhz(void) { return 5865; }
int rf_get_frequency_offset_khz(void) { return 0; }
static uint8_t analog_regs[32];
static bool fail_restore, fail_apply;
uint8_t phy_i2c_readReg(uint8_t b, uint8_t h, uint8_t r)
{ assert(b==0x67 && h==1); return analog_regs[r]; }
#include "../main/phy_rx_lab.c"
void phy_i2c_writeReg(uint8_t b,uint8_t h,uint8_t r,uint8_t v)
{ assert(b==0x67 && h==1); if (!fail_restore) analog_regs[r]=v; }
void phy_11p_set(uint8_t enable,uint8_t mode)
{
    assert(enable==1 && mode==0);
    if (fail_apply) return;
    phy_param[0x26]=enable; phy_param[0x27]=mode;
#ifdef C5VRX_PHY_RX_LAB_PINNED
    for (unsigned i=0;i<4;++i) {
        patch_t x=s_11p_fields[i]; REG(x.addr)=(REG(x.addr)&~x.mask)|x.value;
    }
#endif
    for (unsigned i=6;i<=13;++i) analog_regs[i]=60;
}
static unsigned native_holds, native_releases, native_observations;
static bool fail_native_hold, fail_native_release, interfere_native;
void phy_disable_agc(void)
{
    ++native_holds;
    if (!fail_native_hold) REG(0x600A7030)|=PHYBIT(29);
}
void phy_enable_agc(void)
{
    ++native_releases;
    if (!fail_native_release) {
        REG(0x600A7030)&=~PHYBIT(29);
        REG(0x600A702C)|=PHYBIT(23);
        REG(0x600A702C)&=~PHYBIT(23);
    }
}
static void native_observe(const char *stage,unsigned cycle)
{
    assert(phy_rx_lab_busy()); ++native_observations;
    if (!strcmp(stage,"HOLD")) {
        assert(cycle && (REG(0x600A7030)&PHYBIT(29)));
        assert(!(REG(0x600A702C)&PHYBIT(23)));
        REG(0x600A7030)^=1u; /* unrelated bit must survive */
        if (interfere_native) REG(0x600A7030)&=~PHYBIT(29);
    } else assert(!(REG(0x600A7030)&PHYBIT(29)));
    clock_us+=40000;
}
static unsigned stages;
static void observe(const char *stage)
{
    assert(phy_rx_lab_busy());
    if (stages==0) assert(!strcmp(stage,"BASELINE"));
    else if (stages==1 && !fail_apply) {
        assert(!strcmp(stage,"ENABLED"));
        assert(phy_param[0x26]==1 && phy_param[0x27]==0);
        /* Preserve unrelated volatile bits despite exact owned rollback. */
        REG(0x600A7030)^=1u;
    } else assert(!strcmp(stage,"RESTORED"));
    ++stages;
}
static atomic_bool attempted, acquired;
static void *actuator_contender(void *unused)
{
    (void)unused;
    bool got=phy_rx_lab_try_actuator(phy_rx_lab_generation());
    atomic_store(&acquired,got);
    if (got) phy_rx_lab_end_actuator();
    return NULL;
}
static void *contender(void *unused)
{
    (void)unused; atomic_store(&attempted,true);
    phy_rx_lab_begin("other_task");
    atomic_store(&acquired,true); phy_rx_lab_end(); return NULL;
}
int main(void)
{
    void *memory=mmap((void *)0x600A0000, 0x10000, PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    assert(memory != MAP_FAILED);
    memset(memory, 0xA5, 0x10000);
    phy_rx_lab_osi_event(true);
    phy_rx_lab_begin("boot");
    phy_rx_lab_capture_vendor();
    phy_rx_lab_end();
    assert(s_enables==1 && s_disables==0 && s_generation==1);
    /* A gain/lane actuator is nonblocking, excludes vendor transactions and
     * cannot apply a decision made before a completed retune. */
    uint32_t actuator_generation=phy_rx_lab_generation();
    assert(phy_rx_lab_try_actuator(actuator_generation));
    assert(!phy_rx_lab_busy()); /* not a tune: does not reset controller */
    pthread_t actuator_thread;
    assert(!pthread_create(&actuator_thread,NULL,actuator_contender,NULL));
    assert(!pthread_join(actuator_thread,NULL));
    assert(!atomic_load(&acquired));
    phy_rx_lab_end_actuator();
    assert(phy_rx_lab_generation()==actuator_generation);
    phy_rx_lab_begin("retune_between_measure_and_write");
    assert(!phy_rx_lab_try_actuator(actuator_generation)); /* same-task nesting */
    assert(!pthread_create(&actuator_thread,NULL,actuator_contender,NULL));
    assert(!pthread_join(actuator_thread,NULL));
    assert(!atomic_load(&acquired));
    phy_rx_lab_end();
    assert(!phy_rx_lab_try_actuator(actuator_generation));
    assert(phy_rx_lab_try_actuator(phy_rx_lab_generation()));
    phy_rx_lab_end_actuator();
    uint32_t baseline=REG(0x600A0890);
    REG(0x600A0890)^=1;
    clock_us+=50000;
    phy_rx_lab_poll();
    assert(s_regs[0].changes==0); /* monitoring off by default */
    phy_rx_lab_toggle_monitor();
    REG(0x600A0890)=baseline;
    clock_us+=50000;
    phy_rx_lab_poll();
    assert(s_regs[0].changes==1);
#ifdef C5VRX_PHY_RX_LAB_PINNED
    for (unsigned n=1; n<sizeof(s_profiles)/sizeof(s_profiles[0]); ++n) {
        memset(memory, 0xA5, 0x10000);
        phy_rx_lab_next_profile();
        assert(s_profile==n);
        const profile_t *p=&s_profiles[n];
        for (unsigned i=0; i<p->count; ++i) {
            patch_t x=p->patches[i];
            assert((REG(x.addr)&x.mask)==x.value);
            /* An unrelated field changes while the lab owns its mask. */
            uint32_t outside=(~x.mask)&(x.mask+1u);
            if (!outside) outside=(~x.mask)&(0u-~x.mask);
            REG(x.addr)^=outside;
            s_saved[i]^=outside;
        }
        if (n%3==0) { clock_us+=PROFILE_US; phy_rx_lab_poll(); }
        else if (n%3==1) { phy_rx_lab_begin("retune"); phy_rx_lab_end(); }
        else phy_rx_lab_stock();
        assert(!s_profile);
        for (unsigned i=0; i<p->count; ++i) assert(REG(p->patches[i].addr)==s_saved[i]);
    }
    native=true;
    phy_rx_lab_next_profile();
    assert(!s_profile);
    native=false;
    phy_rx_lab_begin("outer");
    uint32_t generation=s_generation;
    phy_rx_lab_begin("inner");
    phy_rx_lab_next_profile();
    assert(!s_profile);
    phy_rx_lab_end();
    assert(s_generation==generation && phy_rx_lab_busy());
    phy_rx_lab_end();
    assert(s_generation==generation+1 && !phy_rx_lab_busy());
#else
    phy_rx_lab_next_profile();
    assert(!s_profile);
#endif

    /* A competing task cannot join another owner's nested transaction. */
    phy_rx_lab_begin("owner");
    pthread_t thread; assert(!pthread_create(&thread,NULL,contender,NULL));
    while (!atomic_load(&attempted)) sched_yield();
    assert(!atomic_load(&acquired));
    phy_rx_lab_end(); assert(!pthread_join(thread,NULL));
    assert(atomic_load(&acquired));
#ifdef C5VRX_PHY_RX_LAB_PINNED
    for (unsigned i=6;i<=13;++i) analog_regs[i]=i+20;
    phy_param[0x26]=7; phy_param[0x27]=9;
    uint32_t before=REG(0x600A7030), fields[4];
    for (unsigned i=0;i<4;++i) fields[i]=REG(s_11p_fields[i].addr);
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_OK);
    assert(stages==3 && !phy_rx_lab_busy());
    assert(REG(0x600A7030)==(before^1u));
    for (unsigned i=0;i<4;++i)
        assert((REG(s_11p_fields[i].addr)&s_11p_fields[i].mask)==(fields[i]&s_11p_fields[i].mask));
    assert(phy_param[0x26]==7 && phy_param[0x27]==9);
    for (unsigned i=6;i<=13;++i) assert(analog_regs[i]==i+20);
    native=true; stages=0;
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_ERR_INVALID_STATE && !stages);
    native=false;
    fail_apply=true;
    /* A failed application still restores and releases task ownership. */
    assert(phy_rx_lab_run_11p_probe(NULL)==ESP_ERR_INVALID_STATE);
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_ERR_INVALID_RESPONSE);
    assert(stages==2 && !phy_rx_lab_busy());
    fail_apply=false;
    fail_restore=true; stages=0;
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_FAIL);
    assert(stages==2 && !phy_rx_lab_busy()); fail_restore=false;
#else
    assert(phy_rx_lab_run_11p_probe(observe)==ESP_ERR_NOT_SUPPORTED && !stages);
#endif

#ifdef C5VRX_PHY_RX_LAB_PINNED
    native=true;
    REG(0x600A7030)&=~PHYBIT(29); REG(0x600A702C)&=~PHYBIT(23);
    uint32_t ctrl=REG(0x600A7030), force=REG(0x600A702C), rfctrl=REG(0x600A705C);
    assert(phy_rx_lab_run_native_hold(100,native_observe)==ESP_OK);
    assert(native_holds==100 && native_releases==100 && native_observations==201);
    assert(REG(0x600A7030)==ctrl && REG(0x600A702C)==force && REG(0x600A705C)==rfctrl);
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_OK);
    assert(REG(0x600A7030)==(ctrl^1u));
    assert(REG(0x600A702C)==force && REG(0x600A705C)==rfctrl);
    unsigned calls=native_holds;
    assert(phy_rx_lab_run_native_hold(0,native_observe)==ESP_ERR_INVALID_STATE);
    assert(phy_rx_lab_run_native_hold(1,NULL)==ESP_ERR_INVALID_STATE);
    REG(0x600A702C)|=PHYBIT(23);
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_STATE);
    REG(0x600A702C)&=~PHYBIT(23);
    REG(0x600A7030)|=PHYBIT(29);
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_STATE);
    REG(0x600A7030)&=~PHYBIT(29);
    native=false;
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_STATE);
    assert(native_holds==calls); native=true;
    fail_native_hold=true;
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_RESPONSE);
    fail_native_hold=false; interfere_native=true;
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_ERR_INVALID_RESPONSE);
    interfere_native=false; fail_native_release=true;
    assert(phy_rx_lab_run_native_hold(1,native_observe)==ESP_FAIL);
    assert(!phy_rx_lab_busy());
    fail_native_release=false; phy_enable_agc(); native=false;
#else
    assert(phy_rx_lab_run_native_hold(100,native_observe)==ESP_ERR_NOT_SUPPORTED);
    assert(!native_holds && !native_releases);
#endif
    for (unsigned i=0; i<100; ++i) { ++clock_us; phy_rx_lab_osi_event(false); }
    assert(s_disables==100);
    assert(s_event_total>EVENT_CAP);
    phy_rx_lab_mark();
    munmap(memory,0x10000);
    puts("PHY RX lab lifecycle tests passed");
}
