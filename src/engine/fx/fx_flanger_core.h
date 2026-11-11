#pragma once
// A small stereo flanger (plain C++): a short delay swept by a slow LFO, fed back a little.
// The delayed copy mixed with the original makes the moving "comb" sound. A gentle high-cut in the
// feedback path softens the highs, so a mild setting also smooths harsh material.
// Used inside the bit crusher (mild, fixed settings) and later by the flanger fx.
#include <cmath>
#include <cstddef>
#include "fx_common.h" // kPi

namespace fx {

class MiniFlanger
{
  public:
    // Settings (change any time):
    float rate_hz  = 0.25f;  // LFO speed
    float base_ms  = 1.0f;   // shortest delay
    float depth_ms = 1.0f;   // how far the LFO sweeps the delay (longest = base + depth, max about 8 ms)
    float feedback = 0.3f;   // 0..0.9 (more = more ringing)
    float mix      = 0.4f;   // level of the delayed copy against the original
    float damp_hz  = 7000.f; // high-cut in the feedback path
    float width    = 0.25f;  // LFO offset between left and right, in cycles (0 = same, 0.25 = 90 degrees)

    void Init(float sample_rate)
    {
        sr_ = sample_rate;
        Reset();
    }
    void Reset()
    {
        for(int c = 0; c < 2; c++)
        {
            for(size_t i = 0; i < kSize; i++)
                buf_[c][i] = 0.0f;
            damp_[c] = 0.0f;
        }
        w_ = 0;
    }
    /** In and out may be the same arrays. */
    void Process(const float* in_l, const float* in_r, float* out_l, float* out_r, size_t n)
    {
        const float a   = 1.0f - std::exp(-2.0f * kPi * damp_hz / sr_); // one-pole high-cut
        const float inc = rate_hz / sr_;
        const float norm = 1.0f / (1.0f + 0.5f * mix); // keeps the overall level about the same
        for(size_t i = 0; i < n; i++)
        {
            for(int c = 0; c < 2; c++)
            {
                const float x   = c == 0 ? in_l[i] : in_r[i];
                float       ph  = phase_ + (c == 1 ? width : 0.0f);
                ph -= std::floor(ph);
                const float lfo = 0.5f + 0.5f * std::sin(2.0f * kPi * ph);
                float       d   = (base_ms + depth_ms * lfo) * 0.001f * sr_; // delay in samples
                if(d > static_cast<float>(kSize - 2))
                    d = static_cast<float>(kSize - 2);
                float       rp = static_cast<float>(w_) - d;
                if(rp < 0.0f)
                    rp += static_cast<float>(kSize);
                const size_t i0   = static_cast<size_t>(rp) & kMask;
                const size_t i1   = (i0 + 1) & kMask;
                const float  frac = rp - std::floor(rp);
                const float  del  = buf_[c][i0] * (1.0f - frac) + buf_[c][i1] * frac;
                damp_[c] += a * (del - damp_[c]);
                buf_[c][w_] = x + feedback * damp_[c];
                (c == 0 ? out_l : out_r)[i] = (x + mix * damp_[c]) * norm;
            }
            w_ = (w_ + 1) & kMask;
            phase_ += inc;
            if(phase_ >= 1.0f)
                phase_ -= 1.0f;
        }
    }

  private:
    static constexpr size_t kSize = 512; // about 10 ms at 48 kHz
    static constexpr size_t kMask = kSize - 1;
    float  sr_ = 48000.0f, phase_ = 0.0f;
    float  buf_[2][kSize];
    float  damp_[2] = {0.0f, 0.0f};
    size_t w_       = 0;
};

} // namespace fx
