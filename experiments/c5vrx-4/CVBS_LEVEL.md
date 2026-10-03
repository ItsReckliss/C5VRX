# Experimental automatic CVBS level regulation

C5VRX by Twotoz and contributors. This follows Twotoz's report that
output amplitude falls with distance. That report is not yet a measured transfer
curve or proof that RF amplitude directly changes the ideal FM deviation.

Press lowercase **u** to enable/disable and reboot. The setting is stored as
`c5vrx4/level_lab`, defaults off and is refused with legacy M mapping. V/v keeps
its existing video-standard function. T prints `C5V4_LEVEL`. J still performs
read-only snapshots: its nominal voltage estimate describes the original fixed
transfer, not the active regulated DAC. Do not confuse that estimate with scope
measurement. Normal integrated Direct Gain V5 reaches the servo; older ARC
branches that bypass `afc_control` are outside this lab's supported mode.

## Actual behavior

The bounded stride-3 Phase8 observer measures sync and pre-burst black plateaus
on the existing safe completed-descriptor copy. For recovered phase delta d:

`target_uV = 300000 + (d - black_bins) * 300000 / (black_bins - sync_bins)`

Thus black targets 0.300 V and sync targets 0 V under the selected nominal or
measured single-75-ohm DAC table. Video scales by the same factor and saturates
at the physical DAC limits; colour amplitude is also scaled. Ambiguous winding
maps to black. This is output gain/offset normalization, not RF AGC, RF sensitivity
recovery or I/Q DC subtraction.

Three consecutive consistent fresh windows are required. Reject origin collapse,
clipping, ambiguous trajectories, noisy plateaus, changed profile/PHY/gain/lane,
unsettled windows and missing repeated line timing. Span is bounded 12..120
Phase8 bins: about 0.32..3.2 times the initial voltage slope. Noise cannot request
unbounded gain. On invalid data, hold the last applied mapping and reacquire;
losing actuator ownership also clears consecutive evidence. No sync is invented.

At most once per 100 ms, each of the 256 target entries advances by one DAC code.
Consequently initial convergence can take several seconds; this is deliberately
a conservative lab, not fast fade compensation. The DAC ladder is not perfectly
linear or necessarily monotonic with binary code. Host convergence tests use the
loaded electrical table. Dynamic settling, gain pumping and camera colour still
need measurements. There are two identical even planes: writes are sequential,
not a coherent/atomic bank swap. Intermediate transfers differ by at most one
code at a given entry, but one code is not a proven invisible video change.

## Hardware access and refusal

Source references, pinned to ESP-IDF v6.0.2:

- [driver load_lut/load_program](https://github.com/espressif/esp-idf/blob/v6.0.2/components/esp_driver_bitscrambler/src/bitscrambler.c)
- [C5 low-level LUT access](https://github.com/espressif/esp-idf/blob/v6.0.2/components/esp_hal_dma/esp32c5/include/hal/bitscrambler_ll.h)
- [C5 register descriptions](https://github.com/espressif/esp-idf/blob/v6.0.2/components/soc/esp32c5/register/soc/bitscrambler_struct.h)

The driver switches LUT width to 32 during loading. The LL header says width is
seen by the engine, not the host, while the register description makes LUT index
units dependent on mode. Neither proves safe concurrent access. Therefore the
lab first probes one changed bit while the engine is halted, checks all 1024
halfword views and neighbouring decoder entries, restores the probe and checks
again. Refusal reloads the pristine program before starting. This checks observed
addressing, not live RAM arbitration or every address combination.

After a successful probe, the lab writes only six DAC bits in each even LUT16
plane, preserving trajectory bits and every odd Phase8 decoder entry. It does
not change LUT width, instructions, phase counters, FIFO state, ring contents or
RUN/HALT during regulation. Each live write is read back; mismatch freezes the
lab with a fault. A recursive adapter mutex serializes updates against startup,
menu/lab disable and reload; PHY actuator ownership and fresh epochs exclude
concurrent gain/retune decisions. Source/transport remain IQ40M, raw ring,
TX-only three bundles, unique13.333M and duplicated DAC40M. No per-sample CPU path.

**Still unproven:** concurrent LUT access arbitration, stalls, readback side
effects and waveforms during sequential updates. A startup addressing pass and
host/controller tests do not demonstrate continuous live throughput. Hence off
by default until the following acceptance passes.

## Bench acceptance

1. Flash PR159, fixed M mapping, Direct Gain V5. Record T and J, loaded scope
   levels and HDZero live/recording at strong, moderate and weak input before u.
2. Enable u. Require startup_probe=pass, ready=1, faults=0. Compare sync/black
   under the same single 75-ohm load and attenuation. Verify dynamic levels
   converge without clipping colour/white or losing line/field lock.
3. Log existing FIFO/transport faults across many updates; compare output timing
   with regulation off. Any new underflow, timing seam or decoder damage blocks
   promotion. Readback success alone is insufficient.
4. Remove signal, inject noise and retune/change lanes: no noise-driven update;
   last map holds until three fresh consistent windows reacquire. Verify menu,
   restart and labs reload a clean baseline with no table race.

H/V sync regeneration/coasting is not implemented. This lab may preserve AV lock
when phase modulation shrinks but cannot guarantee a goggle stays locked once
sync/phase information is unrecoverable.
