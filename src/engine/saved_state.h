#pragma once
// What is saved to the Seed's flash memory (plain C++ so it can be tested on the Mac).
// Saved: the four layer 1 knob settings and the knob values of all 15 fx slots.
// NOT saved: the latch, the layer, the switches, anything else.
// The struct must stay a plain block of 32-bit numbers (no padding): it is copied to/from flash as is.
// If you ever change it, change kMagic too: old saved data is then ignored (defaults are used).
#include <cmath>
#include <cstdint>
#include <cstring>
#include "fx/fx_engine.h"

struct SavedState
{
    static constexpr uint32_t kMagic = 0x50465801; // 'P' 'F' 'X' + layout version 1

    uint32_t         magic;
    float            input_offset; // the Params values, 0.0 - 1.0 (see params.h)
    float            loop_length;
    float            input_gain;
    float            out_level;
    fx::SlotSettings fx[fx::kNumSlots];

    /** Needed by libDaisy's PersistentStorage (it only writes to flash if something changed). */
    bool operator==(const SavedState &o) const { return std::memcmp(this, &o, sizeof(SavedState)) == 0; }
    bool operator!=(const SavedState &o) const { return !(*this == o); }

    /** True if this is data we wrote (right magic) and every value is a sane 0..1 number. */
    bool IsValid() const
    {
        if(magic != kMagic)
            return false;
        auto ok = [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; };
        if(!ok(input_offset) || !ok(loop_length) || !ok(input_gain) || !ok(out_level))
            return false;
        for(const auto &s : fx)
            for(float v : s.v)
                if(!ok(v))
                    return false;
        return true;
    }
};
static_assert(sizeof(SavedState) == sizeof(uint32_t) + 4 * sizeof(float) + fx::kNumSlots * fx::kNumKnobs * sizeof(float),
              "SavedState must have no padding");
