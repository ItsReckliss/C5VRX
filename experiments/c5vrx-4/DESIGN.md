# C5VRX-4 design starting point

## Accepted scope

- Repository-root location: `experiments/c5vrx-4`.
- Independent development branch from current main, not a fork of PR #122's
  firmware policy. Main currently uses Direct Gain V5 by default.
- Target: maximum usable range with at least Golden Phase5 image quality.
- Phase8, its LUT representation and its two-bundle output geometry may be
  replaced. New hardware is an option to evaluate, not a selected requirement.
- Native AGC remains a user requirement for the experimental receiver; paced
  native operation is a comparison candidate. The main build's gain policy
  must not be mistaken for this experimental design decision.
- The operator subsequently authorized an isolated experimental build. The
  existing-board span-75 prototype is implemented for comparison; it is not
  a demonstrated choice for the final long-range architecture.

## Architecture candidates

| Candidate | Hardware impact | Why investigate | Main unresolved issue |
| --- | --- | --- | --- |
| Existing C5, pre-tap filter characterization and improved lane decoding | existing board | reduce information loss / noise before phase estimation | filter placement and lane ambiguity |
| Three-bundle span-75, 13.333 MS/s unique video | existing board, new streaming program | extra processing stage and calibrated DAC transfer | full-band response, wrapping, dataflow and timing |
| C5 RF front end, twelve DIAG lanes, external DSP/DAC driver | new wiring and processing hardware | full-range I6/Q6, complex filtering and real tracking demodulation | clock export, capture timing, DSP resources and hardware choice |

No candidate has demonstrated additional range. The previous two-bundle
quadrant-conditioned estimator is retained as research history; the operator
rejected it as an adequate final solution.

## Design sequence

1. Define the common RF/video comparison conditions, expected deviation,
   carrier offsets, wanted bandwidth and fade-recovery requirements.
2. Establish what information reaches each available PHY/DIAG tap, including
   filter response and signed ADC-bit identity. Do not use active MAC dump SRAM
   as a live source without a new demonstrated access mechanism.
3. Budget sample rates, lane widths, memory, instructions/logic, output cadence
   and clock crossings for each viable architecture.
4. Derive the proposed estimator/filter against independent full-band video
   cases. Include noise, folding, CFO, multipath and gain transitions.
5. Select the simplest architecture that can improve the real receiver limit.
   Only then add the isolated firmware or hardware build target here.
6. When implementation testing is requested, establish transport continuity
   and compare required RF input at equal Phase5-quality video.

## Evidence distinctions

- Individual DIAG-bit correlation does not prove simultaneous twelve-lane timing.
- Finer sign-preserving lanes fold outside their contiguous windows.
- RF sensitivity, ADC quantization and demodulation threshold are separate.
- Post-demodulation filtering cannot undo information lost before angle detection.
- Matched de-emphasis requires the actual transmitter response; an arbitrary
  shunt capacitor does not establish a matched television emphasis network.
- A successful build, stable timer or noiseless static picture does not prove
  weak/strong gain recovery or extended usable range.

See [RESEARCH.md](RESEARCH.md) for calculations, primary references, source
revisions and corrected assumptions. The prototype uses coarse I4/Q4 lanes,
native AGC with a 1 ms hold cadence and 20 us acquisition window, and fine-stage
setting 127. These are experimental starting settings, not measured optima.
No external processor or claimed sensitivity gain is selected yet. See
[README.md](README.md) for the implementation and build instructions.

## Prototype limits and recommendation

Keep C5VRX-3 as the working receiver and comparison reference. C5VRX-4 has no
measured range advantage. Its three-sample endpoint phase difference can reduce
some noise, but has less frequency-offset headroom and more attenuation near
the colour subcarrier. Phase6 also retains less angle precision than Phase8.
The DAC mapping uses nominal resistor values, not a measured board calibration.

This prototype does not implement full-range I6/Q6 capture, complex filtering,
a tracking FM demodulator or matched de-emphasis. Those require separate design
work; whether additional processing hardware is acceptable remains unresolved.
The first build (`162c2ab`) was flashed and produced usable video. The operator
reported a less clean picture than C5VRX-3; no range improvement was established.
See the README hardware observation for the scope of this first comparison.
