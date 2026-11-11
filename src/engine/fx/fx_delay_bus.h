#pragma once
// The DELAY BUS (plain C++): a stereo echo that keeps running after its input stops, until the
// repeats have died away, then switches itself off (saves CPU). Same idea as the reverb bus.
//
// How an echo delay works, in plain words: the incoming sound is written into a long ring of memory
// (a "delay line"). We read it back a moment later (the delay TIME) and play that. A bit of what we
// read ("feedback") is also written back in again, so the echo repeats and slowly fades.
//
// What is special here:
//  - TIME is locked to the MIDI clock (K1 = a note length: 1/4, dotted 1/8, 1/8, dotted 1/16, 1/16, 1/32).
//    With no clock it keeps the last tempo it saw (120 BPM if it never saw one).
//  - When the time changes, the old and the new echo position are crossfaded (25 ms): no clicks, no pitch swoop.
//  - A BANDPASS FILTER sits in the feedback path (K7 = its centre frequency, K8 = its width; no resonance).
//    Each repeat is filtered again, so repeats lose lows and highs a bit more each time. The top edge is
//    never above 12 kHz, which gives the gentle high-end roll-off.
//  - K4 = ping-pong: 0 = each side repeats on its own side, 100% = the repeats bounce left/right.
//
// The delay memory (2 stereo buffers of kSize floats) is handed in from outside (main.cpp puts it in the
// external SDRAM). This object itself is small and may have a normal constructor.
#include <cmath>
#include <cstddef>
#include <cstdio>
#include "fx_engine.h"
#include "fx_common.h"

namespace fx {

class DelayBus
{
  public:
    static constexpr size_t kSize = 131072; // frames per side: 2.7 s at 48 kHz (a power of two)
    static constexpr size_t kMask = kSize - 1;
    static constexpr int    kNumSteps = 6;

    // ---- knob -> real value (also used by the log and the tests) ----
    /** K1 -> which of the 6 note lengths (0 = 1/4 ... 5 = 1/32). */
    static int StepIndex(float v)
    {
        int i = static_cast<int>(v * static_cast<float>(kNumSteps));
        return i < 0 ? 0 : (i >= kNumSteps ? kNumSteps - 1 : i);
    }
    /** Length of that step in beats (a beat = a quarter note). */
    static float BeatsFor(int step)
    {
        static const float kBeats[kNumSteps] = {1.0f, 0.75f, 0.5f, 0.375f, 0.25f, 0.125f};
        return kBeats[step < 0 ? 0 : (step >= kNumSteps ? kNumSteps - 1 : step)];
    }
    static const char* StepName(int step)
    {
        static const char* const kNames[kNumSteps] = {"1/4", "1/8 dotted", "1/8", "1/16 dotted", "1/16", "1/32"};
        return kNames[step < 0 ? 0 : (step >= kNumSteps ? kNumSteps - 1 : step)];
    }
    /** K2 -> feedback amount 0 .. 0.95 (never reaches 1, so the echoes always die away). */
    static float FeedbackAmt(float v) { return v * 0.95f; }
    /** K7 -> centre frequency of the feedback bandpass: 200 Hz .. 6 kHz. */
    static float CentreHz(float v) { return 200.0f * std::pow(30.0f, v); }
    /** K8 -> width of the bandpass in octaves: 1 .. 8. */
    static float WidthOct(float v) { return 1.0f + 7.0f * v; }
    static constexpr float kTopHz = 12000.0f; // the band never reaches above this (high-end roll-off)

    /** Hand in the memory (2 arrays of kSize floats). Sets EVERY member (the arrays are cleared). */
    void Init(float sample_rate, float* buf_l, float* buf_r)
    {
        sr_    = sample_rate;
        buf_[0] = buf_l;
        buf_[1] = buf_r;
        for(size_t i = 0; i < kSize; i++)
            buf_[0][i] = buf_[1][i] = 0.0f;
        w_        = 0;
        last_spb_ = 2.0f * sample_rate; // 120 BPM: one bar (4 beats) = 2 s
        d_a_ = d_b_ = static_cast<size_t>(0.5f * last_spb_ * 0.25f);
        fade_len_ = static_cast<size_t>(0.025f * sample_rate);
        fade_pos_ = fade_len_;
        quiet_    = 0;
        running_  = false;
        for(int c = 0; c < 2; c++)
            hp_[c].Reset(), lp_[c].Reset();
    }

