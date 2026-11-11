# Hardware test checklist

Tick items off and note the version tested and the date.

Version tested: ______   Date: ______

## Boot and serial
- [ ] Serial monitor shows `Firmware version X.Y.Z` at boot (version matches `src/version.h`)
- [ ] Shift + button 16 long press (1.5 s) prints `Shift + B16 Long press - Firmware version X.Y.Z` and does NOT stutter
- [ ] Every button prints `B<n> Pressed` / `Released`
- [ ] Boot log shows `Looper: max 8 bars, min 130 BPM, 4/4` and the buffer sizes
- [ ] Seed LED blinks

## Shift and soft takeover
- [ ] Holding button 8 prints `Shift on`; releasing prints `Shift off`
- [ ] Moving a knob far from its current value logs `... (pickup)` and the sound does not jump
- [ ] Turning the knob through the current value engages it (`(pickup)` disappears)
- [ ] Shift + K1 sets input gain, Shift + K2 output level

## MIDI clock and transport (DIN, from a DAW or drum machine)
- [ ] With no clock connected: LEDs 9 and 16 blink slowly
- [ ] Start sending clock (transport stopped): log `Clock detected`; bottom row glows dimly
- [ ] Press Play: `Transport Start`, then `Bar 1 Beat 1`, `Bar 1 Beat 2` ... on every beat; the bottom LEDs step 9..16 once per bar (two LEDs per beat), LED 9 on the downbeat
- [ ] Tempo log matches the DAW tempo within about 0.5 BPM (try 60, 100, 120, 140)
- [ ] Top row: K2 sets how many LEDs are lit dimly (1 / 2 / 4 / 8); the bright one steps once per bar and wraps at the cycle length
- [ ] Stop: `Transport Stop`; counters freeze; Play again restarts at `Bar 1 Beat 1`
- [ ] Continue (if the DAW sends it): logged as ignored
- [ ] Unplug or stop the clock: `Clock lost - transport stopped` after about 0.5 s
- [ ] Change tempo in the DAW while playing: tempo log follows within a bar
- [ ] Bar counting stays in step with the DAW's bar counter over several minutes (no drift)

## Looper (0.3.0)  (send MIDI clock from a DAW with a steady beat, e.g. a drum loop, into the audio input)
- [ ] K2 changes the loop length immediately (no `(pickup)` in the log) and LEDs 1-8 show LEDs 1..N lit for about 1.5 s after the last movement; Shift + K2 still uses pick-up
- [ ] B1 with no clock connected: `B1 ignored - no MIDI clock`
- [ ] Play, press B1: `Loop ARMED ...`, Seed LED and LED 1 flash slowly; at the next cycle start `Loop RECORDING`, Seed LED solid, top row fills up
- [ ] At the following cycle start `Loop PLAYBACK`: **no click, no level jump** at the changeover (listen on headphones/monitors)
- [ ] The loop is in time with the DAW beat over several minutes (no drift), and the loop seam (every cycle) is inaudible
- [ ] While the loop plays, the live input is not heard; LED 16 glows
- [ ] B2: switches to live input and back with no click; going back, the loop is still in time with the beat
- [ ] Try loop lengths 1, 2, 4, 8 bars (K2) with correct start bars (`Loop ARMED 4 bars, bar 5` etc.); changing K2 during playback does not change the playing loop
- [ ] B1 while playing: `Loop RE-ARMED`; at the next cycle the live signal is heard while recording, then the new loop plays
- [ ] B1 again while armed cancels (`Arm cancelled`); B1 while recording is ignored
- [ ] Shift + B1: `Loop cleared`, live signal returns with no click; works from every state
- [ ] Arm while the DAW is stopped, press Play: recording starts at bar 1
- [ ] Press Stop in the DAW while recording: `Rec aborted`; while playing: `Loop held`, live signal heard; Play again: loop restarts at bar 1
- [ ] Tempo changes in the DAW while a loop plays: loop re-aligns at the next cycle (listen for any click)
- [ ] Note the audible alignment between DAW audio and loop start (the input offset on Shift + K3 only moves stutter slices; applying it to loop starts is a possible later change)

