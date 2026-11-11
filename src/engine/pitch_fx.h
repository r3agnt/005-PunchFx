#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include "looper_config.h"

/** pitch_fx.h : lowers the pitch of a stereo signal by a whole number of semitones
 *  (used on the beat repeat output while B6 is held, 0.6.0; tightened in 0.6.1).
 *
 *  Plain C++ (no libDaisy), tested on the Mac in test/test_pitch.cpp.
 *
 *  How it works, in plain English:
 *   - The input is always written into a short circular buffer (about 21 ms).
 *   - A "read head" reads from that buffer a little BEHIND the newest sample.
 *     If it falls further behind by `r` samples for every sample that passes
 *     (r = 1 - 2^(-steps/12)), what it reads plays slower, i.e. lower in pitch.
 *   - TIGHT START (0.6.1): at the start of every repeat of the beat repeat, a
 *     NEW head is started only ONE sample behind the live signal and the old
 *     head is cross-faded out over 5 ms (the same 5 ms the repeat itself uses
 *     for its loop point; the new head stands still for those 5 ms, so it is
 *     exactly 1 sample behind at the beat). So the hit at the start of each repeat lands on the
 *     beat with no delay; the pitch drop then develops during the repeat (the
 *     head slowly falls behind, like a tape slowing down, but the repeat keeps
 *     its length because the next repeat starts fresh).
 *   - The first shifted repeat starts from the dry signal the same way, so it
 *     joins with no jump.
 *   - In a long repeat a head keeps falling behind, so the sound would drift
 *     late. When it is more than kMaxDelay (10 ms) behind it hops forward by
 *     kHop (back to about 1 frame behind) with a 5 ms crossfade. So a slow
 *     repeat is never more than about 10 ms late; the price is a slightly
 *     grainy tone (a small splice every 10 ms / r, r = how fast the head falls
 *     behind, e.g. every 20 ms at -12 semitones).
 *   - Process() takes: `steps` = the semitones down for the repeat now playing,
 *     `engaged` = effect wanted, `jump_at` = the sample index in this block
 *     where a new repeat starts its loop crossfade (-1 = none).
 *
 *  Why not DaisySP's PitchShifter: its SetTransposition() only accepts whole
 *  semitones and its downward shifts are inexact (-5 comes out near -7, -12 is
 *  silence), and its delay is much longer.
 */
class PitchFx
{
  public:
    void Init(float sample_rate)
    {
        xfade_ = looper::XfadeFrames(sample_rate);
        if(xfade_ < 1)
            xfade_ = 1;
        for(size_t i = 0; i < kBuf; i++)
        {
            bl_[i] = 0.0f;
            br_[i] = 0.0f;
        }
        wi_ = 0;
        for(int k = 0; k < 2; k++)
            head_[k] = Head();
        for(int k = 0; k < 3; k++)
            ws_[k] = tg_[k] = k == 0 ? 1.0f : 0.0f;
        tc_       = 1;
        fade_len_ = 1;
        cur_      = -1;
    }

    /** Process `n` frames IN PLACE. See the header comment for the arguments. */
    void Process(float *l, float *r, size_t n, int steps, bool engaged, int jump_at)
    {
        if(steps < 0)
            steps = 0;
        if(steps > looper::kPitchMaxSteps)
            steps = looper::kPitchMaxSteps;
        const int desired = engaged ? steps : 0;

        // Block-level decisions (the repeat start itself is handled at its sample).
        if(desired == 0 && cur_ >= 0)
            ToDry(xfade_);
        else if(desired > 0 && cur_ < 0 && jump_at < 0)
            StartHead(desired, xfade_); // B6 came on in the middle of a repeat

        for(size_t i = 0; i < n; i++)
        {
            bl_[wi_] = l[i];
            br_[wi_] = r[i];

            if(static_cast<int>(i) == jump_at && desired > 0)
                StartHead(desired, xfade_); // a new repeat starts: new head, 1 sample behind
            else if(cur_ >= 0 && tc_ >= fade_len_ && head_[cur_].d >= kMaxDelay)
                Hop();                       // fell too far behind in a long repeat

            if(cur_ >= 0 || tc_ < fade_len_)
            {
                const float t = tc_ >= fade_len_ ? 1.0f
                                                 : static_cast<float>(tc_) / static_cast<float>(fade_len_);
                float       ol = 0.0f, orr = 0.0f;
                const float wd = ws_[0] + (tg_[0] - ws_[0]) * t;
                ol             = wd * l[i];
                orr            = wd * r[i];
                for(int k = 0; k < 2; k++)
                {
                    const float w = ws_[1 + k] + (tg_[1 + k] - ws_[1 + k]) * t;
                    if(head_[k].on)
                    {
                        float hl, hr;
                        Read(head_[k].d, hl, hr);
                        ol += w * hl;
                        orr += w * hr;
                        if(head_[k].hold > 0)
                            head_[k].hold--; // (stays 1 frame behind until the repeat's beat)
                        else
                            head_[k].d += head_[k].r;
                        if(head_[k].d > kMaxRead)
                            head_[k].d = kMaxRead;
                    }
                }
                l[i] = ol;
                r[i] = orr;
                if(tc_ < fade_len_)
                    tc_++;
                else
                    for(int k = 0; k < 2; k++)
                        if(head_[k].on && tg_[1 + k] == 0.0f)
                            head_[k].on = false;
            }
            wi_ = (wi_ + 1) & kMask;
        }
    }

