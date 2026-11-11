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
- Clock: `Clock detected`, `Clock lost - transport stopped`, `Transport Start`, `Transport Stop`, `Transport Continue (ignored)`, `Bar 3 Beat 2` (every beat while running), `Tempo 120.0 BPM` (when it changes by more than 0.5).
- Looper: `Loop ARMED 4 bars, bar 5` (or `at Start`), `Loop RE-ARMED ...`, `Arm cancelled`, `Loop RECORDING 4 bars`, `Loop PLAYBACK 4 bars (384000)` (loop length in samples), `Source: BUFFER` / `Source: INPUT`, `Loop cleared`, `Loop held (transport stopped)`, `Rec aborted: transport stopped` / `loop too long`, `B1 ignored - no MIDI clock` / `tempo too slow` (below the 130 BPM minimum for that loop length) / `recording`, `B2 ignored - no loop`.
- Beat repeat: `Stutter 1/8 waiting`, `Stutter 1/8 ON` (fresh slice), `Stutter 1/4 ON (stored)` (flipped to a slice stored earlier in the hold), `Stutter OFF`, `Stutter latch 1/4` / `Stutter latch OFF`, `Stutter ignored - not running` / `too short` / `tempo too slow`. Offset: `Shift + K3 Input offset - +2.5 ms`.
- Pitch fx: `Pitch fx ON` / `Pitch fx OFF` (at the 1/16 line).
- CPU: `CPU avg 12% max 31% (30 s)` every 30 s (max = worst audio block in that period; 100% = dropout).
- Boot: `Firmware version ...`, `Looper: max 8 bars, min 130 BPM, 4/4`, `Buffers (stereo float): loop 5.4 MB, history 1.4 MB of 64` (the stutter slots add about 1.7 MB; see `docs/DESIGN_LOOPER.md`, "Minimum tempo and memory").

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
The B1/B2 looper buttons and B9-B16 beat repeat work on layer 1 only. On layer 2 **B1-B7 and B9-B16 are 15 fx buttons** (B8 = Shift). Only B7 (bit crusher) exists so far; the others log `FX B3 not built yet`.
Planned map: B1 reverb, B2 delay, B3 reverse, B4 pitch down, B5 pitch up, B6 freq shifter, B7 bit crusher, B9 res hipass sweep, B10 res lowpass, B11 distortion, B12 flanger, B13 jet flanger, B14 phaser, B15 comb, B16 tremolo chop.
- **Hold** a button = fx on; **Shift + button** = latch (Shift + same button = off; one latch at a time). Start and stop land on the next 1/16 line (at once if no MIDI clock). Newest button wins. Only one fx runs at a time (5 ms crossfade). A latched fx keeps running when you change layer.
- The last button pressed is the **edit slot**. **Edit mode** (2-way switch): K1-K4 = that fx's own params, K5 out gain (+-8 dB), K6 wet/dry, K7 hipass (20 Hz-2 kHz, bottom = off), K8 lopass (20 kHz-200 Hz, top = off). Order (0.10.1): wet/dry mix -> hipass -> lopass -> gain, all on the whole mix (dry too). **Perf mode**: knobs locked. Soft takeover on every knob: after selecting another fx a knob does nothing until it reaches that fx's stored value (log shows `(pickup)`).
- Bit crusher (B7, 0.10.3): the input is summed to mono, then pre lopass -> noise -> bit depth -> sample rate, then the shared stage. K1 pre lopass 200 Hz-20 kHz (top = off), K2 noise 0-100% (scaled to the quantiser step, so it grows as the depth drops), K3 bit depth 12..1 bits (curved: most travel below 8 bits), K4 sample rate 48 kHz..188 Hz (equal octave steps); then a fixed mild stereo flanger (0.25 Hz, 1-2 ms sweep, mix 0.4, feedback 0.3, highs rolled off at 7 kHz) smooths it and widens it again.
- Not saved yet (reset at power-up); saving to the Seed's flash is planned.
- Log: `FX B7 Bit crusher ON`, `FX OFF`, `FX latch Bit crusher`, `FX latch OFF`, `FX edit: Bit crusher`, `K3 Bit depth - 5.2 bits`, `K5 Out gain - +2.0 dB`, `K6 Wet/dry - 100%`, `K7 Hipass - off`, `K8 Lopass - 5000 Hz`.
- LEDs (layer 2): a fx button LED is dim if that fx exists, brighter = edit slot (edit mode), brighter again = latched, full = ON, flashing = waiting for its line. LED 8 = edit (glow) / perf (dim). Layer 3: LED 8 flashes slowly.

## LEDs (0.4.3)
- **Beat repeat buttons 9-16** (their LED flashes from the press until the 1/16 line; LED 16 is shared with the loop-audible glow; the stutter LED wins): their LED flashes fast while waiting for the slice boundary and glows dimly (0.02, same as the loop-length LEDs) while repeating held, brighter (0.15) while latched on; the eighth-note LED stays brighter on top.
- **Brightness:** `SetLed` takes 0.0-1.0 (256 steps, about 218 distinct levels, roughly 1% apart at the dim end). The devkit inverts the value before libDaisy's gamma table, so low values are brighter than they look: 0.01 = 4% on-time, 0.02 = 6%, 0.04 = 12%, 0.08 = 21%, 0.10 = 26%, 0.25 = 55%, 0.5 = 86%, 1.0 = 100%. The levels used are constants at the top of `MainPage` in `main_page.h`.
- **Setting the loop length:** for 1.5 s after K2 (or CC 21) changes it, LEDs 1-8 show the new length (LEDs 1..N lit for N bars) instead of the normal top-row display. It applies to the next recording.
- **Top row, LEDs 1-8:** one LED per bar of the loop cycle (dim); the bar we are in is bright while the transport runs.
  - ARMED: LED 1 flashes slowly.
  - RECORDING: bars fill up like a progress bar; the current bar flickers.
  - PLAYING: one bright LED moves along the bars.
- **Bottom row, LEDs 9-16:** the eight eighth-notes of the bar; the current one is bright. LED 16 also glows steadily while the loop is what you hear (dark = live input).
- **No MIDI clock:** LEDs 9 and 16 blink slowly. **Clock present but stopped:** the bottom row glows very dimly.
- **Pitch fx (0.6.0):** LED 6 flashes fast from the B6 press until its 1/16 line, then stays on while the effect is on (replaces the bar-6 LED).
- **Seed LED:** slow flash = armed, solid = recording, otherwise a fast heartbeat blink.
- Button presses no longer light their LED.

Defaults at power-up: loop length 4 bars, input gain 0 dB, output at ceiling, no loop.
