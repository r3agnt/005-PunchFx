/** 005-PunchFx : audio looper prototype (Desktop DevKit + Daisy Seed)
 *
 *  What this program does right now:
 *   - Starts the hardware (knobs, buttons, LEDs, audio).
 *   - Prints the firmware version over USB serial at start-up.
 *   - Follows MIDI clock and transport (Start/Stop) from the DIN input and
 *     keeps count of bars, beats and eighth-notes (src/engine/clock_tracker.h).
 *   - Runs audio through: input gain -> beat repeat -> looper -> soft clip ->
 *     output level (ceiling 0.9). The beat repeat (src/engine/beat_repeat.h)
 *     stutters the input while buttons 9-16 are held (1 bar ... 1/64). The looper (src/engine/quantized_loop_buffer.h) records and plays
 *     back loops on the bar grid: B1 arm/record, B2 loop <-> live, Shift+B1 clear.
 *   - Reports every button press and knob move over USB serial.
 *   - Knob and MIDI CC assignments are tables in params.h. Button 8 = SHIFT.
 *
 *  Three things run "at the same time". Keep them separate:
 *   1. AudioCallback        - runs constantly, fast. Audio processing goes here.
 *                             Must be quick: no printing, no waiting.
 *   2. LowPriorityCallback  - runs 250 times per second. Handles button/knob
 *                             events and LEDs (see userinterface.h, main_page.h).
 *   3. main() while loop    - runs whenever there is spare time. Prints log text.
 *
 *  See docs/ARCHITECTURE.md for a longer explanation.
 */
#include <cmath>
#include "desktop_devkit.h"
#include "userinterface.h"
#include "params.h"
#include "version.h"
#include "param_info.h"
#include "modules/gain_stage.h"
#include "engine/beat_repeat.h"
#include "engine/clock_tracker.h"
#include "engine/quantized_loop_buffer.h"
#include "engine/looper_config.h"
#include "engine/pitch_fx.h"
#include "engine/fx/fx_layer.h"
#include "engine/fx/fx_bitcrush.h"
#include "engine/saved_state.h"
#include "engine/fx/fx_reverb_bus.h"
#include "modules/reverb_core.h"
#include "util/PersistentStorage.h"

using namespace daisy;
using namespace daisy::desktop_devkit;

/** The hardware: knobs, buttons, LEDs, audio codec. */
Hardware hw;

/** The user interface: turns knob/button changes into events. */
Ui ui;

/** Parameter values shared between the UI and the audio code (see params.h). */
Params params;

/** Input gain and output ceiling (src/modules/gain_stage.h). */
GainStage gain_stage;

/** Follows MIDI clock and transport (see src/engine/clock_tracker.h). */
ClockTracker clock_tracker;

/** Real-time MIDI messages (clock ticks, Start, Stop, Continue) wait here
 *  until the audio callback picks them up. */
RtEventQueue midi_rt_queue;

/** The looper (src/engine/quantized_loop_buffer.h). Its two buffers live in the
 *  Seed's 64 MB external SDRAM (about 9 MB each). */
QuantizedLoopBuffer quantized_loop;
DSY_SDRAM_BSS static float loop_buf_l[looper::kLoopCapacityFrames];
DSY_SDRAM_BSS static float loop_buf_r[looper::kLoopCapacityFrames];

/** The beat repeat (src/engine/beat_repeat.h): a 2-bar history of the input and
 *  one stored slice per division (up to 8), all in SDRAM (about 1.4 + 1.8 MB at the 130 BPM minimum). */
BeatRepeat beat_repeat;

/** B6 pitch-down effect on the beat repeat output (src/engine/pitch_fx.h). */
static PitchFx pitch_fx; // on the beat repeat output 

/** Which division is latched on by Shift + B9-B16 (-1 = none). Written by the
 *  audio callback, read by the LEDs (main_page.h). */
volatile int8_t stutter_latch = -1;
DSY_SDRAM_BSS static float hist_buf_l[looper::kHistoryCapacityFrames];
DSY_SDRAM_BSS static float hist_buf_r[looper::kHistoryCapacityFrames];
DSY_SDRAM_BSS static float frz_buf_l[looper::kFreezeCapacityFrames];
DSY_SDRAM_BSS static float frz_buf_r[looper::kFreezeCapacityFrames];