  private:
    static constexpr size_t kBuf     = 1024; // power of two (two PitchFx instances are used: beat repeat + slice mode)
    static constexpr size_t kMask    = kBuf - 1;
    static constexpr float  kMaxDelay = static_cast<float>(looper::kPitchMaxDelayFrames);
    static constexpr float  kHop      = static_cast<float>(looper::kPitchHopFrames);
    static constexpr float  kMaxRead  = static_cast<float>(kBuf - 4); // hard limit of the buffer
    static_assert(looper::kPitchMaxDelayFrames + 64 < kBuf - 4, "pitch delay must fit the buffer");
    static_assert(looper::kPitchHopFrames <= looper::kPitchMaxDelayFrames, "hop must not be bigger than the max delay");

    struct Head
    {
        bool  on = false;
        float d  = 1.0f; // frames behind the newest sample
        float r  = 0.0f; // frames the head falls further behind per sample
        size_t hold = 0; // samples to stand still first (until the beat: the fade ends there)
    };

    /** Pitch ratio p = 2^(-steps/12); the head falls behind by (1 - p) per sample. */
    static float RateFor(int steps)
    {
        return 1.0f - std::pow(2.0f, -static_cast<float>(steps * looper::kPitchStepSemitones) / 12.0f);
    }

    void BeginTransition(float dry, float a, float b, size_t len)
    {
        const float t = tc_ >= fade_len_ ? 1.0f
                                         : static_cast<float>(tc_) / static_cast<float>(fade_len_);
        float       w[3];
        w[0] = ws_[0] + (tg_[0] - ws_[0]) * t;
        w[1] = ws_[1] + (tg_[1] - ws_[1]) * t;
        w[2] = ws_[2] + (tg_[2] - ws_[2]) * t;
        for(int k = 0; k < 3; k++)
            ws_[k] = w[k];
        tg_[0]    = dry;
        tg_[1]    = a;
        tg_[2]    = b;
        tc_       = 0;
        fade_len_ = len < 1 ? 1 : len;
    }

    void ToDry(size_t len)
    {
        BeginTransition(1.0f, 0.0f, 0.0f, len);
        cur_ = -1;
    }

    /** Start a new head 1 frame behind the live signal and fade to it. */
    void StartHead(int steps, size_t len)
    {
        const int nk = cur_ == 0 ? 1 : 0;
        if(head_[nk].on) // the other head is still fading out (very rare): cut it
        {
            head_[nk].on = false;
            ws_[1 + nk]  = 0.0f;
        }
        head_[nk].on = true;
        head_[nk].d  = 1.0f;
        head_[nk].r  = RateFor(steps);
        head_[nk].hold = len;
        BeginTransition(0.0f, nk == 0 ? 1.0f : 0.0f, nk == 1 ? 1.0f : 0.0f, len);
        cur_ = nk;
    }

    /** The current head fell too far behind: fade to a new one kHop frames closer. */
    void Hop()
    {
        const int nk = cur_ == 0 ? 1 : 0;
        if(head_[nk].on)
        {
            head_[nk].on = false;
            ws_[1 + nk]  = 0.0f;
        }
        head_[nk].on = true;
        head_[nk].d  = head_[cur_].d - kHop;
        if(head_[nk].d < 1.0f)
            head_[nk].d = 1.0f;
        head_[nk].r  = head_[cur_].r;
        head_[nk].hold = 0;
        BeginTransition(0.0f, nk == 0 ? 1.0f : 0.0f, nk == 1 ? 1.0f : 0.0f, kHopFade);
        cur_ = nk;
    }

    /** The sample `d` frames ago (d >= 1, may be fractional); linear interpolation. */
    void Read(float d, float &l, float &r) const
    {
        const float  p    = static_cast<float>(wi_ + kBuf) - d;
        const size_t i0   = static_cast<size_t>(p);
        const float  frac = p - static_cast<float>(i0);
        const size_t a    = i0 & kMask;
        const size_t b    = (i0 + 1) & kMask;
        l                 = bl_[a] + (bl_[b] - bl_[a]) * frac;
        r                 = br_[a] + (br_[b] - br_[a]) * frac;
    }

    static constexpr size_t kHopFade = 240; // 5 ms

    float  bl_[kBuf];
    float  br_[kBuf];
    size_t wi_ = 0;
    Head   head_[2];
    float  ws_[3] = {1, 0, 0}; // weights at the start of the fade: dry, head 0, head 1
    float  tg_[3] = {1, 0, 0}; // weights at the end of the fade
    size_t tc_       = 1;      // samples into the fade
    size_t fade_len_ = 1;
    int    cur_      = -1;     // the head that is (becoming) audible, -1 = dry
    size_t xfade_    = 240;
};
