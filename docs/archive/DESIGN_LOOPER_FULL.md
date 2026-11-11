# Looper design (agreed with the user)

Audio looper + beat-repeat effect, locked to MIDI clock from the DIN input. Stereo, 4/4 only, 48 kHz. Built in stages; the status of each is at the bottom.

## Signal flow
```
input -> gain -> beat repeat (live, or stutter from the 2-bar history) -> "performance signal"
performance signal -> loop recorder (records this signal)
output = performance signal  <-- Button 2 -->  loop playback
```
- Beat repeat only ever stutters the **input** history (never the loop playback).
- The loop records the beat-repeat output, so stutter performances are captured.
- The dry input is not heard while the loop plays, unless Button 2 switches back to it.

## Why no delay is needed on the live signal
Live input, stutter and loop playback all go through the same converters, so they share the same latency (estimated 2-3 ms round trip at the default 48-sample block; **not measured yet**). Stutter copies a slice that finished an exact number of beats earlier and the loop is cut on the clock's bar boundary, so switching between them is always between signals that are in time with each other (only a short crossfade at each switch).

The real timing risk is **audio vs clock**: if the DAW's audio arrives a few ms late relative to its MIDI clock, a slice boundary cuts slightly early. Fix (0.4.0): an **input offset (ms)** applied to where slices are read from the history buffer (not a delay on the live signal), on Shift+K3, +/-20 ms, centre 0, tuned by ear.

## Clock and transport
- MIDI clock = 24 ticks per beat; 4/4 = 96 ticks per bar; 8 eighth-notes per bar.
- Real-time messages are taken from libDaisy's `StartReceiveRt` callback, queued, and stamped by the audio callback (max 1 block, ~1 ms, late). Tempo is measured over one bar of ticks.
- **Start**: bar counter resets; the next tick is bar 1, beat 1. **Stop**: loop stops; **Start** restarts it. **Continue** and Song Position Pointer are ignored for now.
- No ticks for 0.5 s = "clock lost": counting stops.
- Tempo changes: a recorded loop has a fixed length in samples; it is re-aligned to the clock at each loop restart (no time-stretching).

## Names
- **Quantized loop buffer** = the global looper on B1/B2 (`src/engine/quantized_loop_buffer.h`, class `QuantizedLoopBuffer`, global `quantized_loop`).
- **Slice loop buffer** = the frozen bar (0.5.2; was 2 bars) of slice mode on B7 (`src/engine/slice_loop_buffer.h`, class `SliceLoopBuffer`, global `slice_loop`).
- **Beat repeat** = the stutter on buttons 9-16 (`src/engine/beat_repeat.h`).

## Controls
| Control | Function |
|---|---|
| Knob 2 | Loop cycle length: 1, 2, 4 or 8 bars (table `kLoopBarOptions` in `engine/looper_config.h`). Only affects the **next** recording. |
| Button 1 | Rec arm. Slow flash = armed; recording starts at the next loop cycle (multiple of the chosen length since Start). Does nothing, and logs, if no clock is running. When a loop is playing, pressing it re-arms a new recording at the next cycle (no overdub). |
| Shift + Button 1 | Clear loop (fade out, no click). |
| Button 2 | Switch output between loop playback and live input (short fade). |
| Buttons 9-16 | Beat repeat: 1 bar, 1/2, 3/8, 1/4, 1/8, 1/16, 1/32, 1/64 (of a bar; 3/8 = dotted quarter). Press and release are both quantised to the next 1/16 line (0.4.5), held until released, the most recent button wins. Repeats the last slice-length of audio ending on that line. Flip between held buttons (see rules below). |
| Shift + Buttons 9-16 | Latch (0.4.3): toggles that beat repeat division on/off so it keeps repeating after release. One latch at a time (Shift + another button moves it). A normal held press overrides it while down. Cleared when the transport stops. |
| Button 7 | Slice mode on/off. ON (0.5.2) waits for the next **bar line**, then freezes the whole bar that just ended (beat 1 = B9) and plays it as the Slice loop buffer; waits further if a whole bar has not been recorded yet. OFF is quantised to the 1/8 line. See "Slice loop buffer" below. |
| Button 6 | **Pitch fx on the beat repeat (0.6.0).** Hold: on at the next 1/16 line, off at the next 1/16 line after release. Each repeat of the held slice is 1 semitone lower than the previous one (see "Pitch fx" below). In slice mode (0.7.2) the held slice plays 3 semitones up as a forward/reverse ping-pong (see "Slice loop buffer" below). Ignored while Shift is held and while the transport is stopped. |
| Shift + Button 7 | Nothing while the window is 1 bar (logs `Bank ignored - 1 bar only`; with `kSliceBanks = 2` it would switch bank). |
| K4 | Pitch of the slice mode B6 ping-pong, whole semitones -12..+12 (0.8.0, CC 24; soft takeover; default +3). Takes effect on the next 1/8 line. |
| Shift + K3 | Input offset, -20..+20 ms (centre 0), no pick-up. Moves the beat repeat slice grid against the clock (positive = audio arrives later than the clock). Applies to beat repeat only; the loop start is not shifted yet. |
| Shift + K1 / K2 | Input gain / output level (placeholders kept from the template). |

