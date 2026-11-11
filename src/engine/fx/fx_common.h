#pragma once
// The part every fx shares (knobs 5-8): wet/dry, hipass, lopass and output gain.
// Plain C++ (no libDaisy). Signal path inside one fx slot (all in SERIES, one after the other):
//   mix = dry * (1 - wet%) + fx * wet%   ->   hipass   ->   lopass   ->   gain
// The filters and gain act on the whole mix (dry too), so mixing never makes a phasey sound.
#include <cmath>
#include <cstddef>
#include "fx_engine.h"

namespace fx {

constexpr float kPi = 3.14159265358979f;

/** Knob 5 -> output gain in dB, -8 .. +8 (middle = 0 dB). */
inline float GainDb(float v) { return (v - 0.5f) * 16.0f; }
/** Knob 7 -> hipass cutoff in Hz: 20 Hz .. 2 kHz (bottom = off). */
inline float HpHz(float v) { return 20.0f * std::pow(100.0f, v); }
inline bool  HpOn(float v) { return v > 0.01f; }
/** Knob 8 -> lopass cutoff in Hz: 200 Hz .. 20 kHz (top = off). */
inline float LpHz(float v) { return 200.0f * std::pow(100.0f, v); }
inline bool  LpOn(float v) { return v < 0.99f; }

/** A 2-pole filter (12 dB per octave, no resonance: Q = 0.707) for one channel.
 *  "State variable filter" in the trapezoidal form: stable and clean when the cutoff moves. */
class TwoPole
{
  public:
    void Reset() { ic1_ = ic2_ = 0.0f; }
    /** Start as if the input had been x for ever (a hipass then gives 0 at once, no start-up thump). */
    void Prime(float x)
    {
        ic1_ = 0.0f;
        ic2_ = x;
    }
    void SetCutoff(float hz, float sample_rate)
    {
        const float k = 1.41421356f; // 1 / Q
        if(hz > sample_rate * 0.45f)
            hz = sample_rate * 0.45f;
        const float g = std::tan(kPi * hz / sample_rate);
        k_            = k;
        a1_           = 1.0f / (1.0f + g * (g + k));
        a2_           = g * a1_;
        a3_           = g * a2_;
    }
    /** Returns the lowpass (low = true) or hipass output for one sample. */
    float Process(float x, bool low)
    {
        const float v3 = x - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_           = 2.0f * v1 - ic1_;
        ic2_           = 2.0f * v2 - ic2_;
        return low ? v2 : x - k_ * v1 - v2;
    }

  private:
    float k_ = 1.41421356f, a1_ = 0, a2_ = 0, a3_ = 0, ic1_ = 0, ic2_ = 0;
};

/** The shared stage of one slot (it has its own filter memory). */
class Stage
{
  public:
    void Init(float sample_rate)
    {
        sr_ = sample_rate;
        Reset();
        last_gain_ = 1.0f;
        last_wet_  = 1.0f;
    }
    void Reset()
    {
        for(int c = 0; c < 2; c++)
            hp_[c].Reset(), lp_[c].Reset();
        prime_ = true; // the first sample after a Reset sets the filters' starting state
    }
    /** dry = the signal that went into the fx, fx = what the fx made of it. Writes the result to out
     *  (out may be the same arrays as dry).
     *  `s` = knob values 4-7 (see kGain...). Gain and wet/dry glide across the block (no clicks). */
    void Process(const float* dry_l, const float* dry_r, const float* fx_l, const float* fx_r,
                 float* out_l, float* out_r, size_t n, const SlotSettings& s)
    {
        const bool  hp_on = HpOn(s.v[kHp]), lp_on = LpOn(s.v[kLp]);
        const float gain  = std::pow(10.0f, GainDb(s.v[kGain]) / 20.0f);
        const float wet   = s.v[kWet];
        for(int c = 0; c < 2; c++)
        {
            if(hp_on)
                hp_[c].SetCutoff(HpHz(s.v[kHp]), sr_);
            if(lp_on)
                lp_[c].SetCutoff(LpHz(s.v[kLp]), sr_);
        }
        if(prime_) // a fresh start: no gliding from old settings
        {
            last_gain_ = gain;
            last_wet_  = wet;
        }
        const float inv = n ? 1.0f / static_cast<float>(n) : 0.0f;
        for(size_t i = 0; i < n; i++)
        {
            const float t = static_cast<float>(i + 1) * inv;
            const float g = last_gain_ + (gain - last_gain_) * t;
            const float w = last_wet_ + (wet - last_wet_) * t;
            for(int c = 0; c < 2; c++)
            {
                const float dry = c == 0 ? dry_l[i] : dry_r[i];
                const float fxs = c == 0 ? fx_l[i] : fx_r[i];
                float       x   = dry * (1.0f - w) + fxs * w; // 1. wet/dry
                if(prime_)
                {
                    // Start the filters as if this level had been there for ever: no thump.
                    if(hp_on)
                        hp_[c].Prime(x);
                    if(lp_on)
                        lp_[c].Prime(hp_on ? 0.0f : x);
                }
                if(hp_on)
                    x = hp_[c].Process(x, false); // 2. hipass
                if(lp_on)
                    x = lp_[c].Process(x, true);  // 3. lopass
                (c == 0 ? out_l : out_r)[i] = x * g; // 4. gain
            }
            prime_ = false;
        }
        last_gain_ = gain;
        last_wet_  = wet;
    }

  private:
    float    sr_ = 48000.0f, last_gain_ = 1.0f, last_wet_ = 1.0f;
    bool     prime_ = true;
    TwoPole  hp_[2], lp_[2];
};

} // namespace fx
