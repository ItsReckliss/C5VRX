# Phase8 range: Q4 origin collapse and native AGC (issue #119)

Status: **instrumentation and experiment only, awaiting hardware data.** This
change does not alter the live demodulator or the default gain controller. It
adds what is needed to prove or disprove the origin-collapse hypothesis and to
run the untouched native AGC experiment from #117.

## Hypothesis

The live path quantizes fixed signed `I[9:6]` / `Q[9:6]` to one Q4/I4 byte.
Cell `n` represents `n + 0.5`, so the four cells that touch the origin sit at
roughly +45, +136, -135 and -46 degrees:

```text
raw 0x00 -> LUT 32  (+45)     raw 0xF0 -> LUT 97  (+136)
raw 0xFF -> LUT 160 (-135)    raw 0x0F -> LUT 223 (-46)
```

Every move between two of these cells is a Phase8 step of 63..65 codes
(~88-91 degrees) or ~180 degrees. At weak RF the vector collapses into these
cells, so Phase8 turns quantizer noise into full-scale CVBS movement.
`tools/test_phase8_envelope.c` checks those LUT values and that every
central-cell move counts as a hard step.

## What was added

| Piece | Purpose |
|---|---|
| `main/phase8_envelope.h` | Read-only statistics over completed Q4/I4 bytes. Includes central-cell (4 cell) occupancy, DG3-compatible origin (power <= 4), clip, P50/P90/P95 (DG3 power units), radius histogram (whole cells 0..10), \|Phase8 delta\| histogram (16-code bins), hard tail (\|delta\| >= 60 codes), hard rate given a central endpoint vs given two outer endpoints, and a provisional annulus class. |
| Console `E` | Copies up to 32 completed-descriptor probes (128 x 64 adjacent samples) from the console task and prints one `P8ENV` row. It runs only on demand: no periodic task, no PHY write, no gain decision. |
| Console `N` | Arms the native hardware AGC experiment for the next boot (NVS key `c5vrx/native_agc`) and reboots. Press `N` again to return to firmware gain control. |
| `tools/p8env_sweep.py` | `capture` runs an interactive attenuation sweep (label a step, it sends `E` N times). `analyze` prints per-step medians, Spearman(central_pm, hard_pm), central/outer hard lift, first video-loss step, the empirical annulus, native gain-register movement and transport-fault movement. |

## Native AGC experiment (issue #117 Phase 1, issue #119 Phase 2)

The vendor AGC cannot be restored after `phy_disable_agc()` /
`phy_rfagc_disable()` (#117), so this is a per-boot mode decided in
`rf_start()` before the PHY is used:

- `phy_disable_agc()` / `phy_rfagc_disable()` are never called, not at boot
  and not on channel change.
- Forced gain and FFT scale are released once (`phy_force_rx_gain(false, 0)`,
  `phy_fft_scale_force(false, 0)`). No gain index is ever chosen.
- `rf_set_rx_gain()` and FFT force are refused and counted
  (`blocked=` in `P8ENV`). Offset retunes no longer reassert forced gain.
- `apply_rx_gain_tracked()` is a no-op, every profile is held in
  `ANALOG_AGC_MANUAL`, so no firmware controller makes gain decisions.
  Gain-owning console commands (`g F G R U S K + - k j a s m D I Y X`) are
  ignored. The persisted firmware AGC mode is not overwritten.
- `P8ENV` reports raw `gain_reg` (0x600A702C) and `agc_reg` (0x600A7030) plus
  `gain_reg_changes` during the capture. Their fields are not decoded yet. For
  now, the evidence of free-running AGC is the register moving while
  `blocked` and `fw_gain_epochs` stay flat.

The analyzer flags a native capture as **tainted** if `blocked` grows or any
firmware gain epoch occurs during it.

## Bench procedure

1. Flash the PR build. Use a controlled attenuator (or a fixed-geometry sweep)
   and the same VTX/channel for every run.
2. For each configuration below, run
   `python tools/p8env_sweep.py capture --port COMx --out <name>.log`,
   step the attenuation, and label each step with its dB value:
   - GOLDEN + current Direct Gain V3 (baseline range)
   - Phase8 + current Direct Gain V3 (the regression)
   - Phase8 after `N` (native AGC, zero firmware writes)
3. `python tools/p8env_sweep.py analyze <name>.log`.

Reading the result:

| Result | Meaning |
|---|---|
| `SUPPORTS_ORIGIN_COLLAPSE` (rho >= 0.6), large central/outer lift, Phase8 video loss at a step where GOLDEN survives | Pre-Q4 placement is the limiter (issue case A). Envelope control can recover range. |
| `DISPROVES_ORIGIN_COLLAPSE` (rho <= 0.2) | The hard tail is not driven by central cells. Look at multipath, AFC or demod mapping instead. |
| Native rows with `distinct_gain_regs > 1`, `gain_reg_changes > 0`, no taint | The vendor AGC free-runs without Wi-Fi packets. Next step is #117 Phase 4 tuning. |
| Native `gain_reg` never moves | The vendor AGC needs a bootstrap (#117 Phase 3). Do not fall back to a CPU loop yet. |

## Deliberately not done here

- **Phase8 HOLD/RESEED guard in the BitScrambler.** The issue says not to pick
  a universal threshold before the envelope has been characterized on real
  hardware. `hard_central_pm` / `central_hard_share_pm` measure how often such
  a guard would fire. If the tail is dominated by central endpoints, the guard
  can reject exactly those pairs.
- **Annulus thresholds.** `P8ENV_PROVISIONAL` reuses the DG3 healthy window
  (P50 13..32, P95 <= 65, origin <= 250 pm, clip < 20 pm). Replace it with the
  `empirical annulus` from real sweeps.
- **FFT scale as a fine actuator.** Use the existing `F` probe with fixed RF and
  fixed gain. Only consider FFT scale if raw Q4 moves.
- **Dynamic bit-slice gears.** Out of scope per the issue decision.