    /** True while the delay is running (fed, or its repeats are still audible). */
    bool Running() const { return running_; }

    /** feed = what goes into the delay. `feeding` = true while something is sent in (so the delay must run).
     *  `s` = the delay slot's settings (K1 time, K2 feedback, K4 ping-pong, K7 band centre, K8 band width).
     *  `samples_per_bar` = from the MIDI clock (0 or less = no clock: keep the last tempo).
     *  ret = the echoes only (zeros when it is off). */
    void Process(const float* feed_l, const float* feed_r, float* ret_l, float* ret_r, size_t n,
                 const SlotSettings& s, bool feeding, float samples_per_bar)
    {
        if(!feeding && !running_)
        {
            for(size_t i = 0; i < n; i++)
                ret_l[i] = ret_r[i] = 0.0f;
            return;
        }
        const bool was_off = !running_;
        if(feeding)
        {
            running_ = true;
            quiet_   = 0;
        }
        if(samples_per_bar > 1000.0f)
            last_spb_ = samples_per_bar;

        // The wanted delay in samples (a beat = a quarter of a bar), kept inside the memory.
        float want = BeatsFor(StepIndex(s.v[0])) * last_spb_ * 0.25f;
        if(want < 64.0f)
            want = 64.0f;
        if(want > static_cast<float>(kSize - 16))
            want = static_cast<float>(kSize - 16);
        const size_t tgt = static_cast<size_t>(want + 0.5f);
        if(was_off)
        {
            d_a_ = d_b_ = tgt; // starting fresh: no crossfade
            fade_pos_   = fade_len_;
        }
        else
        {
            const size_t diff = tgt > d_b_ ? tgt - d_b_ : d_b_ - tgt;
            if(diff > kRetargetTol) // (smaller wobbles of the clock tempo are ignored)
            {
                if(fade_pos_ < fade_len_)
                    d_a_ = d_b_; // a fade was still running: finish it at once, then start the next
                d_b_      = tgt;
                fade_pos_ = 0;
            }
        }

        // Bandpass in the feedback path: a hipass at the low edge + a lowpass at the high edge.
        const float half  = std::pow(2.0f, 0.5f * WidthOct(s.v[7]));
        float       lo_hz = CentreHz(s.v[6]) / half;
        float       hi_hz = CentreHz(s.v[6]) * half;
        if(lo_hz < 20.0f)
            lo_hz = 20.0f;
        if(hi_hz > kTopHz)
            hi_hz = kTopHz;
        if(hi_hz < lo_hz * 1.2f)
            hi_hz = lo_hz * 1.2f;
        for(int c = 0; c < 2; c++)
        {
            hp_[c].SetCutoff(lo_hz, sr_);
            lp_[c].SetCutoff(hi_hz, sr_);
        }

        const float fb = FeedbackAmt(s.v[1]);
        const float pp = s.v[3]; // ping-pong amount
        float       peak = 0.0f;
        for(size_t i = 0; i < n; i++)
        {
            // 1. read the echo (from the old position, crossfading to the new one while the time changes)
            const size_t ia = (w_ - d_a_) & kMask;
            float        ol = buf_[0][ia], orr = buf_[1][ia];
            if(fade_pos_ < fade_len_)
            {
                const size_t ib = (w_ - d_b_) & kMask;
                const float  t  = static_cast<float>(fade_pos_ + 1) / static_cast<float>(fade_len_);
                ol              = ol * (1.0f - t) + buf_[0][ib] * t;
                orr             = orr * (1.0f - t) + buf_[1][ib] * t;
                if(++fade_pos_ >= fade_len_)
                    d_a_ = d_b_;
            }
            ret_l[i] = ol;
            ret_r[i] = orr;
            peak     = std::fmax(peak, std::fmax(std::fabs(ol), std::fabs(orr)));

            // 2. what is written back: the echo, crossed over by the ping-pong amount, then bandpassed
            float fl = ol * (1.0f - pp) + orr * pp;
            float fr = orr * (1.0f - pp) + ol * pp;
            fl       = lp_[0].Process(hp_[0].Process(fl, false), true);
            fr       = lp_[1].Process(hp_[1].Process(fr, false), true);

            // 3. the new input: with ping-pong the input goes into the left line only (as a mono sum),
            //    so the echoes bounce left, right, left...
            const float mid = 0.5f * (feed_l[i] + feed_r[i]);
            const float il  = feed_l[i] * (1.0f - pp) + mid * pp;
            const float ir  = feed_r[i] * (1.0f - pp);
            float       wl  = il + fb * fl;
            float       wr  = ir + fb * fr;
            if(std::fabs(wl) < 1e-9f) // tiny numbers make the chip slow: round them to zero
                wl = 0.0f;
            if(std::fabs(wr) < 1e-9f)
                wr = 0.0f;
            buf_[0][w_] = wl;
            buf_[1][w_] = wr;
            w_          = (w_ + 1) & kMask;
        }
        if(!feeding)
        {
            // Nothing is being sent in: switch off once nothing has been heard for one delay time + 0.3 s
            // (the echoes can be a whole delay time apart, so silence between two of them is normal).
            if(peak < 1e-4f)
                quiet_ += n;
            else
                quiet_ = 0;
            const size_t longest = d_a_ > d_b_ ? d_a_ : d_b_;
            if(quiet_ > longest + static_cast<size_t>(0.3f * sr_))
                running_ = false;
        }
    }

