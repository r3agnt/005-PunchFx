# Architecture

Three things run independently:

```
AudioCallback (fast, constant)        LowPriorityCallback (250x/sec)      main() loop (spare time)
  hw.UpdateAllControls()                ui.DoEvents()                       prints msg_fifo to USB serial
  ui.GenerateEvents()  --events-->        -> MainPage::OnButton / OnPotMoved
  process audio                           -> MainPage::Draw (LEDs, 60 Hz)
  msg_fifo.PushBack("text") ------------------------------------------>
```

- **Audio callback** reads knobs/buttons, turns changes into events, and processes audio. Keep it fast: no printing, no memory allocation.
- **Low-priority callback** runs the events and redraws LEDs. This is where controls get assigned (`main_page.h`) by writing into `params`.
- **main loop** is the only place that prints. Anything the audio callback wants to log goes through `msg_fifo`.
- **MIDI clock path:** DIN receive -> `MidiRealtimeCallback` -> `midi_rt_queue` -> `clock_tracker.Advance()` at the start of each audio block (bar/beat/eighth, tempo); LEDs read `clock_tracker.GetStatus()`.
- **Looper path:** B1/B2 (UI) push `LoopCmd` into `loop_cmd_queue`; the audio callback pops them and runs `quantized_loop.Process` per chunk (<= 128 samples); buffers in SDRAM.
- **Beat repeat path:** buttons 9-16 are read straight from the hardware each block (Shift-pressed ones ignored) with the clock position and input offset into `beat_repeat.Process`; its output feeds the looper.
- **Layer 2 path (0.10.0):** each block the audio callback reads the toggles (`current_layer`, `fx_edit_mode`) and buttons B1-B7/B9-B16 straight from the hardware into `fx::BlockInfo`; `fx_layer.Process` starts/stops fx on 1/16 lines. Log lines come out of `fx_layer.PopEvent`. Knobs on layer 2 are handled in `MainPage::HandleFxKnob` (UI context).
- **Saving path (0.11.0):** the audio callback sets `fx_save_request` when the edit/perf switch goes edit -> perf; the main loop copies the settings into a `SavedState` and, if it differs, calls `PersistentStorage::Save()` (flash erase + write; the audio callback keeps running because the program runs from RAM). Loaded in `main()` before audio starts.
- `params` (`params.h`) is the shared list of settings: UI writes, audio reads.

Files: `main.cpp` (all three loops), `userinterface.h` (event generation), `ui_glue.h` (helpers), `main_page.h` (what happens on events).

## Signal chain (the master description; other files refer here)
Per channel (0 = left, 1 = right), in `AudioCallback`, in this exact order:

0. `clock_tracker.Advance` - MIDI clock / transport, once per block (not audio, but the first thing the callback does).
1. `gain_stage.Gain` - input gain, -12 to +24 dB (Shift+K1).
2. `beat_repeat.Process` - passes the live signal or repeats a slice of the 2-bar input history while buttons 9-16 are held (see `docs/DESIGN_LOOPER.md`).
2a. `pitch_fx.Process` - B6 pitch-down on the beat repeat output (each repeat 1 semitone lower; a new pitch head starts at each repeat's loop crossfade, using `beat_repeat.RepeatIndex()` and `JumpStartIndex()`; bypassed unless B6 is on and a slice is repeating).
3. `quantized_loop.Process` - the looper: passes the live signal, records it, or plays the loop (see `docs/DESIGN_LOOPER.md`). Button commands from B1/B2 arrive through `loop_cmd_queue`.
3c. `fx_layer.Process` (reverb bus, 0.12.0: slot B1 or any fx send feeds `ReverbBus` = pre-delay + `ReverbSc` from `src/modules/reverb_core.h`; its return goes through the reverb slot's stage and is ADDED to the layer output; both objects live in SDRAM) - LAYER 2, the punch-in fx (`src/engine/fx/`): passes the looper output through unless an fx is on. One engine runs at a time; each slot = engine + shared stage (hipass, lopass, gain, wet/dry). The layer is chosen by the 3-way toggle (read in the audio callback; beat repeat and the looper buttons only work on layer 1). Fx settings live in `fx_settings[]` (UI writes in edit mode, audio reads).
4. `gain_stage.Limit` - SoftClip (output limited to +/-1.0), multiplied by the output level (Shift+K2; 0.9 at maximum, down to silence), then a hard clamp to +/-(that level). Nothing can exceed 0.9.
