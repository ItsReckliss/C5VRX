# C5VRX-4 integrated alpha

One isolated ESP32-C5 receiver project, assembled from the C5VRX-3 range/control
work and the three-bundle Phase8 Unwrap75 experiments. It extends **C5VRX by
Twotoz and the C5VRX contributors** and their receiver research.
Source: https://github.com/Twotoz/C5VRX · official website and Discord invite:
https://twotoz.github.io/C5VRX/. Existing author notices and GPL-3.0-only apply.

This directory contains its own runtime `main/`, configuration, partition table,
tests and evidence. It does not compile the repository's C5VRX-3 `main/`.
The integration PR changes only `experiments/c5vrx-4/`; merge requires the
operator's explicit approval. Building and publishing a PR alpha is independent
of merging. Current main's existing alpha workflow/flasher can build this project.

## Receiver contract

- MODEM_DIAG packed Q4/I4, positive-edge PARLIO RX at 40 MS/s, raw cyclic ring.
- TX-only Phase8 endpoint decode plus middle-sample quadrant winding; exactly
  three bundles per three input/output bytes. Unique CVBS 13.333 MS/s,
  continuous `[D,D,D]` at a 40 MHz / 40 MB/s physical DAC transport.
- Six original DAC GPIOs and resistor network; no CPU sample-paced output.
- Direct Gain V5 by default: first-window physical correction, 200-us observer,
  descriptor dedupe, table-maximum listening, measured noise lane cap and
  anti-hunt damping. Native AGC remains a separate opt-in gain owner.
- Protected adaptive lanes by default: two fresh overlap windows before a
  one-step finer upgrade; immediate fold escape, 210-us post-switch observation
  exclusion, retained phase state, magnitude-only GPIO routing updates.
  This is not an atomic or tagged hardware handover.
- Severe coarse-lane clipping (>=50%, P95>=95) sends active Direct Gain to G20,
  its existing controller floor. Moderate overload uses staged RF/BB cuts.
  Finer-lane saturation first escapes to coarse. No native/manual gain writes.
- Fixed nominal loaded CVBS transfer STD150 (default): 0.310-V blanking +
  0.150 V/MHz, nearest DAC code with saturation. Assumed nominal sync/white
  targets are 0.010/1.010 V: 0.300 V sync depth and 1.000 V sync-to-white
  under one 75-ohm load. Replaces HR100's undersized output; full amplitude
  leaves only small rail margin, not broad CFO tolerance. CVBS150 and legacy
  remain M comparisons. See CVBS_OUTPUT.md for sources and physical limits.
- Slow sync supervision uses the same stride-3 Phase8/winding transfer estimate,
  instead of the old Phase5 shadow. Snapshot alignment remains approximate.
- AFC V2 measures burst-confirmed sync and burst-free porch, with both endpoints
  valid, bounded/stable 16-window evidence, context/settle/copy refusal and a
  maximum of four 250-kHz acquisition steps. AFC video TRACK is sticky and
  writes nothing while valid; it is independent of gain HOLD. **AUTO defaults
  off**: transmitter reference/sign still need physical calibration.
- PR154 PHY ownership restoration, serialized generation-checked gain writes,
  stale-overload mailbox rejection, reversible pinned PHY/BW/11p/native-hold labs.
- Main's analog-video scanner confidence and centred-RF tie-break are retained.
  They identify candidate channels; a confident scan is not range proof.

## Operator controls

| Key | Action |
|---|---|
| `T` | Detector, mapping, lane geometry and gain-owner status |
| `J` | AFC state plus eight bounded sync/IQ snapshots; no actuator |
| `u` | Toggle default-on sync/black level regulation, reboot; fixed mapping required |
| `M` | Cycle STD150 (default) / CVBS150 / previous full-span transfer, reboot |
| `Z` | Protected adaptive V5 / fixed ultrafine comparison, reboot |
| `h` | STATIC / bounded HISTORY phase decode, reboot |
| `N` | Direct Gain / native AGC, reboot |
| `H`, `{}`, `[]`, `W`, `B`, `A`, `:`, `L` | PR154 shared PHY diagnostics/labs |
| `(`, `)` | Explicit native-only BB hold / 100-cycle reversible lab |

The lane comparison uses the new NVS key `c5vrx4/force_ultra_v2`; the old
PR146 `force_ultra` setting does not silently force the integrated default.
Other C5VRX-4 settings remain in `c5vrx4`, separate from C5VRX-3 `c5vrx`.
STATIC remains the default; HISTORY and fixed ultrafine are comparisons.
Normal probes are disabled at boot. Lab writes require the pinned PHY archive;
unknown libraries keep observation and refuse undocumented writes.

## Build and verification

```sh
cd experiments/c5vrx-4
python3 verify.py
. "$IDF_PATH/export.sh"  # ESP-IDF v6.0.2
idf.py -DIDF_TARGET=esp32c5 build
```

IDF configuration invokes `verify.py`, so the existing alpha workflow executes
this directory's C regressions, AFC cases, Phase8/routing tests and exhaustive
524,386,048-trajectory oracle before cross-compiling. Host Python 3 and GCC are
required; this verification does not need NumPy, network access or hardware.
Generated tables must match checked-in artifacts. No binaries are committed.
The supervisory task stack is 16 KiB to accommodate the bounded snapshot
analyzer; startup refuses allocation failure. The separate adaptive 5/20-ms level worker uses another 16-KiB stack and an 8190-byte heap
snapshot; J capture also has a 16-KiB stack. Heap/stack margin under menu and
concurrent capture still needs hardware observation; T reports level-worker margins.

[INTEGRATION.md](INTEGRATION.md) records PR/issue disposition and acceptance.
[INTEGRATION_SOURCES.json](INTEGRATION_SOURCES.json) pins donor revisions.
[CVBS_OUTPUT.md](CVBS_OUTPUT.md) explains the loaded transfer and scope model.
Earlier research files are donor records; this README defines current defaults.

## Evidence boundary

This is an unmerged test build. Host tests and compiler success do not establish
sample-gapless transport, improved sensitivity/range, PAL/NTSC compliance or
HDZero acceptance. Fixed scaling does not recover phase information lost to
clipping, origin collapse or RF noise. Automatic sync-referenced video level regulation is enabled by default at Leon's
request. It targets 286/300-mV NTSC/PAL sync depth, rejects noisy/stale evidence,
holds through signal loss and latches off on write/transport faults. Three valid
snapshots qualify bounded updates; recovery runs at 5 ms for 100 ms, then
returns to 20 ms. RF settling and lane history are excluded; each DAC entry
slews by at most 32 mV according to the loaded voltage table. Concurrent LUT arbitration and goggle
acceptance remain physical gates. It corrects output gain/offset, not IQ DC.
H/V regeneration/coasting and CPU raw-ring sync repair remain absent.
See [CVBS_LEVEL.md](CVBS_LEVEL.md) for controls, evidence and limits.
