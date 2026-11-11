# Looper design (current behaviour)

Audio looper + beat-repeat (layer 1 of 005-PunchFx), locked to MIDI clock from the DIN input. Stereo, 4/4 only, 48 kHz.
Controls, LEDs and log text: `docs/HARDWARE_MAP.md`. Signal chain: `docs/ARCHITECTURE.md`. Version history: `docs/CHANGELOG.md`.
Older, longer version of this file (with rewrite history): `docs/archive/DESIGN_LOOPER_FULL.md`.

## Signal flow
```
input -> gain -> beat repeat (live, or stutter from the 2-bar history) -> pitch fx (B6)
      -> Quantized loop buffer (records this; plays the loop or passes live)
      -> SoftClip -> output level (ceiling 0.9)
```
- Beat repeat only stutters the **input** history, never the loop playback. The loop records the beat-repeat output.
- Live input, stutter and loop playback share the same converter latency, so switching between them is in time (only a short crossfade). Round-trip latency not measured.
- Real timing risk is **audio vs clock** (DAW audio late against its MIDI clock): handled by the **input offset** (Shift + K3, +/-20 ms) applied to where beat-repeat slices are read, not as a delay on the live signal. It does not shift loop starts yet.

## Names
**Quantized loop buffer** = global looper on B1/B2 (`quantized_loop_buffer.h`). **Beat repeat** = stutter on B9-B16 (`beat_repeat.h`).

## Clock and transport
- 24 ticks per beat; 96 per bar; 8 eighths per bar. Messages come from libDaisy `StartReceiveRt`, are queued and stamped per audio block (up to ~1 ms late). Tempo is measured over one bar.
- **Start**: counters reset, next tick = bar 1 beat 1. **Stop**: loop held/stopped; Start restarts it. **Continue** and Song Position Pointer ignored. No ticks for 0.5 s = "clock lost".
- A recorded loop has a fixed length in samples and is re-aligned to the clock at each restart (no time-stretching).
- Known limit: positions are per-block, so loop starts and slice boundaries can be up to ~1 ms off. Fix idea: hardware-timer timestamps.

## Quantized loop buffer (B1 / B2)
- States: IDLE, ARMED, RECORDING, PLAYING, PLAYING+ARMED.
- **B1**: IDLE -> ARMED; ARMED -> cancel; PLAYING -> re-arm (old loop plays until the new cycle, then the new take is recorded over live); RECORDING -> ignored.
- **Arming** needs clock ticks (not necessarily Play). Refused with no clock or if the tempo is too slow for the chosen length.
- **Cycle start** = a bar that is a multiple of the chosen length counted from Start (1/2/4/8 bars from K2, latched at arming).
- **Loop length** = samples recorded between two cycle starts; playback restarts every cycle with a 5 ms crossfade (corrects clock drift). 20 ms recorded past the end is blended into the start (seam).
- **B2**: loop <-> live, 5 ms crossfade; loop keeps running silently. **Shift + B1**: clear (5 ms fade to live; aborts arm/recording).
- **Stop / clock lost**: recording aborted; playing loop held (live heard), restarts at the next bar 1; ARMED stays armed.
- Records after input gain, before soft clip and output level. Buffer overflow aborts with a log message.
- Pure tones can cancel at a seam (tests use a worst-case sine); real music does not.

## Beat repeat (B9-B16)
- Buttons = 1 bar, 1/2, 3/8, 1/4, 1/8, 1/16, 1/32, 1/64 of a bar (3/8 grid counts from bar 1 beat 1 and lines up every 3 bars). Held state is read from the hardware every block (not from events, because a long press swallows the release).
- **Quantised to 1/16** (`kQuantizeDen`): press and release both wait for the next 1/16 line (shifted by input offset). A press repeats the last slice-length of audio ending on that line. A tap shorter than the wait still gives one 1/16. About 115 ms max delay at 130 BPM.
- **Crossfades END on the line** (5 ms, using pre-roll audio) so each hit is at full level.
- **Stored slices**: each division copies its slice (256 frames per block) into its own slot. Several buttons: newest wins; a division already used in this hold flips back to its ORIGINAL stored slice, in time, without re-capturing; releasing everything forgets all slices.
- **Latch** (Shift + B9-B16): toggles a division on/off; one at a time; cleared on Stop; a held press overrides it.
- Refused with log: stopped (`not running`), slice under 10 ms (`too short`), slice does not fit its slot (`tempo too slow`). Stop returns to live at once. A stutter button pressed while Shift is held is ignored until released.
- Heard only when the output is live (B2) or while recording; what is recorded into the loop is the stutter output.

