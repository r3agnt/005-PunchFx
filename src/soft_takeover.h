#pragma once

#include <cmath>

/** SoftTakeover : stops a knob from making a value JUMP.
 *
 *  When a value was set somewhere else (MIDI, a different knob layer, power-up
 *  default), the knob's physical position does not match it. The knob is
 *  ignored until you turn it past the current value (or close to it); from
 *  then on it controls the value normally.
 *
 *  One of these exists for every knob + layer (normal / shift).
 */
struct SoftTakeover
{
    bool  engaged      = false;
    bool  has_last     = false;
    float last_pos     = 0.f;
    float last_written = 0.f;

    /** Call when the knob moves. `pos` = knob position 0-1, `current` = the
     *  parameter's current value. Returns true if the knob may write it. */
    bool Update(float pos, float current)
    {
        // Value changed by something else (e.g. MIDI)? Knob must pick up again.
        if(engaged && std::fabs(current - last_written) > 0.0001f)
            engaged = false;

        if(!engaged)
        {
            const bool close   = std::fabs(pos - current) < 0.02f;
            const bool crossed = has_last
                                 && (last_pos - current) * (pos - current) <= 0.f;
            if(close || crossed)
                engaged = true;
        }
        last_pos = pos;
        has_last = true;
        return engaged;
    }

    /** Call after writing the value so changes from elsewhere can be spotted. */
    void Written(float value) { last_written = value; }

    /** Forget everything (e.g. when shift is pressed or released). */
    void Reset()
    {
        engaged  = false;
        has_last = false;
    }
};
