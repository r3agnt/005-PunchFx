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
- **MIDI clock path:** the DIN receive code calls `MidiRealtimeCallback` (main.cpp) for every clock tick / Start / Stop / Continue; it only pushes the message into `midi_rt_queue`. At the start of each audio block `clock_tracker.Advance()` takes them, updates bar/beat/eighth and the tempo estimate, and queues log lines. The LEDs (`MainPage::DrawLeds`) read a copy of the position via `clock_tracker.GetStatus()`.
- **Looper path:** B1/B2 presses (UI context) push a `LoopCmd` into `loop_cmd_queue`; the audio callback pops them, then runs `quantized_loop.Process` on each block (in chunks of at most 128 samples). The loop buffers are two float arrays in SDRAM. Log lines come out through `msg_fifo`; the LEDs read `quantized_loop.GetStatus()`.
- **Beat repeat path:** each audio block reads buttons 9-16 straight from the hardware (`hw.button[8..15].Pressed()`; ones pressed while Shift is held are ignored), the clock position at the start of the block (`clock_tracker.BarAtBlockStart`, `PositionAtBlockStart`) and the input offset, and passes them to `beat_repeat.Process`. Its output feeds the looper. Log lines come out through `msg_fifo`; the LEDs read `beat_repeat.GetStatus()`.
- **Slice mode path (0.5.0):** B7 / Shift + B7 presses (UI context) push a `SliceCmd` into `slice_cmd_queue`; the audio callback pops them. Every chunk `slice_loop.Process` records the live input into its own 2-bar buffer, and while slice mode is on it plays the frozen bar (0.5.2; was 2 bars) (buttons 9-16 are read from the hardware like the beat repeat's, and passed to it instead). It returns `slice` (its audio) and `base_w` (weight of the normal signal); the callback mixes `base_w * normal + slice` into the Quantized loop buffer's input and again into its output. The LEDs read `slice_loop.GetStatus()`.
- `params` (`params.h`) is the shared list of settings: UI writes, audio reads.

Files: `main.cpp` (all three loops), `userinterface.h` (event generation), `ui_glue.h` (helpers), `main_page.h` (what happens on events).

## Signal chain (the master description; other files refer here)
Per channel (0 = left, 1 = right), in `AudioCallback`, in this exact order:

0. `clock_tracker.Advance` - MIDI clock / transport, once per block (not audio, but the first thing the callback does).
1. `gain_stage.Gain` - input gain, -12 to +24 dB (Shift+K1).
2. `beat_repeat.Process` - passes the live signal or repeats a slice of the 2-bar input history while buttons 9-16 are held (see `docs/DESIGN_LOOPER.md`).
2a. `pitch_fx.Process` - B6 pitch-down on the beat repeat output (each repeat 1 semitone lower; a new pitch head starts at each repeat's loop crossfade, using `beat_repeat.RepeatIndex()` and `JumpStartIndex()`; bypassed unless B6 is on and a slice is repeating).
2b. `slice_loop.Process` (B6 in slice mode: the slice voice plays +3 semitones as a forward/reverse ping-pong inside the engine, 0.7.2) + mix - slice mode (B7): `performance = base_w * beat repeat output + slice` is what goes into the Quantized loop buffer (see `docs/DESIGN_LOOPER.md`, "Slice loop buffer"). With slice mode off `base_w` is 1 and `slice` is 0, so nothing changes.
3. `quantized_loop.Process` - the looper: passes the live signal, records it, or plays the loop (see `docs/DESIGN_LOOPER.md`). Button commands from B1/B2 arrive through `loop_cmd_queue`.
3b. Slice mode mix again: `output = base_w * Quantized loop buffer output + slice` (so slices are heard over a playing loop).
4. `gain_stage.Limit` - SoftClip (output limited to +/-1.0), multiplied by the output level (Shift+K2; 0.9 at maximum, down to silence), then a hard clamp to +/-(that level). Nothing can exceed 0.9.
