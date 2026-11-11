#pragma once
// Bit crusher: fewer bits (rough steps in level) and a lower sample rate (audio held for a few samples).
#include <cmath>
#include "fx_engine.h"

namespace fx {

class BitCrush : public Engine
{
  public:
    void Init(float) override { Reset(); }
    void Reset() override
    {
        count_ = 0;
        held_[0] = held_[1] = 0.0f;
    }
    /** knobs[0] = bit depth 16 (knob 0) .. 2 bits (knob 1); knobs[1] = sample hold 1 .. 32 samples. */
    void Process(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t n,
                 const float* knobs) override
    {
        const float step = 2.0f / std::pow(2.0f, Bits(knobs[0])); // size of one level step
        const int   hold = Hold(knobs[1]);
        for(size_t i = 0; i < n; i++)
        {
            if(count_ <= 0) // time to take a new sample
            {
                held_[0] = std::floor(in_l[i] / step + 0.5f) * step;
                held_[1] = std::floor(in_r[i] / step + 0.5f) * step;
                count_   = hold;
            }
            count_--;
            out_l[i] = held_[0];
            out_r[i] = held_[1];
        }
    }
    const char* KnobName(int i) const override
    {
        return i == 0 ? "Bit depth" : (i == 1 ? "Sample hold" : nullptr);
    }
    float KnobReal(int i, float v) const override
    {
        return i == 0 ? Bits(v) : static_cast<float>(Hold(v));
    }
    const char* KnobUnit(int i) const override { return i == 0 ? " bits" : " samples"; }
    void        SetDefaults(SlotSettings& s) const override
    {
        s.v[0] = 0.5f; // 9 bits
        s.v[1] = 0.3f; // hold 3
    }

    static float Bits(float v) { return 16.0f - v * 14.0f; }
    static int   Hold(float v) { return 1 + static_cast<int>(v * v * 31.0f + 0.5f); }

  private:
    int   count_   = 0;
    float held_[2] = {0.0f, 0.0f};
};

} // namespace fx