## Beat repeat (0.4.0 / 0.4.1)  (clock running, a drum loop with a clear snare on the audio input)
- [ ] Hold B12 (1/8): LED flashes, `Stutter 1/8 waiting`, then at the next eighth `Stutter 1/8 ON` and the last eighth repeats (a whole hit, never half a snare), LED glows steadily
- [ ] Release: back to live immediately, no click, `Stutter OFF`
- [ ] Try all eight divisions on B9-B16: 1 bar, 1/2, 3/8, 1/4, 1/8, 1/16, 1/32, 1/64 (1/64 should sound like a very fast buzz; 3/8 is a dotted-quarter feel, a repeat every 1.5 beats that slides against the bar and realigns every 3 bars)
- [ ] Hold 1 bar for more than 2 bars: the repeat never changes or glitches
- [ ] Hold B12 (1/4), then also B14 (1/16): `Stutter 1/16 waiting` / `ON`, the newest takes over at its own boundary
- [ ] Release B14 (still holding B12): at once `Stutter 1/4 ON (stored)`, and you hear the SAME 1/4 slice as before (not a new one), in time
- [ ] Press B14 again (B12 still held): `ON (stored)`, the same 1/16 slice as before; flip back and forth several times: always the original slices, no new waiting, no clicks
- [ ] Release B12 while B14 is held: nothing changes; release B14: live, `Stutter OFF`; press B12 again: it waits and captures a NEW slice
- [ ] Hold a 1 bar slice for several bars, flip to another division and back: the 1-bar slice is still the original one
- [ ] Press while the DAW is stopped: `Stutter ignored - not running`, nothing happens
- [ ] **Input offset (Shift + K3):** with a snare, find the offset where the repeat starts exactly on the attack (too negative = tail of the previous sound at the start, too positive = the attack is cut); note the value for your setup
- [ ] Hold a stutter while a loop is recording, then play the loop back: the stutter is in the loop; with the loop playing from the buffer, holding a stutter changes nothing you hear (B2 to live to hear it)
- [ ] No clicks at any press, release or division change
- [ ] Tempo below the 130 BPM minimum (e.g. 100 BPM): B1 with 8 bars logs `B1 ignored - tempo too slow` but 4 bars records; a 1-bar or 1/4 stutter logs `Stutter ignored - tempo too slow` while 1/16 still works
- [ ] At your usual fast tempos (130+): an 8-bar loop records, and every stutter division works

## Audio
- [ ] Defaults (nothing touched): sounds like the input, no hiss/clicks
- [ ] Loud input at maximum input gain never exceeds 0.9 (-0.9 dBFS) on a DAW meter
- [ ] Shift + K2 turns the output down smoothly to silence

## MIDI (DIN)
- [ ] CC 20, 21 (loop length), 22, 23 (input offset) and 24 (slice pitch) move the right parameter and log `CC <n> <name> - <value>`
- [ ] Works on any MIDI channel
- [ ] After a CC changes a value, the matching knob must be turned through it before it takes over

## Notes

## 0.8.1 checks (reverse switch)
- [ ] B6 + a slice in slice mode with reverse OFF (the power-up default): the slice plays forward at the raised pitch, restarting every eighth (compare with 0.7.2): in time, no click at the repeat; K4 changes the pitch
- [ ] Shift + B6: `Pitch reverse ON` (and the press does not switch B6 on by itself); the held slice becomes the forward-then-backwards ping-pong on the next eighth; Shift + B6 again: `Pitch reverse OFF`
- [ ] Which do you prefer, forward only or ping-pong? Forward only at high pitches reads past the slice into the next one (silence on the last slice at +12): is that OK?

