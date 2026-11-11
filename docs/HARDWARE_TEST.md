# Hardware test checklist

Full old checklists (004-Looper): `docs/archive/HARDWARE_TEST_FULL.md` (not loaded each session).

## Passed
Inherited from 004-Looper v0.8.1 (tested 2026-11-11): clock/transport, quantized loop, beat repeat + latch, B6 pitch fx on beat repeat, CC control. CPU 9% avg / 10% max (with slice mode; expect lower now).

## Open items
- [ ] Input offset (Shift + K3): record the best value for your setup: ______
- [ ] Round-trip latency not measured.

## Template for the next version
Version tested: ______   Date: ______
- [ ] 0.11.0 saving: first boot logs `FX settings: defaults` or `loaded`; edit B7's knobs and K2/gain, flip the switch to perf (`FX settings saved`), power-cycle: values come back (knobs need pick-up); flip edit -> perf again with no change logs `unchanged`; no audio glitch while it saves; hold Shift at power-up -> `RESET to defaults`; reflash the firmware and check the settings survive.
- [ ] 0.10.3: bit crusher is mono (a hard-panned input comes out in the middle/both sides), the mild flanger smooths it without sounding like an obvious flanger; CPU line OK.
- [ ] 0.10.1: filters sound right with wet/dry < 100%; no thump when an fx starts with hipass up; bit crusher K1 pre lopass, K2 noise, K3 depth (audible across most of the knob), K4 sample rate.
- [ ] 0.10.0 layers: 3-way toggle up/middle/down logs `Layer 1/2/3`; on layers 2 and 3 B1/B2 and B9-B16 do NOT run the looper/beat repeat; layer 1 works as before.
- [ ] 0.10.0 edit/perf: 2-way toggle logs `Edit mode` / `Perf mode` (if backwards, flip `kEditWhenSwitchPressed` in `main.cpp`).
- [ ] 0.10.0 layer 2, B7 bit crusher: hold = on at the next 1/16 line, release = off at the next line; Shift + B7 latches; B7 LED shows state; other buttons log `not built yet`.
- [ ] 0.10.0 edit mode: K1 bit depth, K2 sample hold, K5 gain (+-8 dB), K6 wet/dry, K7 hipass, K8 lopass all audible; perf mode locks the knobs; knobs need pick-up after switching.
- [ ] 0.10.0 latched fx keeps running when you change layer; no clicks starting/stopping; CPU line.
- [ ] 0.9.0: boot log shows `Firmware version 0.9.0`; B7 and K4 do nothing; beat repeat, B6 pitch drop and the looper still work; CPU line on serial.
