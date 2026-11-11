#pragma once
// Layer 2 (punch-in fx): the common shape of one effect, plus the settings every fx has.
// Plain C++ (no libDaisy), so it can be tested on the Mac.
#include <cstddef>

namespace fx {

constexpr int kNumSlots = 15; // layer 2 buttons: B1-B7 = slots 0-6, B9-B16 = slots 7-14

/** Every fx has 8 knob values (0.0 - 1.0), one per knob:
 *  knobs 1-4 (index 0-3) = the fx's own parameters (each fx decides what they mean),
 *  knobs 5-8 (index 4-7) = the same for every fx: output gain, wet/dry, hipass, lopass. */
constexpr int kNumKnobs = 8;
constexpr int kGain = 4, kWet = 5, kHp = 6, kLp = 7;
/** Plus two "send" amounts per slot (set with Shift + K1 / Shift + K2 in edit mode): how much of
 *  this fx's output goes into the reverb bus / the delay bus (0 = none). */
constexpr int kNumValues = 10;
constexpr int kSendReverb = 8, kSendDelay = 9;

struct SlotSettings
{
    float v[kNumValues];
};

/** Neutral starting values: own params in the middle, 0 dB gain, 100% wet, filters off. */
inline SlotSettings DefaultSettings()
{
    SlotSettings s;
    for(int i = 0; i < 4; i++)
        s.v[i] = 0.5f;
    s.v[kGain] = 0.5f;
    s.v[kWet]  = 1.0f;
    s.v[kHp]   = 0.0f;
    s.v[kLp]   = 1.0f;
    s.v[kSendReverb] = 0.0f;
    s.v[kSendDelay]  = 0.0f;
    return s;
}

/** One effect. A new fx = a new class with these functions (see fx_bitcrush.h). */
class Engine
{
  public:
    virtual ~Engine() {}
    /** true = a BUS fx (reverb, delay): the layer feeds the input into a shared bus instead of calling Process. */
    virtual bool        IsBus() const { return false; }
    virtual void        Init(float sample_rate)  = 0;
    /** Forget old audio (called each time the fx starts). */
    virtual void        Reset()                  = 0;
    /** `knobs` = the first 4 knob values (0-1). In and out may be different arrays. */
    virtual void        Process(const float* in_l, const float* in_r, float* out_l, float* out_r,
                                size_t n, const float* knobs) = 0;
    /** Name of own knob i (0-3) for the log; nullptr = this knob does nothing. */
    virtual const char* KnobName(int i) const    = 0;
    /** Real-world value of knob i for the log (e.g. bits), and its unit text. */
    virtual float       KnobReal(int i, float v) const = 0;
    virtual const char* KnobUnit(int i) const    = 0;
    /** Starting values for this fx's own knobs. */
    virtual void        SetDefaults(SlotSettings& s) const { (void)s; }
};

} // namespace fx
