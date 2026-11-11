# Hardware test checklist

Full old checklists (all ticked items): `docs/archive/HARDWARE_TEST_FULL.md` (not loaded each session).

## Passed
**Version 0.8.1, tested 2026-11-11 (user): all features from 0.2.0 to 0.8.1 tested and working** (clock/transport, loop engine, beat repeat + latch + 1/16 quantising, slice mode, B6 pitch fx on beat repeat and in slice mode, bar-line reset, K4 pitch, CC control).
- CPU meter: **9% average, 10% maximum** (30 s readings).

## Open items
- [ ] **Slice mode B6 pitch reverse (Shift + B6) needs work** (user, 2026-11-11). Parked; details to be planned.
- [ ] Input offset (Shift + K3): record the best value for your setup: ______
- [ ] Round-trip latency not measured.
- [ ] Forward-only B6 at +12 st reads past the slice (silence on the last slice): acceptable?

## Template for the next version
Version tested: ______   Date: ______
- [ ] Boot log shows the right `Firmware version`
- [ ] (add one checkbox per new feature, then fold into "Passed" when done)
