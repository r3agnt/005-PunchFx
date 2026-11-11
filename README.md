# 005-PunchFx

Audio looper prototype on the Daisy Seed 3 + Desktop DevKit. Forked from 003-ChannelStrip (framework only; EQ and saturator removed).

Currently (v0.8.1): follows MIDI clock/transport on the DIN input and records/plays bar-quantised loops of the audio input (B1 arm/record, B2 loop/live, Shift+B1 clear, K2 = 1/2/4/8 bars), then soft clip -> 0.9 ceiling. Beat repeat on buttons 9-16 (1 bar, 1/2, 3/8, 1/4 ... 1/64; held, press and release quantised to 1/16; flip between held buttons; Shift + button latches it on/off) with an input offset on Shift+K3. B6 = pitch fx: each repeat of the held beat-repeat slice is a semitone lower; in slice mode the held slice plays 3 semitones up, forward then reversed (ping-pong), every eighth (Shift + B6 switches the reverse half on/off; K4 sets the pitch, -12..+12 semitones). At every bar line a held slice is choked and the loop restarts from beat 1. B7 = slice mode: at the next bar line freezes the bar that just ended and plays it as a loop you can slice with buttons 9-16 (B9 = beat 1, eighth notes). See `docs/DESIGN_LOOPER.md` for the plan. Controlled by knobs (button 8 = shift) or DIN MIDI CC 20/22; every control is printed over USB serial; firmware version prints at boot and on Shift + button 16 long-press.

See `CLAUDE.md` (project rules and file map), `docs/HARDWARE_MAP.md` and `docs/ARCHITECTURE.md`.

## Build
    make clean && make

Output: `build/005-PunchFx.bin`.

## Test (Mac-side, no hardware)
    make -C test

Runs the engine tests (MIDI clock tracking, the looper and its click/level checks) with the Mac's own compiler.

## Flash
Uses the external-USB bootloader (flash once with `make program-boot` over the Seed's own USB port, or via https://flash.daisy.audio/ choosing "v6.x (external)").

Then connect USB-C to the devkit's port, press RESET then BOOT on the Seed (USR LED pulses slowly), and run:

    make program-dfu

## Serial monitor
Connect to the devkit's USB serial port. You will see `Page Opened!`, `B1 Pressed`, `K2 Loop length - 4 bars`, `Transport Start`, `Bar 1 Beat 1`, `Tempo 120.0 BPM`, `Loop ARMED 4 bars, bar 5`, `Loop RECORDING 4 bars`, `Shift + K1 Input gain - +6.0 dB`, `CC 20 Input gain - +6.0 dB`.
