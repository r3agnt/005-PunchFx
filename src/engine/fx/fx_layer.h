#pragma once
// Layer 2: the punch-in fx. Plain C++ (no libDaisy), tested on the Mac (test/test_fx.cpp).
//
// What it does, in plain words:
//  - 15 buttons (slots). Hold a button = that fx is on. Shift + button = LATCH (stays on
//    after you let go; Shift + the same button again = off; one latch at a time).
//  - Starts and stops land on the next 1/16 of the bar (like the beat repeat). With no
//    MIDI clock running they happen at once.
//  - Several buttons held: the newest wins. Let go of it and the older held one (or the
//    latch) takes over.
//  - Only ONE fx runs at a time. Switching fades from the old to the new over 5 ms.
//  - The last button pressed is the "edit slot": the fx whose knobs you are editing.
//  - BUS fx (the reverb on B1) is a SEND: while its button is on, the layer input is sent into the
//    reverb; the dry signal is never touched; the wet knob (K6) is the reverb's return level.
//    When you let go the input stops and the tail rings out. Any other fx can also feed the reverb with its
//    send amount (Shift + K1 in edit mode). The reverb's return goes through that slot's
//    gain / hipass / lopass.
//  - If the layer is not selected (buttons_active = false) button presses are ignored, but a
//    latched fx keeps running.
#include <cstddef>
#include <cstdint>
#include <cmath>
#include "fx_engine.h"
#include "fx_common.h"
#include "fx_reverb_bus.h"

namespace fx {

/** Names for the log, one per slot (B1-B7, B9-B16). */
inline const char* SlotName(int slot)
{
    static const char* const kNames[kNumSlots] = {
        "Reverb", "Delay", "Reverse", "Pitch down", "Pitch up", "Freq shifter", "Bit crusher",
        "Res hipass", "Res lowpass", "Distortion", "Flanger", "Jet flanger", "Phaser", "Comb",
        "Trem chop"};
    return (slot >= 0 && slot < kNumSlots) ? kNames[slot] : "?";
}

/** The most audio the layer is given at once. */
constexpr size_t kMaxChunk = 128;

enum class EventType : uint8_t { On, Off, LatchOn, LatchOff, Edit, NotBuilt };
struct Event
{
    EventType type;
    int8_t    slot;
};

/** What the layer needs to know each block. */
struct BlockInfo
{
    bool     running         = false; // MIDI clock running?
    float    pos_in_bar      = 0.0f;  // samples since the bar line, at the start of this block
    float    samples_per_bar = 0.0f;
    uint16_t raw_mask        = 0;     // bit s = button of slot s is down
    bool     shift           = false; // Shift (B8) held
    bool     buttons_active  = false; // layer 2 selected on the switch
};

struct Status
{
    int8_t active  = -1; // fx that is on now
    int8_t pending = -1; // fx waiting for its 1/16 line
    int8_t latch   = -1;
    int8_t edit    = -1; // the fx the knobs edit
    bool   reverb_ringing = false; // the reverb bus is running (fed, or its tail is still audible)
};

class Layer
{
  public:
    /** `settings` = kNumSlots knob sets that the UI edits and this layer reads. */
    void Init(float sample_rate, SlotSettings* settings)
    {
        sr_       = sample_rate;
        settings_ = settings;
        xf_len_   = static_cast<size_t>(0.005f * sample_rate);
        xf_pos_   = xf_len_;
        for(int s = 0; s < kNumSlots; s++)
            stages_[s].Init(sample_rate);
        bus_stage_.Init(sample_rate);
        for(size_t i = 0; i < kMaxChunk; i++)
            zero_[i] = 0.0f;
    }

    /** Plug in the reverb bus; `slot` = the slot that controls it (its settings = time/tone/pre-delay/wet...). */
    void SetReverbBus(ReverbBus* bus, int slot)
    {
        reverb_bus_ = bus;
        bus_slot_   = slot;
    }