/** LAYERS (3-way switch): 0 = layer 1 (looper + beat repeat), 1 = layer 2 (punch-in fx),
 *  2 = layer 3 (not built yet). Written by the audio callback, read by the UI. */
volatile uint8_t current_layer = 0;
/** 2-way switch: true = EDIT mode (knobs change the last-triggered fx), false = PERF mode
 *  (knobs locked). If the switch works the wrong way round, flip this one constant. */
static constexpr bool kEditWhenSwitchPressed = true;
volatile bool         fx_edit_mode           = false;

/** Layer 2: the punch-in fx (src/engine/fx/). fx_settings = the knob values of every fx
 *  (the UI edits them in edit mode, the audio reads them). */
fx::Layer        fx_layer;
fx::SlotSettings fx_settings[fx::kNumSlots];
static fx::BitCrush fx_bitcrush; // slot 6 = button 7
/** The reverb (slot 0 = button 1): a bus. ReverbSc itself is big (395 KB), so it lives in SDRAM too. */
DSY_SDRAM_BSS static daisysp::ReverbSc sc_reverb;
static ScReverbCore  reverb_core;
DSY_SDRAM_BSS static fx::ReverbBus reverb_bus; // pre-delay memory (131 KB) is too big for the fast RAM
static fx::ReverbSlot fx_reverb_slot;

/** SAVING (0.11.0): the fx settings and the four layer 1 settings are kept in the Seed's flash
 *  memory (QSPI) with libDaisy's PersistentStorage. It is written when the 2-way switch goes
 *  from EDIT to PERF (audio code sets the flag, the main loop does the slow write) and read
 *  at power-up. Hold SHIFT (B8) while powering up to wipe it back to defaults.
 *  Offset 1 MB: clear of the program (it lives from 0x40000) and of the first 256 KB that
 *  Electrosmith keeps free. */
static constexpr uint32_t kStorageOffset = 0x100000;
static PersistentStorage<SavedState> storage(hw.seed.qspi);
volatile bool fx_save_request = false;

/** Copy the live settings into a SavedState (what gets written). */
static SavedState CaptureState()
{
    SavedState s;
    s.magic        = SavedState::kMagic;
    s.input_offset = params.input_offset;
    s.loop_length  = params.loop_length;
    s.input_gain   = params.input_gain;
    s.out_level    = params.out_level;
    for(int i = 0; i < fx::kNumSlots; i++)
        s.fx[i] = fx_settings[i];
    return s;
}

/** Put saved values back into the live settings. */
static void ApplyState(const SavedState &s)
{
    params.input_offset = s.input_offset;
    params.loop_length  = s.loop_length;
    params.input_gain   = s.input_gain;
    params.out_level    = s.out_level;
    for(int i = 0; i < fx::kNumSlots; i++)
        fx_settings[i] = s.fx[i];
}

/** Button commands (B1, B2) waiting for the audio callback. */
SpscQueue<LoopCmd, 16> loop_cmd_queue;

/** Last tempo that was printed, so we only print when it changes. */
static float last_logged_bpm = 0.0f;

/** A queue of short messages waiting to be printed over USB serial.
 *  The audio callback may add messages (PushBack) but must never print
 *  directly. The main() loop below does the actual printing.
 *  Example (inside AudioCallback):  msg_fifo.PushBack("hello");
 */
FIFO<FixedCapStr<32>, 16> msg_fifo;

/** Called by libDaisy the moment a MIDI real-time message arrives
 *  (only real-time messages come here; CCs etc. go through the normal queue).
 *  It must be quick: it only drops the message in a queue. */
void MidiRealtimeCallback(MidiEvent &msg)
{
    if(msg.type != SystemRealTime)
        return;
    switch(msg.srt_type)
    {
        case TimingClock: midi_rt_queue.Push(RtEvent::Tick); break;
        case Start: midi_rt_queue.Push(RtEvent::Start); break;
        case Stop: midi_rt_queue.Push(RtEvent::Stop); break;
        case Continue: midi_rt_queue.Push(RtEvent::Continue); break;
        default: break; // active sensing etc. are ignored
    }
}

/** Queue one line of text for the serial log (safe to call from audio). */
static void Say(const FixedCapStr<32> &line)
{
    msg_fifo.PushBack(line);
}

