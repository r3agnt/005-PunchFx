#pragma once

#include "daisy.h"
#include "params.h"
#include "modules/gain_stage.h"
#include "engine/looper_config.h"

/** param_info.h : names and real-unit display of every parameter, used for
 *  the serial log (e.g. "Shift + K1 Input gain - +6.0 dB").
 *  Add a row here when you add a parameter to Params.
 */
enum class Unit { Percent, Db, Hz, Q, Bars, Ms, Semitones };

struct ParamInfo
{
    float Params::*param;
    const char    *name;
    Unit           unit;
    float (*to_real)(float); // control 0..1 -> real unit (Hz, dB, Q, 0..1)
};

// Small adapter functions (the module versions return double or are overloaded).
inline float ToPercent(float v) { return v; }
inline float ToInputGainDb(float v) { return GainStage::InputGainDb(v); }
inline float ToOutLevelDb(float v) { return GainStage::OutputLevelDb(v); }
inline float ToOffsetMs(float v) { return looper::OffsetMsFromControl(v); }
inline float ToLoopBars(float v) { return static_cast<float>(looper::LoopBarsFromControl(v)); }

static const ParamInfo kParamInfo[] = {
    {&Params::input_offset, "Input offset", Unit::Ms, ToOffsetMs},
    {&Params::loop_length, "Loop length", Unit::Bars, ToLoopBars},
    {&Params::input_gain, "Input gain", Unit::Db, ToInputGainDb},
    {&Params::out_level, "Output level", Unit::Db, ToOutLevelDb},
};

/** Find the info row for a parameter (nullptr if not listed). */
inline const ParamInfo *FindParamInfo(float Params::*p)
{
    for(const auto &info : kParamInfo)
        if(info.param == p)
            return &info;
    return nullptr;
}

/** Append a control value (0..1) in real units, e.g. "1.25 kHz", "+6.0 dB". */
inline void AppendValue(daisy::FixedCapStr<64> &s, const ParamInfo &info, float v01)
{
    const float real = info.to_real(v01);
    switch(info.unit)
    {
        case Unit::Percent:
            s.AppendInt(static_cast<int>(real * 100.f + 0.5f));
            s.Append("%");
            break;
        case Unit::Db:
            if(real < -90.f)
                s.Append("-inf dB");
            else
            {
                s.AppendFloat(real, 1, false, true);
                s.Append(" dB");
            }
            break;
        case Unit::Hz:
            if(real < 1000.f)
            {
                s.AppendInt(static_cast<int>(real + 0.5f));
                s.Append(" Hz");
            }
            else
            {
                s.AppendFloat(real / 1000.f, 2);
                s.Append(" kHz");
            }
            break;
        case Unit::Q:
            s.Append("Q ");
            s.AppendFloat(real, 2);
            break;
        case Unit::Ms:
            s.AppendFloat(real, 1, false, true);
            s.Append(" ms");
            break;
        case Unit::Semitones:
        {
            const int st = static_cast<int>(real + (real < 0.0f ? -0.5f : 0.5f));
            if(st > 0)
                s.Append("+");
            s.AppendInt(st);
            s.Append(" st");
            break;
        }
        case Unit::Bars:
            s.AppendInt(static_cast<int>(real + 0.5f));
            s.Append(real < 1.5f ? " bar" : " bars");
            break;
    }
}

/** Log a MIDI CC that was applied, e.g. "CC 20 Input gain - +6.0 dB". */
inline void LogCc(uint8_t cc, uint8_t value)
{
    for(const auto &a : kCcMap)
    {
        if(a.cc != cc)
            continue;
        daisy::FixedCapStr<64> s("CC ");
        s.AppendInt(cc);
        s.Append(" ");
        const ParamInfo *info = FindParamInfo(a.param);
        if(info)
        {
            s.Append(info->name);
            s.Append(" - ");
            AppendValue(s, *info, value / 127.0f);
        }
        daisy::Logger<daisy::LOGGER_EXTERNAL>::PrintLine("%s", s.Cstr());
        return;
    }
}