    /** Plug an effect into a slot (before Init of the engine is fine; it is initialised here). */
    void SetEngine(int slot, Engine* e)
    {
        if(slot < 0 || slot >= kNumSlots)
            return;
        engines_[slot] = e;
        if(e && settings_)
        {
            e->Init(sr_);
            e->SetDefaults(settings_[slot]);
        }
    }
    const Engine* EngineFor(int slot) const
    {
        return (slot >= 0 && slot < kNumSlots) ? engines_[slot] : nullptr;
    }

    Status GetStatus() const
    {
        Status s;
        s.active  = static_cast<int8_t>(cur_);
        s.pending = static_cast<int8_t>(pending_start_ >= 0 ? pending_start_ : (want_ != cur_ ? want_ : -1));
        s.latch   = static_cast<int8_t>(latch_);
        s.edit    = static_cast<int8_t>(edit_);
        s.reverb_ringing = reverb_bus_ && reverb_bus_->Running();
        return s;
    }

    bool PopEvent(Event& e)
    {
        if(ev_head_ == ev_tail_)
            return false;
        e        = ev_[ev_tail_];
        ev_tail_ = (ev_tail_ + 1) % kEv;
        return true;
    }

    /** Process n <= kMaxChunk samples. `in` and `out` may be the same arrays. */
    void Process(const BlockInfo& info, const float* in_l, const float* in_r, float* out_l, float* out_r, size_t n)
    {
        if(n > kMaxChunk)
            n = kMaxChunk;
        Buttons(info);

        int at = -1; // sample where the next fx change lands
        if(pending_start_ >= 0 || want_ != cur_)
            at = LineIndex(info, n);

        const size_t a = at < 0 ? n : static_cast<size_t>(at);
        Segment(in_l, in_r, out_l, out_r, 0, a);
        if(at >= 0)
        {
            SwitchNow();
            Segment(in_l, in_r, out_l, out_r, a, n - a);
        }

        // The reverb bus: runs while something feeds it, and until its tail has died away.
        if(reverb_bus_ && bus_slot_ >= 0 && engines_[bus_slot_])
        {
            const bool feeding = FeedsBus(cur_) || (xf_pos_ < xf_len_ && FeedsBus(prev_));
            float      ret_l[kMaxChunk], ret_r[kMaxChunk], bl[kMaxChunk], br[kMaxChunk];
            reverb_bus_->Process(feed_l_, feed_r_, ret_l, ret_r, n, settings_[bus_slot_], feeding);
            // the return: wet level, hipass, lopass, gain (the reverb slot's knobs 5-8)
            bus_stage_.Process(zero_, zero_, ret_l, ret_r, bl, br, n, settings_[bus_slot_]);
            for(size_t i = 0; i < n; i++)
            {
                out_l[i] += bl[i];
                out_r[i] += br[i];
            }
        }
    }

  private:
    void Emit(EventType t, int slot)
    {
        const size_t next = (ev_head_ + 1) % kEv;
        if(next == ev_tail_)
            return; // full: drop
        ev_[ev_head_] = Event{t, static_cast<int8_t>(slot)};
        ev_head_      = next;
    }