State flow: `IDLE -> ARMED -> RECORDING -> PLAYBACK` with no gap; all switches get a 3-10 ms crossfade, and the loop's wrap point is crossfaded too (a few ms are recorded past the end for this).

## LEDs (LED n is beside button n; PWM brightness)
- **Top row (1-8):** one LED per bar of the loop cycle. Dim = in the cycle, bright = current bar. From 0.3.0: ARMED = the start-bar LED blinks slowly; RECORDING = LEDs fill up like a progress bar; PLAYBACK = one LED moves along.
- **Bottom row (9-16):** the eight eighth-notes of the bar. Held stutter button = medium glow on its LED (0.4.0).
- Armed/recording is also shown on the Seed's onboard LED, and LED 16 (brighter = buffer, dimmer = input) shows the Button 2 state (0.3.0).
- **No clock:** LEDs 9 and 16 blink slowly. **Clock but stopped:** bottom row glows very dimly.

## Loop engine rules (0.3.0, `src/engine/quantized_loop_buffer.h`)
- **States:** IDLE, ARMED, RECORDING, PLAYING, PLAYING+ARMED (a re-record is armed while a loop plays).
- **B1:** IDLE -> ARMED; ARMED -> cancel; PLAYING -> PLAYING+ARMED (re-record; the old loop keeps playing until the new cycle, then the live signal is heard while the new take is recorded over it; the new loop then plays from the buffer even if the live input had been selected); PLAYING+ARMED -> cancel; RECORDING -> ignored (logged).
- **Arming** needs MIDI clock ticks (not necessarily Play): arming while stopped starts recording at bar 1 when Play is pressed. Refused with no clock, and refused if the tempo is too slow for the chosen length.
- **Cycle start** = a bar number that is a multiple of the chosen length counted from Start. The length is latched when you arm; turning knob 2 later never changes a loop that exists.
- **Loop length** = the samples actually recorded between the two cycle starts. At every later cycle start the playback restarts from the beginning with a 5 ms crossfade (this corrects drift between the DAW clock and the Seed's audio clock).
- **Seam:** 20 ms of audio is recorded past the loop end and blended into the loop start.
- **B2:** loop <-> live signal, 5 ms crossfade; the loop keeps running silently so it stays in time. Ignored (logged) with no loop.
- **Shift+B1 (clear):** crossfades to the live signal over 5 ms, then forgets the loop. Aborts an armed or running recording.
- **Transport Stop / clock lost:** a recording is aborted (nothing kept); a playing loop is held (live signal heard) and restarts from its beginning at the next bar 1; ARMED stays armed.
- **Recording point:** after input gain, before soft clip and output level, so the output level knob applies to loop and live alike.
- **Buffer overflow** (tempo estimate not known yet when armed, then too long): recording is aborted with a log message.

## Memory (Seed has 64 MB SDRAM)
Stereo float (32-bit), about 384 KB per second. Sizes come from `kMaxLoopBars` (8), `kMinBpm` (130, since 0.4.2) and `kHistoryBars` (2) in `engine/looper_config.h`: loop buffer 5.7 MB, 2-bar history 1.4 MB, stutter slots 1.8 MB, Slice loop buffer 1.4 MB (0.5.0) = **10.4 MB** in total (15.5% of the 64 MB). The boot log prints the loop and history sizes.

## Minimum tempo and memory (decision: 130 BPM, no control)
**Decision (0.4.2):** the user only plays faster styles, so the buffers are sized for a minimum tempo of **130 BPM** (`kMinBpm` in `src/engine/looper_config.h`). It is a fixed compile-time constant: there is no knob or setting for it (a Shift+K8 selector with persistent storage was discussed and dropped, because the buffers are fixed arrays sized at compile time, so a runtime knob could not free any memory anyway).

**Why memory depends on it:** every buffer holds a number of *bars*, and a bar is longer (more samples) at a slow tempo. All buffer sizes are calculated from `kMinBpm`, so memory is proportional to 1 / kMinBpm. Nothing else needs to change when you edit it.

**To change it later:** edit `kMinBpm` (and `kMaxLoopBars` for longer loops) in `looper_config.h`, rebuild, check the SDRAM line of `make` (limit 64 MB, 67.1 MB in decimal), and update the table below and the tests (`make -C test`; `test_clock.cpp` `TestConfig` checks the 130 BPM sizes, `test_quantized_loop_buffer.cpp` `TestRefusals` uses 100 and 150 BPM around the limit, and `test_beat.cpp` runs at 138.89 BPM, where a tick is exactly 864 samples, so keep the test tempo at or above `kMinBpm`).

Memory by minimum tempo (8-bar loop, 2-bar history, eight stutter slots; decimal MB; free = of the 67.1 MB of SDRAM):

| Min BPM | Loop | History | Slots | Total | Free |
|---|---|---|---|---|---|
| 40 (0.4.1) | 18.4 | 4.6 | 5.6 | 28.7 | 38.5 |
| 80 | 9.2 | 2.3 | 2.9 | 14.4 | 52.7 |
| 100 | 7.4 | 1.9 | 2.3 | 11.6 | 55.5 |
| 120 | 6.2 | 1.6 | 2.0 | 9.7 | 57.4 |
| **130 (now)** | 5.7 | 1.4 | 1.8 | **8.9** | 58.2 |
| 140 | 5.3 | 1.3 | 1.7 | 8.3 | 58.8 |
| 160 | 4.6 | 1.2 | 1.5 | 7.3 | 59.8 |
| 170 | 4.3 | 1.1 | 1.4 | 6.9 | 60.2 |

**What 130 BPM means in use** (one bar = 1.85 s at 130 BPM; an 8-bar loop = 14.8 s):
- A loop of N bars is accepted only if it fits: 8 bars down to 130 BPM, 4 bars down to 65 BPM, 2 bars down to 32 BPM, 1 bar down to 16 BPM. Otherwise B1 logs `B1 ignored - tempo too slow` (nothing is recorded).
- Stutter slices are stored in slots sized for the slice at 130 BPM, so below about 130 BPM the long divisions (1 bar, 1/2, 3/8, 1/4, 1/8) are refused with `Stutter ignored - tempo too slow` (the short ones, 1/16 and below, still work). At a faster tempo everything fits with room to spare.
- If you ever need 120 BPM material: set `kMinBpm` to 120 (9.7 MB) or lower; there is plenty of memory.
- Larger values give more room for something else: at 160 BPM the same memory that held 8 bars at 40 BPM would hold about 32 bars.

## Logging (serial)
Boot: version and buffer sizes. Every button and knob in real units. Clock: detected/lost, tempo when it changes by more than 0.5 BPM, `Transport Start/Stop`, `Bar n Beat n` on every beat. From 0.3.0: looper state changes, source switches, clears, `B1 ignored - no MIDI clock`. From 0.4.0: stutter armed/on/off.

## Beat repeat rules (0.4.0, changed in 0.4.1; `src/engine/beat_repeat.h`)
- **Buttons 9-16** = 1 bar, 1/2, 3/8, 1/4, 1/8, 1/16, 1/32, 1/64 of a bar. 3/8 (dotted quarter, 1.5 beats) was added in 0.4.1; its grid is counted from bar 1 beat 1 and runs on across bar lines, so it lines up with the bar every 3 bars. Held state is read from the hardware every audio block (not from press/release events, because libDaisy swallows the release after a long press, which would leave a stutter stuck).
- **Quantised (0.4.5):** every press AND every release waits for the next **1/16-bar line** (`kQuantizeDen` in `looper_config.h`; multiples of bar/16 from the bar start, shifted by the input offset), whichever division the button is. The press then repeats the last slice-length of audio that ends on that line (the slice length is the button's own division, so it no longer starts on the division's own grid, but it ends on a 1/16 line). Up to one 1/16 of delay (about 115 ms at 130 BPM) is the price for landing in time. A tap shorter than the wait is remembered: it starts at the line and ends at the following line (one 1/16 of repeat at least). Before 0.4.5 the press waited for the division's own boundary and the release was immediate.
- **Crossfades end on the 1/16 line:** every change (live -> stutter, the loop point of each repeat, switching division) is a 5 ms linear crossfade that is scheduled to finish exactly on the boundary. The first hit of each repeat is therefore at full level. (This uses 5 ms of "pre-roll" audio from before the slice.)
- **Stored slices:** the history ring keeps being overwritten while you hold, so each captured slice is copied (2048 frames per audio block) into its division's own slot; the voice switches to it when complete (identical samples, no audible change). A 1-bar slice can be held indefinitely.
- **Release:** back to live at the next 1/16 line (5 ms crossfade ending on the line) after the last button is released. Transport Stop / clock lost is still immediate.
- **Several buttons (0.4.1, flip between them):** the most recently pressed wins. A division used for the first time in this hold captures its slice at the next 1/16 line (the old one keeps repeating until then). A division that already has a stored slice does NOT capture again: at the next 1/16 line it switches (5 ms crossfade ending on the line) to that original slice, started at the position it would have reached if it had been repeating since it was captured (in time). Releasing the newest button falls back to the older held one the same way; releasing an older one while a newer is held changes nothing. Releasing the last button returns to live and forgets all stored slices; the next press captures fresh audio. (0.4.0 re-captured at the next boundary instead; changed at the user's request.)
- **Transport stopped / tempo unknown:** presses are ignored and logged (`Stutter ignored - not running`); a press made while stopped stays ignored until released. Stop while repeating returns to live.
- **Too short:** slices under 10 ms (about 375 BPM and faster at 1/64) are refused (`Stutter ignored - too short`). **Too slow:** a slice that does not fit its slot (tempo below the 130 BPM minimum) is refused (`Stutter ignored - tempo too slow`).
- **Source:** the history records the input after input gain (before the loop). While a loop plays from the buffer the stutter is inaudible; it is heard when the output is live (B2) or while recording, and what is recorded into the loop is the stutter output.
- **Memory (0.4.2, 130 BPM minimum):** history ring 2 bars (1.4 MB) + one slot per division (together 2.4 bars, 1.8 MB) = 8.9 MB of SDRAM in total with the loop buffer (see "Minimum tempo and memory" above). Below 130 BPM the long divisions are refused (`Stutter ignored - tempo too slow`).
- **Shift:** a stutter button that is pressed while Shift (B8) is held is ignored until released, so Shift + B16 long press (version print) does not stutter. 3/8 uses the bar number (`ClockTracker::BarAtBlockStart`) as well as the position in the bar.

## Pitch fx on the beat repeat (0.6.0, B6; `src/engine/pitch_fx.h` + `beat_repeat.h`)
- **B6 held** = effect on. On and off both wait for the next 1/16 line (same grid and offset as the stutter); a tap shorter than the wait still gives one line. B6 pressed while Shift is held, while stopped, or in slice mode does nothing until released.
- **Stepping:** the beat repeat counts how many times the held slice has wrapped round (`LoopCount()`); the pitch stage lowers the sound by that many semitones (`kPitchStepSemitones` = 1 each, at most `kPitchMaxSteps` = 12). The 1st play of a slice is normal. The count restarts at 0 for a new slice (a fresh capture, a flip to a stored slice) and when the stutter is released.
- **Armed:** B6 on with no stutter held does nothing to the live signal; the next stutter then steps down from its first repeat.
- **How (0.6.1):** `PitchFx` is a delay-line pitch shifter that keeps the length of the sound, so the repeat stays on the grid. A read head sits behind the newest sample and falls further behind each sample (ratio 2^(-steps/12)), which lowers the pitch. **Every repeat starts a new head only 1 sample behind**, with a 5 ms crossfade that ends on the repeat's beat (it starts when the repeat's own loop crossfade starts: `BeatRepeat::JumpStartIndex()`), so the hit at the start of each repeat is on the beat at full level (no flam) and the pitch drop develops during the repeat. The first shifted repeat starts from the dry signal the same way. A head that falls more than `kPitchMaxDelayFrames` (10 ms, 0.6.2) behind hops forward `kPitchHopFrames` (all of it, back to about 1 frame behind) with a 5 ms crossfade, so even a slow repeat (1/4, 1 bar) held for many repeats is never more than about 12 ms late (10 ms + drift during the hop crossfade). Costs: a splice every 10 ms / r (r = how fast the head falls behind: 0.056 at -1 semitone, 0.5 at -12; so every 180 ms down to every 20 ms) = slightly grainy tone in slow, deeply lowered repeats; the audio skipped by a hop is lost (about half of it at -12). A pure tone is detuned by up to about 25 Hz at the deepest drop; music is not. (0.6.1 allowed 32 ms of lag: slow repeats drifted out of time.) Bigger `kPitchMaxDelayFrames` / `kPitchHopFrames` = smoother tone, later slow repeats. (0.6.0 had a fixed 43 ms window and a constant ~20 ms delay: flammy.)
- **In slice mode (0.7.2):** B6 does not use this stage. While B6 is on, a slice button plays its eighth **3 semitones up as a forward-then-reverse ping-pong** (first 16th forward at 2^(3/12) speed, second 16th backwards over the same piece, repeating every eighth), triggered on 1/8 lines as without B6. See "Slice loop buffer" below. (0.7.0/0.7.1 retriggered on 1/16 lines with a pitch drop; replaced.)
- **Order in the signal chain:** beat repeat -> pitch fx -> (slice mix) -> Quantized loop buffer, so the pitched repeats are recorded into the loop.
- **Later:** more FX on the beat repeat and on the slice mode (same stage can be reused there).

## Slice loop buffer (0.5.0, changed in 0.5.1 and 0.5.2; B7; `src/engine/slice_loop_buffer.h`)
- **Own recording:** it always records the live input (after input gain) into its own 2-bar ring (+1.4 MB), separate from the beat repeat's history. B7 stops that recording at the line, so the frozen window never changes until B7 is pressed again; the beat repeat keeps a normal history the whole time.
- **B7 on (0.5.2):** at the next **bar line** (shifted by the input offset) the recording freezes: the window is the one whole bar (8 eighths, `N = 8 * round(bar/8)` samples, `kSliceBanks` = 1) that ends on that line, so its first sample is beat 1 and B9 = beat 1. If less than a whole bar has been recorded (after power-up/Start, or since the last unfreeze) it does not switch: B7 stays pending and is tried again at each following bar line (LED 7 flashes fast). It can be cancelled by pressing B7 again. (Before 0.5.2: next 1/8 line, 2-bar window, so B9 was not the downbeat and the part before an unfreeze could be silent.) The window plays as a loop, in time with the clock, 5 ms crossfade ending on the line; the output at the line is the audio from exactly 2 bars earlier. Every window (8 lines after the entry line) the loop is restarted on the line with a crossfade, so DAW/Seed clock drift never builds up. While it is on, you hear this instead of the Quantized loop buffer / live input (those keep running underneath).
- **Slices:** the window is 8 slices of one eighth: B9 = beat 1 ... B16 = the last eighth. (With `kSliceBanks` = 2 it would be 16 slices in 2 banks, older/newer bar, Shift + B7 to switch.)
- **Bar line = reset (0.8.0):** at every bar line (the end of the Slice loop buffer, `phase == 0`) the output restarts from beat 1, in time, so nothing runs across the bar: a slice that is **held** is **choked** (taken off the stack; only a new press puts a button back, so it stays choked until released and pressed again), and a rotated loop (after releasing a slice) goes back to the plain loop. A button pressed before the bar line lands on it and plays from there (not choked); a release exactly on the bar line goes to the plain loop. `Slice loop` is logged. (Before 0.8.0 a held slice kept repeating across the bar line and a rotated loop kept its rotation.)
- **Slice button:** the press waits for the next 1/8 line, then that eighth repeats while the button is held (repeat, like the beat repeat). The newest button wins; releasing it falls back to an older held button (starts at the line). Releasing the last button (0.5.1) starts the whole window looping from the slice AFTER the one that was playing (0.5.3; e.g. you played slice 3 -> 4, 5 ... 8, then 1, 2 ...; starting on slice 3 again made it sound twice), at the next 1/8 line, and it keeps looping like that until a new slice is pressed or B7 ends slice mode. It is re-aligned to the clock every window (8 lines after the release line), keeping the rotation. (Before 0.5.1 it went back to the unrotated loop.) A tap shorter than the wait still plays one eighth. A button held down when slice mode starts does nothing until released. A bank switch while a slice is held changes the next press, not the slice that is playing.
- **B7 off:** at the next 1/8 line back to the normal signal; the 2-bar recording resumes after the crossfade. If you press B7 again soon, it waits until a whole bar has been recorded since the unfreeze (0.5.2), then switches at that bar line.
- **Reverse switch (0.8.1):** Shift + B6 turns the backwards half on/off (default OFF). Off = the whole eighth plays forward at the raised pitch (read faster, like tape: a pitched-up eighth uses more than one eighth of the recording, a pitched-down one less), restarted every 1/8 line with a 5 ms crossfade at the repeat; on = the ping-pong below. Changing it while a slice is held restarts the slice on the next 1/8 line.
- **B6 pitch ping-pong (0.7.2):** while B6 is on (held; switches on/off on a 1/8 line, a tap gives one eighth), a slice button plays its eighth **3 semitones up by default** (`kSliceFxSemitones`; K4 sets -12..+12): the position in the slice moves along a triangle, forward through the first 16th (read at 2^(3/12) speed, like tape) then backwards over the same piece for the second 16th, every eighth. It is continuous everywhere, so no seam/crossfade/stutter, and each repeat is exactly one eighth (reading faster and cutting, or looping, would not be). B6 on/off while a slice is held, or a change of the **pitch knob (K4, 0.8.0, -12..+12 semitones, default +3, CC 24)**, restarts it on the next 1/8 line (new mode / new pitch). B6 with no slice held does nothing audible; the loop and the release behaviour (carries on from the next slice) are unchanged and at normal pitch. B6 held before slice mode starts, with Shift, or while stopped: ignored. (0.7.0/0.7.1: 1/16 retrigger with a pitch drop; replaced.)
- **Beat repeat:** while slice mode is on, buttons 9-16 do not stutter (the beat repeat sees no buttons and releases at its next line); the Shift + B9-B16 latch does nothing. Other things (B1/B2, knobs) work as usual.
- **Recording into the Quantized loop buffer:** the slice mode output is what the Quantized loop buffer records (base signal * `base_w` + slice mode output, in `main.cpp`), exactly like the beat repeat output. It is also mixed over the Quantized loop buffer output, so slices are heard over a loop that is playing.
- **Transport Stop / clock lost:** slice mode ends at once (B7 does nothing and logs `B7 ignored - not running` while stopped).
- **Limits:** the frozen window has a fixed length in samples; if the tempo changes while frozen, the slices stay the length they had (the loop restarts on every 2-bar line). The window must fit the ring (`Slice mode ignored - too slow` otherwise; with a 1-bar window this is far from a limit); an eighth under 10 ms is refused (`too short`).
- **Log:** `Slice mode ON`, `Slice mode OFF`, `Slice bank 2` (only with 2 banks), `Slice 3` (repeating slice 3), `Slice loop` (last button released: the 2 bars loop from that slice), `B7 ignored - not running`, `Bank ignored - no slice mode`.

## Known limits (0.3.0)
- **Loop-start timing is stamped per audio block**, so each cycle start can be up to one block (about 1 ms) early or late relative to the true clock. At a perfectly steady tempo it is consistent; with tempo wobble or clock drift the loop can shift by up to about 1 ms between cycles. A fix would be sub-block timestamps from a hardware timer (idea for a later 0.3.x).
- **Timing of slice boundaries has the same up-to-1 ms block jitter** as loop starts (the clock position is estimated per block).
- **Pure tones can cancel at a loop seam** (two copies of a sine in opposite phase cancel while crossfading). Real music does not do this; the tests deliberately use a worst-case sine. Mixing two unrelated signals dips about 3 dB for 5 ms.
- Loop-seam alignment relies on the DAW audio and MIDI clock being in time; the input offset in 0.4.0 compensates.

## Testing
The engine code (`src/engine/`) is plain C++ with no libDaisy, so `make -C test` builds and runs it on the Mac with fake clock ticks and test signals. `test/test_beat.cpp` runs the real clock tracker and beat repeat at 138.89 BPM (a tick is exactly 864 samples = 18 blocks, so every position is a whole number of samples) with hash-noise input and compares the output with the input sample for sample: every division, a 6-bar hold with a 2-bar ring, +/-20 ms offset, a snare that arrives late, flipping between held buttons (original stored slice, in time, at every position in the slice; no re-capture; forgotten after release), a 1-bar slice recalled after the history was overwritten, 3/8 over bar lines, release, stopped transport, too-short and too-slow slices, clicks at every transition, and a stutter performed into the loop. `test/test_quantized_loop_buffer.cpp` runs the real clock tracker and the loop engine with a fake DAW (exact tempo, +/-100 and 300 ppm clock drift) and checks timing, level and clicks at record start/end, loop wrap, B2 (source switch), clear, re-record, Stop/Start, refusals and overflow; it also runs under address/undefined-behaviour checking. Add a test here for every engine feature, including numerical click checks at every crossfade, before it goes into the firmware.

## Stages
| Version | Content | Status |
|---|---|---|
| 0.2.0 | MIDI clock + transport, bar/beat/eighth counters, tempo estimate, LEDs, logging, host test | built, not flashed |
| 0.3.0 | Loop engine: ARM / REC / PLAYBACK, source switch, clear, re-arm, click tests | built, not flashed |
| 0.4.0 | Beat repeat on the 2-bar history, quantised and held, input offset; stutter performed into the loop (needs no extra code: the loop records the beat repeat output) | built, not flashed |
| 0.4.1 | Beat repeat: 3/8 division (B9-B16), flip between held buttons using stored slices, version print on Shift + B16 | built, not flashed |
| 0.5.0 | Slice mode on B7: Slice loop buffer (frozen 2 bars, 16 eighth slices in 2 banks, drum-kit style); loop_engine renamed Quantized loop buffer | built, not flashed |
| 0.5.1 | Slice release loops the whole window from the triggered slice | built, not flashed |
| 0.5.2 | Slice mode: 1 bar captured on a bar line (B9 = beat 1); waits for a full bar | built, not flashed |
| 0.6.0 | B6 pitch-down fx on the beat repeat (1 semitone lower per repeat) | built, not flashed |
| 0.7.2 | B6 in slice mode: +3 semitone forward/reverse ping-pong on 1/8 lines; CPU meter on serial | built, not flashed |
| 0.8.0 | Slice mode: bar line = reset (held slice choked, rotated loop back to beat 1), pitch knob K4 | built, not flashed |
| 0.9.0 | Polish (apply the input offset to loop starts, ...) | planned |

## Assumptions made without asking (change if wrong)
- Loop lengths are 1 / 2 / 4 / 8 bars (the user's earlier list was 1, 4, 8, 16, 32 before the 8-bar limit was set).
- "No clock" and "clock but stopped" LED patterns as above.
- K2 also on CC 21 (CC 20 input gain, CC 22 output level).
- The old "LED follows button" behaviour was removed to make room for the bar/eighth display.
