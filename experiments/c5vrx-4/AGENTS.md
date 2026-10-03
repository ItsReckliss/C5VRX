# Isolated C5VRX-4 integration ledger

Inherit the repository attribution, hardware and evidence instructions. This
directory extends C5VRX by Twotoz and contributors; README.md and INTEGRATION.md
define its scoped defaults, donor lineage and pending physical acceptance.

Only this directory may change for this integration. Do not edit shared main,
root configuration, workflows or website to make the isolated build work.
Its main/ snapshot is intentional isolation; C5VRX-3 remains untouched. Keep
subsequent donor updates explicit in INTEGRATION_SOURCES.json.

Record source format, acquired rate, CPU/hardware ownership, raw ring, three
bundles, unique 13.333M output and physical DAC40M separately. No CPU per-sample
output, transformed ring, concurrent RX/TX BitScrambler or boundary DSP resets.

Protected adaptive V5 lanes are default; fixed ultrafine is a Z comparison.
Native is opt-in and owns gain exclusively. Severe coarse clipping may cut
active Direct Gain to its established G20 floor; fresh epochs and settling
refusal still apply. Manual/native never use this automatic gain response.

Sync scoring is a stride-3 semantic estimate, not tagged physical DAC output.
AFC uses adjacent raw Phase8 burst/clean-porch evidence and separate sticky video
TRACK; AUTO remains off until explicitly selected, with a bounded correction
budget. Do not use gain HOLD alone as a video lock or a CFO-write permission.

Default has no running LUT writes. The user-authorized opt-in `u` video-level
lab uses a stopped-engine addressing probe and bounded DAC-only LUT16 writes;
keep it experimental/off by default until live arbitration, physical timing and
FIFO continuity are demonstrated. H/V regeneration and IQ DC correction remain
absent. Tests/compiler results are not range/HDZero
or sample-gapless RF proof. Run verify.py and the exact-head IDF build before
requesting merge approval; merge only after the project maintainer explicitly
approves.