    /** Buttons -> want_ (which fx the buttons ask for right now), latch, edit slot. */
    void Buttons(const BlockInfo& info)
    {
        const uint16_t all   = static_cast<uint16_t>((1u << kNumSlots) - 1u);
        const uint16_t raw   = info.buttons_active ? static_cast<uint16_t>(info.raw_mask & all) : 0;
        const uint16_t newly = static_cast<uint16_t>(raw & ~prev_raw_);
        for(int s = 0; s < kNumSlots; s++)
        {
            const uint16_t bit = static_cast<uint16_t>(1u << s);
            if(!(newly & bit))
                continue;
            if(info.shift || !engines_[s])
                blocked_ |= bit; // a Shift press (or an fx that is not built) is not a "hold"
            if(!engines_[s])
            {
                Emit(EventType::NotBuilt, s);
                continue;
            }
            if(edit_ != s)
            {
                edit_ = s;
                Emit(EventType::Edit, s);
            }
            if(info.shift)
            {
                if(latch_ == s)
                {
                    latch_ = -1;
                    Emit(EventType::LatchOff, s);
                }
                else
                {
                    latch_         = s;
                    pending_start_ = s;
                    Emit(EventType::LatchOn, s);
                }
            }
            else
            {
                last_pressed_  = s;
                pending_start_ = s;
            }
        }
        blocked_ &= raw; // forget buttons that were let go
        prev_raw_ = raw;
        const uint16_t held = static_cast<uint16_t>(raw & ~blocked_);
        if(held)
        {
            if(last_pressed_ >= 0 && (held & (1u << last_pressed_)))
                want_ = last_pressed_;
            else
            {
                want_ = 0;
                while(!(held & (1u << want_)))
                    want_++;
            }
        }
        else
            want_ = latch_;
    }

    /** First 1/16-of-a-bar line inside this block (sample index), or -1. With no clock: 0 (at once). */
    int LineIndex(const BlockInfo& info, size_t n) const
    {
        if(!info.running || info.samples_per_bar <= 0.0f)
            return 0;
        const double spb = info.samples_per_bar, spl = spb / 16.0;
        double       pos = std::fmod(static_cast<double>(info.pos_in_bar), spb);
        if(pos < 0.0)
            pos += spb;
        const double line = std::floor(pos / spl);
        if(pos - line * spl < 1.0)
            return 0; // we are (just) on a line
        const int idx = static_cast<int>(std::ceil((line + 1.0) * spl - pos));
        return (idx >= 0 && static_cast<size_t>(idx) < n) ? idx : -1;
    }

    /** The line has arrived: start / stop / change the fx. */
    void SwitchNow()
    {
        const int target = pending_start_ >= 0 ? pending_start_ : want_;
        pending_start_   = -1;
        if(target == cur_)
            return;
        prev_   = cur_;
        cur_    = target;
        xf_pos_ = 0;
        if(cur_ >= 0)
        {
            engines_[cur_]->Reset();
            stages_[cur_].Reset();
            Emit(EventType::On, cur_);
        }
        else
            Emit(EventType::Off, prev_);
    }

    /** Does this slot send audio into the reverb bus? (the reverb slot itself, or an fx with a send above 0) */
    bool FeedsBus(int slot) const
    {
        if(slot < 0 || !engines_[slot])
            return false;
        return engines_[slot]->IsBus() || settings_[slot].v[kSendReverb] > 0.001f;
    }

    /** What one slot makes of the input (slot < 0 = the input itself), and what it sends to the reverb bus
     *  (feed): the reverb slot sends its whole input and lets the dry through at (1 - wet); other fx send
     *  their output times their send amount. */
    void SlotOut(int slot, const float* in_l, const float* in_r, float* out_l, float* out_r,
                 float* feed_l, float* feed_r, size_t off, size_t len)
    {
        if(slot < 0 || !engines_[slot])
        {
            for(size_t i = 0; i < len; i++)
            {
                out_l[i]  = in_l[off + i];
                out_r[i]  = in_r[off + i];
                feed_l[i] = feed_r[i] = 0.0f;
            }
            return;
        }
        if(engines_[slot]->IsBus())
        {
            // A real SEND: the dry signal gets no wet/dry change (so letting go changes nothing but the
            // input to the reverb stopping and the tail rings out). The slot's wet knob (K6) is the
            // return level, applied to the reverb output in Process(). The OUTPUT GAIN (K5) applies to
            // the whole output while the reverb is on (dry here, reverb return in Process()).
            const float g = std::pow(10.0f, GainDb(settings_[slot].v[kGain]) / 20.0f);
            for(size_t i = 0; i < len; i++)
            {
                const float gi = bus_last_gain_ + (g - bus_last_gain_) * static_cast<float>(i + 1) / static_cast<float>(len);
                out_l[i]       = in_l[off + i] * gi;
                out_r[i]       = in_r[off + i] * gi;
                feed_l[i]      = in_l[off + i]; // the reverb is fed the un-gained input
                feed_r[i]      = in_r[off + i];
            }
            bus_last_gain_ = g;
            return;
        }
        float fl[kMaxChunk], fr[kMaxChunk];
        engines_[slot]->Process(in_l + off, in_r + off, fl, fr, len, settings_[slot].v);
        stages_[slot].Process(in_l + off, in_r + off, fl, fr, out_l, out_r, len, settings_[slot]);
        const float send = reverb_bus_ ? settings_[slot].v[kSendReverb] : 0.0f;
        for(size_t i = 0; i < len; i++)
        {
            feed_l[i] = out_l[i] * send;
            feed_r[i] = out_r[i] * send;
        }
    }

