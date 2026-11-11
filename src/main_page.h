#pragma once

#include "daisy.h"
#include "params.h"
#include "version.h"
#include "soft_takeover.h"
#include "param_info.h"
#include "engine/beat_repeat.h"
#include "engine/clock_tracker.h"
#include "engine/quantized_loop_buffer.h"
#include "engine/slice_loop_buffer.h"
#include "engine/looper_config.h"

/** The MIDI clock follower, created in main.cpp. Read here for the LEDs. */
extern ClockTracker clock_tracker;

/** The looper and its button-command queue, created in main.cpp. */
extern QuantizedLoopBuffer quantized_loop;
extern BeatRepeat beat_repeat;
extern SliceLoopBuffer slice_loop;
extern SpscQueue<SliceCmd, 8> slice_cmd_queue;
extern volatile int8_t stutter_latch;
extern SpscQueue<LoopCmd, 16> loop_cmd_queue;

/** MainPage : the one and only UI page.
 *
 *  libDaisy calls the functions below when something happens:
 *   - Draw()        about 60 times per second: updates the LEDs.
 *   - OnButton()    when a button is pressed, released or long-pressed.
 *   - OnPotMoved()  when a knob moves.
 *   - OnShow()      once, when the page opens.
 *
 *  All pots/buttons print to USB serial. Knob assignments are in the
 *  KNOB MAP in params.h. Button 8 is SHIFT (hold it to use the second
 *  function of a knob). Knobs use soft takeover (soft_takeover.h).
 *  Button/pot numbers printed are 1-based (Pot 1 .. Pot 8, Button 1 .. 16),
 *  matching the labels on the devkit. In code they are 0-based.
 */
class MainPage : public daisy::UiPage
{
  public:
    /** Short name for logging over USB serial. */
    using Log = daisy::Logger<daisy::LOGGER_EXTERNAL>;

    void Init() {}

    void Draw(const daisy::UiCanvasDescriptor &canvasDescriptor) override
    {
        auto *hw = reinterpret_cast<daisy::desktop_devkit::Hardware *>(
            canvasDescriptor.handle_);

        // Shift = button 8 held. Read it here (not from button events) so it
        // can never get stuck after a long press.
        const bool shift = hw->button[7].Pressed();
        if(shift != shift_)
        {
            shift_ = shift;
            for(auto &layer : takeover_)
                for(auto &t : layer)
                    t.Reset(); // knobs must pick up again on the new layer
            Log::PrintLine(shift_ ? "Shift on" : "Shift off");
        }

        // Seed LED: slow flash = loop armed, solid = recording, otherwise
        // a fast blink as a "program is running" sign.
        auto             now = daisy::System::GetNow();
        const LoopStatus ls  = quantized_loop.GetStatus();
        if(ls.state == LoopState::Recording)
            hw->seed.SetLed(true);
        else if(ls.state == LoopState::Armed || ls.state == LoopState::PlayingArmed)
            hw->seed.SetLed((now & 1023) < 512);
        else
            hw->seed.SetLed((now & 511) < 255);

        DrawLeds(hw, now);
    }

    /** buttonID is 0-based. numberOfPresses: 0 = released, 1 = pressed,
     *  2 = long press (held 1.5 s). */
    bool OnButton(uint16_t buttonID,
                  uint8_t  numberOfPresses,
                  bool     isRetriggering) override
    {
        switch(numberOfPresses)
        {
            case 0: Log::PrintLine("B%d Released", buttonID + 1); break;
            case 1: Log::PrintLine("B%d Pressed", buttonID + 1); break;
            case 2: Log::PrintLine("B%d Long press", buttonID + 1); break;
            default: break;
        }

        // ===== ASSIGN CONTROLS HERE (buttons) =====
        // Button 8 = SHIFT (handled in Draw()).
        // Button 1 = record arm (Shift + B1 = clear). Button 2 = loop <-> live.
        // These only send a command; the audio callback does the work.
        if(numberOfPresses == 1)
        {
            if(buttonID == 0)
                loop_cmd_queue.Push(shift_ ? LoopCmd::Clear : LoopCmd::ArmToggle);
            else if(buttonID == 1)
                loop_cmd_queue.Push(LoopCmd::ToggleSource);
            // Button 7 = slice mode on/off (Shift + B7 = switch bank; with 1 bank it just logs).
            else if(buttonID == 6)
                slice_cmd_queue.Push(shift_ ? SliceCmd::ToggleBank : SliceCmd::ToggleMode);
        }
        // Shift + button 16 long press: print the firmware version.
        // (Button 16 is also the 1/64 stutter; the audio callback ignores a
        // stutter button that is pressed while Shift is held.)
        if(buttonID == 15 && numberOfPresses == 2 && shift_)
            Log::PrintLine("Shift + B16 Long press - Firmware version %s", FIRMWARE_VERSION);
        // Other buttons are not assigned yet. Change values in `params`.

        return false;
    }