## Pitch fx on the beat repeat (B6; `pitch_fx.h`)
- B6 held = on; on/off wait for the next 1/16 line. Ignored with Shift and while stopped.
- Each repeat of the held slice is 1 semitone lower (max 12, `kPitchStepSemitones`/`kPitchMaxSteps`); count restarts on a new slice, a flip, or release. B6 with no stutter held is "armed".
- `PitchFx` is an exact delay-line shifter that keeps the slice length. Each repeat starts a new head 1 sample behind (5 ms crossfade ending on the beat, `BeatRepeat::JumpStartIndex()`), so attacks are on the beat. A head more than 10 ms behind hops forward (`kPitchMaxDelayFrames`/`kPitchHopFrames` = 480): slow deep drops are slightly grainy. Raise both for smoother but later repeats. (DaisySP `PitchShifter` was rejected: inexact downwards.)

## Memory (Seed has 64 MB SDRAM) and minimum tempo
Stereo float, ~384 KB/s. Sizes come from `kMaxLoopBars` (8), `kMinBpm` (**130**, fixed, no control) and `kHistoryBars` (2) in `looper_config.h`: loop 5.7 MB, history 1.4 MB, stutter slots 1.8 MB = **8.9 MB** (13% of 64 MB). Memory is proportional to 1 / kMinBpm.

| Min BPM | Loop | History | Slots | Total (MB) |
|---|---|---|---|---|
| 100 | 7.4 | 1.9 | 2.3 | 11.6 |
| 120 | 6.2 | 1.6 | 2.0 | 9.7 |
| **130 (now)** | 5.7 | 1.4 | 1.8 | 8.9 |
| 160 | 4.6 | 1.2 | 1.5 | 7.3 |

At 130 BPM a bar = 1.85 s. A loop of N bars is accepted only if it fits (8 bars down to 130 BPM, 4 down to 65, 2 down to 32). Below ~130 BPM the long stutter divisions are refused (`Stutter ignored - tempo too slow`). To change: edit `kMinBpm`, rebuild, check the SDRAM line of `make`, and update tests (`test_clock.cpp` `TestConfig`, `test_quantized_loop_buffer.cpp` `TestRefusals` at 100/150 BPM, `test_beat.cpp` runs at 138.89 BPM so keep it at or above `kMinBpm`).

## Testing
`make -C test` builds the plain-C++ engine (`src/engine/`) on the Mac with fake clock ticks and test signals: `test_beat.cpp` (beat repeat, sample-exact at 138.89 BPM), `test_quantized_loop_buffer.cpp` (loop with a fake DAW, drift, clicks, also run with address/UB checking), `test_pitch.cpp`, `test_clock.cpp`. Add a test for every engine feature, including numerical click checks at every crossfade, before it goes into the firmware.

## Assumptions (change if wrong)
- Loop lengths 1/2/4/8 bars. K2 also on CC 21 (CC 20 input gain, 22 output level, 23 offset).
- "No clock" (LEDs 9 and 16 blink) and "clock but stopped" (bottom row dim) patterns.

## Ideas / not done
Apply the input offset to loop starts; hardware-timer clock timestamps; 16-bar loops; USB MIDI clock; Continue / Song Position Pointer; overdub; save/restore. Punch-in fx (layer 2) and the reverse fx: see `docs/IDEAS.md` and `CLAUDE.md`.
