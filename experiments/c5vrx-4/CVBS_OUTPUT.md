# Unwrap75 CVBS output conditioning

Extends Twotoz and the C5VRX contributors' Phase8/Unwrap75,
resistor-DAC and sync investigations. This build combines PR #146's detector
and fixed-ultrafine comparison, PR #154's PHY ownership/range-race fixes,
and main's versioned alpha/PR publication. No Phase5 rollback is included.

## Implemented live change

The detector's phase range no longer sets the video-output slope. STATIC and
HISTORY retain their original phase decode, quadrant trajectory decisions,
counter/parity routing, 3 bundles, IQ40M, and [D,D,D] DAC40M / unique13.333M.
Only the low six DAC bits of the two even LUT planes change.

The default CVBS150 transfer uses:

    frequency_hz = delta_bins / (256 * 75e-9)
    target_volts = 0.300 + frequency_hz / 1e6 * 0.150
    code = closest measured-or-nominal loaded DAC voltage

Final voltage saturates to the physical output range. Frequency winding is
resolved first, never by multiplying modulo counter terms. An ambiguous
trajectory maps to the blanking reference. The 0.150 V/MHz slope is an initial
Golden-compatible engineering value, not a universal camera/VTX calibration.
Approximately -2 MHz from reference gives sync near zero; +4.667 MHz gives
white near 1 V. Different VTX deviations/CFO need bench assessment. Strong
colour peaks and tails must be checked for clipping; the present passive
network has limited headroom, so standards compliance is not claimed.

The previous Unwrap75 transfer mapped the full +/-6.667 MHz interval to the
DAC's range and put zero deviation near half scale. It remains an A/B option.
The nominal loaded voltages explicitly include 3.3-V GPIO, all six resistor
conductances, the 200-ohm shunt and one 75-ohm AV load. GPIO impedance,
resistor tolerance, dynamic settling and reconstruction filtering are not
measured by this model.

## Controls

- `M`: toggle CVBS150 / LEGACY_FULL and reboot. Default CVBS150. NVS key
  `c5vrx4/cvbs_legacy` is independent of HISTORY and gain selection.
- `J`: eight bounded snapshots, 50 ms apart, on a temporary low-priority task.
  No permanent observer and no PHY, gain, LUT, DAC or ring writes.
- `T`: report transfer, nominal/measured calibration, lane and gain ownership.
- `h`: toggle STATIC/HISTORY and reboot. Uppercase `H` remains available for
  PR #154's shared PHY invariant snapshot; it no longer changes the demodulator.
- `Z`: retains the fixed-ultrafine / baseline-lane reboot comparison.

The mapping is selected at boot and every normal live restart/menu exit.
Default has no live LUT writes; the opt-in `u` lab is documented in CVBS_LEVEL.md. The pinned IDF 6.0.2 `bitscrambler_load_lut()`
changes the active LUT width to 32 bits while loading; `load_program()` halts
execution. Neither is a safe seamless in-flight gain actuator for this LUT16
program. Do not turn the diagnostic gain proposal into a live call to either.

## Diagnostic evidence

J copies one completed 4092-byte RX descriptor. It refuses invalid channels,
menu/PHY labs and stale context, checks DMA position stability and a <=50-us
copy duration. Profile, PHY and gain/lane epochs fence the copy. Copying a
completed descriptor is not a sample-gapless capture proof.

The frozen snapshot is decoded as a local stride-3 Phase8/trajectory estimate,
with startup discarded. The actual live alignment/history is not tagged, so
reported codes/volts are **semantic estimates**, not exact DAC bytes or scope
measurements. Sync candidates need negative contrast, 3.9–5.7-us pulse width,
repetition near the PAL/NTSC line period, and robust interior/porch median/MAD.
A five-point majority mask tolerates small quantizer holes only for detection;
level statistics still use the original decoded deltas. Noise, missing pulses,
constant input, strong origin collapse and unrelated pulse widths are rejected.
There is no decoded field/burst lock or validated carrier detector here.

Telemetry separates phase span from nominal output span, origin/ambiguity/rail
occupancy, and raw mean I/Q. Mean IQ is **not** a validated DC estimate: finite
windows and static video can have a real nonzero mean. No offset is subtracted.
Correcting endpoint phase without also correcting the middle-sample quadrant
classifier could invalidate winding; do not silently make that change.

The suggested scale is bounded to 0.75–1.5 and reported only after valid levels.
It does not actuate or persist. A smaller span can represent wrong scaling,
DC/quantization distortion or genuine information loss. Do not normalize noise.

## Scope calibration

Optionally provide `experiments/c5vrx-4/dac_calibration.json` before generating:

    {"load_ohms": 75, "volts_by_code": [64 measured voltages in DAC-code order]}

Measure all codes with one actual 75-ohm input load. The generator rejects a
wrong load, malformed/nonfinite voltages, missing sync reference or <0.95-V
headroom. Generated programs and `cvbs_tables.h` embed the resulting transfer;
T marks it as measured. Rebuild after changing calibration. Static calibration
still does not prove dynamic video bandwidth/settling.

## Acceptance and remaining work

Host checks establish unchanged decoding/routing, saturation, calibrated-table
selection, synthetic PAL/NTSC/CFO levels, the legacy comparison, smaller-phase
telemetry and refusal of tested noise/invalid captures. The 524,386,048-case
trajectory oracle remains applicable independently of the output transfer.

Bench: compare legacy/fixed on the same board, load, VTX, RF channel and lane;
scope sync, porch, white and chroma, then repeat with attenuation. Confirm
HDZero picture/recording, PAL/NTSC, colour/detail, menu exit, native/V5 and
power cycle. Observe FIFO faults and J copy/work timing. No measured range
improvement, PAL/NTSC compliance or HDZero fix is claimed until then.

**Default disabled:** the experimental `u` output gain/offset servo.
**Not implemented:** automatic IQ DC correction,
and H/V sync regeneration/coasting. These require a safe hardware actuator,
verified IQ-centering evidence and full field/burst timing respectively. The
previous CPU flywheel was throughput-limited; no unverified raw-ring repair
has been enabled here. Output amplitude is addressed; lost sync information
at the RF threshold is not recovered by this change.

Vendor API source: ESP-IDF v6.0.2
https://github.com/espressif/esp-idf/blob/v6.0.2/components/esp_driver_bitscrambler/src/bitscrambler.c
