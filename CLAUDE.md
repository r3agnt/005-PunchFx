# 005-PunchFx — context for Claude
Names: **Quantized loop buffer** = B1/B2 looper (`quantized_loop`); **Beat repeat** = stutter on B9-B16 (layer 1).

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
Size limits: `CLAUDE.md` + the two imports **25 KB total**; `CHANGELOG.md` **6 KB**; `HARDWARE_TEST.md` **5 KB**; `DESIGN_LOOPER.md` **16 KB**; `DESIGN_FX.md` **10 KB**. Checked at the start of every session and after every version bump. Over a limit = propose a cleanup (list the cuts, wait for my OK):
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
| `docs/DESIGN_FX.md` | working on layer 2 fx (rules, reverb/delay/bit crusher as built, later fx) |
| `docs/WORKFLOW.md` | planning how to split up work / where each doc update goes |
| `docs/archive/` | almost never: old full changelog, test checklists, old design text |

## How to work with me
- I have **no C++ background**. Explain in plain English, comment code simply, avoid jargon or explain it.
- **Before making changes:** list every change you plan to make, ask questions about anything ambiguous (give options and let me add my own comments), and wait for my answers. Don't guess; if my spec has a conflict or gap (e.g. two controls on one knob), point it out.
- **Commit only when I ask.** Clear message; if the session gives attribution lines, end the message with them, otherwise omit them.
- **With every code change:** bump `FIRMWARE_VERSION` in `src/version.h`, add ONE line to `docs/CHANGELOG.md`, update `docs/HARDWARE_MAP.md` (controls) and `docs/DESIGN_LOOPER.md` (behaviour) only if they changed, and finish with a clean `make` (no warnings). Doc-only changes need no version bump. Don't put per-version history in this file.
- You cannot flash or hear the hardware: say clearly what is built-but-untested and tell me what to check.
- Keep sessions short: one task per session, commit when it works (see `docs/WORKFLOW.md`).

## Current state (v0.13.1: delay per-slot time/feedback built, NOT yet flashed/tested. v0.13.0 delay flashed and working)
Layers (3-way toggle up/middle/down = 1/2/3; 2-way toggle = edit/perf). **Layer 1** = MIDI clock follower; Quantized loop buffer (B1 arm/record, B2 loop/live, Shift+B1 clear, K2 length 1/2/4/8 bars; Shift+K1 input gain, Shift+K2 output level, Shift+K3 input offset); beat repeat B9-B16 (1/16 quantised, latch Shift+B9-B16); B6 pitch-down on the beat repeat. **Layer 2** = punch-in fx (`src/engine/fx/`): B1 reverb bus, B2 delay bus and B7 bit crusher built, the other 13 slots log "not built yet". **Layer 3** = tbd. Settings saved to flash. Slice mode was removed (idea in `docs/IDEAS.md`).
Signal chain: input gain -> beat repeat -> pitch fx -> looper -> layer 2 fx -> SoftClip -> output level, ceiling 0.9 (`docs/ARCHITECTURE.md`).
Controls: layer 1 tables in `src/params.h` (`kPotMap`, `kCcMap`); layer 2 knobs in `MainPage::HandleFxKnob`; B8 = SHIFT; soft takeover everywhere, reset on every layer / mode / edit-slot / Shift change. Full list: `docs/HARDWARE_MAP.md`. Firmware version prints at boot and on Shift + B16 long press. **USB MIDI is disabled** (USB carries serial logging); DIN MIDI works.

## The plan: punch-in fx on layer 2 (design details: `docs/DESIGN_FX.md`, read it before touching `src/engine/fx/`)
- **Layer 2 button map (agreed):** B1 reverb, B2 delay, B3 reverse, B4 pitch down, B5 pitch up, B6 freq shifter, B7 bit crusher, B9 res hipass sweep, B10 res lowpass, B11 distortion+prefilter, B12 flanger, B13 jet flanger, B14 phaser, B15 comb, B16 tremolo chop (tight gate). Hold = on, Shift + button = latch, quantised to 1/16. Per slot: K1-K4 own params, K5 gain +-12 dB, K6 wet/dry (return level on bus fx), K7 hipass, K8 lopass; Shift+K1 / Shift+K2 = reverb / delay send, Shift+K3 / Shift+K4 = this slot's delay time / feedback (B2: Shift+K4 = reverb<>delay routing).
- **Build order:** 0.9 fork+strip, 0.10 fx framework + bit crusher, 0.11 flash save, 0.12 reverb bus, 0.13 delay bus (tested), 0.13.1 per-slot delay settings (awaits hardware test) -> **NEXT: simple inserts** (res lowpass, distortion, tremolo chop) -> modulation fx (flanger, jet, phaser, comb) -> pitch / freq shifter / reverse (shimmer = reverb K4) -> stacking + snapshots.
- **SDRAM objects** (`DSY_SDRAM_BSS`) must have NO constructor and no `= value` member defaults: it runs at power-up before SDRAM is on and FREEZES the Seed (0.12.0 bug). Set every member in `Init()`; guard with `static_assert(std::is_trivially_default_constructible<...>)`.
- Layer = one header with `Init`, `Process(block)`, `GetStatus`; `main.cpp` only chains them. Fx engines: plain C++ in `src/engine/fx/` with Mac tests.

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
| `src/engine/fx/` | Layer 2: `fx_layer.h`, `fx_common.h` (HP/LP/gain/wet), `fx_engine.h`, `fx_bitcrush.h`, `fx_reverb_bus.h`, `fx_delay_bus.h`, `fx_flanger_core.h` |
| `src/engine/spsc_queue.h`, `looper_config.h` | Lock-free queue; fixed numbers (4/4, bars, min BPM, buffer sizes, pitch limits) |
| `src/modules/gain_stage.h` | Input gain, soft clip, output level, ceiling |
| `src/userinterface.h`, `src/ui_glue.h` | Template UI event/LED code (rarely edit) |
| `test/` | Mac-side tests: `make -C test` |

## Build / flash
- Build: `make` (clean: `make clean && make`). Output `build/005-PunchFx.bin`. Zero warnings.
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
- **Knob/CC assignments are tables in `params.h`.** Knob and CC write the same value; last one wins. Fx settings + the 4 layer 1 settings (K2 length, input gain, output level, offset) are SAVED to the Seed flash (`SavedState`, `src/engine/saved_state.h`) when the 2-way switch goes edit -> perf, and loaded at power-up; hold Shift (B8) at power-up to reset them. Stored at QSPI offset 0x100000 (program lives from 0x40000; never write below 0x40000+program size or in the first 256 KB).
- **Engine is plain C++, tested on the Mac first.** Buffers are fixed stereo float arrays in SDRAM sized from `looper_config.h` (min 130 BPM, no runtime control).
- **Beat repeat stutters the input history only; the loop records the beat-repeat output.** Crossfades end on the quantise line; MIDI clock goes through a queue, stamped per audio block (~1 ms); an input offset on slice reads handles audio-vs-clock misalignment.

## Known limits
- Round-trip latency not measured. Loop-start timing has up to ~1 ms block jitter. The Makefile depends on libDaisy/DaisySP at `../../DaisyExamples/`; `USE_FATFS = 1` is left on though SD is unused.

## Roadmap (ideas, not committed to)
- Apply the input offset to loop recording starts; hardware-timer timestamps for MIDI clock.
- Later: 16-bar loops, USB MIDI clock, Continue / Song Position Pointer, overdub, SD-card snapshots.
