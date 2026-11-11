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
| Toggles | 2 | `hw.tog_sw17` (2-way), `hw.tog_sw18` (3-way) | Unused |
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
- Knob: `K4 Slice pitch (B6) - +3 st`; MIDI `CC 24 Slice pitch (B6) - +3 st`.
- Pitch fx: `Pitch fx ON` / `Pitch fx OFF` (at the 1/16 line); in slice mode `Pitch fx ON (+3 ping-pong)` / `Pitch fx OFF` (at a 1/8 line).
- CPU: `CPU avg 12% max 31% (30 s)` every 30 s (max = worst audio block in that period; 100% = dropout).
- Slice mode: `Slice mode ON` / `Slice mode OFF`, `Slice bank 2` (only with 2 banks), `Bank ignored - 1 bar only`, `Slice 3` (repeating slice 3), `Slice loop` (last button released: the bar loops from that slice), `B7 ignored - not running`, `Bank ignored - no slice mode`.
- Boot: `Firmware version ...`, `Looper: max 8 bars, min 130 BPM, 4/4`, `Buffers (stereo float): loop 5.4 MB, history 1.4 MB of 64` (the stutter slots add about 1.7 MB; see `docs/DESIGN_LOOPER.md`, "Minimum tempo and memory").

| Button | Function |
|---|---|
| B1 | Record arm: IDLE -> ARMED -> (next loop cycle) RECORDING -> (next cycle) PLAYBACK. While a loop plays it arms a re-record at the next cycle. Pressing it again while armed cancels. |
| Shift + B1 | Clear the loop (crossfades to live) |
| B2 | Switch output between loop and live input |
| B9-B16 | Beat repeat (hold): 1 bar, 1/2, 3/8, 1/4, 1/8, 1/16, 1/32, 1/64 (3/8 = dotted quarter). Press and release each wait for the next 1/16 line (0.4.5), then the last slice-length of audio ending on that line repeats until released. Most recent button wins; flip between held buttons (each keeps its own stored slice until all are released). |
| Shift + B9-B16 | Latch beat repeat on/off: that division keeps repeating after release. Shift + same button = off; Shift + another button moves the latch (one at a time). A normal held press overrides it while down. Cleared when the transport stops. |
| B6 | **Pitch fx (0.6.0):** hold = each repeat of the held beat-repeat slice is 1 semitone lower than the last (1st repeat normal, down to -12); on/off both land on the next 1/16 line; with no stutter held it is armed for the next one. In slice mode (0.7.2): the held slice plays 3 semitones up (K4: -12..+12) as a forward-then-reverse ping-pong (first 16th forward, second 16th backwards, every eighth), triggered on 1/8 lines. Ignored with Shift held and while stopped. |
| B7 | Slice mode on/off. ON waits for the next bar line, then freezes the bar that just ended (0.5.2) and plays it as a loop (the Slice loop buffer); it keeps waiting if a whole bar has not been recorded yet. OFF at the next 1/8 line = back to the Quantized loop buffer / live. |
| Shift + B7 | Nothing for now (1-bar window): logs `Bank ignored - 1 bar only` |
| B9-B16 in slice mode | Play the 8 eighth notes of the frozen bar (B9 = beat 1 ... B16 = last eighth): repeat while held, newest wins, at the BAR line (loop end) a held slice is choked (stays choked until re-pressed) and the loop restarts from beat 1 (0.8.0); release (0.5.1) carries on looping the whole bar from the next slice after the one you played, until another slice is pressed. The beat repeat and the latch are off meanwhile. |
| Shift + B6 | Toggle the REVERSE half of the slice mode B6 effect (log `Pitch reverse ON/OFF`; default off = forward only; not saved) |
| B8 | Shift (hold) |
| Shift + B16 long press | Print firmware version (the press itself also toggles the 1/64 latch) |

| Knob | Normal | Shift + knob | CC |
|---|---|---|---|
| 1 | - | Input gain (-12 to +24 dB) | 20 |
| 2 | Loop length: 1 / 2 / 4 / 8 bars (no pick-up: acts immediately) | Output level (0.9 ceiling down to silence) | 21 / 22 |
| 3 | - | Input offset (-20..+20 ms, centre 0, no pick-up) | 23 |
| 4 | Slice mode B6 pitch: -12..+12 semitones (default +3; soft takeover; changes on the next 1/8 line) | - | 24 |
| 5-8 | - | - | - |

CC numbers are accepted on **any MIDI channel**; each parameter has exactly one CC (values 0-127 map to 0.0-1.0 of the control, then to real units as for the knob).

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
- **Slice mode (0.5.0):** LED 7 = mode (steady = on, fast flash = B7 pressed and waiting for its bar line, or waiting for a whole bar to be recorded; it replaces the bar-7 LED). LEDs 9-16 = the 8 slices of the bank (dim glow), the repeating one brighter (`kLedSlicePlaying`), a slice button waiting for its line flashes; the eighth-note marker still moves on top.
- **Pitch fx (0.6.0):** LED 6 flashes fast from the B6 press until its 1/16 line, then stays on while the effect is on (replaces the bar-6 LED).
- **Seed LED:** slow flash = armed, solid = recording, otherwise a fast heartbeat blink.
- Button presses no longer light their LED.

Defaults at power-up: loop length 4 bars, input gain 0 dB, output at ceiling, no loop.
