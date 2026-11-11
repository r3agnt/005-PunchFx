# Changelog

Newest first, ONE line per version. Details (why, how, test counts) go in the git commit message.
Full old entries: `docs/archive/CHANGELOG_FULL.md` (not loaded each session; read only if needed).

- **0.8.1** - Slice mode B6: Shift + B6 toggles the reverse half (default OFF = whole eighth forward at the raised pitch; ON = forward/reverse ping-pong). Reverse "needs work" (user, 2026-11-11), parked.
- **0.8.0** - Slice mode: bar line = reset (held slice choked, rotated loop back to beat 1); K4 = B6 pitch -12..+12 st (CC 24, default +3).
- **0.7.2** - Slice mode B6 = +3 st forward/reverse ping-pong on 1/8 lines; CPU meter on serial every 30 s. (0.7.0 / 0.7.1 1/16-retrigger versions superseded.)
- **0.6.2** - B6 pitch fx: slow repeats stay in time (head lag limit 32 -> 10 ms).
- **0.6.1** - B6 pitch fx tightened: new pitch head per repeat, no 20 ms flam.
- **0.6.0** - B6 pitch-down on the beat repeat (1 semitone lower per repeat, max 12), `pitch_fx.h`.
- **0.5.3** - Slice release carries on from the NEXT slice (no double play).
- **0.5.2** - Slice mode captures ONE bar on a bar line (B9 = beat 1); waits for a full bar; Shift + B7 ignored.
- **0.5.1** - Slice release loops the whole window from the triggered slice.
- **0.5.0** - Slice mode on B7 (Slice loop buffer); `loop_engine` renamed `quantized_loop_buffer`.
- **0.4.5** - Beat repeat presses and releases quantised to a 1/16-bar grid.
- **0.4.4** - Slot copy chunk 2048 -> 256 frames per block (fixed first-pass glitch).
- **0.4.3** - Beat repeat latch: Shift + B9-B16.
- **0.4.2** - Minimum tempo 40 -> 130 BPM (buffers 28.7 -> 8.9 MB).
- **0.4.1** - Beat repeat: 3/8 division, flip between held buttons with stored slices, version print on Shift + B16 long press.
- **0.4.0** - Beat repeat on buttons 9-16 + input offset (Shift + K3).
- **0.3.3** - Dimmer top-row LEDs.
- **0.3.2** - Button layout: B1 = record arm, B2 = loop/live.
- **0.3.1** - K2 loop length has no soft takeover; LEDs show the length for 1.5 s.
- **0.3.0** - Loop engine (arm, record, playback, source switch, clear, re-record).
- **0.2.0** - MIDI clock + transport, bar/beat counters, tempo estimate, LEDs, logging, Mac tests.
- **0.1.0** - Forked from 003-ChannelStrip v0.3.1 (EQ and saturator removed).