/** Turn the clock tracker's events into log lines. Called every audio block. */
static void LogClockEvents(uint32_t ev)
{
    if(ev == 0)
        return;

    if(ev & kEvClockFound)
        Say(FixedCapStr<32>("Clock detected"));
    if(ev & kEvClockLost)
    {
        Say(FixedCapStr<32>("Clock lost - transport stopped"));
        last_logged_bpm = 0.0f;
    }
    if(ev & kEvStart)
        Say(FixedCapStr<32>("Transport Start"));
    if(ev & kEvStop)
        Say(FixedCapStr<32>("Transport Stop"));
    if(ev & kEvContinueIgnrd)
        Say(FixedCapStr<32>("Transport Continue (ignored)"));

    if(ev & kEvBeat)
    {
        const ClockStatus st = clock_tracker.GetStatus();
        FixedCapStr<32>   line("Bar ");
        line.AppendInt(st.bar + 1);
        line.Append(" Beat ");
        line.AppendInt(st.beat + 1);
        Say(line);

        // Tempo: print when it has moved by more than 0.5 BPM.
        const float bpm = clock_tracker.Bpm();
        if(bpm > 0.0f && std::fabs(bpm - last_logged_bpm) > 0.5f)
        {
            last_logged_bpm = bpm;
            FixedCapStr<32> t("Tempo ");
            t.AppendFloat(bpm, 1);
            t.Append(" BPM");
            Say(t);
        }
    }
}

/** Turn the looper's events into log lines. Called every audio block. */
static void LogLoopEvents()
{
    LoopEvent e;
    while(quantized_loop.PopEvent(e))
    {
        FixedCapStr<32> s;
        switch(e.type)
        {
            case LoopEventType::Armed:
            case LoopEventType::ReArmed:
                s.Append(e.type == LoopEventType::Armed ? "Loop ARMED " : "Loop RE-ARMED ");
                s.AppendInt(e.a);
                s.Append(" bars, ");
                if(e.b > 0)
                {
                    s.Append("bar ");
                    s.AppendInt(e.b);
                }
                else
                    s.Append("at Start");
                break;
            case LoopEventType::ArmCancelled: s.Append("Arm cancelled"); break;
            case LoopEventType::RecordStart:
                s.Append("Loop RECORDING ");
                s.AppendInt(e.a);
                s.Append(" bars");
                break;
            case LoopEventType::PlaybackStart:
                s.Append("Loop PLAYBACK ");
                s.AppendInt(e.a);
                s.Append(" bars (");
                s.AppendInt(e.b);
                s.Append(")");
                break;
            case LoopEventType::RecordAborted:
                s.Append(e.a == kAbortTooLong ? "Rec aborted: loop too long"
                                              : "Rec aborted: transport stopped");
                break;
            case LoopEventType::Cleared: s.Append("Loop cleared"); break;
            case LoopEventType::SourceBuffer: s.Append("Source: BUFFER"); break;
            case LoopEventType::SourceInput: s.Append("Source: INPUT"); break;
            case LoopEventType::Held: s.Append("Loop held (transport stopped)"); break;
            case LoopEventType::Refused:
                switch(e.a)
                {
                    case kRefuseNoClock: s.Append("B1 ignored - no MIDI clock"); break;
                    case kRefuseTooSlow: s.Append("B1 ignored - tempo too slow"); break;
                    case kRefuseRecording: s.Append("B1 ignored - recording"); break;
                    default: s.Append("B2 ignored - no loop"); break;
                }
                break;
        }
        Say(s);
    }
}

/** "1 bar", "1/2", "3/8" ... "1/64" (div = division index) */
static void AppendDivision(FixedCapStr<32> &s, uint32_t div)
{
    s.Append(looper::DivisionLabel(div));
}

/** Turn the beat repeat's events into log lines. Called every audio block. */
static void LogBeatEvents()
{
    BeatEvent e;
    while(beat_repeat.PopEvent(e))
    {
        FixedCapStr<32> s;
        switch(e.type)
        {
            case BeatEventType::Waiting:
                s.Append("Stutter ");
                AppendDivision(s, e.div);
                s.Append(" waiting");
                break;
            case BeatEventType::On:
                s.Append("Stutter ");
                AppendDivision(s, e.div);
                s.Append(" ON");
                break;
            case BeatEventType::OnStored:
                s.Append("Stutter ");
                AppendDivision(s, e.div);
                s.Append(" ON (stored)");
                break;
            case BeatEventType::Off: s.Append("Stutter OFF"); break;
            case BeatEventType::IgnoredNotRunning: s.Append("Stutter ignored - not running"); break;
            case BeatEventType::IgnoredTooShort: s.Append("Stutter ignored - too short"); break;
            case BeatEventType::IgnoredTooSlow: s.Append("Stutter ignored - tempo too slow"); break;
            case BeatEventType::PitchOn: s.Append("Pitch fx ON"); break;
            case BeatEventType::PitchOff: s.Append("Pitch fx OFF"); break;
        }
        Say(s);
    }
}

