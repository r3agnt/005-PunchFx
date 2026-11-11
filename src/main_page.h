#pragma once

#include "daisy.h"
#include "params.h"
#include "version.h"
#include "soft_takeover.h"
#include "param_info.h"
#include "engine/beat_repeat.h"
#include "engine/clock_tracker.h"
#include "engine/quantized_loop_buffer.h"
#include "engine/looper_config.h"
#include "engine/fx/fx_layer.h"

/** The MIDI clock follower, created in main.cpp. Read here for the LEDs. */
extern ClockTracker clock_tracker;

/** The looper and its button-command queue, created in main.cpp. */
extern QuantizedLoopBuffer quantized_loop;
extern BeatRepeat beat_repeat;
extern volatile int8_t stutter_latch;
extern SpscQueue<LoopCmd, 16> loop_cmd_queue;

/** Layers and punch-in fx, created in main.cpp. current_layer: 0 = layer 1 (looper),
 *  1 = layer 2 (fx), 2 = layer 3. fx_edit_mode: true = knobs edit the last-triggered fx. */
extern volatile uint8_t current_layer;
extern volatile bool    fx_edit_mode;
extern fx::Layer        fx_layer;
extern fx::SlotSettings fx_settings[fx::kNumSlots];

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
        // Knobs must pick up again after: Shift changes, another layer, edit <-> perf,
        // or another fx being edited (its saved values are not where the knobs are).
        const int edit_slot = fx_layer.GetStatus().edit;
        const bool changed = shift != shift_ || current_layer != last_layer_
                             || fx_edit_mode != last_edit_mode_ || edit_slot != last_edit_slot_;
        if(changed)
        {
            for(auto &layer : takeover_)
                for(auto &t : layer)
                    t.Reset();
            for(auto &t : takeover_fx_)
                t.Reset();
        }
        if(shift != shift_)
        {
            shift_ = shift;
            Log::PrintLine(shift_ ? "Shift on" : "Shift off");
        }
        last_layer_     = current_layer;
        last_edit_mode_ = fx_edit_mode;
        last_edit_slot_ = edit_slot;

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
        // (B1/B2 are looper buttons on layer 1 only; on layer 2 they are fx buttons.)
        if(numberOfPresses == 1 && current_layer == 0)
        {
            if(buttonID == 0)
                loop_cmd_queue.Push(shift_ ? LoopCmd::Clear : LoopCmd::ArmToggle);
            else if(buttonID == 1)
                loop_cmd_queue.Push(LoopCmd::ToggleSource);
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
        // Layer 2: the knobs edit the fx (separate code below). Layer 3: nothing yet.
        if(current_layer == 1)
            return HandleFxKnob(potID, newPosition);
        if(current_layer == 2)
        {
            Log::PrintLine("K%d (layer 3 unassigned) - %d%%", potID + 1, static_cast<int>(newPosition * 100.0f + 0.5f));
            return false;
        }

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
    /** Layer 2 knobs. K1-K4 = the edit slot's own params, K5-K8 = gain, wet/dry, hipass, lopass.
     *  Only in EDIT mode (perf mode locks them). Soft takeover as everywhere: after selecting
     *  another fx a knob does nothing until it reaches that fx's stored value. */
    bool HandleFxKnob(uint16_t potID, float pos)
    {
        static const char *const kSharedNames[4] = {"Out gain", "Wet/dry", "Hipass", "Lopass"};
        daisy::FixedCapStr<64>   line;
        if(shift_)
            line.Append("Shift + ");
        line.Append("K");
        line.AppendInt(potID + 1);
        line.Append(" ");

        const int         slot = fx_layer.GetStatus().edit;
        const fx::Engine *eng  = fx_layer.EngineFor(slot);
        const char       *name = nullptr;
        int               value_index = potID; // which of the slot's values this knob edits
        if(eng && potID < fx::kNumKnobs)
        {
            if(!shift_)
                name = potID < 4 ? eng->KnobName(potID) : kSharedNames[potID - 4];
            else if(potID == 0 && !eng->IsBus())
            {
                name        = "Send reverb"; // Shift + K1: how much of this fx goes into the reverb
                value_index = fx::kSendReverb;
            }
        }
        if(!eng || name == nullptr)
        {
            line.Append(eng ? "(unassigned) - " : "(no fx selected) - ");
            line.AppendFloat(pos, 3);
            Log::PrintLine("%s", line.Cstr());
            return false;
        }
        line.Append(name);
        line.Append(" - ");
        AppendFxValue(line, value_index, eng, pos);

        if(!fx_edit_mode)
            line.Append(" (perf mode: locked)");
        else
        {
            float &value = fx_settings[slot].v[value_index];
            auto  &t     = takeover_fx_[value_index];
            if(t.Update(pos, value))
            {
                value = pos;
                t.Written(pos);
            }
            else
                line.Append(" (pickup)");
        }
        Log::PrintLine("%s", line.Cstr());
        return false;
    }

    /** Value of an fx knob in real units, for the log. */
    static void AppendFxValue(daisy::FixedCapStr<64> &line, int knob, const fx::Engine *eng, float v)
    {
        if(knob < 4)
        {
            line.AppendFloat(eng->KnobReal(knob, v), 1);
            line.Append(eng->KnobUnit(knob));
        }
        else if(knob == fx::kSendReverb)
        {
            line.AppendInt(static_cast<int>(v * 100.0f + 0.5f));
            line.Append("%");
        }
        else if(knob == fx::kGain)
        {
            const float db = fx::GainDb(v);
            line.Append(db >= 0.0f ? "+" : "");
            line.AppendFloat(db, 1);
            line.Append(" dB");
        }
        else if(knob == fx::kWet)
        {
            line.AppendInt(static_cast<int>(v * 100.0f + 0.5f));
            line.Append("%");
        }
        else if(knob == fx::kHp)
        {
            if(fx::HpOn(v))
            {
                line.AppendInt(static_cast<int>(fx::HpHz(v) + 0.5f));
                line.Append(" Hz");
            }
            else
                line.Append("off");
        }
        else
        {
            if(fx::LpOn(v))
            {
                line.AppendInt(static_cast<int>(fx::LpHz(v) + 0.5f));
                line.Append(" Hz");
            }
            else
                line.Append("off");
        }
    }

    /** LEDs on layers 2 and 3. Layer 2: each fx button's LED is dim if that fx exists, brighter
     *  for the one being edited (edit mode), brighter again if latched, full while it is ON,
     *  flashing while it waits for its 1/16 line. LED 8 (Shift) = edit mode (glow) / perf (dim).
     *  Layer 3: LED 8 flashes slowly (nothing built yet). */
    void DrawFxLeds(daisy::desktop_devkit::Hardware *hw, uint32_t now_ms)
    {
        float      led[daisy::desktop_devkit::kNumLeds] = {};
        const bool fast = (now_ms & 255) < 128;
        if(current_layer == 1)
        {
            const fx::Status fs = fx_layer.GetStatus();
            auto             btn = [](int slot) { return slot < 7 ? slot : slot + 1; };
            for(int slot = 0; slot < fx::kNumSlots; slot++)
                if(fx_layer.EngineFor(slot))
                    led[btn(slot)] = kLedCycleDim;
            if(fs.edit >= 0 && fx_edit_mode)
                led[btn(fs.edit)] = kLedEdit;
            if(fs.latch >= 0)
                led[btn(fs.latch)] = kLedStutterLatched;
            if(fs.reverb_ringing && led[0] < kLedStutterLatched)
                led[0] = kLedStutterLatched; // B1 LED glows while the reverb is on or its tail rings
            if(fs.active >= 0)
                led[btn(fs.active)] = kLedFull;
            if(fs.pending >= 0)
                led[btn(fs.pending)] = fast ? kLedFull : 0.0f;
            led[7] = fx_edit_mode ? kLedEdit : kLedCycleDim;
        }
        else
            led[7] = (now_ms & 1023) < 512 ? kLedFull : kLedCycleDim;
        for(size_t i = 0; i < daisy::desktop_devkit::kNumLeds; i++)
            hw->SetLed(i, led[i]);
    }

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
     *  For 1.5 s after knob 2 (or CC 21) changes the loop length, LEDs 1-8
     *  show the new length instead (LEDs 1..N lit).
     *  No clock: LEDs 9 and 16 blink slowly. Clock but stopped: the bottom
     *  row glows very dimly. */
    void DrawLeds(daisy::desktop_devkit::Hardware *hw, uint32_t now_ms)
    {
        if(current_layer != 0)
        {
            DrawFxLeds(hw, now_ms);
            return;
        }
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
        const bool pitch_pending = bs.pitch_pending;
        const bool pitch_on      = bs.pitch_on;
        // B6 pitch effect: LED 6 flashes fast until its 1/16 line, then glows
        // (replaces the bar-6 LED while it is on).
        if(pitch_pending)
            led[5] = fast ? kLedFull : 0.0f;
        else if(pitch_on)
            led[5] = kLedFull;
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
    static constexpr float kLedEdit     = 0.25f; // layer 2: the fx being edited (about 55%)
    static constexpr float kLedStutterHeld    = kLedCycleDim; // button LED while its stutter repeats (button held): same as the loop-length LEDs
    static constexpr float kLedStutterLatched = 0.15f;        // ... while it is latched on with Shift + button (brighter, about 37%)

    /** How long (ms) the loop length stays on LEDs 1-8 after K2 moves. */
    static constexpr uint32_t kLoopShowMs = 1500;

    float        last_loop_len_   = -1.0f;
    uint32_t     loop_show_until_ = 0;
    bool         shift_ = false;
    SoftTakeover takeover_[2][daisy::desktop_devkit::kNumPots];
    SoftTakeover takeover_fx_[fx::kNumValues];
    uint8_t      last_layer_     = 0;
    bool         last_edit_mode_ = false;
    int          last_edit_slot_ = -1;
};
