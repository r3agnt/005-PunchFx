# Layer 2 design: punch-in fx (current behaviour + what is next)

Read this when working on layer 2 (`src/engine/fx/`). Controls and log text: `docs/HARDWARE_MAP.md`. Signal chain: `docs/ARCHITECTURE.md`. Old text: `docs/archive/`.

## Rules for every fx (layer 2 = 3-way toggle in the middle)
- Layer 2 sits AFTER the Quantized loop buffer (post input, post loop; not recorded into the loop). 15 slots: B1-B7 = slots 0-6, B9-B16 = slots 7-14, B8 = Shift. The looper / beat-repeat buttons are layer 1 only.
- **Trigger:** hold = on, Shift + button = latch (one latch at a time, Shift + same = off). Start/stop land on the next 1/16 line (at once with no MIDI clock). Newest button wins. A tap shorter than 1/16 still plays one 1/16. A latch keeps running when you change layer.
- **One insert engine runs at a time**, 5 ms crossfade when it changes. Last button pressed = the EDIT SLOT. 2-way toggle: EDIT = knobs edit that slot; PERF = knobs locked. Soft takeover on every knob.
- **Shared stage** (`fx_common.h`), per slot, in series on the whole mix: wet/dry (K6) -> hipass (K7, 20 Hz-2 kHz, bottom = off) -> lopass (K8, 20 kHz-200 Hz, top = off) -> gain (K5, +-12 dB). Filters start primed (no thump). K1-K4 = the fx's own params (`Engine::KnobName/KnobReal/KnobUnit/SetDefaults`).
- **Settings** per slot = 10 values (`SlotSettings`: 8 knobs + `kSendReverb` + `kSendDelay`), saved to flash (`saved_state.h`, offset 0x100000, magic = layout version) when the switch goes edit -> perf, loaded at boot; Shift held at power-up resets. Change the layout = change `SavedState::kMagic`.
- **Memory rule:** objects in SDRAM (`DSY_SDRAM_BSS`) must have NO constructor (a constructor froze the Seed in 0.12.0; guarded by `static_assert(is_trivially_default_constructible)`), are not zeroed, and need every member set in `Init()`. DTCM (fast RAM) is ~68% full: big buffers go to SDRAM.

## Reverb (B1) = a BUS / SEND (`fx_reverb_bus.h`, real core `src/modules/reverb_core.h` = DaisySP ReverbSc, LGPL on)
- Pressing B1 sends the layer input into the reverb. The dry is never ducked, so letting go changes nothing except the input stopping: the tail rings out. The bus runs until the tail is under -80 dB for 0.3 s, then switches itself off.
- K1 Time = tail length 0.4-12 s in even steps (feedback = 10^(-3 * 0.0628 / seconds); 0.0628 s = mean ReverbSc delay line). K2 Tone = damping 800 Hz-18 kHz (default 4686 Hz). K3 Pre-delay 0-100 ms (default 0). K4 free (shimmer later).
- K6 = Return level (default 40%). K5 gain lifts dry + return while the reverb is on. K7 hipass filters the return. **K8 lopass filters the reverb INPUT** (not the dry, not the return). Fixed +8 dB make-up gain (`kMakeup`). Fixed input diffusion: 4 allpass per side (~19 ms) so ReverbSc's sparse bright first echoes (8 lines of 40-86 ms) are blurred.
- **Sends:** in edit mode Shift + K1 on any other fx = how much of its output goes into the reverb (`kSendReverb`). It runs while that fx is on, with the reverb slot's settings as its return.
- Known: held input builds up at long Time (steady level +5 dB from 5.7 s to 21 s tail); no level compensation yet. CPU with the reverb not recorded yet (open item in HARDWARE_TEST).

## Bit crusher (B7, `fx_bitcrush.h`)
Mono sum -> K1 pre lopass -> K2 noise (scaled to the quantiser step) -> K3 bit depth 12..1 (curved) -> K4 sample rate 48 kHz..188 Hz (octave steps) -> fixed mild stereo flanger (`fx_flanger_core.h` MiniFlanger: 0.25 Hz, 1-2 ms, mix 0.4, feedback 0.3, 7 kHz damping). Parked: bit depth "not right yet", flanger fine-tuning.

## NEXT: delay bus (B2) - to be agreed in the next session (these are PROPOSALS, ask first)
- Same pattern as the reverb: a send, tail (repeats) rings out after release, auto-off when silent, K6 = return level, Shift + K2 on other fx = delay send (`kSendDelay` already exists in `SlotSettings`; Shift + K2 is unassigned in the UI until built), delay memory in SDRAM (no-constructor rule), plain-C++ engine + Mac test (`ReverbBus` is the model).
- Ask: tempo-synced times from the MIDI clock (1/4 ... 1/32, dotted?) or free milliseconds; max time; K1-K4 choice (suggest K1 time, K2 feedback, K3 tone in the feedback path, K4 ping-pong/stereo spread); does the feedback repeat get the K7/K8 filters?

## Later fx (planned params)
Reverse (B3): recent audio backwards from the quantised position, no pitch. Pitch down (B4) / pitch up (B5, must be tight; short-grain shifter ~5-10 ms latency) / freq shifter (B6, Hilbert + quadrature osc). Res hipass sweep (B9): K1 freq, K2 res, K3 LFO depth, K4 ramp LFO speed 2 bars..1/32, LFO locked to the bar or the trigger. Res lowpass (B10), distortion + prefilter (B11), flanger (B12, reuse MiniFlanger), jet flanger (B13), phaser (B14), comb (B15), tremolo chop / tight gate (B16). Shimmer = pitch shifter in the reverb feedback (K4). Later: stacked inserts + snapshots, layer 3, SD-card snapshots.
