# 005-PunchFx — context for Claude

Daisy Seed (STM32H750, Seed 3) on the Electrosmith **Desktop DevKit**. Goal: looper + punch-in fx.
Forked from `../004-Looper` v0.8.1 (kept as the working reference; its slice mode was removed here). This is a prototype for smaller ideas that will feed a larger project. The user has **no C++ background** — keep
code simple, heavily commented in plain English, and explain changes in plain terms.

## Start of every session
1. Read this file (the two docs imported below load automatically).
2. Run `make clean && make` (zero warnings) and `make -C test` (Mac-side engine tests).
3. Run `git log --oneline | head` and `git status`.
4. **Size check:** run `wc -c CLAUDE.md docs/*.md`. If a file is over its limit (rule below), tell me and propose the cleanup before starting other work.
5. Ask what I want to do next. Don't edit before the 'before making changes' steps below.

@docs/ARCHITECTURE.md
@docs/HARDWARE_MAP.md

## Cleanup rule (keeps sessions cheap)
Size limits: `CLAUDE.md` + the two imports **25 KB total**; `CHANGELOG.md` **6 KB**; `HARDWARE_TEST.md` **5 KB**; `DESIGN_LOOPER.md` **16 KB**. Checked at the start of every session and after every version bump. Over a limit = propose a cleanup (list the cuts, wait for my OK):
- Changelog / test results: older entries move to `docs/archive/` (append to the `*_FULL.md` file), active file keeps one line per version and only open items.
- `DESIGN_LOOPER.md`: delete rewrite history and replaced behaviour; keep current rules only. `CLAUDE.md`: no per-version history.
- Archive = move, never delete. Doc-only commit, no version bump.
Also: once a feature is tested on hardware, fold its checklist into "Passed" in the same session.

## Read on demand (NOT loaded automatically: read only what the task needs)
| File | Read it when |
|---|---|
| `docs/DESIGN_LOOPER.md` | changing looper / beat repeat / pitch fx behaviour (agreed rules, memory table) |
| `docs/CHANGELOG.md` | adding a version line (one line per version only) |
| `docs/HARDWARE_TEST.md` | recording hardware test results |
| `docs/WORKFLOW.md` | planning how to split up work / where each doc update goes |
| `docs/archive/` | almost never: old full changelog, test checklists, old design text |

## How to work with me
- I have **no C++ background**. Explain in plain English, comment code simply, avoid jargon or explain it.
- **Before making changes:** list every change you plan to make, ask questions about anything ambiguous (give options and let me add my own comments), and wait for my answers. Don't guess; if my spec has a conflict or gap (e.g. two controls on one knob), point it out.
- **Commit only when I ask.** Clear message; if the session gives attribution lines, end the message with them, otherwise omit them.
- **With every code change:** bump `FIRMWARE_VERSION` in `src/version.h`, add ONE line to `docs/CHANGELOG.md`, update `docs/HARDWARE_MAP.md` (controls) and `docs/DESIGN_LOOPER.md` (behaviour) only if they changed, and finish with a clean `make` (no warnings). Doc-only changes need no version bump. Don't put per-version history in this file.
- You cannot flash or hear the hardware: say clearly what is built-but-untested and tell me what to check.
- Keep sessions short: one task per session, commit when it works (see `docs/WORKFLOW.md`).

## Current state (v0.10.1; 0.10.0 flashed, layers + bit crusher work)
Layers: 3-way toggle (up/middle/down = 1/2/3), 2-way toggle = edit/perf. **Layer 1** = MIDI clock/transport follower; Quantized loop buffer (B1 arm/record, B2 loop/live, Shift+B1 clear, K2 length 1/2/4/8 bars); beat repeat on B9-B16 (quantised to 1/16, latch on Shift+B9-B16); B6 pitch-down on the beat repeat. Slice mode removed (idea kept in `docs/IDEAS.md`). **Layer 2 (0.10.0)** = punch-in fx framework (`src/engine/fx/`: `fx_layer.h` buttons/latch/quantise/crossfade, `fx_common.h` shared stage, `fx_engine.h` interface, `fx_bitcrush.h`); only B7 bit crusher exists (pre lopass, noise, depth, rate), the other 14 slots log "not built yet". Settings not saved yet.
Signal chain: input gain -> beat repeat -> pitch fx -> looper -> SoftClip -> output level, ceiling 0.9 (`docs/ARCHITECTURE.md`).
Controls: tables in `src/params.h` (`kPotMap`, `kCcMap`); B8 = SHIFT; soft takeover on knobs. Full list in `docs/HARDWARE_MAP.md`.
Firmware version prints on serial at boot and on Shift + B16 long press. **USB MIDI is disabled** (USB carries serial logging); DIN MIDI works.

