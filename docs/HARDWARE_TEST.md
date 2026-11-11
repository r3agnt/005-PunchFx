# Hardware test checklist

Full old checklists (004-Looper): `docs/archive/HARDWARE_TEST_FULL.md` (not loaded each session).

## Passed
- Inherited from 004-Looper v0.8.1 (2026-11-11): clock/transport, quantized loop, beat repeat + latch, B6 pitch fx, CC control.
- **0.10.0-0.10.3 (user, flashed and tested):** layers (3-way toggle), edit/perf (2-way toggle), layer 2 hold/latch/quantised start, shared stage (filters in series on the whole mix, no start-up thump), bit crusher B7 (pre lopass, noise, depth, sample rate down to 188 Hz, mono + mild flanger).
- **0.11.0 (user, flashed and tested OK):** fx + layer 1 settings saved to flash on edit -> perf, loaded at power-up, Shift at power-up resets.
- **0.12.1-0.12.8 reverb (user, flashed, tuned by ear):** B1 reverb works as a send (tail rings out, no dry jump on release), Time scaling, tone default, pre-delay, input diffusion ("a lot better"), K8 lopass before the reverb. (0.12.0 froze at boot: fixed in 0.12.1.)

## Open items
- [ ] Reverb: CPU line (avg / max) with the reverb running: ______ ; Shift + K1 send from another fx into the reverb (not confirmed yet); Shift + B1 latch + B1 LED glow while the tail rings; long Time still builds up when a note is held (no level compensation yet).
- [ ] Bit crusher bit depth (K3) not yet right (user, "not happy", parked); mono flanger settings to fine tune later.
- [ ] Input offset (Shift + K3): record the best value for your setup: ______
- [ ] Round-trip latency not measured.

## Template for the next version
Version tested: ______   Date: ______
- [ ] (add one checkbox per new feature, then fold into "Passed" when done)