## 0.8.0 checks (slice mode: bar-line reset, pitch knob K4)
- [ ] Hold a slice button across the bar line (the end of the Slice loop buffer): at the bar line the slice is cut off and the plain loop restarts from beat 1; it does NOT come back while you keep holding; let go and press again: it plays
- [ ] Release a slice mid-bar: the bar carries on from the next slice up to the bar line, then restarts from beat 1 (slice 1 on the downbeat), in time
- [ ] A slice button pressed just before the bar line plays on the bar line and is not choked; no clicks at the choke
- [ ] K4 (no Shift) sets the B6 ping-pong pitch, -12..+12 semitones (log `K4 Slice pitch (B6) - +3 st`; soft takeover: turn it through +3 first); turning it while a ping-pong slice is held changes the pitch on the next eighth without clicks; CC 24 does the same
- [ ] Is the knob range/steps what you want? (whole semitones; `kSliceFxMinSemitones/Max` in `looper_config.h`)

## 0.7.2 checks (B6 in slice mode: +3 ping-pong) - slice mode on a drum loop
- [ ] Hold a slice button (B9-B16) and B6: `Pitch fx ON (+3 ping-pong)`, LED 6 on; the slice plays 3 semitones up, forward then backwards, repeating every eighth, in time (no stutter, no flam, no clicks at the turn)
- [ ] Slice presses and B6 land on the 1/8 line (not 1/16); B6 pressed while a slice is held restarts it pitched on the next 1/8 line, B6 released: back to the normal repeat on the next 1/8 line
- [ ] Release the slice: the bar carries on from the next slice at normal pitch; B6 with no slice held does nothing audible; B6 while slice mode is off does nothing
- [ ] Is +3 right, and do you like the forward/reverse sound? (change `kSliceFxSemitones` in `looper_config.h`; try a different length for the forward part if it should be longer)
- [ ] CPU line every 30 s (`CPU avg .. max ..`): note the max with slice mode + B6 + a stutter + loop recording; anything near 100% is a dropout risk

## 0.6.0 checks (B6 pitch-down on the beat repeat) - clock running, a drum loop on the input
- [ ] Hold B12 (1/8) and B6: `Pitch fx ON` at the next 1/16 line (LED 6 flashes, then stays on); repeat 1 sounds normal, repeat 2 is one semitone lower, repeat 3 two lower ... (listen for the steps on a drum loop or a note)
- [ ] Release B6: `Pitch fx OFF` on a 1/16 line, the repeats go back to normal pitch
- [ ] B6 held first, then a stutter button: the stutter starts normal and steps down from its first repeat ("armed")
- [ ] Release the stutter and press again: the steps start again from normal; changing division (flipping buttons) also restarts
- [ ] 0.6.1: is the flam gone? The hit at the start of every shifted repeat should land on the beat. Is the shifted sound acceptable (grainy/warbly)? Try 1/4, 1/8, 1/16, 1/32, 1/64; note which ones sound bad
- [ ] 0.6.2: slow repeats (1/4, 3/8, 1/2, 1 bar) held for 8+ repeats: do the hits inside the repeat stay in time? Is the tone too grainy/choppy now (a splice every 20-180 ms)? If too grainy raise `kPitchMaxDelayFrames` / `kPitchHopFrames` (e.g. 960 / 960) for a middle ground
- [ ] No clicks at B6 on/off, and no audio dropouts (CPU): watch for glitches while holding B6 + a stutter + the loop recording; note it if so
- [ ] Shift + B6 does nothing; B6 while the DAW is stopped does nothing (B6 in slice mode: see the 0.7.0 checks)
- [ ] Hold B6 + stutter while recording a loop (B1): the pitched repeats are in the loop

## 0.5.3 check
- [ ] Tap or hold a slice button and release: that slice sounds once (held length), then the loop carries on with the NEXT slice (no double)