    /** potID is 0-based. newPosition is 0.0 to 1.0. */
    bool OnPotMoved(uint16_t potID, float newPosition) override
    {
        // Find which parameter this knob controls (normal or shift layer).
        float Params::*target = nullptr;
        if(potID < daisy::desktop_devkit::kNumPots)
            target = shift_ ? kPotMap[potID].shifted : kPotMap[potID].normal;

        // ===== ASSIGN CONTROLS HERE (pots): edit KNOB MAP in params.h =====
        bool took_over = false;
        if(target != nullptr)
        {
            // Layers marked "no takeover" in the KNOB MAP write straight away.
            const bool want_takeover
                = shift_ ? kPotMap[potID].shifted_takeover : kPotMap[potID].normal_takeover;
            if(!want_takeover)
            {
                params.*target = newPosition;
                took_over      = true;
            }
            else
            {
                auto &t   = takeover_[shift_ ? 1 : 0][potID];
                took_over = t.Update(newPosition, params.*target);
                if(took_over)
                {
                    params.*target = newPosition;
                    t.Written(newPosition);
                }
            }
        }

        // Log line: "[Shift + ]K<n> <Param name> - <value>[ (pickup)]"
        // The value is the KNOB position in real units. "(pickup)" means the
        // knob has not yet picked up the parameter (soft takeover), so the
        // parameter itself has not changed.
        daisy::FixedCapStr<64> line;
        if(shift_)
            line.Append("Shift + ");
        line.Append("K");
        line.AppendInt(potID + 1);
        line.Append(" ");
        const ParamInfo *info = target ? FindParamInfo(target) : nullptr;
        if(info)
        {
            line.Append(info->name);
            line.Append(" - ");
            AppendValue(line, *info, newPosition);
            if(!took_over)
                line.Append(" (pickup)");
        }
        else
        {
            line.Append("(unassigned) - ");
            line.AppendFloat(newPosition, 3);
        }
        Log::PrintLine("%s", line.Cstr());

        return false;
    }

    void OnShow() override { Log::PrintLine("Page Opened!"); }

