# Changelog

Newest first, ONE line per version. Details (why, how, test counts) go in the git commit message.
Full history up to 004-Looper v0.8.1: `docs/archive/CHANGELOG_FULL.md` (not loaded each session).

- **0.12.7** - Reverb: fixed input diffusion (4 allpass per side, ~19 ms) smooths the bright slapback-like first echoes.
- **0.12.6** - Reverb: K1 Time scaled by tail length 0.4-12 s (even steps); tone default 4686 Hz; pre-delay 0-100 ms, default 0.
- **0.12.5** - Reverb slot: K8 lopass filters the dry only (return keeps its hipass).
- **0.12.4** - Output gain (K5) is +-12 dB; on the reverb it now also applies to the dry while the reverb is on (post wet/dry).
- **0.12.3** - Reverb is a true send: dry untouched, K6 = return level, no level jump on release (the tail just rings out).
- **0.12.2** - Reverb return +8 dB make-up gain (`ReverbBus::kMakeup` = 2.5).
- **0.12.1** - FIX: 0.12.0 froze at power-up (ReverbBus in SDRAM had a constructor that ran before the SDRAM was on). ReverbBus now has no constructor + a compile-time check.
- **0.12.0** - Reverb bus on B1 (DaisySP ReverbSc, LGPL on): time/tone/pre-delay, tail rings out, auto-off; per-fx reverb send on Shift + K1; SavedState layout v2 (saved fx settings reset once).
- **0.11.0** - Settings saved to flash: fx settings + loop length/input gain/output level/offset, on edit -> perf; loaded at power-up; Shift at power-up resets. `saved_state.h`, `test_saved_state.cpp`.
- **0.10.3** - Bit crusher is mono, followed by a mild fixed stereo flanger (`fx_flanger_core.h`, reusable).
- **0.10.2** - Bit crusher sample rate goes down to 188 Hz (octave steps).
- **0.10.1** - Shared filters now act on the whole wet/dry mix and start without a thump; bit crusher = pre lopass, noise, bit depth (curved 12..1), sample rate.
- **0.10.0** - Layers (3-way toggle) + layer 2 punch-in fx framework: 15 fx buttons, hold/latch, 1/16 quantised, edit/perf (2-way toggle), shared stage (gain, wet/dry, HP, LP), per-fx soft-takeover knobs, bit crusher on B7. `src/engine/fx/`, `test_fx.cpp`.
- **0.9.0** - Forked from 004-Looper v0.8.1. Slice mode (B7, K4 pitch, Shift+B6 reverse, CC 24) and its code/test removed. Looper + beat repeat + B6 pitch drop unchanged.
