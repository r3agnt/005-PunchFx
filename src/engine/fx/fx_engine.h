#pragma once
// Layer 2 (punch-in fx): the common shape of one effect, plus the settings every fx has.
// Plain C++ (no libDaisy), so it can be tested on the Mac.
#include <cstddef>

namespace fx {

constexpr int kNumSlots = 15; // layer 2 buttons: B1-B7 = slots 0-6, B9-B16 = slots 7-14
constexpr int kReverbSlot = 0; // B1: the reverb bus
constexpr int kDelaySlot  = 1; // B2: the delay bus

/** Every fx has 8 knob values (0.0 - 1.0), one per knob:
 *  knobs 1-4 (index 0-3) = the fx's own parameters (each fx decides what they mean),
 *  knobs 5-8 (index 4-7) = the same for every fx: output gain, wet/dry, hipass, lopass. */
constexpr int kNumKnobs = 8;
constexpr int kGain = 4, kWet = 5, kHp = 6, kLp = 7;
/** Plus two "send" amounts per slot (set with Shift + K1 / Shift + K2 in edit mode): how much of
 *  this fx's output goes into the reverb bus / the delay bus (0 = none).
 *  On the two bus slots the same values mean: reverb slot kSendDelay = how much reverb goes into the delay;
 *  delay slot kSendReverb = how much delay goes into the reverb.
 *  Value 10 (kRouting) is used by the DELAY slot only (Shift + K4): below 0.5 = reverb feeds the delay,
 *  0.5 or more = delay feeds the reverb (never both, so they can't feed each other in a loop). */
constexpr int kNumValues = 13;
constexpr int kSendReverb = 8, kSendDelay = 9, kRouting = 10;
/** Values 11 and 12: this slot's OWN delay time and feedback, used while this slot feeds the delay bus
 *  (Shift + K3 / Shift + K4 in edit mode, on every slot except the delay slot itself, which uses its K1/K2).
 *  The bottom of each knob (0) = "follow the delay slot's K1 / K2". */
constexpr int kDelayTime = 11, kDelayFb = 12;

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
    s.v[kRouting]    = 0.0f;
    s.v[kDelayTime]  = 0.0f; // follow the delay slot
    s.v[kDelayFb]    = 0.0f;
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
    /** Optional: a different log name for the shared knob 4-7 (gain, wet, hipass, lopass), or nullptr = the usual name. */
    virtual const char* KnobNameOverride(int knob) const { (void)knob; return nullptr; }
    /** Optional: text for the log instead of "number + unit" (knob 0-7). Return false = use the usual way. */
    virtual bool        KnobLabel(int knob, float v, char* buf, size_t cap) const
    {
        (void)knob; (void)v; (void)buf; (void)cap;
        return false;
    }
    /** Starting values for this fx's own knobs. */
    virtual void        SetDefaults(SlotSettings& s) const { (void)s; }
};

} // namespace fx
