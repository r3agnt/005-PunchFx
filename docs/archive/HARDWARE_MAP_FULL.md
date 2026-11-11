# Hardware map: log message list (moved out of docs/HARDWARE_MAP.md on 2026-11-11)

- Clock: `Clock detected`, `Clock lost - transport stopped`, `Transport Start`, `Transport Stop`, `Transport Continue (ignored)`, `Bar 3 Beat 2` (every beat while running), `Tempo 120.0 BPM` (when it changes by more than 0.5).
- Looper: `Loop ARMED 4 bars, bar 5` (or `at Start`), `Loop RE-ARMED ...`, `Arm cancelled`, `Loop RECORDING 4 bars`, `Loop PLAYBACK 4 bars (384000)` (loop length in samples), `Source: BUFFER` / `Source: INPUT`, `Loop cleared`, `Loop held (transport stopped)`, `Rec aborted: transport stopped` / `loop too long`, `B1 ignored - no MIDI clock` / `tempo too slow` (below the 130 BPM minimum for that loop length) / `recording`, `B2 ignored - no loop`.
- Beat repeat: `Stutter 1/8 waiting`, `Stutter 1/8 ON` (fresh slice), `Stutter 1/4 ON (stored)` (flipped to a slice stored earlier in the hold), `Stutter OFF`, `Stutter latch 1/4` / `Stutter latch OFF`, `Stutter ignored - not running` / `too short` / `tempo too slow`. Offset: `Shift + K3 Input offset - +2.5 ms`.
- Pitch fx: `Pitch fx ON` / `Pitch fx OFF` (at the 1/16 line).
- CPU: `CPU avg 12% max 31% (30 s)` every 30 s (max = worst audio block in that period; 100% = dropout).
- Boot: `Firmware version ...`, `Looper: max 8 bars, min 130 BPM, 4/4`, `Buffers (stereo float): loop 5.4 MB, history 1.4 MB of 64` (the stutter slots add about 1.7 MB; see `docs/DESIGN_LOOPER.md`, "Minimum tempo and memory").



## Cut from HARDWARE_MAP on the 0.12.0 cleanup
Planned map: B1 reverb, B2 delay, B3 reverse, B4 pitch down, B5 pitch up, B6 freq shifter, B7 bit crusher, B9 res hipass sweep, B10 res lowpass, B11 distortion, B12 flanger, B13 jet flanger, B14 phaser, B15 comb, B16 tremolo chop.
- **Brightness:** `SetLed` takes 0.0-1.0 (256 steps, about 218 distinct levels, roughly 1% apart at the dim end). The devkit inverts the value before libDaisy's gamma table, so low values are brighter than they look: 0.01 = 4% on-time, 0.02 = 6%, 0.04 = 12%, 0.08 = 21%, 0.10 = 26%, 0.25 = 55%, 0.5 = 86%, 1.0 = 100%. The levels used are constants at the top of `MainPage` in `main_page.h`.
- Log: `FX B7 Bit crusher ON`, `FX OFF`, `FX latch Bit crusher`, `FX latch OFF`, `FX edit: Bit crusher`, `K3 Bit depth - 5.2 bits`, `K5 Out gain - +2.0 dB`, `K6 Wet/dry - 100%`, `K7 Hipass - off`, `K8 Lopass - 5000 Hz`.


## LED section before compaction (2026-11-11)
## LEDs (0.4.3)
- **Beat repeat buttons 9-16** (their LED flashes from the press until the 1/16 line; LED 16 is shared with the loop-audible glow; the stutter LED wins): their LED flashes fast while waiting for the slice boundary and glows dimly (0.02, same as the loop-length LEDs) while repeating held, brighter (0.15) while latched on; the eighth-note LED stays brighter on top.
- **Brightness:** `SetLed` 0.0-1.0; the devkit inverts it, so low values are brighter than they look (0.02 = 6% on-time, 0.25 = 55%, 1.0 = 100%). Levels are constants at the top of `MainPage` in `main_page.h`.
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