/** Turn the punch-in fx events into log lines. Called every audio block. */
static void LogFxEvents()
{
    fx::Event e;
    while(fx_layer.PopEvent(e))
    {
        FixedCapStr<32> s;
        const int       button = e.slot < 7 ? e.slot + 1 : e.slot + 2; // slots 0-6 = B1-B7, 7-14 = B9-B16
        switch(e.type)
        {
            case fx::EventType::On:
                s.Append("FX B");
                s.AppendInt(button);
                s.Append(" ");
                s.Append(fx::SlotName(e.slot));
                s.Append(" ON");
                break;
            case fx::EventType::Off: s.Append("FX OFF"); break;
            case fx::EventType::LatchOn:
                s.Append("FX latch ");
                s.Append(fx::SlotName(e.slot));
                break;
            case fx::EventType::LatchOff: s.Append("FX latch OFF"); break;
            case fx::EventType::Edit:
                s.Append("FX edit: ");
                s.Append(fx::SlotName(e.slot));
                break;
            case fx::EventType::NotBuilt:
                s.Append("FX B");
                s.AppendInt(button);
                s.Append(" not built yet");
                break;
        }
        Say(s);
    }
}

/** Largest block the looper is given at once (libDaisy's default is 48). */
static constexpr size_t kMaxChunk = 128;
static_assert(kMaxChunk == fx::kMaxChunk, "the fx layer handles the same block size");

/** CPU load meter (libDaisy util/CpuLoadMeter.h): times each audio block with the chip's cycle
 *  counter (a couple of cycles per block, so it adds no real load). The main loop prints
 *  the result every 30 s: avg = smoothed average, max = the worst block in the 30 s,
 *  as a percent of the time one block has (100% = the audio would drop out). */
static CpuLoadMeter cpu_meter;