## 0.5.2 checks (slice mode: 1 bar captured on a bar line)
- [ ] Press B7 mid-bar: LED 7 flashes fast, at the next BAR line `Slice mode ON`; the loop you hear is the bar that just ended
- [ ] B9 = beat 1 of that bar (the kick of your drum loop if it is on the downbeat), B16 = the last eighth; the loop restarts on beat 1 every bar, in time
- [ ] Press B7 within the first bar after Start or right after leaving slice mode: it waits (LED 7 flashing), then switches at a later bar line: never silence
- [ ] Press B7 again while waiting: cancels (`Slice mode OFF` is not logged; LED 7 stops flashing)
- [ ] Shift + B7 logs `Bank ignored - 1 bar only` and nothing else
- [ ] Slice release: the bar loops from the slice you pressed; no clicks (including at the wrap)

## 0.5.0 checks (slice mode on B7; Slice loop buffer) - clock running, a drum loop on the input
- [ ] B7: LED 7 flashes fast until the next 1/8 line, then stays on; log `Slice mode ON`; you now hear the last 2 bars as a loop, in time with the beat, with no click at the start
- [ ] The loop is the audio from 2 bars ago (it does not follow what you play now); it stays in time with the DAW for several minutes (no drift)
- [ ] Buttons 9-16 (bank 1 = older bar): each plays one eighth of the 2 bars and repeats while held; starts on the next 1/8 line; log `Slice n`
- [ ] Release (0.5.1): the whole 2 bars loop starting at the slice you pressed (slice 3 -> 4 ... 16 -> 1, 2 ...), starting on the next 1/8 line, in time (`Slice loop`); no clicks, including where the window wraps. Press another slice: it repeats, then the loop restarts from that one
- [ ] Shift + B7: bank 2 (LED 7 pulses) = the newer bar; `Slice bank 2`; Shift + B7 again: bank 1
- [ ] Two buttons held: newest wins, release it: falls back to the other one
- [ ] A very short tap still plays one eighth
- [ ] B7 again: back to normal at the 1/8 line (Quantized loop buffer if one is playing, otherwise live); then the beat repeat (B9-B16) works normally with a full history
- [ ] Quantized loop buffer playing (B1 loop), then B7: you hear the slice mode instead of the loop; B7 off: the loop is back in time
- [ ] Record a loop (B1) while playing slices: the slices end up in the loop (play it back after B7 off)
- [ ] In slice mode buttons 9-16 do not stutter and Shift + B9-B16 does not latch
- [ ] Stop the DAW in slice mode: slice mode ends at once; B7 while stopped logs `B7 ignored - not running`
- [ ] Shift + B7 outside slice mode logs `Bank ignored - no slice mode`
- [ ] Does anything glitch or stutter in the audio (CPU)? Note it if so (slice mode adds a second buffer and mixing in the audio callback)

## 0.4.5 checks (press and release quantised to 1/16)
- [ ] Press a stutter button a little early or late against a beat: the stutter starts exactly on the next 1/16, in time with the groove (up to about 115 ms delay at 130 BPM is expected)
- [ ] The button's LED flashes between the press and the 1/16 line, then glows
- [ ] Release mid-way: the repeat carries on until the next 1/16 line, then back to live with no click
- [ ] A very short tap (shorter than the wait) still gives one 1/16 of repeat
- [ ] Try 1 bar, 1/4, 1/8, 1/32, 1/64: the repeated slice has the length of the button; check that the *start* of the slice sounds right (it now ends on a 1/16 line instead of on the division's own grid)
- [ ] Latch (Shift + B9-B16) and flipping between held buttons also land on the 1/16 line
- [ ] Transport Stop is still immediate
- [ ] Decide if 1/16 is the right grid (try 1/32 by changing `kQuantizeDen` to 32 if it feels too late)

## 0.4.3 / 0.4.4 checks
- [ ] Shift + B9-B16 latches a division; Shift + same button turns it off; Shift + another moves the latch; a held press overrides it; Transport Stop clears it.
- [ ] Held stutter LED is dim, latched one is brighter.
- [ ] 1 bar and 1/2: the first pass has no glitch (0.4.4 copy-chunk fix). If it still glitches, note whether it is at the start or at the first wrap, and a click or a dropout.
