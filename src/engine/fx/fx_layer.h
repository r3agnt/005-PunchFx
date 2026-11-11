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
//    reverb; the wet knob (K6) is the reverb's return level, the output gain (K5) lifts dry + return,
//    the lopass (K8) filters the reverb's INPUT only, the hipass (K7) filters the return only.
//    When you let go the input stops and the tail rings out. Any other fx can also feed the reverb with its
//    send amount (Shift + K1 in edit mode). The reverb's return goes through that slot's
//    gain / hipass / lopass.
//  - The DELAY (B2) is a bus in the same way (own tail, own return level K6). Any other fx can feed it with its
//    delay send (Shift + K2). The reverb and the delay can feed EACH OTHER, but only one way at a time
//    (the delay slot's routing value, Shift + K4): reverb > delay (default) or delay > reverb. So there is
//    never a loop. The one that is fed second keeps ringing while the first one is still making sound.
//  - If the layer is not selected (buttons_active = false) button presses are ignored, but a
//    latched fx keeps running.
#include <cstddef>
#include <cstdint>
#include <cmath>
#include "fx_engine.h"
#include "fx_common.h"
#include "fx_reverb_bus.h"
#include "fx_delay_bus.h"

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
    bool   delay_ringing  = false; // the same for the delay bus
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
        delay_stage_.Init(sample_rate);
        for(size_t i = 0; i < kMaxChunk; i++)
            zero_[i] = 0.0f;
        for(int s = 0; s < kNumSlots; s++)
            bus_last_gain_[s] = 1.0f;
    }

    /** Plug in the reverb bus; `slot` = the slot that controls it (its settings = time/tone/pre-delay/wet...). */
    void SetReverbBus(ReverbBus* bus, int slot)
    {
        reverb_bus_ = bus;
        bus_slot_   = slot;
    }

    /** Plug in the delay bus; `slot` = the slot that controls it (time, feedback, return level...). */
    void SetDelayBus(DelayBus* bus, int slot)
    {
        delay_bus_  = bus;
        delay_slot_ = slot;
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
        s.delay_ringing  = delay_bus_ && delay_bus_->Running();
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

        // The two buses (reverb, delay): each runs while something feeds it, and until its tail has died away.
        // Only ONE of them feeds the other (the routing value of the delay slot), so there is never a loop:
        //   routing < 0.5 (default): the reverb runs first and its return is added to the delay's input.
        //   routing >= 0.5:          the delay runs first and its return is added to the reverb's input.
        const bool xf       = xf_pos_ < xf_len_;
        const bool rev_on   = reverb_bus_ && bus_slot_ >= 0 && engines_[bus_slot_];
        const bool del_on   = delay_bus_ && delay_slot_ >= 0 && engines_[delay_slot_];
        bool       rev_feed = FeedsReverb(cur_) || (xf && FeedsReverb(prev_));
        bool       del_feed = FeedsDelay(cur_) || (xf && FeedsDelay(prev_));
        float      rl[kMaxChunk], rr[kMaxChunk], dl[kMaxChunk], dr[kMaxChunk];
        for(size_t i = 0; i < n; i++)
            rl[i] = rr[i] = dl[i] = dr[i] = 0.0f;
        if(del_on && settings_[delay_slot_].v[kRouting] >= 0.5f)
        {
            // delay > reverb
            RunDelay(info.samples_per_bar, n, del_feed, dl, dr);
            if(rev_on)
            {
                const float send = settings_[delay_slot_].v[kSendReverb];
                for(size_t i = 0; i < n; i++)
                {
                    feed_l_[i] += dl[i] * send;
                    feed_r_[i] += dr[i] * send;
                }
                if(send > 0.001f && delay_bus_->Running())
                    rev_feed = true; // keep the reverb running while the delay is still making sound
                RunReverb(n, rev_feed, rl, rr);
            }
        }
        else
        {
            // reverb > delay
            if(rev_on)
            {
                RunReverb(n, rev_feed, rl, rr);
                if(del_on)
                {
                    const float send = settings_[bus_slot_].v[kSendDelay];
                    for(size_t i = 0; i < n; i++)
                    {
                        feed2_l_[i] += rl[i] * send;
                        feed2_r_[i] += rr[i] * send;
                    }
                    if(send > 0.001f && reverb_bus_->Running())
                        del_feed = true; // keep the delay running while the reverb is still making sound
                }
            }
            if(del_on)
                RunDelay(info.samples_per_bar, n, del_feed, dl, dr);
        }
        for(size_t i = 0; i < n; i++)
        {
            out_l[i] += rl[i] + dl[i];
            out_r[i] += rr[i] + dr[i];
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

    /** Run the reverb bus on feed_l_/feed_r_; ret = its return after the slot's wet level, hipass and gain. */
    void RunReverb(size_t n, bool feeding, float* ret_l, float* ret_r)
    {
        float tl[kMaxChunk], tr[kMaxChunk];
        reverb_bus_->Process(feed_l_, feed_r_, tl, tr, n, settings_[bus_slot_], feeding);
        // the return: wet level, hipass, lopass, gain (the reverb slot's knobs 5-8)
        SlotSettings rs = settings_[bus_slot_];
        rs.v[kLp]       = 1.0f; // lopass off on the return (K8 filters the reverb input instead)
        bus_stage_.Process(zero_, zero_, tl, tr, ret_l, ret_r, n, rs);
    }

    /** Run the delay bus on feed2_l_/feed2_r_; ret = its return after the slot's return level and gain.
     *  (K7 / K8 of the delay slot place the bandpass inside the feedback loop, so the stage's hipass / lopass are off.) */
    void RunDelay(float samples_per_bar, size_t n, bool feeding, float* ret_l, float* ret_r)
    {
        float tl[kMaxChunk], tr[kMaxChunk];
        delay_bus_->Process(feed2_l_, feed2_r_, tl, tr, n, settings_[delay_slot_], feeding, samples_per_bar);
        SlotSettings ds = settings_[delay_slot_];
        ds.v[kHp]       = 0.0f;
        ds.v[kLp]       = 1.0f;
        delay_stage_.Process(zero_, zero_, tl, tr, ret_l, ret_r, n, ds);
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
            if(engines_[cur_]->IsBus())
                feed_lp_prime_ = true;
            Emit(EventType::On, cur_);
        }
        else
            Emit(EventType::Off, prev_);
    }

    /** Does this slot send audio into the reverb bus? (the reverb slot itself, or another fx with a reverb send above 0;
     *  the delay slot does not: its return reaches the reverb through the routing, in Process) */
    bool FeedsReverb(int slot) const
    {
        if(slot < 0 || !engines_[slot])
            return false;
        if(slot == bus_slot_)
            return true;
        return slot != delay_slot_ && settings_[slot].v[kSendReverb] > 0.001f;
    }

    /** The same for the delay bus. */
    bool FeedsDelay(int slot) const
    {
        if(slot < 0 || !engines_[slot] || !delay_bus_)
            return false;
        if(slot == delay_slot_)
            return true;
        return slot != bus_slot_ && settings_[slot].v[kSendDelay] > 0.001f;
    }

    /** What one slot makes of the input (slot < 0 = the input itself), and what it sends to the two buses:
     *  feed_l/r = into the reverb, feed2_l/r = into the delay.
     *  A bus slot (reverb or delay) sends its whole input into its OWN bus and lets the dry through at the
     *  output gain; other fx send their output times their send amounts. */
    void SlotOut(int slot, const float* in_l, const float* in_r, float* out_l, float* out_r,
                 float* feed_l, float* feed_r, float* feed2_l, float* feed2_r, size_t off, size_t len)
    {
        if(slot < 0 || !engines_[slot])
        {
            for(size_t i = 0; i < len; i++)
            {
                out_l[i]   = in_l[off + i];
                out_r[i]   = in_r[off + i];
                feed_l[i]  = feed_r[i] = 0.0f;
                feed2_l[i] = feed2_r[i] = 0.0f;
            }
            return;
        }
        if(engines_[slot]->IsBus())
        {
            // A real SEND: the dry signal gets no wet/dry change (so letting go changes nothing but the
            // input to the bus stopping and the tail rings out). The slot's wet knob (K6) is the
            // return level, applied to the bus output in Process(). The OUTPUT GAIN (K5) applies to
            // the whole output while the bus is on (dry here, return in Process()).
            // REVERB slot: the LOPASS (K8) filters what goes INTO the reverb (before the input diffusion and
            // the reverb itself); the dry signal and the return get no lopass. The return gets the hipass (K7).
            // DELAY slot: K7 / K8 are the bandpass inside the feedback loop, so the input is sent in unfiltered.
            const bool  is_delay = slot == delay_slot_;
            const float g        = std::pow(10.0f, GainDb(settings_[slot].v[kGain]) / 20.0f);
            const bool  lp_on    = !is_delay && LpOn(settings_[slot].v[kLp]);
            if(lp_on)
            {
                feed_lp_[0].SetCutoff(LpHz(settings_[slot].v[kLp]), sr_);
                feed_lp_[1].SetCutoff(LpHz(settings_[slot].v[kLp]), sr_);
            }
            float& last_g = bus_last_gain_[slot];
            for(size_t i = 0; i < len; i++)
            {
                const float gi = last_g + (g - last_g) * static_cast<float>(i + 1) / static_cast<float>(len);
                const float dl = in_l[off + i], dr = in_r[off + i];
                float       fl = dl, fr = dr; // what the bus is fed
                if(lp_on)
                {
                    if(feed_lp_prime_) // start the filter as if this level had been there for ever (no thump)
                    {
                        feed_lp_[0].Prime(dl);
                        feed_lp_[1].Prime(dr);
                        feed_lp_prime_ = false;
                    }
                    fl = feed_lp_[0].Process(dl, true);
                    fr = feed_lp_[1].Process(dr, true);
                }
                out_l[i] = dl * gi; // dry: only the output gain
                out_r[i] = dr * gi;
                if(is_delay)
                {
                    feed_l[i]  = feed_r[i] = 0.0f;
                    feed2_l[i] = fl;
                    feed2_r[i] = fr;
                }
                else
                {
                    feed_l[i]  = fl; // reverb input: lowpassed (K8), no gain
                    feed_r[i]  = fr;
                    feed2_l[i] = feed2_r[i] = 0.0f;
                }
            }
            last_g = g;
            return;
        }
        float fl[kMaxChunk], fr[kMaxChunk];
        engines_[slot]->Process(in_l + off, in_r + off, fl, fr, len, settings_[slot].v);
        stages_[slot].Process(in_l + off, in_r + off, fl, fr, out_l, out_r, len, settings_[slot]);
        const float send_r = reverb_bus_ ? settings_[slot].v[kSendReverb] : 0.0f;
        const float send_d = delay_bus_ ? settings_[slot].v[kSendDelay] : 0.0f;
        for(size_t i = 0; i < len; i++)
        {
            feed_l[i]  = out_l[i] * send_r;
            feed_r[i]  = out_r[i] * send_r;
            feed2_l[i] = out_l[i] * send_d;
            feed2_r[i] = out_r[i] * send_d;
        }
    }

    /** Process in[off .. off+len) into out[off ..] (and the bus feeds), fading old -> new fx if a switch is under way. */
    void Segment(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t off, size_t len)
    {
        if(len == 0)
            return;
        float al[kMaxChunk], ar[kMaxChunk], afl[kMaxChunk], afr[kMaxChunk], agl[kMaxChunk], agr[kMaxChunk];
        if(xf_pos_ >= xf_len_)
        {
            prev_ = -1;
            SlotOut(cur_, in_l, in_r, al, ar, afl, afr, agl, agr, off, len);
            for(size_t i = 0; i < len; i++)
            {
                out_l[off + i]    = al[i];
                out_r[off + i]    = ar[i];
                feed_l_[off + i]  = afl[i];
                feed_r_[off + i]  = afr[i];
                feed2_l_[off + i] = agl[i];
                feed2_r_[off + i] = agr[i];
            }
            return;
        }
        float bl[kMaxChunk], br[kMaxChunk], bfl[kMaxChunk], bfr[kMaxChunk], bgl[kMaxChunk], bgr[kMaxChunk];
        SlotOut(prev_, in_l, in_r, al, ar, afl, afr, agl, agr, off, len);
        SlotOut(cur_, in_l, in_r, bl, br, bfl, bfr, bgl, bgr, off, len);
        for(size_t i = 0; i < len; i++)
        {
            float t = static_cast<float>(xf_pos_ + i + 1) / static_cast<float>(xf_len_);
            if(t > 1.0f)
                t = 1.0f;
            out_l[off + i]    = al[i] * (1.0f - t) + bl[i] * t;
            out_r[off + i]    = ar[i] * (1.0f - t) + br[i] * t;
            feed_l_[off + i]  = afl[i] * (1.0f - t) + bfl[i] * t;
            feed_r_[off + i]  = afr[i] * (1.0f - t) + bfr[i] * t;
            feed2_l_[off + i] = agl[i] * (1.0f - t) + bgl[i] * t;
            feed2_r_[off + i] = agr[i] * (1.0f - t) + bgr[i] * t;
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
    float      bus_last_gain_[kNumSlots];  // per bus slot, for gliding the dry gain while that slot is on
    TwoPole    feed_lp_[2];               // the reverb slot's lopass (K8): filters what goes INTO the reverb
    bool       feed_lp_prime_ = true;
    float      feed_l_[kMaxChunk], feed_r_[kMaxChunk]; // what is sent into the reverb this block
    // delay bus (same idea)
    DelayBus*  delay_bus_  = nullptr;
    int        delay_slot_ = -1;
    Stage      delay_stage_;              // return level / gain of the delay return
    float      feed2_l_[kMaxChunk], feed2_r_[kMaxChunk]; // what is sent into the delay this block
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