/** Runs for every small block of audio. `in` and `out` hold left/right samples:
 *  in[0][i] = left input,  in[1][i] = right input
 *  out[0][i] = left output, out[1][i] = right output
 */
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    cpu_meter.OnBlockStart();

    // Read all knobs/buttons/switches and create UI events from them.
    hw.UpdateAllControls();
    ui.GenerateEvents();

    // The two toggle switches: 3-way = layer (up = 1, middle = 2, down = 3), 2-way = edit / perf.
    uint8_t layer;
    {
        const int pos = hw.tog_sw18.Read();
        layer = pos == daisy::Switch3::POS_UP ? 0 : (pos == daisy::Switch3::POS_DOWN ? 2 : 1);
        const bool edit = hw.tog_sw17.Pressed() == kEditWhenSwitchPressed;
        if(layer != current_layer)
        {
            current_layer = layer;
            FixedCapStr<32> s("Layer ");
            s.AppendInt(layer + 1);
            Say(s);
        }
        if(edit != fx_edit_mode)
        {
            if(fx_edit_mode && !edit)
                fx_save_request = true; // leaving EDIT: save (the main loop does it)
            fx_edit_mode = edit;
            Say(FixedCapStr<32>(edit ? "Edit mode" : "Perf mode"));
        }
    }

    // Take the MIDI clock messages that arrived, advance the song position
    // by one block, and queue any log lines.
    clock_tracker.Advance(midi_rt_queue, size);
    const uint32_t clock_events = clock_tracker.TakeEvents();
    LogClockEvents(clock_events);

    // Tell the looper what the clock is doing, then apply button commands.
    const ClockStatus st = clock_tracker.GetStatus();
    LoopBlockInfo     info;
    info.clock_present   = st.present;
    info.running         = st.running;
    info.bar_start       = (clock_events & kEvBar) != 0;
    info.bar             = st.bar;
    info.samples_per_bar = clock_tracker.SamplesPerBar();

    quantized_loop.SetLoopBars(looper::LoopBarsFromControl(params.loop_length));
    LoopCmd cmd;
    while(loop_cmd_queue.Pop(cmd))
        quantized_loop.Command(cmd, info);

    // Beat repeat: which of buttons 9-16 are down (read straight from the
    // hardware each block: a long press would hide the release event).
    BeatBlockInfo beat_info;
    beat_info.running         = st.running;
    beat_info.bar             = clock_tracker.BarAtBlockStart();
    beat_info.pos_in_bar      = clock_tracker.PositionAtBlockStart(size);
    beat_info.samples_per_bar = clock_tracker.SamplesPerBar();
    beat_info.offset_samples  = looper::OffsetMsFromControl(params.input_offset)
                               * hw.seed.AudioSampleRate() * 0.001f;
    // Stutter buttons and the latch.
    //  - A stutter button pressed while Shift (B8) is held does NOT stutter
    //    while it is down; instead it toggles the LATCH: that division keeps
    //    repeating after you let go. Shift + the same button again turns it
    //    off; Shift + a different button moves the latch to that division
    //    (only one latch at a time).
    //  - A normal held press works as before and overrides the latch while
    //    it is down (on release the latched division carries on).
    //  - Transport stopped / clock lost: the latch is cleared.
    {
        static uint32_t prev_raw = 0, shift_blocked = 0;
        uint32_t        raw      = 0;
        for(size_t i = 0; i < looper::kNumDivisions; i++)
            if(hw.button[8 + i].Pressed())
                raw |= 1u << i;
        if(layer != 0)
            raw = 0; // the beat repeat belongs to layer 1 only
        const uint32_t newly = raw & ~prev_raw;
        if(hw.button[7].Pressed())
        {
            shift_blocked |= newly; // newly pressed while Shift is down
            for(size_t i = 0; i < looper::kNumDivisions; i++)
            {
                if(!(newly & (1u << i)) || !st.running)
                    continue;
                FixedCapStr<32> s;
                if(stutter_latch == static_cast<int8_t>(i))
                {
                    stutter_latch = -1;
                    s.Append("Stutter latch OFF");
                }
                else
                {
                    stutter_latch = static_cast<int8_t>(i);
                    s.Append("Stutter latch ");
                    AppendDivision(s, i);
                }
                Say(s);
            }
        }
        shift_blocked &= raw; // forget buttons that were released
        prev_raw = raw;
        if(!st.running || layer != 0)
            stutter_latch = -1;
        // B6 = pitch-down effect (hold). Like the stutter buttons, a B6 that is
        // pressed while Shift is held does nothing until it is released.
        static bool prev_b6 = false, b6_blocked = false;
        const bool  b6      = layer == 0 && hw.button[5].Pressed();
        if(b6 && !prev_b6 && hw.button[7].Pressed())
            b6_blocked = true;
        if(!b6)
            b6_blocked = false;
        prev_b6 = b6;
        beat_info.pitch_held = b6 && !b6_blocked;
        beat_info.held_mask = raw & ~shift_blocked;
        if(stutter_latch >= 0)
            beat_info.held_mask |= 1u << stutter_latch;
    }
    // Layer 2: which of its 15 buttons are down (B1-B7 = slots 0-6, B9-B16 = slots 7-14).
    fx::BlockInfo fx_info;
    fx_info.running         = st.running;
    fx_info.samples_per_bar = beat_info.samples_per_bar;
    fx_info.shift           = hw.button[7].Pressed();
    fx_info.buttons_active  = layer == 1;
    for(int slot = 0; slot < fx::kNumSlots; slot++)
        if(hw.button[slot < 7 ? slot : slot + 1].Pressed())
            fx_info.raw_mask |= static_cast<uint16_t>(1u << slot);

    // Pass the latest settings to the effects.
    gain_stage.SetControls(params.input_gain, params.out_level);

    // SIGNAL CHAIN (left = channel 0, right = channel 1):
    //   input gain -> beat repeat -> pitch fx -> Quantized loop buffer -> punch-in fx (layer 2) -> soft clip + output level + ceiling
    size_t done = 0;
    while(done < size)
    {
        const size_t n = (size - done) < kMaxChunk ? (size - done) : kMaxChunk;
        float        perf_l[kMaxChunk], perf_r[kMaxChunk];
        float        rep_l[kMaxChunk], rep_r[kMaxChunk];
        float        loop_l[kMaxChunk], loop_r[kMaxChunk];
        float        fx_l[kMaxChunk], fx_r[kMaxChunk];
        for(size_t i = 0; i < n; i++)
        {
            perf_l[i] = gain_stage.Gain(in[0][done + i]);
            perf_r[i] = gain_stage.Gain(in[1][done + i]);
        }
        beat_repeat.Process(beat_info, perf_l, perf_r, rep_l, rep_r, n);
        // B6 pitch effect: each repeat of the held slice is one semitone lower.
        pitch_fx.Process(rep_l, rep_r, n, beat_repeat.RepeatIndex(),
                         beat_repeat.PitchOn() && beat_repeat.Repeating(),
                         beat_repeat.JumpStartIndex());
        const float chunk_pos = beat_info.pos_in_bar;
        beat_info.pos_in_bar += static_cast<float>(n);
        quantized_loop.Process(info, rep_l, rep_r, loop_l, loop_r, n);
        info.bar_start = false; // the bar boundary belongs to the first chunk only
        // Layer 2 punch-in fx (after the looper). Passes the signal through when no fx is on.
        fx_info.pos_in_bar = chunk_pos;
        fx_layer.Process(fx_info, loop_l, loop_r, fx_l, fx_r, n);
        for(size_t i = 0; i < n; i++)
        {
            out[0][done + i] = gain_stage.Limit(fx_l[i]);
            out[1][done + i] = gain_stage.Limit(fx_r[i]);
        }
        done += n;
    }

    LogLoopEvents();
    LogBeatEvents();
    LogFxEvents();
    cpu_meter.OnBlockEnd();
}

