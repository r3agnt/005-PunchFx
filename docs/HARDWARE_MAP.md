# Hardware map (from `libDaisy/src/desktop_devkit.h`)

Only the assignments in "Control assignments" below are active. Every other pot/button is initialised and logged to serial but does nothing.

| Item | Count | Code | Notes |
|---|---|---|---|
| Pots | 8 | `hw.pot[0..7]` | Value 0.0–1.0. Log: `K<n> <name> - <value>` |
| Buttons | 16 | `hw.button[0..15]` | B1 = record arm, B2 = loop/live, B8 = shift, B9-B16 = beat repeat, Shift + B9-B16 = latch, Shift + B16 long press = version. Log: `B<n> Pressed` / `Released` / `Long press` (≥1.5 s) |
| LEDs | 16 | `hw.SetLed(i, 0..1)` | Top row 1-8: bars of the loop cycle; bottom row 9-16: eighth-notes (see below) |
| Seed LED | 1 | `hw.seed.SetLed(bool)` | Blinks as heartbeat |
| CV inputs | 2 | `hw.cv[0..1]` | Unused |
| Gate inputs | 2 | `hw.gate_in[0..1]` | Unused |
| Toggles | 2 | `hw.tog_sw17` (2-way), `hw.tog_sw18` (3-way) | **3-way = layer** (up = 1 looper + beat repeat, middle = 2 punch-in fx, down = 3 not built). **2-way = edit / perf** mode of layer 2 (`kEditWhenSwitchPressed` in `main.cpp` flips which way is edit). Log: `Layer 2`, `Edit mode` / `Perf mode`. |
| MIDI DIN | in/out | `hw.midi` | Input used for CC (below) and for MIDI clock / Start / Stop. USB MIDI disabled for now |
| DAC out | 2 | `hw.SetDacOut(channel, 0..4095)` | Unused |
| Audio | stereo in/out | AudioCallback `in[0..1]`, `out[0..1]` | Processed by the signal chain (see ARCHITECTURE.md) |
| Plug/SD detect | 3 | `hw.left_detect`, `hw.right_detect`, `hw.sd_detect` | Unused |

## Control assignments
Hold **Button 8 (SHIFT)** for the second function. Knobs use soft takeover: after a layer change, MIDI change or power-up, a knob does nothing until you turn it past (or onto) the current value. Serial log format (values in real units, from `src/param_info.h`):
- Knob: `K2 Loop length - 4 bars`, with shift `Shift + K1 Input gain - +6.0 dB`. The value is the knob position; ` (pickup)` at the end means the knob has not picked up the parameter yet, so the parameter did not change. Unassigned knobs: `K8 (unassigned) - 0.500`.
- MIDI CC: `CC 20 Input gain - +6.0 dB`.
- Buttons: `B1 Pressed`, `B1 Released`, `B1 Long press`; `Shift + B16 Long press - Firmware version 0.4.2`; shift prints `Shift on` / `Shift off`.
- Other log lines (clock, looper, beat repeat, pitch fx, CPU, boot) are in the source (`LogClockEvents`, `LogLoopEvents`, `LogBeatEvents`, `LogFxEvents` in `main.cpp`); the old full list is in `docs/archive/HARDWARE_MAP_FULL.md`. Every 30 s: `CPU avg 12% max 31% (30 s)` (100% = dropout).

| Button | Function |
|---|---|
| B1 | Record arm: IDLE -> ARMED -> (next loop cycle) RECORDING -> (next cycle) PLAYBACK. While a loop plays it arms a re-record at the next cycle. Pressing it again while armed cancels. |
| Shift + B1 | Clear the loop (crossfades to live) |
| B2 | Switch output between loop and live input |
| B9-B16 | Beat repeat (hold): 1 bar, 1/2, 3/8, 1/4, 1/8, 1/16, 1/32, 1/64 (3/8 = dotted quarter). Press and release each wait for the next 1/16 line (0.4.5), then the last slice-length of audio ending on that line repeats until released. Most recent button wins; flip between held buttons (each keeps its own stored slice until all are released). |
| Shift + B9-B16 | Latch beat repeat on/off: that division keeps repeating after release. Shift + same button = off; Shift + another button moves the latch (one at a time). A normal held press overrides it while down. Cleared when the transport stops. |
| B6 | **Pitch fx (0.6.0):** hold = each repeat of the held beat-repeat slice is 1 semitone lower than the last (1st repeat normal, down to -12); on/off both land on the next 1/16 line; with no stutter held it is armed for the next one. Ignored with Shift held and while stopped. |
| B8 | Shift (hold) |
| Shift + B16 long press | Print firmware version (the press itself also toggles the 1/64 latch) |

| Knob | Normal | Shift + knob | CC |
|---|---|---|---|
| 1 | - | Input gain (-12 to +24 dB) | 20 |
| 2 | Loop length: 1 / 2 / 4 / 8 bars (no pick-up: acts immediately) | Output level (0.9 ceiling down to silence) | 21 / 22 |
| 3 | - | Input offset (-20..+20 ms, centre 0, no pick-up) | 23 |
| 4 | - | - | - |
| 5-8 | - | - | - |

