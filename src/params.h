#pragma once

#include <cstdint>
#include "desktop_devkit.h"

/** Params : the named settings of the looper.
 *
 *  This is the "list of things you can adjust". Knobs (main_page.h) and
 *  MIDI CC write these values; the audio code (main.cpp) reads them.
 *  All values are 0.0 to 1.0. Converting to real units (dB, Hz) happens
 *  inside each module (e.g. modules/gain_stage.h).
 *
 *  Defaults are "neutral": 0 dB input gain, full output.
 *
 *  To add a parameter:
 *   1. Add a float below.
 *   2. Add it to kCcMap (MIDI CC) and/or kPotMap (knobs) below.
 *   3. Read it in the AudioCallback in main.cpp.
 */
struct Params
{
    float input_offset = 0.5f;       // input offset against the clock (beat repeat): -20..+20 ms (0.5 = 0)
    float loop_length = 0.5f;        // loop cycle: 1, 2, 4 or 8 bars (0.5 = 4)
    float input_gain  = 1.0f / 3.0f; // -12..+24 dB, this default = 0 dB
    float out_level   = 1.0f;        // 1.0 = ceiling (0.9), lower = quieter
};

/** Defined in main.cpp. */
extern Params params;

/** KNOB MAP. One row per knob (row 1 = Pot 1 ... row 8 = Pot 8).
 *  Left column: used normally. Right column: used while SHIFT (button 8)
 *  is held. nullptr = not assigned. */
struct PotAssign
{
    float Params::*normal;
    float Params::*shifted;
    /** Soft takeover per layer. false = the knob always writes the value
     *  at once (use for settings where a jump is harmless). */
    bool normal_takeover  = true;
    bool shifted_takeover = true;
};
constexpr PotAssign kPotMap[daisy::desktop_devkit::kNumPots] = {
    {nullptr, &Params::input_gain},          // Pot 1  (Shift: input gain)
    {&Params::loop_length, &Params::out_level, false}, // Pot 2  loop length, no pick-up (Shift: output level)
    {nullptr, &Params::input_offset, true, false}, // Pot 3  (Shift: input offset, no pick-up)
    {nullptr, nullptr},             // Pot 4
    {nullptr, nullptr},             // Pot 5
    {nullptr, nullptr},             // Pot 6
    {nullptr, nullptr},             // Pot 7
    {nullptr, nullptr},             // Pot 8
};

/** MIDI CC MAP. Any MIDI channel is accepted. Value 0-127 -> 0.0-1.0. */
struct CcAssign
{
    uint8_t        cc;
    float Params::*param;
};
constexpr CcAssign kCcMap[] = {
    {20, &Params::input_gain},
    {21, &Params::loop_length},
    {22, &Params::out_level},
    {23, &Params::input_offset},
};

/** Set a parameter from a CC message. Returns true if the CC number is used. */
inline bool ApplyCC(uint8_t cc, uint8_t value)
{
    for(const auto &a : kCcMap)
    {
        if(a.cc == cc)
        {
            params.*(a.param) = value / 127.0f;
            return true;
        }
    }
    return false;
}