/** Runs 250 times per second (every 4 ms). Handles UI events and LED updates. */
void LowPriorityCallback(void* data)
{
    ui.DoEvents();
}

/** Boot log: loop limits and buffer memory (for the Seed's 64 MB SDRAM). */
static void LogLooperConfig(float sr)
{
    const size_t loop_frames = looper::FramesForBars(looper::kMaxLoopBars, sr);
    const size_t hist_frames = looper::FramesForBars(looper::kHistoryBars, sr);
    const float  loop_mb = looper::BytesForFrames(loop_frames) / (1024.f * 1024.f);
    const float  hist_mb = looper::BytesForFrames(hist_frames) / (1024.f * 1024.f);

    FixedCapStr<64> a("Looper: max ");
    a.AppendInt(looper::kMaxLoopBars);
    a.Append(" bars, min ");
    a.AppendInt(static_cast<int>(looper::kMinBpm));
    a.Append(" BPM, 4/4");
    Logger<LOGGER_EXTERNAL>::PrintLine("%s", a.Cstr());

    FixedCapStr<64> b("Buffers (stereo float): loop ");
    b.AppendFloat(loop_mb, 1);
    b.Append(" MB, history ");
    b.AppendFloat(hist_mb, 1);
    b.Append(" MB of 64");
    Logger<LOGGER_EXTERNAL>::PrintLine("%s", b.Cstr());
}

