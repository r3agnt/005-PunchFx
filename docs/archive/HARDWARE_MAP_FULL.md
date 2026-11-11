# Hardware map: log message list (moved out of docs/HARDWARE_MAP.md on 2026-11-11)

- Clock: `Clock detected`, `Clock lost - transport stopped`, `Transport Start`, `Transport Stop`, `Transport Continue (ignored)`, `Bar 3 Beat 2` (every beat while running), `Tempo 120.0 BPM` (when it changes by more than 0.5).
- Looper: `Loop ARMED 4 bars, bar 5` (or `at Start`), `Loop RE-ARMED ...`, `Arm cancelled`, `Loop RECORDING 4 bars`, `Loop PLAYBACK 4 bars (384000)` (loop length in samples), `Source: BUFFER` / `Source: INPUT`, `Loop cleared`, `Loop held (transport stopped)`, `Rec aborted: transport stopped` / `loop too long`, `B1 ignored - no MIDI clock` / `tempo too slow` (below the 130 BPM minimum for that loop length) / `recording`, `B2 ignored - no loop`.
- Beat repeat: `Stutter 1/8 waiting`, `Stutter 1/8 ON` (fresh slice), `Stutter 1/4 ON (stored)` (flipped to a slice stored earlier in the hold), `Stutter OFF`, `Stutter latch 1/4` / `Stutter latch OFF`, `Stutter ignored - not running` / `too short` / `tempo too slow`. Offset: `Shift + K3 Input offset - +2.5 ms`.
- Pitch fx: `Pitch fx ON` / `Pitch fx OFF` (at the 1/16 line).
- CPU: `CPU avg 12% max 31% (30 s)` every 30 s (max = worst audio block in that period; 100% = dropout).
- Boot: `Firmware version ...`, `Looper: max 8 bars, min 130 BPM, 4/4`, `Buffers (stereo float): loop 5.4 MB, history 1.4 MB of 64` (the stutter slots add about 1.7 MB; see `docs/DESIGN_LOOPER.md`, "Minimum tempo and memory").