CC numbers are accepted on **any MIDI channel**; each parameter has exactly one CC (values 0-127 map to 0.0-1.0 of the control, then to real units as for the knob).

## Layer 2: punch-in fx (0.10.0; 3-way switch in the middle)
The B1/B2 looper buttons and B9-B16 beat repeat work on layer 1 only. On layer 2 **B1-B7 and B9-B16 are 15 fx buttons** (B8 = Shift). Only B1 (reverb), B2 (delay) and B7 (bit crusher) exist so far; the others log `FX B3 not built yet`.
Button map: see `CLAUDE.md`.
- **Hold** a button = fx on; **Shift + button** = latch (Shift + same button = off; one latch at a time). Start and stop land on the next 1/16 line (at once if no MIDI clock). Newest button wins. Only one fx runs at a time (5 ms crossfade). A latched fx keeps running when you change layer.
- The last button pressed is the **edit slot**. **Edit mode** (2-way switch): K1-K4 = that fx's own params, K5 out gain (+-12 dB; on the reverb it lifts the dry AND the return while the reverb is on), K6 wet/dry, K7 hipass (20 Hz-2 kHz, bottom = off), K8 lopass (20 kHz-200 Hz, top = off). Order (0.10.1): wet/dry mix -> hipass -> lopass -> gain, all on the whole mix (dry too). **Perf mode**: knobs locked. Soft takeover on every knob: after selecting another fx a knob does nothing until it reaches that fx's stored value (log shows `(pickup)`).
- **Reverb (B1) = a send** (design: `docs/DESIGN_FX.md`): K1 Time (tail 0.4-12 s), K2 Tone (default 4686 Hz), K3 Pre-delay (0-100 ms, default 0), K4 free, K5 gain (lifts dry + return), K6 Return level (default 40%), K7 hipass on the return, **K8 lopass on the reverb input**. Releasing B1 lets the tail ring out.
- **Sends:** edit mode, **Shift + K1** = reverb send of the edit fx, **Shift + K2** = delay send (0-100%, saved per fx; not on the bus fx themselves). On B1, Shift + K2 = how much reverb goes into the delay; on B2, Shift + K1 = how much delay goes into the reverb.
- **Delay (B2) = a send** (design: `docs/DESIGN_FX.md`): K1 Time (1/4, dotted 1/8, 1/8, dotted 1/16, 1/16, 1/32, synced to the MIDI clock), K2 Feedback, K3 free, K4 Ping-pong, K5 gain (lifts dry + return), K6 Return level (default 40%), **K7 Band centre and K8 Band width = a bandpass in the feedback loop** (not hipass/lopass). **Shift + K4 on B2 = routing**: below half reverb > delay (default), above half delay > reverb (one way at a time, never a loop; saved). Releasing B2 lets the repeats ring out.
- **Bit crusher (B7):** mono, K1 pre lopass, K2 noise, K3 bit depth 12..1, K4 sample rate 48 kHz..188 Hz, then a fixed mild stereo flanger (details: `docs/DESIGN_FX.md`).
- **Saving (0.11.0):** the fx settings AND the layer 1 settings (loop length, input gain, output level, input offset) are saved to the Seed's flash when the 2-way switch goes from EDIT to PERF (log `FX settings saved` / `FX settings unchanged`) and loaded at power-up (`FX settings loaded from flash`). **Hold Shift (B8) while powering up** to reset them to defaults (`FX settings RESET to defaults`). The latch and the layer are not saved.
- Log: `K1 Time - 1/8 dotted`, `Shift + K4 Routing - reverb > delay`, `FX B7 Bit crusher ON`, `FX OFF`, `FX latch ...`, `FX edit: ...`, `K3 Bit depth - 5.2 bits`, `K5 Out gain - +2.0 dB`.
- LEDs (layer 2): a fx button LED is dim if that fx exists, brighter = edit slot (edit mode), brighter again = latched, full = ON, flashing = waiting for its line. LED 8 = edit (glow) / perf (dim). Layer 3: LED 8 flashes slowly.

## LEDs, layer 1 (layer 2: see its section above)
- **Top row 1-8:** one dim LED per bar of the loop cycle, the current bar bright. ARMED: LED 1 flashes. RECORDING: bars fill up. PLAYING: one bright LED moves along. For 1.5 s after K2 (or CC 21) changes the length, LEDs 1..N show the new length.
- **Bottom row 9-16:** the eight eighth-notes (current one bright); LED 16 also glows while the loop is what you hear. Beat repeat: the button LED flashes while waiting for its 1/16 line, glows dim while held, brighter (0.15) while latched. LED 6 = pitch fx (flashes until its line, then on).
- **No MIDI clock:** LEDs 9 and 16 blink slowly. **Clock but stopped:** bottom row glows very dimly.
- **Seed LED:** slow flash = armed, solid = recording, else a fast heartbeat.
- **Brightness:** `SetLed` 0.0-1.0; the devkit inverts it (0.02 = 6% on-time, 0.25 = 55%, 1.0 = 100%). Levels are constants at the top of `MainPage`.

Defaults at power-up: loop length 4 bars, input gain 0 dB, output at ceiling, no loop (unless saved).