  private:
    static constexpr size_t kRetargetTol = 48; // 1 ms: changes of the wanted delay smaller than this are ignored
    float        sr_ = 48000.0f, last_spb_ = 96000.0f;
    float*       buf_[2] = {nullptr, nullptr};
    size_t       w_ = 0, d_a_ = 24000, d_b_ = 24000, fade_len_ = 1200, fade_pos_ = 1200, quiet_ = 0;
    bool         running_ = false;
    TwoPole      hp_[2], lp_[2];
};

/** The delay's slot (B2): knob names and defaults. The audio is handled by the layer + DelayBus. */
class DelaySlot : public Engine
{
  public:
    bool        IsBus() const override { return true; }
    void        Init(float) override {}
    void        Reset() override {}
    void        Process(const float* il, const float* ir, float* ol, float* orr, size_t n, const float*) override
    {
        for(size_t i = 0; i < n; i++) // not used (the layer feeds the bus), but pass the signal through
        {
            ol[i]  = il[i];
            orr[i] = ir[i];
        }
    }
    const char* KnobName(int i) const override
    {
        // K3 is free for now (nullptr = does nothing)
        static const char* const kNames[4] = {"Time", "Feedback", nullptr, "Ping-pong"};
        return (i >= 0 && i < 4) ? kNames[i] : nullptr;
    }
    // K7 / K8 are NOT hipass / lopass here: they place the bandpass inside the feedback loop.
    const char* KnobNameOverride(int knob) const override
    {
        if(knob == kHp)
            return "Band centre";
        if(knob == kLp)
            return "Band width";
        return nullptr;
    }
    float KnobReal(int i, float v) const override
    {
        if(i == 1)
            return DelayBus::FeedbackAmt(v) * 100.0f;
        if(i == 3)
            return v * 100.0f;
        return v;
    }
    const char* KnobUnit(int i) const override { return (i == 1 || i == 3) ? "%" : ""; }
    // Text for the log: the note length, the band centre in Hz, the band width in octaves (whole numbers only).
    bool KnobLabel(int knob, float v, char* buf, size_t cap) const override
    {
        if(knob == 0)
            std::snprintf(buf, cap, "%s", DelayBus::StepName(DelayBus::StepIndex(v)));
        else if(knob == kHp)
            std::snprintf(buf, cap, "%d Hz", static_cast<int>(DelayBus::CentreHz(v) + 0.5f));
        else if(knob == kLp)
        {
            const int tenths = static_cast<int>(DelayBus::WidthOct(v) * 10.0f + 0.5f);
            std::snprintf(buf, cap, "%d.%d oct", tenths / 10, tenths % 10);
        }
        else
            return false;
        return true;
    }
    void SetDefaults(SlotSettings& s) const override
    {
        s.v[0]           = 0.4167f; // time: 1/8 (the middle of step 3 of 6)
        s.v[1]           = 0.4f;    // feedback: 38%
        s.v[2]           = 0.5f;    // (free)
        s.v[3]           = 0.0f;    // ping-pong off
        s.v[kWet]        = 0.4f;    // return level 40%
        s.v[kHp]         = 0.6f;    // band centre ~1.5 kHz
        s.v[kLp]         = 0.75f;   // band width ~6 octaves: bright and open
        s.v[kSendReverb] = 0.5f;    // delay -> reverb: 50% (only used when the routing is delay > reverb)
        s.v[kRouting]    = 0.0f;    // routing: reverb feeds the delay
    }
};

} // namespace fx
