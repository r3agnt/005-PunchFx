# Changelog

Newest first, ONE line per version. Details (why, how, test counts) go in the git commit message.
Full history up to 004-Looper v0.8.1: `docs/archive/CHANGELOG_FULL.md` (not loaded each session).

- **0.10.3** - Bit crusher is mono, followed by a mild fixed stereo flanger (`fx_flanger_core.h`, reusable).
- **0.10.2** - Bit crusher sample rate goes down to 188 Hz (octave steps).
- **0.10.1** - Shared filters now act on the whole wet/dry mix and start without a thump; bit crusher = pre lopass, noise, bit depth (curved 12..1), sample rate.
- **0.10.0** - Layers (3-way toggle) + layer 2 punch-in fx framework: 15 fx buttons, hold/latch, 1/16 quantised, edit/perf (2-way toggle), shared stage (gain, wet/dry, HP, LP), per-fx soft-takeover knobs, bit crusher on B7. `src/engine/fx/`, `test_fx.cpp`.
- **0.9.0** - Forked from 004-Looper v0.8.1. Slice mode (B7, K4 pitch, Shift+B6 reverse, CC 24) and its code/test removed. Looper + beat repeat + B6 pitch drop unchanged.