## The plan: layers + punch-in fx (agreed 2026-11-11; details settle step by step)
- **Layers** chosen with the 3-way toggle `hw.tog_sw18`: up = layer 1 (looper + beat repeat), middle = layer 2 (punch-in fx), down = layer 3 (tbd). 2-way toggle `hw.tog_sw17` = edit / perf mode of the last-triggered fx.
- **Layer 2 sits AFTER the Quantized loop buffer** (post input and post loop; not recorded into the loop). Trigger = hold, Shift + button = latch (like the stutter). Triggers quantised.
- **One insert engine runs at a time** (last triggered, 5 ms crossfade between); two buses (reverb, delay) run only while a send is up or the tail rings. Shared stage on every fx: K5 out gain +-8 dB, K6 wet/dry, K7 hipass 2-pole no res, K8 lopass 2-pole no res; K1-K4 = per-fx params (edit mode). Shift+K1 sends the insert to the reverb bus, Shift+K2 to the delay bus.
- **Planned fx** (layer 2: one fx per button on B1-B7 and B9-B16 = 15 slots, B8 = Shift; no banks; the looper buttons live on layer 1 only): reverb (+shimmer), delay, resonant hipass sweep (K1 freq, K2 res, K3 LFO depth, K4 ramp LFO speed 2 bars..1/32, LFO locked to the bar or to the trigger), bit crusher, jet flanger, flanger, phaser, comb, resonant lowpass, distortion with prefilter, freq shifter, pitch up/down (up must be tight), **tremolo chop / tight gate** (B16), **reverse** (plays recent audio backwards from the quantised position, no pitch). Later: stacked inserts and snapshots.
- **Controls are per layer** (decided 2026-11-11): each layer has its own knob map, normal and Shift. Layer 1 keeps today's map (K2 loop length; Shift+K1 input gain, Shift+K2 output level, Shift+K3 input offset). Layer 2 = the fx controls above (its own Shift layer: sends on Shift+K1/K2). Soft takeover resets on every layer change. Input gain / output level are only reachable on layer 1 (MIDI CC 20/22 always work).
- **Layer 2 button map (agreed):** B1 reverb, B2 delay, B3 reverse, B4 pitch down, B5 pitch up, B6 freq shifter, B7 bit crusher, B9 res hipass sweep, B10 res lowpass, B11 distortion+prefilter, B12 flanger, B13 jet flanger, B14 phaser, B15 comb, B16 tremolo chop (tight gate).
- **Build order:** 0.9.0 fork+strip (done) -> 0.10.0 fx framework + shared stage + toggles + bit crusher (done) -> flash save of all fx on edit->perf (planned: Seed QSPI flash via `PersistentStorage`, only if changed, written from the main loop) -> simple inserts -> modulation fx -> buses (enable `USE_DAISYSP_LGPL` for ReverbSc) -> pitch / freq shifter -> stacking.
- Layer = one header with `Init`, `Process(block)`, `GetStatus`; `main.cpp` only chains them. Fx engines go in `src/engine/fx/` as plain C++ with Mac tests.

## Golden rule: no guessing
Only use libDaisy / DaisySP APIs that exist on this machine. **Read the header before using any function** and cite it. Locations (relative to this folder):
- libDaisy: `../../DaisyExamples/libDaisy` (devkit API: `src/desktop_devkit.h`, `.cpp`; UI: `src/ui/`)
- DaisySP:  `../../DaisyExamples/DaisySP/Source` (Filters, Effects, Dynamics, Utility/dsp.h, Utility/delayline.h)
If something is not found there, say so instead of inventing it.

