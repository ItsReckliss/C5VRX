# Range max: from physics to picture

Goal: the weakest RF input at which the goggle still shows a usable picture.
Every 6 dB is roughly 2x the distance.

## dB budget

| Link | Sets | State | Achievable |
|---|---|---|---|
| RX antenna | signal | linear WiFi antenna (~3 dB loss on RHCP by definition) | RHCP omni +3-6 dB, patch 8-13 dBic, tracker |
| Diversity | fades | one receiver | two C5, vertical-blank switch: +5-10 dB effective in flight |
| Noise figure | noise floor | C5 internal LNA (WiFi class) | external ~1 dB NF LNA: +3-5 dB |
| Noise bandwidth | noise floor | 40 MHz | BW20 at the edge: +2-3 dB (built) |
| Quantization | information | coarse Q4 at the edge | noise-referenced lanes (built) |
| Demodulator | FM threshold | endpoint Phase8 | see "Demodulator limits" |
| Sync keeping | usable vs lost lock | raw sync | sync flywheel + colour killer (built) |

Rough numbers: kTB over 40 MHz = -98 dBm; with a ~5 dB NF the floor is ~-93 dBm,
and a ~10 dB FM threshold puts the picture threshold near -83 dBm.

## Measured: receiver noise vs quantizer

With the VTX off at maximum gain (G81), ultrafine lanes read P50 7, 34 %
origin. That is sigma ~2.2 ultrafine steps, i.e. ~0.56 coarse step. Widrow:
a uniform quantizer is nearly linear once sigma >= ~0.5 step, so coarse Q4
already loses little at the edge. Finer lanes help by ~1-3 dB, not 12.

## Built

### Range lanes with a noise cap (`direct_gain_v3.c`, `rf.c`)
Fine {9,7,6,5} and ultrafine {9,6,5,4} are exact 2x/4x rescales inside their
windows. They are entered only at the table's maximum analog gain; the analog
gain trims between the 6 dB steps. The receiver noise r^2 (P50 - 1, exact 4^k
per lane) is learned from quiet windows. A carrier is held at the first lane
where noise reaches sigma ~0.95 step (fine on this board); listening uses the
finest lane. Fold guard: hard saturation drops at once; soft evidence (rail
codes, wide incoherent junk) must persist 2 windows (hardware showed ~9
single-window bursts/s); 5 ms re-entry hold-off.

### Bandwidth gear (`video.c`, BW mode AUTO, default)
BW20 only at maximum gain, on the lane cap, with a present but starved or
incoherent carrier for 1 s; back to BW40 after 1 s of clear recovery. BW20
was rejected as a fixed mode (chroma/detail); at the edge it trades a little
colour for ~3 dB of CNR, as analog receivers narrow their IF.

### Sync flywheel + colour killer (`sync_flywheel.c`)
The BitScrambler demodulates on the fly from the ring TX reads ~409 us after
RX writes it. The flywheel mirrors the Phase8 code formula on a small window
per predicted line, tracks line phase and period with a PLL (phase 1/4,
frequency 1/64, period within 0.3 %), and coasts up to 300 lines. For lines
whose pulse is missing or malformed it rewrites the raw IQ bytes of the pulse
ahead of the TX read so Phase8 itself emits a clean sync at the learned sync
level (constant phase rotation per endpoint, strong cells r 4..6.5). Clean
lines are never modified; the first 20 and last 8 lines of each field are
never written (vertical interval). Above ~25 % repaired lines (EMA ~65 ms)
the burst window is written at blank level so the goggle switches to
monochrome instead of rainbow colour; released below 5 %. The tracked period
also drives the PAL/NTSC detection. Console: `B` repair, `M` colour killer.

Host test (`tools/test_sync_flywheel.c`): synthetic PAL CVBS -> FM -> Q4
cells in the live ring layout with the hardware RX/TX lag, decoded with the
exact BitScrambler formula: 745/745 broken lines repaired (61,020 samples at
sync level, 0 wrong), 0 bytes touched on clean or vertical-interval lines,
colour killer on during a 500-line fade and released afterwards.

### Telemetry and logging
`p` prints `DG3_OBS` (lane, lane_cap, noise_r2_q4, receiver DC, bw40,
bw_switches, fold_drops) and `SFW` (lock, standard, repaired, missed, levels,
colour killer). `tools/range_logger.py PORT out.csv` logs it continuously;
typed lines become marker rows ("picture lost", "30 dB").

## Demodulator limits (two bundles per output pair)

The live program reads one 16-bit pair per two bundles and can do exactly one
LUT lookup per bundle (the LUT address is bits 16..25 of the bundle word).
Phase8 uses them for: phase of the new endpoint, and the retained minus term.

- The live Phase8 already spans 50 ns endpoint-to-endpoint (the test feeds
  pairs `(0, endpoint)`), so there is no "+6 dB from the unused half".
- Sigma (`wrap(d01) + wrap(d12)`, keeps the winding the endpoint loses on
  ~8 % of weak-IQ pairs) needs the middle sample's phase as well: three
  lookups and two wraps per pair.
- Click suppression by IQ confidence and SNR-adaptive MMSE tables need the
  *pair* jointly as a LUT address (16 bits of IQ); the LUT has 10 address
  bits, so each sample first needs a reducing lookup: at least four lookups
  per pair.

None fits two bundles; earlier exact-adjacent attempts hit the same limit.
They need a different architecture (e.g. halving the output rate, or a second
processing stage), not a LUT change.

## Hardware plan (not firmware)

1. Measurement bench: VTX -> SMA attenuators (10/20/30 dB) -> C5, and the same
   into the goggle's own receiver as reference. Walk tests vary +-10 dB.
2. RHCP antenna, then a patch.
3. 5.8 GHz LNA (~1 dB NF, 15-20 dB gain) with a band filter; V5 and the lanes
   keep the extra gain in range close by.
4. Diversity: two C5s; switch their DAC outputs with an analog video switch
   during vertical blanking, driven by the better `SFW`/coherence score.
