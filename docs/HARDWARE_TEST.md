# Hardware test checklist

Full old checklists (004-Looper): `docs/archive/HARDWARE_TEST_FULL.md` (not loaded each session).

## Passed
Inherited from 004-Looper v0.8.1 (tested 2026-11-11): clock/transport, quantized loop, beat repeat + latch, B6 pitch fx on beat repeat, CC control. CPU 9% avg / 10% max (with slice mode; expect lower now).

## Open items
- [ ] Input offset (Shift + K3): record the best value for your setup: ______
- [ ] Round-trip latency not measured.

## Template for the next version
Version tested: ______   Date: ______
- [ ] 0.10.0 layers: 3-way toggle up/middle/down logs `Layer 1/2/3`; on layers 2 and 3 B1/B2 and B9-B16 do NOT run the looper/beat repeat; layer 1 works as before.
- [ ] 0.10.0 edit/perf: 2-way toggle logs `Edit mode` / `Perf mode` (if backwards, flip `kEditWhenSwitchPressed` in `main.cpp`).
- [ ] 0.10.0 layer 2, B7 bit crusher: hold = on at the next 1/16 line, release = off at the next line; Shift + B7 latches; B7 LED shows state; other buttons log `not built yet`.
- [ ] 0.10.0 edit mode: K1 bit depth, K2 sample hold, K5 gain (+-8 dB), K6 wet/dry, K7 hipass, K8 lopass all audible; perf mode locks the knobs; knobs need pick-up after switching.
- [ ] 0.10.0 latched fx keeps running when you change layer; no clicks starting/stopping; CPU line.
- [ ] 0.9.0: boot log shows `Firmware version 0.9.0`; B7 and K4 do nothing; beat repeat, B6 pitch drop and the looper still work; CPU line on serial.
