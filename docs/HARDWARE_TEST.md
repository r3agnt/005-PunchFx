# Hardware test checklist

Full old checklists (004-Looper): `docs/archive/HARDWARE_TEST_FULL.md` (not loaded each session).

## Passed
Inherited from 004-Looper v0.8.1 (tested 2026-11-11): clock/transport, quantized loop, beat repeat + latch, B6 pitch fx on beat repeat, CC control. CPU 9% avg / 10% max (with slice mode; expect lower now).

## Open items
- [ ] Input offset (Shift + K3): record the best value for your setup: ______
- [ ] Round-trip latency not measured.

## Template for the next version
Version tested: ______   Date: ______
- [ ] 0.9.0: boot log shows `Firmware version 0.9.0`; B7 and K4 do nothing; beat repeat, B6 pitch drop and the looper still work; CPU line on serial.