  private:
    /** LED layout (LED n is next to button n):
     *   Top row (LEDs 1-8): one LED per bar of the loop cycle (dim). The bar
     *     we are in is bright.
     *       ARMED:     LED 1 flashes slowly (recording starts at the cycle start).
     *       RECORDING: bars fill up like a progress bar; the current bar flickers.
     *       PLAYING:   the bright LED moves along the bars.
     *   Bottom row (LEDs 9-16): the eight eighth-notes of the bar; the current
     *     one is bright. LED 16 glows steadily while the loop is what you hear
     *     (dark = live input).
     *  Buttons 9-16 (beat repeat): fast flash = waiting for the slice boundary,
     *  dim glow = repeating while held, brighter glow = latched on (Shift + button).
     *  Slice mode (B7): LED 7 on (bank 2 pulses if there are 2 banks), LEDs 9-16 show the slices
     *  (see the code below).
     *  For 1.5 s after knob 2 (or CC 21) changes the loop length, LEDs 1-8
     *  show the new length instead (LEDs 1..N lit).
     *  No clock: LEDs 9 and 16 blink slowly. Clock but stopped: the bottom
     *  row glows very dimly. */
    void DrawLeds(daisy::desktop_devkit::Hardware *hw, uint32_t now_ms)
    {
        const ClockStatus st   = clock_tracker.GetStatus();
        const LoopStatus  ls   = quantized_loop.GetStatus();
        const uint32_t    bars = ls.bars > 0 ? ls.bars : 1;
        const bool        slow = (now_ms & 1023) < 512;
        const bool        fast = (now_ms & 255) < 128;

        // Loop length changed (knob 2 or CC 21)? Show it for a moment.
        if(last_loop_len_ < 0.0f)
            last_loop_len_ = params.loop_length; // first call: nothing to show
        if(params.loop_length != last_loop_len_)
        {
            last_loop_len_  = params.loop_length;
            loop_show_until_ = now_ms + kLoopShowMs;
        }

        float led[daisy::desktop_devkit::kNumLeds] = {};

        // Top row: the loop cycle (dim), then the loop state.
        // While the loop length is being set, the top row instead shows the
        // setting: LEDs 1..N lit, N = bars. (Applies to the NEXT recording.)
        const bool showing_len = static_cast<int32_t>(loop_show_until_ - now_ms) > 0;
        for(uint32_t i = 0; i < bars && i < 8; i++)
            led[i] = kLedCycleDim;
        const uint32_t cur = st.running ? st.bar % bars : 0;
        switch(ls.state)
        {
            case LoopState::Recording:
                if(st.running)
                {
                    for(uint32_t i = 0; i < cur && i < 8; i++)
                        led[i] = 1.0f;
                    led[cur < 8 ? cur : 7] = fast ? 1.0f : 0.3f;
                }
                break;
            case LoopState::Armed:
            case LoopState::PlayingArmed:
                if(st.running)
                    led[cur < 8 ? cur : 7] = 1.0f;
                led[0] = slow ? kLedFull : kLedCycleDim;
                break;
            default:
                if(st.running)
                    led[cur < 8 ? cur : 7] = 1.0f;
                break;
        }

        if(showing_len)
        {
            const uint32_t n = looper::LoopBarsFromControl(params.loop_length);
            for(uint32_t i = 0; i < 8; i++)
                led[i] = i < n ? 1.0f : 0.0f;
        }

        // Bottom row: eighth-notes, or a waiting pattern.
        if(ls.buffer_audible)
            led[15] = 0.25f;

        // Beat repeat buttons 9-16: fast flash while waiting for the slice
        // boundary, steady glow while repeating.
        const BeatStatus bs = beat_repeat.GetStatus();
        if(bs.active >= 0)
            led[8 + bs.active]
                = bs.active == stutter_latch ? kLedStutterLatched : kLedStutterHeld;
        if(bs.pending >= 0)
            led[8 + bs.pending] = fast ? kLedFull : 0.0f;
        // B6 pitch effect: LED 6 flashes fast until its 1/16 line, then glows
        // (replaces the bar-6 LED while it is on).
        const SliceStatus ss0 = slice_loop.GetStatus();
        const bool        pitch_pending = ss0.on ? ss0.pitch_pending : bs.pitch_pending;
        const bool        pitch_on      = ss0.on ? ss0.pitch_on : bs.pitch_on;
        if(pitch_pending)
            led[5] = fast ? kLedFull : 0.0f;
        else if(pitch_on)
            led[5] = kLedFull;
        // Slice mode (B7): LED 7 = mode (bank 1 steady, bank 2 pulsing, fast flash
        // while the B7 press waits for its 1/8 line). LEDs 9-16 = the 8 slices of the
        // bank (dim), the repeating one brighter, a pressed one flashes until its line.
        const SliceStatus ss = slice_loop.GetStatus();
        if(ss.on || ss.changing)
        {
            led[6] = ss.changing ? (fast ? kLedFull : 0.0f)
                                 : (ss.bank == 0 ? kLedFull : (slow ? kLedFull : kLedCycleDim));
            if(ss.on)
            {
                for(int i = 8; i < 16; i++)
                    led[i] = kLedSliceDim;
                if(ss.playing >= 0)
                    led[8 + ss.playing] = kLedSlicePlaying;
                if(ss.pending >= 0)
                    led[8 + ss.pending] = fast ? kLedFull : 0.0f;
            }
        }
        if(st.running)
        {
            led[8 + st.eighth] = 1.0f;
        }
        else if(st.present)
        {
            for(int i = 8; i < 16; i++)
                if(led[i] < 0.04f)
                    led[i] = 0.04f;
        }
        else if((now_ms & 1023) < 150)
        {
            led[8]  = 0.3f;
            led[15] = 0.3f;
        }

        for(size_t i = 0; i < daisy::desktop_devkit::kNumLeds; i++)
            hw->SetLed(i, led[i]);
    }

    /** LED brightness levels (0.0 - 1.0). NOTE: the devkit inverts the value
     *  before libDaisy's gamma table, so low numbers are brighter than they
     *  look. Approximate LED on-time: 0.01 = 4%, 0.02 = 6%, 0.04 = 12%,
     *  0.08 = 21%, 0.10 = 26%, 0.25 = 55%, 0.5 = 86%, 1.0 = 100%. */
    static constexpr float kLedCycleDim = 0.02f; // top row: LEDs of the loop cycle
    static constexpr float kLedFull     = 1.0f;  // active bar, flashes, eighth-note
    static constexpr float kLedStutterHeld    = kLedCycleDim; // button LED while its stutter repeats (button held): same as the loop-length LEDs
    static constexpr float kLedSliceDim       = 0.04f; // slice mode: the 8 slices of the bank (about 12%)
    static constexpr float kLedSlicePlaying   = 0.5f;  // slice mode: the slice that is repeating (about 86%)
    static constexpr float kLedStutterLatched = 0.15f;        // ... while it is latched on with Shift + button (brighter, about 37%)

    /** How long (ms) the loop length stays on LEDs 1-8 after K2 moves. */
    static constexpr uint32_t kLoopShowMs = 1500;

    float        last_loop_len_   = -1.0f;
    uint32_t     loop_show_until_ = 0;
    bool         shift_ = false;
    SoftTakeover takeover_[2][daisy::desktop_devkit::kNumPots];
};
