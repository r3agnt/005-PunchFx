#pragma once
// Bit crusher. Signal path, in order:
//   pre lopass -> add noise -> bit depth (quantise) -> sample rate (hold)
// then the shared stage adds wet/dry, post hipass, post lopass and gain (fx_common.h).
//   K1 Pre lopass   200 Hz .. 20 kHz (top = off)   (tames the highs before they get crushed)
//   K2 Noise        0 .. 100%: white noise added before the quantiser. It is scaled to the size
//                   of one quantiser step, so it gets stronger as the bit depth drops (grit).
//   K3 Bit depth    12 bits .. 1 bit (most of the travel is the audible low end)
//   K4 Sample rate  48 kHz .. 1 kHz (the audio is held for 1..48 samples)
#include <cmath>
#include <cstdint>
#include "fx_engine.h"
#include "fx_common.h"

namespace fx {

class BitCrush : public Engine
{
  public:
    void Init(float sample_rate) override
    {
        sr_ = sample_rate;
        Reset();
    }
    void Reset() override
    {
        count_   = 0;
        held_[0] = held_[1] = 0.0f;
        pre_[0].Reset();
        pre_[1].Reset();
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
            pre_[1].SetCutoff(LpHz(knobs[0]), sr_);
        }
        for(size_t i = 0; i < n; i++)
        {
            float l = in_l[i], r = in_r[i];
            if(pre_on)
            {
                if(prime_)
                {
                    pre_[0].Prime(l);
                    pre_[1].Prime(r);
                }
                l = pre_[0].Process(l, true);
                r = pre_[1].Process(r, true);
            }
            prime_ = false;
            if(count_ <= 0) // time to take a new sample
            {
                held_[0] = std::floor((l + noise * Noise()) / step + 0.5f) * step;
                held_[1] = std::floor((r + noise * Noise()) / step + 0.5f) * step;
                count_   = hold;
            }
            count_--;
            out_l[i] = held_[0];
            out_r[i] = held_[1];
        }
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
        s.v[3] = 0.3f; // hold 3
    }

    /** Knob 0..1 -> bits: 12 at 0, 1 at 1, bending so the audible 8..1 bit range gets most of the travel. */
    static float Bits(float v) { return 1.0f + 11.0f * std::pow(1.0f - v, 1.5f); }
    /** Knob 0..1 -> samples to hold: 1 .. 48. */
    static int Hold(float v) { return 1 + static_cast<int>(v * v * 47.0f + 0.5f); }

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
    float    held_[2] = {0.0f, 0.0f};
    TwoPole  pre_[2];
    bool     prime_ = true;
    uint32_t seed_  = 2463534242u;
};

} // namespace fx
