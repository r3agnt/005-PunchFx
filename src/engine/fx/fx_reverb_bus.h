#pragma once
// The REVERB BUS (plain C++): pre-delay + a reverb core, which keeps running after its input stops
// until the tail has died away, then switches itself off (saves CPU).
// The real reverb core on the Seed is DaisySP's ReverbSc (src/modules/reverb_core.h); the Mac tests
// use a simple stand-in. Controls (the reverb slot's knobs): K1 Time (tail length 0.4-12 s), K2 Tone, K3 Pre-delay (0-100 ms).
#include <cmath>
#include <cstddef>
#include "fx_engine.h"

namespace fx {

/** What the bus needs from a reverb. */
class ReverbCore
{
  public:
    virtual ~ReverbCore() {}
    virtual void Init(float sample_rate) = 0;
    /** Reverb time: 0 .. 1, tail becomes infinite near 1. */
    virtual void SetFeedback(float fb) = 0;
    /** Damping filter: how bright the tail is. */
    virtual void SetToneHz(float hz) = 0;
    virtual void Process(float in_l, float in_r, float* out_l, float* out_r) = 0;
};

/** A Schroeder allpass filter: passes every frequency at the same level but smears the timing, so one
 *  click becomes a short dense burst. Used to diffuse the input of the reverb. No constructor on purpose
 *  (it lives inside the bus, which lives in SDRAM): call Init() before use. */
class Allpass
{
  public:
    static constexpr size_t kMax = 1024;
    void Init(size_t length, float gain)
    {
        len_ = length < 1 ? 1 : (length > kMax ? kMax : length);
        g_   = gain;
        idx_ = 0;
        for(size_t i = 0; i < kMax; i++)
            buf_[i] = 0.0f;
    }
    float Process(float x)
    {
        const float old = buf_[idx_];
        const float w   = x + g_ * old;
        buf_[idx_]      = w;
        if(++idx_ >= len_)
            idx_ = 0;
        return old - g_ * w;
    }

  private:
    float  buf_[kMax];
    size_t len_, idx_;
    float  g_;
};

class ReverbBus
{
  public:
    // K1 TIME is scaled by the TAIL LENGTH (how long the reverb takes to fade by 60 dB) in even
    // steps (each knob step multiplies the length by the same amount), then converted into the
    // feedback amount ReverbSc wants. ReverbSc's 8 delay lines are 62.8 ms long on average and lose
    // "feedback" every pass, so  feedback = 10 ^ (-3 * 0.0628 / tail_seconds).
    static constexpr float kTimeMinS    = 0.4f;    // tail length with the knob at 0
    static constexpr float kTimeMaxS    = 12.0f;   // ... and at 1
    static constexpr float kMeanDelayS  = 0.0628f; // average delay line length in ReverbSc
    static float TailSeconds(float v) { return kTimeMinS * std::pow(kTimeMaxS / kTimeMinS, v); }
    static float Feedback(float v) { return std::pow(10.0f, -3.0f * kMeanDelayS / TailSeconds(v)); }
    static float ToneHz(float v) { return 800.0f * std::pow(22.5f, v); } // K2: knob -> 800 Hz .. 18 kHz
    static float PreDelayMs(float v) { return v * 100.0f; }              // K3: knob -> 0 .. 100 ms

    // INPUT DIFFUSION (fixed): 4 allpass filters per side in front of the reverb core, so the first
    // echoes of its 8 delay lines (which come out sparse and bright, like a slapback) are blurred
    // into a dense burst. Lengths in samples at 48 kHz (left / right differ a little).
    static constexpr size_t kApCount    = 4;
    static constexpr float  kApGain     = 0.7f;
    static constexpr size_t kApLen(int side, size_t k)
    {
        return side == 0 ? (k == 0 ? 142 : k == 1 ? 107 : k == 2 ? 379 : 277)
                         : (k == 0 ? 151 : k == 1 ? 113 : k == 2 ? 397 : 293);
    }
    /** Fixed make-up gain on the reverb output (ReverbSc is quiet): 2.5 = +8 dB. K5 adds up to +8 dB more. */
    static constexpr float kMakeup = 2.5f;

    void Init(float sample_rate, ReverbCore* core)
    {
        sr_   = sample_rate;
        core_ = core;
        core_->Init(sample_rate);
        for(size_t i = 0; i < kSize; i++)
            pre_[0][i] = pre_[1][i] = 0.0f;
        // Set EVERY starting value here: on the Seed this object lives in SDRAM, which is not
        // cleared at power-up and must not have a constructor.
        w_       = 0;
        pd_      = 0.0f;
        quiet_   = 0;
        running_ = false;
        diffuse_ = true;
        for(int side = 0; side < 2; side++)
            for(size_t k = 0; k < kApCount; k++)
                ap_[side][k].Init(static_cast<size_t>(static_cast<float>(kApLen(side, k)) * sample_rate / 48000.0f + 0.5f), kApGain);
    }