    /** Process in[off .. off+len) into out[off ..] (and feed_l_/feed_r_), fading old -> new fx if a switch is under way. */
    void Segment(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t off, size_t len)
    {
        if(len == 0)
            return;
        float al[kMaxChunk], ar[kMaxChunk], afl[kMaxChunk], afr[kMaxChunk];
        if(xf_pos_ >= xf_len_)
        {
            prev_ = -1;
            SlotOut(cur_, in_l, in_r, al, ar, afl, afr, off, len);
            for(size_t i = 0; i < len; i++)
            {
                out_l[off + i]  = al[i];
                out_r[off + i]  = ar[i];
                feed_l_[off + i] = afl[i];
                feed_r_[off + i] = afr[i];
            }
            return;
        }
        float bl[kMaxChunk], br[kMaxChunk], bfl[kMaxChunk], bfr[kMaxChunk];
        SlotOut(prev_, in_l, in_r, al, ar, afl, afr, off, len);
        SlotOut(cur_, in_l, in_r, bl, br, bfl, bfr, off, len);
        for(size_t i = 0; i < len; i++)
        {
            float t = static_cast<float>(xf_pos_ + i + 1) / static_cast<float>(xf_len_);
            if(t > 1.0f)
                t = 1.0f;
            out_l[off + i]   = al[i] * (1.0f - t) + bl[i] * t;
            out_r[off + i]   = ar[i] * (1.0f - t) + br[i] * t;
            feed_l_[off + i] = afl[i] * (1.0f - t) + bfl[i] * t;
            feed_r_[off + i] = afr[i] * (1.0f - t) + bfr[i] * t;
        }
        xf_pos_ += len;
    }

    float         sr_       = 48000.0f;
    SlotSettings* settings_ = nullptr;
    Engine*       engines_[kNumSlots] = {};
    Stage         stages_[kNumSlots];

    // reverb bus
    ReverbBus* reverb_bus_ = nullptr;
    int        bus_slot_   = -1;
    Stage      bus_stage_;                // gain / hipass / lopass / wet level of the reverb return
    float      bus_last_gain_ = 1.0f;     // for gliding the dry gain while the reverb slot is on
    float      feed_l_[kMaxChunk], feed_r_[kMaxChunk]; // what is sent into the reverb this block
    float      zero_[kMaxChunk];

    uint16_t prev_raw_ = 0, blocked_ = 0;
    int      last_pressed_ = -1, want_ = -1, latch_ = -1, edit_ = -1, pending_start_ = -1;
    int      cur_ = -1, prev_ = -1; // fx that is on / the one fading out
    size_t   xf_len_ = 240, xf_pos_ = 240;

    static constexpr size_t kEv = 16;
    Event  ev_[kEv];
    size_t ev_head_ = 0, ev_tail_ = 0;
};

} // namespace fx