int main(void)
{
    // Start the hardware and wait briefly for it to settle.
    hw.Init();
    System::Delay(50);

    // Start USB serial logging on the devkit's external USB port.
    // `false` = do not wait for a serial monitor to connect before running.
    Logger<LOGGER_EXTERNAL>::StartLog(false);

    // Tell the clock follower and the looper the sample rate.
    clock_tracker.Init(hw.seed.AudioSampleRate());
    quantized_loop.Init(loop_buf_l, loop_buf_r, looper::kLoopCapacityFrames, hw.seed.AudioSampleRate());
    pitch_fx.Init(hw.seed.AudioSampleRate());
    for(auto &s : fx_settings)
        s = fx::DefaultSettings();
    fx_layer.Init(hw.seed.AudioSampleRate(), fx_settings);
    reverb_core.Attach(&sc_reverb);
    reverb_bus.Init(hw.seed.AudioSampleRate(), &reverb_core);
    fx_layer.SetReverbBus(&reverb_bus, 0);
    fx_layer.SetEngine(0, &fx_reverb_slot); // button 1
    fx_layer.SetEngine(6, &fx_bitcrush); // button 7; the other slots come in later versions
    beat_repeat.Init(hist_buf_l, hist_buf_r, looper::kHistoryCapacityFrames,
                     frz_buf_l, frz_buf_r, looper::kFreezeCapacityFrames,
                     hw.seed.AudioSampleRate());

    // Modules that need the sample rate get it here, e.g.
    //   my_module.Init(hw.seed.AudioSampleRate());

    // Print the firmware version so you can see what is flashed.
    Logger<LOGGER_EXTERNAL>::PrintLine("Firmware version %s", FIRMWARE_VERSION);

    // Print the looper limits and how much memory its buffers will need.
    LogLooperConfig(hw.seed.AudioSampleRate());

    // Saved settings: read them from flash (or reset them if SHIFT is held at power-up).
    {
        // The buttons need a few readings before they are reliable (debounce).
        for(int i = 0; i < 12; i++)
        {
            hw.UpdateAllControls();
            System::Delay(2);
        }
        const SavedState defaults = CaptureState();
        storage.Init(defaults, kStorageOffset); // empty flash: writes the defaults now
        if(hw.button[7].Pressed())
        {
            storage.RestoreDefaults();
            Logger<LOGGER_EXTERNAL>::PrintLine("FX settings RESET to defaults (Shift held at power-up)");
        }
        else if(storage.GetSettings().IsValid())
        {
            ApplyState(storage.GetSettings());
            Logger<LOGGER_EXTERNAL>::PrintLine("FX settings loaded from flash");
        }
        else
            Logger<LOGGER_EXTERNAL>::PrintLine("FX settings: defaults (nothing valid saved)");
    }

    // Start listening for MIDI on the DIN input (without this no MIDI arrives).
    // Clock/transport messages go to MidiRealtimeCallback straight away; CCs
    // and other messages go through the normal queue handled below.
    hw.midi.StartReceiveRt(MidiRealtimeCallback);
    // USB MIDI is intentionally NOT enabled yet: the devkit USB port is used
    // for serial logging. See CLAUDE.md.

    // Start the UI, the 250 Hz UI callback, and audio.
    ui.Init(&hw);
    hw.StartLowPriorityCallback(LowPriorityCallback, 250);
    cpu_meter.Init(hw.seed.AudioSampleRate(), static_cast<int>(hw.seed.AudioBlockSize()));
    hw.seed.StartAudio(AudioCallback);

    uint32_t last_cpu_report = System::GetNow();
    while(true)
    {
        // Handle incoming DIN MIDI. Control Change messages set parameters.
        hw.midi.Listen(); // restarts MIDI receive if the UART hit an error
        while(hw.midi.HasEvents())
        {
            auto msg = hw.midi.PopEvent();
            if(msg.type == ControlChange)
            {
                auto cc = msg.AsControlChange();
                if(ApplyCC(cc.control_number, cc.value))
                    LogCc(cc.control_number, cc.value);
            }
        }

        // Save the settings if the switch just went from EDIT to PERF. Flash erase + write takes
        // a moment; it is done here (not in the audio code) and the audio keeps running.
        if(fx_save_request)
        {
            fx_save_request       = false;
            const SavedState now  = CaptureState();
            if(now == storage.GetSettings())
                Logger<LOGGER_EXTERNAL>::PrintLine("FX settings unchanged");
            else
            {
                storage.GetSettings() = now;
                storage.Save();
                Logger<LOGGER_EXTERNAL>::PrintLine("FX settings saved");
            }
        }

        // Print any messages the audio callback has queued up.
        while(!msg_fifo.IsEmpty())
        {
            auto msg = msg_fifo.PopFront();
            Logger<LOGGER_EXTERNAL>::PrintLine(msg.Cstr());
        }

        // Every 30 s: print the CPU load of the audio callback (see cpu_meter above), then
        // start a new measuring period. (Reading from here while the audio callback is
        // writing is harmless: they are single numbers and only used for this printout.)
        if(System::GetNow() - last_cpu_report >= 30000)
        {
            last_cpu_report = System::GetNow();
            FixedCapStr<48> c("CPU avg ");
            c.AppendInt(static_cast<int>(cpu_meter.GetAvgCpuLoad() * 100.0f + 0.5f));
            c.Append("% max ");
            c.AppendInt(static_cast<int>(cpu_meter.GetMaxCpuLoad() * 100.0f + 0.5f));
            c.Append("% (30 s)");
            Logger<LOGGER_EXTERNAL>::PrintLine("%s", c.Cstr());
            cpu_meter.Reset();
        }

        System::Delay(1);
    }

    return 0;
}