    /** Switch the input diffusion on/off (on after Init; the Mac tests switch it off to measure other things). */
    void SetDiffusion(bool on) { diffuse_ = on; }

    /** True while the reverb is running (fed, or its tail is still audible). */
    bool Running() const { return running_; }

    /** feed = what goes into the reverb (zeros if nothing). `feeding` = true while something is sent
     *  in (so the reverb must run). `s` = the reverb slot's settings (K1 time, K2 tone, K3 pre-delay).
     *  ret = the reverb's output (zeros when it is off). */
    void Process(const float* feed_l, const float* feed_r, float* ret_l, float* ret_r, size_t n,
                 const SlotSettings& s, bool feeding)
    {
        if(!feeding && !running_)
        {
            for(size_t i = 0; i < n; i++)
                ret_l[i] = ret_r[i] = 0.0f;
            return;
        }
        if(feeding)
        {
            running_ = true;
            quiet_   = 0;
        }
        core_->SetFeedback(Feedback(s.v[0]));
        core_->SetToneHz(ToneHz(s.v[1]));
        // Pre-delay glides towards the knob (a jump would sound like a pitch change).
        const float target = PreDelayMs(s.v[2]) * 0.001f * sr_;
        pd_ += (target - pd_) * 0.2f;
        float peak = 0.0f;
        for(size_t i = 0; i < n; i++)
        {
            pre_[0][w_] = feed_l[i];
            pre_[1][w_] = feed_r[i];
            float rp = static_cast<float>(w_) - pd_;
            if(rp < 0.0f)
                rp += static_cast<float>(kSize);
            const size_t i0   = static_cast<size_t>(rp) & kMask;
            const size_t i1   = (i0 + 1) & kMask;
            const float  frac = rp - std::floor(rp);
            float        dl   = pre_[0][i0] * (1.0f - frac) + pre_[0][i1] * frac;
            float        dr   = pre_[1][i0] * (1.0f - frac) + pre_[1][i1] * frac;
            w_                = (w_ + 1) & kMask;
            if(diffuse_)
                for(size_t k = 0; k < kApCount; k++)
                {
                    dl = ap_[0][k].Process(dl);
                    dr = ap_[1][k].Process(dr);
                }
            core_->Process(dl, dr, &ret_l[i], &ret_r[i]);
            peak = std::fmax(peak, std::fmax(std::fabs(ret_l[i]), std::fabs(ret_r[i])));
        }
        if(!feeding)
        {
            // Nothing is being sent in: switch off once the tail has been inaudible (-80 dB) for 0.3 s.
            if(peak < 1e-4f)
                quiet_ += n;
            else
                quiet_ = 0;
            if(quiet_ > static_cast<size_t>(0.3f * sr_))
                running_ = false;
        }
        for(size_t i = 0; i < n; i++) // make-up gain (after the tail check above, so auto-off is unchanged)
        {
            ret_l[i] *= kMakeup;
            ret_r[i] *= kMakeup;
        }
    }

  private:
    static constexpr size_t kSize = 16384; // 340 ms, enough for the 200 ms pre-delay
    static constexpr size_t kMask = kSize - 1;
    // NO "= value" defaults here on purpose: this object lives in SDRAM on the Seed, and a
    // constructor would run before the SDRAM is switched on (that froze the 0.12.0 firmware).
    // Init() sets every member instead.
    float        sr_, pd_;
    ReverbCore*  core_;
    float        pre_[2][kSize];
    size_t       w_, quiet_;
    bool         running_, diffuse_;
    Allpass      ap_[2][kApCount];
};

/** The reverb's slot (B1): knob names and defaults. The audio is handled by the layer + ReverbBus. */
class ReverbSlot : public Engine
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
        static const char* const kNames[3] = {"Time", "Tone", "Pre-delay"};
        return (i >= 0 && i < 3) ? kNames[i] : nullptr;
    }
    float KnobReal(int i, float v) const override
    {
        if(i == 0)
            return ReverbBus::TailSeconds(v);
        if(i == 1)
            return ReverbBus::ToneHz(v);
        return ReverbBus::PreDelayMs(v);
    }
    const char* KnobUnit(int i) const override
    {
        static const char* const kUnits[3] = {" s", " Hz", " ms"};
        return (i >= 0 && i < 3) ? kUnits[i] : "";
    }
    void SetDefaults(SlotSettings& s) const override
    {
        s.v[0]     = 0.78f;   // time: about 5.7 s
        s.v[1]     = 0.5678f; // tone: 4686 Hz
        s.v[2]     = 0.0f;    // pre-delay: 0 ms
        s.v[kWet]  = 0.4f; // 40% reverb
        s.v[kSendDelay] = 0.5f; // reverb -> delay: 50% (only used when the routing is reverb > delay, the default)
    }
};

} // namespace fx