## Files
| File | Role |
|---|---|
| `src/main.cpp` | Hardware, audio callback, low-priority callback, serial log loop |
| `src/main_page.h` | UI page: button/pot events, LEDs |
| `src/params.h` | `Params` (all settings), knob map, MIDI CC map |
| `src/soft_takeover.h`, `src/param_info.h`, `src/version.h` | Knob pick-up; parameter names/units for the log (add a row per new parameter); `FIRMWARE_VERSION` |
| `src/engine/clock_tracker.h` | MIDI clock/transport follower + queue to the audio callback |
| `src/engine/beat_repeat.h` | Beat repeat: history ring, stored slices, 1/16 quantising, crossfades |
| `src/engine/quantized_loop_buffer.h` | The B1/B2 looper state machine, loop reader, crossfades |
| `src/engine/pitch_fx.h` | `PitchFx`: exact pitch-down by whole semitones, keeps length |
| `src/engine/fx/` | Layer 2: `fx_layer.h`, `fx_common.h` (HP/LP/gain/wet), `fx_engine.h`, `fx_bitcrush.h` |
| `src/engine/spsc_queue.h`, `looper_config.h` | Lock-free queue; fixed numbers (4/4, bars, min BPM, buffer sizes, pitch limits) |
| `src/modules/gain_stage.h` | Input gain, soft clip, output level, ceiling |
| `src/userinterface.h`, `src/ui_glue.h` | Template UI event/LED code (rarely edit) |
| `test/` | Mac-side tests: `make -C test` |

## Build / flash
- Build: `make` (clean: `make clean && make`). Output `build/004-Looper.bin`. Zero warnings.
- Flash: press RESET then BOOT on the Seed, then `make program-dfu` (external-USB bootloader; see README).
- Serial: connect a serial monitor to the devkit's USB-C port.

## Conventions
- Header-only modules in `src/modules/`, included by `main.cpp`; `CPP_SOURCES` stays `src/main.cpp`.
- Audio callback: floats only, **no printing, no heap allocation, no blocking**. To log from it use `msg_fifo.PushBack("text")`.
- Pots give 0.0–1.0. Convert to real units inside the module, not in the UI.
- Assign controls by editing the tables in `params.h`; the audio callback reads `params`.
- Log format `[Shift + ]K<n> <Param name> - <value>` and `B<n> ...`; `Log::PrintLine("%s", str)` for pre-formatted text. Pot/button numbers are 0-based in code, 1-based in logs and docs.
- Engine logic worth testing goes in `src/engine/` as **plain header-only C++ with no libDaisy includes**, with a Mac test (incl. numerical click checks at crossfades); run `make -C test` before building the firmware. Hardware glue stays in `main.cpp` / `main_page.h`.
- Adding a module: settings in `Params`; `src/modules/<name>.h` with `Init(sample_rate)`, setters, `Process`; include + global + `Init` before `StartAudio` + `Process` in `AudioCallback`; assign controls in the `params.h` tables.

## Design decisions (and why)
- **Output ceiling 0.9** for headroom; SoftClip then hard clamp. Output level only turns it down.
- **DIN MIDI only; USB MIDI off** (USB-C carries the serial log).
- **Shift = button 8, read live in `Draw()`** (a long press suppresses the release event and would leave shift stuck).
- **Soft takeover on every knob layer**, except K2 normal (loop length, `normal_takeover = false`).
- **Knob/CC assignments are tables in `params.h`.** Knob and CC write the same value; last one wins. Values reset at power-up (nothing saved).
- **Engine is plain C++, tested on the Mac first.** Buffers are fixed stereo float arrays in SDRAM sized from `looper_config.h` (min 130 BPM, no runtime control).
- **Beat repeat stutters the input history only; the loop records the beat-repeat output.** Crossfades end on the quantise line. MIDI clock arrives via `StartReceiveRt` into a queue, stamped per audio block (~1 ms). No delay on the live input; an input offset on beat-repeat slice reads handles audio-vs-clock misalignment.

## Names (use these)
**Quantized loop buffer** = global B1/B2 looper (`quantized_loop`). **Beat repeat** = stutter on B9-B16 (layer 1).

## Known limits
- Round-trip latency not measured. Loop-start timing has up to ~1 ms block jitter. The Makefile depends on libDaisy/DaisySP at `../../DaisyExamples/`; `USE_FATFS = 1` is left on though SD is unused.

## Roadmap (ideas, not committed to)
- Apply the input offset to loop recording starts; hardware-timer timestamps for MIDI clock.
- Later: 16-bar loops, USB MIDI clock, Continue / Song Position Pointer, overdub, save/restore.
