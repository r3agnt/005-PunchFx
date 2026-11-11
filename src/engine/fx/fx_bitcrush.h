#pragma once
// Bit crusher. Signal path, in order:
//   left+right summed to MONO -> pre lopass -> add noise -> bit depth (quantise) -> sample rate (hold)
//   -> mild stereo flanger (smooths the sound and gives the stereo width back)
// then the shared stage adds wet/dry, post hipass, post lopass and gain (fx_common.h).
//   K1 Pre lopass   200 Hz .. 20 kHz (top = off)   (tames the highs before they get crushed)
//   K2 Noise        0 .. 100%: white noise added before the quantiser. It is scaled to the size
//                   of one quantiser step, so it gets stronger as the bit depth drops (grit).
//   K3 Bit depth    12 bits .. 1 bit (most of the travel is the audible low end)
//   K4 Sample rate  48 kHz .. 188 Hz (the audio is held for 1..256 samples, equal octave steps)
#include <cmath>
#include <cstdint>
#include "fx_engine.h"
#include "fx_common.h"
#include "fx_flanger_core.h"

namespace fx {

class BitCrush : public Engine
{
  public:
    void Init(float sample_rate) override
    {
        sr_ = sample_rate;
        flanger_.Init(sample_rate);
        // Mild, slow, mostly a smoother: short sweep (1-2 ms), little feedback, highs rolled off.
        flanger_.rate_hz  = 0.25f;
        flanger_.base_ms  = 1.0f;
        flanger_.depth_ms = 1.0f;
        flanger_.feedback = 0.3f;
        flanger_.mix      = 0.4f;
        flanger_.damp_hz  = 7000.0f;
        Reset();
    }
    void Reset() override
    {
        count_   = 0;
        held_    = 0.0f;
        pre_[0].Reset();
        pre_[1].Reset();
        flanger_.Reset();
        prime_ = true;
    }
    void Process(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t n,
                 const float* knobs) override
    {
        const bool  pre_on = LpOn(knobs[0]);
        const float step   = 2.0f / std::pow(2.0f, Bits(knobs[2])); // size of one level step
        const float noise  = knobs[1] * (0.01f + 0.5f * step);      // noise size (grows with the crush)
        const int   hold   = Hold(knobs[3]);
        if(pre_on)
        {
            pre_[0].SetCutoff(LpHz(knobs[0]), sr_);
        }
        for(size_t i = 0; i < n; i++)
        {
            float m = 0.5f * (in_l[i] + in_r[i]); // mono
            if(pre_on)
            {
                if(prime_)
                    pre_[0].Prime(m);
                m = pre_[0].Process(m, true);
            }
            prime_ = false;
            if(count_ <= 0) // time to take a new sample
            {
                held_ = std::floor((m + noise * Noise()) / step + 0.5f) * step;
                count_ = hold;
            }
            count_--;
            out_l[i] = held_;
            out_r[i] = held_;
        }
        if(smooth_flanger)
            flanger_.Process(out_l, out_r, out_l, out_r, n);
    }
    const char* KnobName(int i) const override
    {
        static const char* const kNames[4] = {"Pre lopass", "Noise", "Bit depth", "Sample rate"};
        return (i >= 0 && i < 4) ? kNames[i] : nullptr;
    }
    float KnobReal(int i, float v) const override
    {
        switch(i)
        {
            case 0: return LpOn(v) ? LpHz(v) : 20000.0f;
            case 1: return v * 100.0f;
            case 2: return Bits(v);
            default: return sr_ / static_cast<float>(Hold(v));
        }
    }
    const char* KnobUnit(int i) const override
    {
        static const char* const kUnits[4] = {" Hz", "%", " bits", " Hz"};
        return (i >= 0 && i < 4) ? kUnits[i] : "";
    }
    void SetDefaults(SlotSettings& s) const override
    {
        s.v[0] = 1.0f; // pre lopass off
        s.v[1] = 0.0f; // no noise
        s.v[2] = 0.5f; // about 5 bits
        s.v[3] = 0.3f; // hold 5 (about 9 kHz)
    }

    /** Knob 0..1 -> bits: 12 at 0, 1 at 1, bending so the audible 8..1 bit range gets most of the travel. */
    static float Bits(float v) { return 1.0f + 11.0f * std::pow(1.0f - v, 1.5f); }
    /** Knob 0..1 -> samples to hold: 1 .. 256, in equal octave steps (2^(8 v)): 48 kHz down to 188 Hz. */
    static int Hold(float v) { return static_cast<int>(std::pow(2.0f, v * 8.0f) + 0.5f); }

  public:
    /** The smoothing flanger after the crusher (on by default; tests switch it off to check the crush maths). */
    bool smooth_flanger = true;

  private:
    /** White noise -1..1 (a small fixed-seed generator: no library, no memory). */
    float Noise()
    {
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 17;
        seed_ ^= seed_ << 5;
        return static_cast<float>(seed_) * (2.0f / 4294967296.0f) - 1.0f;
    }

    float    sr_      = 48000.0f;
    int      count_   = 0;
    float    held_  = 0.0f;
    TwoPole  pre_[2];
    MiniFlanger flanger_;
    bool     prime_ = true;
    uint32_t seed_  = 2463534242u;
};

} // namespace fx
