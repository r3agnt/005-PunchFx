# 004-Looper — context for Claude

Daisy Seed (STM32H750, Seed 3) on the Electrosmith **Desktop DevKit**. Goal: an audio looper.
This is a prototype for smaller ideas that will feed a larger project. The user has **no C++ background** — keep
code simple, heavily commented in plain English, and explain changes in plain terms.

## Start of every session
1. Read this file and the docs imported below.
2. Run `make clean && make` to confirm a clean build (zero warnings), and `make -C test` to run the Mac-side engine tests.
3. Run `git log --oneline | head` and `git status` to see where things stand.
4. Read the Roadmap below, then ask what I want to do next (don't start editing before the 'before making changes' steps below).

@docs/DESIGN_LOOPER.md
@docs/HARDWARE_MAP.md
@docs/ARCHITECTURE.md
@docs/CHANGELOG.md
@docs/HARDWARE_TEST.md
@README.md

## How to work with me
- I have **no C++ background**. Explain in plain English, comment code simply, avoid jargon or explain it.
- **Before making changes:** list every change you plan to make, ask questions about anything ambiguous (give options and let me add my own comments), and wait for my answers. Don't guess; if my spec has a conflict or gap (e.g. two controls on one knob), point it out.
- **Commit only when I ask.** Use a clear message; if the session gives attribution lines, end the message with them, otherwise omit them.
- **With every change:** bump `FIRMWARE_VERSION` in `src/version.h`, add a line to `docs/CHANGELOG.md`, update `docs/HARDWARE_MAP.md` / this file if controls or behaviour changed, and finish with a clean `make` (no warnings).
- You cannot flash or hear the hardware: say clearly what is built-but-untested and tell me what to check.

## Current state (v0.8.1)
Forked from 003-ChannelStrip v0.3.1 (EQ and saturator removed). The full looper design and the staged plan are in `docs/DESIGN_LOOPER.md` (read it).
v0.2.0 = MIDI clock + transport follower, bar/beat/eighth counters, tempo estimate, LEDs, logging. v0.3.0 = loop engine: B1 arm/record, B2 loop <-> live, Shift+B1 clear (rules in `docs/DESIGN_LOOPER.md`). v0.4.0 = beat repeat (held, quantised to the slice grid, input offset on Shift+K3; the loop records the stutter output). v0.4.1 = beat repeat on buttons 9-16 (1 bar, 1/2, 3/8, 1/4, 1/8, 1/16, 1/32, 1/64); with several buttons held each division keeps its own stored slice and you flip between them (no re-capture until all are released); version print moved to Shift + B16 long press. v0.4.2 = minimum tempo 130 BPM (buffers 8.9 MB; fixed constant, no control; see "Minimum tempo and memory" in `docs/DESIGN_LOOPER.md` before changing it). v0.4.3 = Shift + B9-B16 latches a beat repeat division on/off (one at a time; logic in `main.cpp`, global `stutter_latch`; cleared on transport stop; held stutter LED dim = `kLedCycleDim`, latched brighter). v0.4.4 = slot copy chunk 2048 -> 256 frames per block (confirmed working on hardware). v0.4.5 = beat repeat presses and releases are quantised to a 1/16-bar grid (`kQuantizeDen` in `looper_config.h`): a press waits for the next 1/16 line then repeats the last slice-length ending on that line; a release also waits for the line; a short tap still gives one 1/16 (engine in `beat_repeat.h`: `NoteButtons` + `ApplyLine`; flashed, user says it feels tighter). v0.5.0 = **slice mode on B7** (`src/engine/slice_loop_buffer.h`): freezes the last 2 bars of the input at the next 1/8 line and plays them as a loop (the **Slice loop buffer**); buttons 9-16 play its 8 eighth-note slices (repeat while held), Shift + B7 switches bank 1/2 (older/newer bar), B7 again returns; everything quantised to 1/8; the slice output is recorded into / heard over the Quantized loop buffer (built, Mac tests pass, not yet heard). Also renamed `loop_engine` -> `quantized_loop_buffer`.
v0.5.1 = slice button release now loops the whole 2 bars starting at the triggered slice (rotated loop) until a new slice is pressed (built, Mac tests pass, not yet heard).
v0.5.2 = slice mode captures ONE bar on a BAR line (`kSliceBanks = 1`; B9 = beat 1); B7 waits until a whole bar is recorded (no silence); Shift + B7 is ignored (built, Mac tests pass, not yet heard).
v0.5.3 = after the last slice button is released the loop carries on from the NEXT slice (was the same slice, which played it twice) (built, Mac tests pass, not yet heard).
v0.6.0 = **B6 pitch-down fx on the beat repeat** (`src/engine/pitch_fx.h`, `PitchFx`; `BeatRepeat` counts repeats and does the 1/16-quantised B6 on/off; each repeat 1 semitone lower, max 12; own exact delay-line shifter because DaisySP `PitchShifter` is inexact downwards) (built, Mac tests pass, not yet heard; CPU untested). v0.6.1 = tightened: every repeat starts a new pitch head 1 sample behind (no 20 ms flam), `BeatRepeat::RepeatIndex()/JumpStartIndex()` feed `PitchFx::Process(..., jump_at)`. v0.6.2 = slow repeats stay in time: pitch head lag limit 32 -> 10 ms (`kPitchMaxDelayFrames`/`kPitchHopFrames` = 480; trade-off: grainier in slow deep-drop repeats). v0.8.1 = **Shift + B6 toggles the reverse half** of the slice mode B6 effect (default OFF = whole eighth forward at the raised pitch; ON = forward/reverse ping-pong); `SliceBlockInfo::fx_reverse`, static `slice_reverse` in `main.cpp` (built, Mac tests pass, not yet heard). v0.8.0 = slice mode **bar line = reset** (a held slice is choked at the bar line and stays choked until re-pressed; a rotated loop goes back to the plain loop from beat 1) and **K4 = pitch of the B6 ping-pong** (-12..+12 st, default +3, CC 24, `Params::slice_pitch`, `looper::SlicePitchFromControl`, `SliceBlockInfo::fx_semitones`) (built, Mac tests pass, not yet heard). v0.7.2 = **B6 in slice mode = +3 semitones as a forward-then-reverse ping-pong** (first 16th forward at 2^(3/12) speed, second 16th backwards, every eighth; triggered on 1/8 lines; tape-style read inside `slice_loop_buffer.h`, `Voice::pp`; `kSliceFxSemitones`). 0.7.0/0.7.1 (1/16 retrigger + pitch drop) were replaced. Also a **CPU meter**: `CPU avg x% max y% (30 s)` on serial every 30 s (libDaisy `CpuLoadMeter`). Built, Mac tests pass, not yet heard.
Firmware version prints on serial at boot and on Shift + button 16 long-press (edit `src/version.h` each flash).
Signal chain: input gain -> beat repeat -> looper (loop or live) -> SoftClip -> output level, ceiling 0.9. Master description in `docs/ARCHITECTURE.md` (link to it, don't duplicate it).
Controls: tables in `src/params.h` (`kPotMap` knobs, `kCcMap` MIDI CC). Button 8 = SHIFT. Knobs use soft takeover (`src/soft_takeover.h`). K2 = loop length (CC 21), K4 = slice pitch (CC 24), Shift+K1 input gain (CC 20), Shift+K2 output level (CC 22), Shift+K3 input offset (CC 23). See `docs/HARDWARE_MAP.md`.
**USB MIDI is intentionally disabled** (devkit USB port is used for serial logging); DIN MIDI works.

## Golden rule: no guessing
Only use libDaisy / DaisySP APIs that exist on this machine. **Read the header before using
any function** and cite it. Locations (relative to this folder):
- libDaisy: `../../DaisyExamples/libDaisy` (devkit API: `src/desktop_devkit.h`, `.cpp`; UI: `src/ui/`)
- DaisySP:  `../../DaisyExamples/DaisySP/Source` (Filters/svf.h, soap.h, onepole.h; Effects/overdrive.h, wavefolder.h; Dynamics/limiter.h; Utility/dsp.h has SoftClip, SoftLimit, soft_saturate; Utility/delayline.h)
If something is not found there, say so instead of inventing it.

## Files
| File | Role |
|---|---|
| `src/main.cpp` | Hardware, audio callback, low-priority callback, serial log loop |
| `src/main_page.h` | UI page: button/pot events, LEDs. "ASSIGN CONTROLS HERE" markers |
| `src/params.h` | `Params` (all settings), knob map, MIDI CC map |
| `src/soft_takeover.h` | Knob pick-up logic |
| `src/param_info.h` | Parameter names + real-unit display for the serial log (add a row per new parameter) |
| `src/version.h` | `FIRMWARE_VERSION` string |
| `src/engine/clock_tracker.h` | MIDI clock/transport follower + the queue that carries messages to the audio callback. Plain C++ (no libDaisy), tested on the Mac |
| `src/engine/beat_repeat.h` | Beat repeat: history ring, one stored slice per division, slice grid, button logic, crossfades (plain C++, tested on the Mac) |
| `src/engine/quantized_loop_buffer.h` | The **Quantized loop buffer**: the global looper state machine (B1/B2), loop reader, crossfades (plain C++, tested on the Mac) |
| `src/engine/pitch_fx.h` | `PitchFx`: exact pitch-down by whole semitones that keeps the length (B6 fx on the beat repeat; plain C++, tested on the Mac) |
| `src/engine/slice_loop_buffer.h` | The **Slice loop buffer**: slice mode (B7), frozen 2-bar loop played as 16 eighth slices (plain C++, tested on the Mac) |
| `src/engine/spsc_queue.h` | Lock-free queue used for MIDI messages and button commands -> audio callback |
| `src/engine/looper_config.h` | Fixed numbers: 4/4, max bars, min BPM, loop-length options, buffer sizes |
| `test/` | Mac-side tests: `make -C test` |
| `docs/DESIGN_LOOPER.md` | The agreed looper design, controls, LEDs, stages, assumptions |
| `src/modules/gain_stage.h` | Input gain, soft clip, output level, ceiling (also the example module) |
| `src/userinterface.h` | `Ui` class: generates button/pot events (template code, rarely edit) |
| `src/ui_glue.h` | LED flush/clear + pot reader helpers (template code) |
| `docs/HARDWARE_MAP.md` | What each pot/button/LED is |
| `docs/ARCHITECTURE.md` | The three execution contexts and the master signal chain description |
| `docs/CHANGELOG.md` | One line per version |
| `docs/HARDWARE_TEST.md` | Checklist of what to verify on the real hardware |

## Build / flash
- Build: `make` (clean: `make clean && make`). Output `build/004-Looper.bin`. Must build with zero warnings.
- Flash: press RESET then BOOT on the Seed, then `make program-dfu` (external-USB bootloader; see README).
- Serial: connect a serial monitor to the devkit's USB-C port.

## Conventions
- Header-only modules in `src/modules/` (e.g. `gain_stage.h`), included by `main.cpp`; `CPP_SOURCES` stays `src/main.cpp`.
- Audio callback: floats only, **no printing, no heap allocation, no blocking**. To log from it use `msg_fifo.PushBack("text")`.
- Pots give 0.0–1.0. Convert to real units (Hz, dB, seconds) inside the module, not in the UI.
- Assign controls by editing the tables in `params.h`; the audio callback reads `params`.
- Log lines use the format `[Shift + ]K<n> <Param name> - <value>` and `B<n> ...`; use `Log::PrintLine("%s", str)` for pre-formatted text (it can contain `%`). Pot/button numbers are 0-based in code, 1-based in log output and docs.

## Engine code and tests
Anything with logic worth testing (clock, loop state machine, crossfades, stutter slicing) goes in `src/engine/` as **plain header-only C++ with no libDaisy includes**, so `test/` can build it with the Mac's own compiler. Add a test for every engine feature (including numerical click checks at crossfades) and run `make -C test` before building the firmware. Hardware glue (UART callback, LEDs, logging) stays in `main.cpp` / `main_page.h`.

## Adding a module
1. Add its settings to `Params` in `params.h`.
2. Create `src/modules/<name>.h` with a small class: `Init(sample_rate)`, parameter setters, `Process(float in)`.
3. In `main.cpp`: include it, create a global instance, call `Init(hw.seed.AudioSampleRate())` in `main()` before `StartAudio`, call `Process` in `AudioCallback`.
4. Assign controls in the `params.h` tables. Build clean, then update the docs.

## Design decisions (and why)
Inherited from the template (kept):
- **Output ceiling 0.9 (about -0.9 dBFS)** for headroom with external gear; SoftClip then hard clamp guarantees nothing exceeds it. Output level (Shift+K2) only turns it down.
- **DIN MIDI only; USB MIDI deliberately off:** the devkit's USB-C port carries the serial log (`Logger<LOGGER_EXTERNAL>`).
- **Shift = button 8, read live in `Draw()`** (not from button events) because a long press suppresses the release event and would leave shift stuck.
- **Soft takeover on every knob layer** (a knob also re-arms if MIDI changes the value), **except K2 normal (loop length)**: it acts at once, set by `normal_takeover = false` in `kPotMap`.
- **Knob/CC assignments are tables in `params.h`**, so reassigning needs no logic changes.

Looper-specific (details in `docs/DESIGN_LOOPER.md`):
- **Beat repeat crossfades END on the 1/16 line** (look-ahead of 5 ms, pre-roll audio) so a repeated hit keeps its full-level attack; held buttons are read directly from the hardware each block.
- **Engine is plain C++, tested on the Mac first**; the firmware only adds hardware glue.
- **MIDI clock arrives through `StartReceiveRt` into a queue**, stamped per audio block (about 1 ms); tempo is measured over a bar so the jitter averages out.
- **Stereo float buffers in SDRAM**, sized from `kMaxLoopBars` / `kMinBpm` (**130 BPM**, because the user only plays faster styles; no runtime control, since the buffers are fixed arrays) in `looper_config.h`. Memory table and how to change it: `docs/DESIGN_LOOPER.md`, "Minimum tempo and memory".
- **No delay on the live input:** all paths share the converter latency; an input *offset* on the slice read position (0.4.0) handles audio-vs-clock misalignment.
- **Beat repeat stutters the input history only; the loop records the beat-repeat output.**

## Names (use these)
**Quantized loop buffer** = the global B1/B2 looper (`quantized_loop`, `QuantizedLoopBuffer`). **Slice loop buffer** = the frozen 2 bars of slice mode on B7 (`slice_loop`, `SliceLoopBuffer`). **Beat repeat** = the stutter on buttons 9-16. In slice mode buttons 9-16 play slices instead.

## Known limits / untested
- The loop engine (0.3.x) has been flashed and works. v0.4.0-0.4.4 (beat repeat, tempo limit) is flashed and working; v0.4.5 (1/16 quantised presses/releases) is flashed; v0.5.0 (slice mode, B7) is built and its Mac tests pass, but it is **not yet flashed or heard**. Round-trip latency has not been measured. Use `docs/HARDWARE_TEST.md`, and record results there.
- Knob and MIDI CC write the same value; last one moved wins. Values reset to defaults at power-up (nothing saved).
- The Makefile depends on libDaisy/DaisySP at `../../DaisyExamples/`; `USE_FATFS = 1` is left on though SD is unused.

## Roadmap (ideas, not committed to)
- 0.5.0 polish; apply the input offset to loop recording starts too (see `docs/DESIGN_LOOPER.md`).
- Sub-block (hardware timer) timestamps for MIDI clock, to remove the up-to-1 ms loop-start jitter.
- Later: 16-bar loops, USB MIDI clock, Continue / Song Position Pointer, overdub, save/restore.


## Planned-fx paragraph (moved out of CLAUDE.md, 2026-11-11)
- **Planned fx** (layer 2: one fx per button on B1-B7 and B9-B16 = 15 slots, B8 = Shift; no banks; the looper buttons live on layer 1 only): reverb (+shimmer), delay, resonant hipass sweep (K1 freq, K2 res, K3 LFO depth, K4 ramp LFO speed 2 bars..1/32, LFO locked to the bar or to the trigger), bit crusher, jet flanger, flanger, phaser, comb, resonant lowpass, distortion with prefilter, freq shifter, pitch up/down (up must be tight), **tremolo chop / tight gate** (B16), **reverse** (plays recent audio backwards from the quantised position, no pitch). Later: stacked inserts and snapshots.


## Design decision bullet (cut 2026-11-11)
- **Beat repeat stutters the input history only; the loop records the beat-repeat output.** Crossfades end on the quantise line. MIDI clock arrives via `StartReceiveRt` into a queue, stamped per audio block (~1 ms). No delay on the live input; an input offset on beat-repeat slice reads handles audio-vs-clock misalignment.


## Current state + plan sections before the 0.12.8 rewrite (2026-11-11)
## Current state (v0.12.1: reverb built, NOT yet flashed; 0.12.0 froze at boot, fixed; 0.11.0 and earlier tested OK)
Layers: 3-way toggle (up/middle/down = 1/2/3), 2-way toggle = edit/perf. **Layer 1** = MIDI clock/transport follower; Quantized loop buffer (B1 arm/record, B2 loop/live, Shift+B1 clear, K2 length 1/2/4/8 bars); beat repeat on B9-B16 (quantised to 1/16, latch on Shift+B9-B16); B6 pitch-down on the beat repeat. Slice mode removed (idea kept in `docs/IDEAS.md`). **Layer 2 (0.10.0)** = punch-in fx framework (`src/engine/fx/`: `fx_layer.h` buttons/latch/quantise/crossfade, `fx_common.h` shared stage, `fx_engine.h` interface, `fx_bitcrush.h`); B1 reverb (bus: `fx_reverb_bus.h`, ReverbSc, sends via Shift+K1) and B7 bit crusher exist, the other 13 slots log "not built yet". Settings are saved to flash (0.11.0).
Signal chain: input gain -> beat repeat -> pitch fx -> looper -> SoftClip -> output level, ceiling 0.9 (`docs/ARCHITECTURE.md`).
Controls: tables in `src/params.h` (`kPotMap`, `kCcMap`); B8 = SHIFT; soft takeover on knobs. Full list in `docs/HARDWARE_MAP.md`.
Firmware version prints on serial at boot and on Shift + B16 long press. **USB MIDI is disabled** (USB carries serial logging); DIN MIDI works.

## The plan: layers + punch-in fx (agreed 2026-11-11; details settle step by step)
- **Layers** chosen with the 3-way toggle `hw.tog_sw18`: up = layer 1 (looper + beat repeat), middle = layer 2 (punch-in fx), down = layer 3 (tbd). 2-way toggle `hw.tog_sw17` = edit / perf mode of the last-triggered fx.
- **Layer 2 sits AFTER the Quantized loop buffer** (post input and post loop; not recorded into the loop). Trigger = hold, Shift + button = latch (like the stutter). Triggers quantised.
- **One insert engine runs at a time** (last triggered, 5 ms crossfade between); two buses (reverb, delay) run only while a send is up or the tail rings. Shared stage on every fx: K5 out gain +-12 dB, K6 wet/dry, K7 hipass 2-pole no res, K8 lopass 2-pole no res; K1-K4 = per-fx params (edit mode). Shift+K1 sends the insert to the reverb bus, Shift+K2 to the delay bus.
- **Fx slots** (one per button on B1-B7 and B9-B16, B8 = Shift; no banks; looper buttons live on layer 1 only): see the button map below. Resonant hipass sweep: K1 freq, K2 res, K3 LFO depth, K4 ramp LFO speed 2 bars..1/32, LFO locked to the bar or to the trigger. Pitch up must be tight; reverse plays recent audio backwards from the quantised position (no pitch). Later: stacked inserts and snapshots.
- **Controls are per layer** (decided 2026-11-11): each layer has its own knob map, normal and Shift. Layer 1 keeps today's map (K2 loop length; Shift+K1 input gain, Shift+K2 output level, Shift+K3 input offset). Layer 2 = the fx controls above (its own Shift layer: sends on Shift+K1/K2). Soft takeover resets on every layer change. Input gain / output level are only reachable on layer 1 (MIDI CC 20/22 always work).
- **Layer 2 button map (agreed):** B1 reverb, B2 delay, B3 reverse, B4 pitch down, B5 pitch up, B6 freq shifter, B7 bit crusher, B9 res hipass sweep, B10 res lowpass, B11 distortion+prefilter, B12 flanger, B13 jet flanger, B14 phaser, B15 comb, B16 tremolo chop (tight gate).
- **Build order:** 0.9.0 fork+strip (done) -> 0.10.0 fx framework + shared stage + toggles + bit crusher (done) -> 0.11.0 flash save (done: Seed QSPI via `PersistentStorage`, written from the main loop, only if changed) -> 0.12.0 reverb bus (done, LGPL on) -> delay bus (send field already in `SlotSettings`) -> simple inserts -> modulation fx -> pitch / freq shifter (shimmer = reverb K4) -> stacking.
- **SDRAM objects** (`DSY_SDRAM_BSS`) must have NO constructor (no `= value` defaults on members, no ctor): it would run at power-up before SDRAM is on and FREEZE the Seed (0.12.0 bug). They are not zeroed: set every member in `Init()`. Check: `static_assert(std::is_trivially_default_constructible<...>)`. Fast RAM (DTCM, 128 KB) is 68% full; big buffers go to SDRAM.
- Layer = one header with `Init`, `Process(block)`, `GetStatus`; `main.cpp` only chains them. Fx engines go in `src/engine/fx/` as plain C++ with Mac tests.

