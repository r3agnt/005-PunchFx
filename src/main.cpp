/** 004-Looper : audio looper prototype (Desktop DevKit + Daisy Seed)
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
#include "engine/slice_loop_buffer.h"
#include "engine/looper_config.h"
#include "engine/pitch_fx.h"

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
static PitchFx pitch_fx; // on the beat repeat output (slice mode pitches the slice itself)

/** Slice mode (B7), src/engine/slice_loop_buffer.h: the SLICE LOOP BUFFER, a frozen
 *  2-bar loop played like a drum kit. It has its own 2-bar recording of the input
 *  (about 1.4 MB at the 130 BPM minimum), separate from the beat repeat's history. */
SliceLoopBuffer slice_loop;
DSY_SDRAM_BSS static float slice_buf_l[looper::kHistoryCapacityFrames];
DSY_SDRAM_BSS static float slice_buf_r[looper::kHistoryCapacityFrames];

/** B7 / Shift + B7 commands waiting for the audio callback. */
SpscQueue<SliceCmd, 8> slice_cmd_queue;

/** Which division is latched on by Shift + B9-B16 (-1 = none). Written by the
 *  audio callback, read by the LEDs (main_page.h). */
volatile int8_t stutter_latch = -1;
DSY_SDRAM_BSS static float hist_buf_l[looper::kHistoryCapacityFrames];
DSY_SDRAM_BSS static float hist_buf_r[looper::kHistoryCapacityFrames];
DSY_SDRAM_BSS static float frz_buf_l[looper::kFreezeCapacityFrames];
DSY_SDRAM_BSS static float frz_buf_r[looper::kFreezeCapacityFrames];

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

/** Turn the slice mode's events into log lines. Called every audio block. */
static void LogSliceEvents()
{
    SliceEvent e;
    while(slice_loop.PopEvent(e))
    {
        FixedCapStr<32> s;
        switch(e.type)
        {
            case SliceEventType::ModeOn: s.Append("Slice mode ON"); break;
            case SliceEventType::ModeOff: s.Append("Slice mode OFF"); break;
            case SliceEventType::Bank:
                s.Append("Slice bank ");
                s.AppendInt(static_cast<int>(e.a) + 1);
                break;
            case SliceEventType::SliceOn:
                s.Append("Slice ");
                s.AppendInt(static_cast<int>(e.a) + 1);
                break;
            case SliceEventType::BackToLoop: s.Append("Slice loop"); break;
            case SliceEventType::IgnoredNotRunning: s.Append("B7 ignored - not running"); break;
            case SliceEventType::IgnoredNoMode: s.Append("Bank ignored - no slice mode"); break;
            case SliceEventType::IgnoredOneBank: s.Append("Bank ignored - 1 bar only"); break;
            case SliceEventType::PitchOn: s.Append("Pitch fx ON (+3 ping-pong)"); break;
            case SliceEventType::PitchOff: s.Append("Pitch fx OFF"); break;
            case SliceEventType::IgnoredTooShort: s.Append("Slice mode ignored - too short"); break;
            case SliceEventType::IgnoredTooSlow: s.Append("Slice mode ignored - too slow"); break;
        }
        Say(s);
    }
}

/** Largest block the looper is given at once (libDaisy's default is 48). */
static constexpr size_t kMaxChunk = 128;

/** CPU load meter (libDaisy util/CpuLoadMeter.h): times each audio block with the chip's cycle
 *  counter (a couple of cycles per block, so it adds no real load). The main loop prints
 *  the result every 30 s: avg = smoothed average, max = the worst block in the 30 s,
 *  as a percent of the time one block has (100% = the audio would drop out). */
static CpuLoadMeter cpu_meter;

/** Slice mode B6: with the backwards half (ping-pong) or forward only. Shift + B6 toggles it.
 *  Default off (forward only). Nothing is saved: it is off again at power-up. */
