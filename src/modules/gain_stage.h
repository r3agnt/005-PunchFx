#pragma once

#include <cmath>
#include "Utility/dsp.h" // DaisySP: SoftClip(), fclamp()

/** GainStage : input gain and a safe output ceiling.
 *
 *  This is the small "example module" kept from the channel strip. It shows
 *  the pattern every module follows (see "Adding a module" in CLAUDE.md):
 *  SetControls() once per audio block, then one small function per sample.
 *
 *   1. Gain()   input gain, -12 dB to +24 dB.
 *   2. Limit()  daisysp::SoftClip (smooth curve, tops out at +/-1.0),
 *               scaled by the output level, then a hard clamp so NOTHING
 *               can exceed +/-ceiling.
 *
 *  Audio here is "float" where 1.0 = 0 dBFS (0 dB in a DAW).
 *  The ceiling is 0.9 (about -0.9 dBFS) for headroom with external gear.
 *  The output-level control only turns it DOWN from there (to silence).
 */
class GainStage
{
  public:
    /** Maximum output level (linear). 0.9 = about -0.9 dBFS. */
    static constexpr float kCeiling = 0.9f;
    /** Input gain range in dB (control 0.0 = min, 1.0 = max). */
    static constexpr float kMinGainDb = -12.0f;
    static constexpr float kMaxGainDb = 24.0f;

    /** Both values are 0.0 to 1.0. Call once per audio block. */
    void SetControls(float input_gain01, float out_level01)
    {
        gain_ = powf(10.0f, InputGainDb(input_gain01) / 20.0f);
        const float lvl = daisysp::fclamp(out_level01, 0.f, 1.f);
        ceiling_        = kCeiling * lvl * lvl; // squared = smoother taper
    }

    /** Control (0..1) -> input gain in dB. Also used by the serial log. */
    static float InputGainDb(float v)
    {
        return kMinGainDb + daisysp::fclamp(v, 0.f, 1.f) * (kMaxGainDb - kMinGainDb);
    }

    /** Control (0..1) -> output ceiling in dBFS (very negative = silent). */
    static float OutputLevelDb(float v)
    {
        const float lvl = daisysp::fclamp(v, 0.f, 1.f);
        return 20.0f * log10f(kCeiling * lvl * lvl + 1e-9f);
    }

    /** Step 1: input gain. */
    float Gain(float in) const { return in * gain_; }

    /** Step 2: soft clip, output level, hard ceiling. */
    float Limit(float x) const
    {
        x = daisysp::SoftClip(x) * ceiling_; // SoftClip is +/-1.0, so this is +/-ceiling
        return daisysp::fclamp(x, -ceiling_, ceiling_);
    }

  private:
    float gain_    = 1.0f;
    float ceiling_ = kCeiling;
};