static bool slice_reverse = false;

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

    // Slice mode (B7): apply its button commands.
    const bool slice_can = st.running && clock_tracker.SamplesPerBar() > 0.0f;
    SliceCmd   slice_cmd;
    while(slice_cmd_queue.Pop(slice_cmd))
        slice_loop.Command(slice_cmd, slice_can);

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
    uint32_t slice_held = 0; // buttons 9-16 for the slice mode (bit i = slice i of the bank)
    bool     slice_pitch_held = false; // B6 for the slice mode
    {
        static uint32_t prev_raw = 0, shift_blocked = 0;
        uint32_t        raw      = 0;
        for(size_t i = 0; i < looper::kNumDivisions; i++)
            if(hw.button[8 + i].Pressed())
                raw |= 1u << i;
        const uint32_t newly = raw & ~prev_raw;
        if(hw.button[7].Pressed())
        {
            shift_blocked |= newly; // newly pressed while Shift is down
            for(size_t i = 0; i < looper::kNumDivisions; i++)
            {
                // (In slice mode Shift + B9-B16 does nothing: no latch.)
                if(!(newly & (1u << i)) || !st.running || slice_loop.IsOn())
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
        if(!st.running)
            stutter_latch = -1;
        // B6 = pitch-down effect (hold). Like the stutter buttons, a B6 that is
        // pressed while Shift is held does nothing until it is released.
        static bool prev_b6 = false, b6_blocked = false, prev_b6_for_rev = false;
        const bool  b6      = hw.button[5].Pressed();
        if(b6 && !prev_b6 && hw.button[7].Pressed())
            b6_blocked = true;
        if(!b6)
            b6_blocked = false;
        prev_b6 = b6;
        // Shift + B6 toggles the REVERSE half of the slice mode B6 effect (default off:
        // forward only). The press itself does not switch the effect on (b6_blocked).
        if(b6 && !prev_b6_for_rev && hw.button[7].Pressed())
        {
            slice_reverse = !slice_reverse;
            Say(FixedCapStr<32>(slice_reverse ? "Pitch reverse ON" : "Pitch reverse OFF"));
        }
        prev_b6_for_rev = b6;
        beat_info.pitch_held = b6 && !b6_blocked && !slice_loop.IsOn();
        slice_pitch_held     = b6 && !b6_blocked; // (the slice engine ignores it unless slice mode is on)
        beat_info.held_mask = raw & ~shift_blocked;
        if(stutter_latch >= 0)
            beat_info.held_mask |= 1u << stutter_latch;
        slice_held = raw & ~shift_blocked;
        // In slice mode buttons 9-16 play slices, not the beat repeat.
        if(slice_loop.IsOn())
            beat_info.held_mask = 0;
    }
    SliceBlockInfo slice_info;
    slice_info.running         = st.running;
    slice_info.bar             = beat_info.bar;
    slice_info.samples_per_bar = beat_info.samples_per_bar;
    slice_info.offset_samples  = beat_info.offset_samples;
    slice_info.held_mask       = slice_held;
    slice_info.pitch_held      = slice_pitch_held;
    slice_info.fx_semitones    = looper::SlicePitchFromControl(params.slice_pitch);
    slice_info.fx_reverse      = slice_reverse;

    // Pass the latest settings to the effects.
    gain_stage.SetControls(params.input_gain, params.out_level);

    // SIGNAL CHAIN (left = channel 0, right = channel 1):
    //   input gain -> beat repeat -> [Slice loop buffer mixed in] -> Quantized loop buffer
    //   -> [Slice loop buffer mixed in again] -> soft clip + output level + ceiling
    size_t done = 0;
    while(done < size)
    {
        const size_t n = (size - done) < kMaxChunk ? (size - done) : kMaxChunk;
        float        perf_l[kMaxChunk], perf_r[kMaxChunk];
        float        rep_l[kMaxChunk], rep_r[kMaxChunk];
        float        loop_l[kMaxChunk], loop_r[kMaxChunk];
        float        sl_l[kMaxChunk], sl_r[kMaxChunk], base_w[kMaxChunk];
        float        pp_l[kMaxChunk], pp_r[kMaxChunk];
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
        // Slice mode: base_w = how much of the normal signal is left, sl = the slice
        // loop buffer's part. output = base_w * normal + sl, applied twice:
        //  1) to what the Quantized loop buffer RECORDS (so slices get recorded),
        //  2) to what you HEAR (so slices are heard over a playing loop).
        slice_info.pos_in_bar = beat_info.pos_in_bar;
        slice_loop.Process(slice_info, perf_l, perf_r, sl_l, sl_r, base_w, n);
        beat_info.pos_in_bar += static_cast<float>(n);
        for(size_t i = 0; i < n; i++)
        {
            pp_l[i] = base_w[i] * rep_l[i] + sl_l[i];
            pp_r[i] = base_w[i] * rep_r[i] + sl_r[i];
        }
        quantized_loop.Process(info, pp_l, pp_r, loop_l, loop_r, n);
        info.bar_start = false; // the bar boundary belongs to the first chunk only
        for(size_t i = 0; i < n; i++)
        {
            out[0][done + i] = gain_stage.Limit(base_w[i] * loop_l[i] + sl_l[i]);
            out[1][done + i] = gain_stage.Limit(base_w[i] * loop_r[i] + sl_r[i]);
        }
        done += n;
    }

    LogLoopEvents();
    LogBeatEvents();
    LogSliceEvents();
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
    slice_loop.Init(slice_buf_l, slice_buf_r, looper::kHistoryCapacityFrames, hw.seed.AudioSampleRate());
    pitch_fx.Init(hw.seed.AudioSampleRate());
    beat_repeat.Init(hist_buf_l, hist_buf_r, looper::kHistoryCapacityFrames,
                     frz_buf_l, frz_buf_r, looper::kFreezeCapacityFrames,
                     hw.seed.AudioSampleRate());

    // Modules that need the sample rate get it here, e.g.
    //   my_module.Init(hw.seed.AudioSampleRate());

    // Print the firmware version so you can see what is flashed.
    Logger<LOGGER_EXTERNAL>::PrintLine("Firmware version %s", FIRMWARE_VERSION);

    // Print the looper limits and how much memory its buffers will need.
    LogLooperConfig(hw.seed.AudioSampleRate());

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
