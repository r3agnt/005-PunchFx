#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include "looper_config.h"

/** slice_loop_buffer.h : "slice mode" (B7) - a frozen 2-bar loop that you play like a
 *  drum kit.
 *
 *  Plain C++ (no libDaisy) so it is tested on the Mac in test/.
 *  Design and rules: docs/DESIGN_LOOPER.md ("Slice loop buffer").
 *
 *  NAMES (so the two loops are never confused):
 *   - QUANTIZED LOOP BUFFER = the global looper on B1/B2 (quantized_loop_buffer.h).
 *   - SLICE LOOP BUFFER     = this file: the frozen 2 bars made by B7.
 *
 *  What it does, in plain English:
 *   - It always keeps its own 2-bar recording of the live input (after input
 *     gain). It is separate from the beat repeat's history, so the beat repeat
 *     keeps working normally after you leave slice mode.
 *   - B7 (ToggleMode): at the next BAR line (0.5.2) the recording FREEZES (the whole
 *     bar that just ended, from its beat 1: 8 eighth notes) and the frozen bar
 *     starts playing as a loop, in time with the clock, replacing what you hear.
 *     If a whole bar has not been recorded yet (just after Start or just after
 *     leaving slice mode) B7 keeps waiting, and switches at the first bar line
 *     where it can. The window length is kSliceBanks bars (1 now; 2 = two banks).
 *   - B7 again: at the next 1/8 line the output goes back to what it was
 *     (the Quantized loop buffer, or the live input) and the recording
 *     unfreezes.
 *   - The frozen bar is 8 slices of one eighth note each: button 9 = beat 1 of the
 *     bar ... button 16 = the last eighth. (With kSliceBanks = 2 there would be two
 *     BANKS of 8: older bar, newer bar; Shift + B7 (ToggleBank) switches. With one
 *     bank Shift + B7 does nothing and logs.)
 *   - A slice button: at the next 1/8 line the output jumps to that eighth and
 *     repeats it while the button is held. The newest button wins; releasing it
 *     falls back to an older held button. Releasing the last button (0.5.1, 0.5.3)
 *     starts the whole Slice loop buffer playing as a loop from the slice AFTER the
 *     one you triggered (so slice 3 plays, then 4, 5 ... 8, then it wraps round
 *     to 1, 2 ...), at the next 1/8 line. It keeps looping like
 *     that until a new slice is pressed (or B7 ends slice mode).
 *   - BAR LINE = RESET (0.8.0): the window is one bar, and at every bar line (the end of
 *     the Slice loop buffer) the output restarts from beat 1, in time: any slice that
 *     is HELD is choked (it stays choked until its button is released and pressed
 *     again), a loop that was started by releasing a slice (rotated) goes back to the
 *     plain loop from the start. A slice button pressed just before the bar line still
 *     plays: its press lands on the bar line and it starts there.
 *   - REVERSE can be switched off (0.8.1, `fx_reverse` in the block info, Shift + B6 in
 *     main.cpp): then B6 plays the whole eighth FORWARD at the raised pitch (read faster,
 *     like tape, so a pitched-up eighth uses more than one eighth of the recording), with
 *     a 5 ms crossfade at each repeat, instead of the forward-then-backwards ping-pong.
 *   - B6 (0.7.2): while it is on, a slice button plays that eighth 3 semitones HIGHER
 *     as a "ping-pong": forward through the first 16th (read faster, like tape), then
 *     BACKWARDS through the same piece for the second 16th, and so on every eighth. The
 *     position moves up and down a triangle, so there is no jump (no seam, no stutter),
 *     and every repeat is exactly one eighth long (no timing drift when pitching up).
 *     B6 switches on/off on a 1/8 line like everything else here.
 *   - Every change is a 5 ms crossfade that ENDS exactly on the 1/8 line.
 *   - Each 2 bars the playing loop is re-aligned to the clock (crossfaded), so
 *     clock drift never builds up (the same idea as the Quantized loop buffer).
 *   - Transport Stop / clock lost: slice mode ends at once.
 *
 *  How the result is used (in main.cpp): Process() returns `slice` (the part of
 *  the output that comes from this buffer) and `base_w` (how much of the normal
 *  signal is left). Then:   output = base_w * normal + slice.
 *  That is applied twice: to the signal the Quantized loop buffer RECORDS (so
 *  slices are recorded like the beat repeat output) and to what you HEAR (so
 *  slices are heard on top of a playing loop).
 *
 *  Threading: runs in the audio callback. Commands come from the UI through a
 *  queue; events and status are read back.
 */

enum class SliceCmd : uint8_t
{
    ToggleMode, // B7
    ToggleBank  // Shift + B7
};

enum class SliceEventType : uint8_t
{
    ModeOn,            // slice mode started (frozen + playing)
    ModeOff,           // slice mode ended
    Bank,              // bank changed (a = bank 0/1)
    SliceOn,           // repeating slice a (0..7)
    BackToLoop,        // no slice held: playing the full Slice loop buffer
    IgnoredNotRunning, // B7 ignored: transport stopped / tempo unknown
    IgnoredNoMode,     // Shift + B7 ignored: not in slice mode
    IgnoredTooShort,   // eighth under 10 ms
    IgnoredTooSlow,    // the window does not fit the buffer (tempo below the minimum)
    IgnoredOneBank,    // Shift + B7 ignored: only one bank (1 bar)
    PitchOn,           // B6 pitch fx on (at a 1/8 line)
    PitchOff           // B6 pitch fx off
};

struct SliceEvent
{
    SliceEventType type = SliceEventType::ModeOff;
    uint32_t       a    = 0;
};

/** What the clock and buttons are doing in this audio block. */
struct SliceBlockInfo
{
    bool     running         = false; // transport running
    uint32_t bar             = 0;     // bars since Start, at the START of the chunk
    float    pos_in_bar      = 0.0f;  // samples since the bar began, at the START of the chunk
    float    samples_per_bar = 0.0f;  // tempo estimate (0 = unknown)
    uint32_t held_mask       = 0;     // bit i = button 9+i is down (slice i of the bank)
    float    offset_samples  = 0.0f;  // input offset in samples (+ = audio later than clock)
    bool     pitch_held      = false; // B6 is down (pitched ping-pong slices)
    int      fx_semitones    = looper::kSliceFxSemitones; // pitch of the ping-pong (pitch knob), whole semitones
    bool     fx_reverse      = false; // true = forward then backwards (ping-pong), false = forward only
};

/** A copy of the state for the LEDs. */
struct SliceStatus
{
    bool   on       = false; // slice mode is active
    bool   changing = false; // a B7 press is waiting for its 1/8 line
    int8_t bank     = 0;     // 0 = older bar, 1 = newer bar
    int8_t playing  = -1;    // -1 = full Slice loop buffer, 0..7 = repeating that slice
    int8_t pending  = -1;    // slice button pressed, waiting for the line (0..7)
    bool   pitch_on      = false; // B6 effect is on
    bool   pitch_pending = false; // B6 pressed, waiting for its 1/8 line
};

class SliceLoopBuffer
{
  public:
    /** `buf_l/buf_r` = the 2-bar recording (`n` frames each, at least
     *  looper::kHistoryCapacityFrames). Cleared here. */
    void Init(float *buf_l, float *buf_r, size_t n, float sample_rate)
    {
        rl_    = buf_l;
        rr_    = buf_r;
        cap_   = n;
        xfade_ = looper::XfadeFrames(sample_rate);
        if(xfade_ < 1)
            xfade_ = 1;
        for(size_t i = 0; i < cap_; i++)
        {
            rl_[i] = 0.0f;
            rr_[i] = 0.0f;
        }
        hist_abs_       = 0;
        wi_             = 0;
        freeze_end_abs_ = kNever;
        write_from_abs_ = 0;
        valid_from_     = 0;
        mode_on_        = false;
        want_on_        = false;
        bank_           = 0;
        want_bank_      = 0;
        cur_            = kBase;
        v_[0]           = Voice();
        v_[1]           = Voice();
        cur_voice_      = -1;
        for(int k = 0; k < 3; k++)
        {
            ws_[k] = k == 0 ? 1.0f : 0.0f;
            tg_[k] = ws_[k];
        }
        tc_            = xfade_;
        prev_held_     = 0;
        held_now_      = 0;
        press_mask_    = 0;
        pend_slice_    = -1;
        nstack_        = 0;
        fx_prev_ = fx_held_ = fx_press_ = fx_ign_ = fx_on_ = slice_fx_ = false;
        last_line_abs_ = -kNever;
        ev_head_       = 0;
        ev_tail_       = 0;
        Publish();
    }

    /** B7 / Shift + B7. Call before Process() in the same block. `can` = the
     *  transport is running and the tempo is known. */
    void Command(SliceCmd c, bool can)
    {
        if(c == SliceCmd::ToggleMode)
        {
            if(!want_on_ && !can)
            {
                Emit(SliceEventType::IgnoredNotRunning);
                return;
            }
            want_on_ = !want_on_;
        }
        else
        {
            if(!want_on_)
            {
                Emit(SliceEventType::IgnoredNoMode);
                return;
            }
            if(looper::kSliceBanks < 2)
            {
                Emit(SliceEventType::IgnoredOneBank);
                return;
            }
            want_bank_ = (want_bank_ + 1) % static_cast<int>(looper::kSliceBanks);
        }
        Publish();
    }

    /** Process one chunk. `in` = the live input (after input gain). Outputs:
     *  `slice` = this buffer's part of the output, `base_w` = weight of the normal
     *  signal (1 = slice mode not heard at all). output = base_w * normal + slice. */
    void Process(const SliceBlockInfo &info,
                 const float *inl, const float *inr,
                 float *slice_l, float *slice_r, float *base_w, size_t n)
    {
        const bool can = info.running && info.samples_per_bar > 0.0f;
        NoteButtons(info.held_mask & kButtonMask, can);
        NoteFx(info.pitch_held, can);

        // Is the next 1/8 line inside this chunk? (The crossfade must END on
        // the line, so we look for the moment `xfade_` samples before it.)
        int ev_k = -1;
        if(can)
        {
            const float bar = info.samples_per_bar;
            const float lf  = bar / static_cast<float>(looper::kSliceQuantizeDen);
            float       g   = std::fmod(info.pos_in_bar - info.offset_samples
                                        + static_cast<float>(xfade_), bar);
            if(g < 0.0f)
                g += bar;
            const float r  = std::fmod(g, lf);
            const float dt = r <= 0.001f ? 0.0f : lf - r;
            if(dt < static_cast<float>(n))
            {
                ev_k = static_cast<int>(dt);
                // Never act twice on the same line (float rounding at a chunk edge).
                if(hist_abs_ + ev_k - last_line_abs_ < static_cast<int64_t>(lf * 0.5f))
                    ev_k = -1;
            }
        }

        for(size_t i = 0; i < n; i++)
        {
            // Record the input, until the freeze point.
            if(hist_abs_ < freeze_end_abs_ || hist_abs_ >= write_from_abs_)
            {
                rl_[wi_] = inl[i];
                rr_[wi_] = inr[i];
            }

            if(static_cast<int>(i) == ev_k)
            {
                last_line_abs_ = hist_abs_;
                ApplyLine(info, static_cast<int>(i));
            }

            float w[3];
            Weights(w);
            float ol = 0.0f, orr = 0.0f;
            for(int vi = 0; vi < 2; vi++)
            {
                if(!v_[vi].active)
                    continue;
                float vl, vr;
                ReadVoice(v_[vi], vl, vr);
                ol += w[1 + vi] * vl;
                orr += w[1 + vi] * vr;
            }
            slice_l[i] = ol;
            slice_r[i] = orr;
            base_w[i]  = w[0];

            if(tc_ < xfade_)
                tc_++;
            else
                for(int vi = 0; vi < 2; vi++)
                    if(v_[vi].active && tg_[1 + vi] == 0.0f)
                        v_[vi].active = false;
            hist_abs_++;
            if(++wi_ >= cap_) // (wi_ is always hist_abs_ modulo the buffer size)
                wi_ = 0;
        }
        Publish();
    }

    bool PopEvent(SliceEvent &e)
    {
        if(ev_head_ == ev_tail_)
            return false;
        e        = ev_[ev_tail_];
        ev_tail_ = (ev_tail_ + 1) % kEvSize;
        return true;
    }

    SliceStatus GetStatus() const
    {
        SliceStatus s;
        s.on       = pub_on_;
        s.changing = pub_changing_;
        s.bank     = pub_bank_;
        s.playing  = pub_playing_;
        s.pending  = pub_pending_;
        s.pitch_on      = pub_pitch_on_;
        s.pitch_pending = pub_pitch_pending_;
        return s;
    }

    /** True while slice mode is on (the beat repeat is switched off then). */
    bool IsOn() const { return pub_on_; }

    /** B6 (audio thread): PitchOn() = effect on (B6 held, switched on a 1/8 line). */
    bool PitchOn() const { return fx_on_; }

  private:
    static constexpr int64_t  kNever      = INT64_MAX / 2;
    static constexpr uint32_t kButtonMask = (1u << looper::kSlicesPerBank) - 1u;
    static constexpr int      kBase       = -2; // (cur_) normal signal, slice mode off
    static constexpr int      kLoop       = -1; // (cur_) the full 2-bar Slice loop buffer
    static constexpr int      kRotated    = -3; // (cur_) the 2-bar loop starting at the last slice

    /** A region of the frozen recording that repeats: `L` frames starting at
     *  absolute position `S`; `t0` = a time at which it is at its start. */
    struct Region
    {
        int64_t S  = 0;
        int64_t L  = 1;
        int64_t t0 = 0;
        // Rotated loop (0.5.1): the region is the whole window, but it starts
        // `rot` frames into it and wraps round at the window end.
        bool    wrap = false;
        int64_t rot  = 0;
    };

    /** One playing region. `u` = position inside the region. */
    struct Voice
    {
        bool    active = false;
        Region  r;
        int64_t u     = 0; // position inside the region
        int64_t s_idx = 0; // buffer index of the region's start (S modulo the buffer size)
        bool    pp    = false; // B6 "ping-pong": pitched, forward then backwards (see ReadVoice)
        double  rate  = 1.0;   // playback speed of the ping-pong = 2^(semitones/12)
        bool    rev   = true;  // ping-pong with the backwards half (false = forward only, crossfaded at the repeat)
    };

    void Emit(SliceEventType t, uint32_t a = 0)
    {
        const uint32_t next = (ev_head_ + 1) % kEvSize;
        if(next == ev_tail_)
            return;
        ev_[ev_head_] = {t, a};
        ev_head_      = next;
    }

    // ----- reading the frozen recording -----
    /** One frame at `d` frames from the start of the voice's region (d may be
     *  slightly negative: the pre-roll). Anything not valid audio is silence:
     *  before the valid start (never recorded, or recorded before the last
     *  unfreeze gap) or after the freeze point. Uses add/compare only (no
     *  division) because it runs for every sample. */
    void Fetch(const Voice &v, int64_t d, float &l, float &r) const
    {
        FetchP(v, d, l, r);
    }

    /** Same, but `p` = position from the window start (S). */
    void FetchP(const Voice &v, int64_t p, float &l, float &r) const
    {
        const int64_t abs = v.r.S + p;
        if(abs < valid_from_ || abs < 0 || abs >= freeze_end_abs_)
        {
            l = 0.0f;
            r = 0.0f;
            return;
        }
        int64_t i = v.s_idx + p; // the ring index of region start + p
        if(i < 0)
            i += static_cast<int64_t>(cap_);
        else if(i >= static_cast<int64_t>(cap_))
            i -= static_cast<int64_t>(cap_);
        l = rl_[i];
        r = rr_[i];
    }

    /** The recording at (fractional) position `p` frames from the voice's region start (may be
     *  negative: before it), linear interpolation. */
    void FetchInterp(const Voice &v, double p, float &l, float &r) const
    {
        const double fl = std::floor(p);
        const float  fr = static_cast<float>(p - fl);
        float        al, ar, bl, br;
        Fetch(v, static_cast<int64_t>(fl), al, ar);
        Fetch(v, static_cast<int64_t>(fl) + 1, bl, br);
        l = al + (bl - al) * fr;
        r = ar + (br - ar) * fr;
    }

    void ReadVoice(Voice &v, float &l, float &r)
    {
        const int64_t X = static_cast<int64_t>(xfade_);
        float         al, ar;
        if(v.pp)
        {
            // B6 pitched slice: `v.u` = frames since the eighth line (0..L-1).
            if(v.rev)
            {
                // Ping-pong: the position in the slice goes up a triangle: forward for the
                // first half (a 16th), then back down to 0 for the second half, read at
                // `v.rate` (pitched). It is continuous everywhere (also at the turn and at
                // the repeat), so there is no seam to crossfade.
                const int64_t half = v.r.L / 2;
                const int64_t tri  = v.u <= half ? v.u : v.r.L - v.u;
                FetchInterp(v, static_cast<double>(tri) * v.rate, l, r);
            }
            else
            {
                // Forward only: position = u * rate. The repeat jumps from the end back to the
                // start, so in the last X frames crossfade into the audio just BEFORE the
                // slice start (negative positions, the "pre-roll"), like the normal loop.
                FetchInterp(v, static_cast<double>(v.u) * v.rate, al, ar);
                if(v.u >= v.r.L - X)
                {
                    float       bl, br;
                    const float g = static_cast<float>(v.u - (v.r.L - X)) / static_cast<float>(X);
                    FetchInterp(v, static_cast<double>(v.u - v.r.L) * v.rate, bl, br);
                    al += (bl - al) * g;
                    ar += (br - ar) * g;
                }
                l = al;
                r = ar;
            }
            v.u++;
            if(v.u >= v.r.L)
                v.u -= v.r.L;
            return;
        }
        if(v.r.wrap)
        {
            // Rotated loop: the loop point is not a seam (the window is read
            // round and round). The seam is where the window wraps from its
            // end to its start: crossfade the last X frames of the window
            // into the audio just before the window start (the pre-roll).
            int64_t p = v.r.rot + v.u;
            if(p >= v.r.L)
                p -= v.r.L;
            FetchP(v, p, al, ar);
            if(p >= v.r.L - X)
            {
                float       bl, br;
                const float g = static_cast<float>(p - (v.r.L - X)) / static_cast<float>(X);
                FetchP(v, p - v.r.L, bl, br);
                al += (bl - al) * g;
                ar += (br - ar) * g;
            }
            v.u++;
            if(v.u >= v.r.L)
                v.u -= v.r.L;
            l = al;
            r = ar;
            return;
        }
        Fetch(v, v.u, al, ar);
        // Last X frames of the region: crossfade into the audio just before its
        // start (the "pre-roll"), so the wrap back to the start has no click.
        if(v.u >= v.r.L - X)
        {
            float       bl, br;
            const float g = static_cast<float>(v.u - (v.r.L - X)) / static_cast<float>(X);
            Fetch(v, v.u - v.r.L, bl, br);
            al += (bl - al) * g;
            ar += (br - ar) * g;
        }
        v.u++;
        if(v.u >= v.r.L)
            v.u -= v.r.L;
        l = al;
        r = ar;
    }

    // ----- crossfades between the normal signal and the two voices -----
    void Weights(float w[3]) const
    {
        const float t = tc_ >= xfade_ ? 1.0f
                                      : static_cast<float>(tc_) / static_cast<float>(xfade_);
        for(int k = 0; k < 3; k++)
            w[k] = ws_[k] + (tg_[k] - ws_[k]) * t;
    }

    void BeginTransition(float base, float v0, float v1)
    {
        float w[3];
        Weights(w);
        for(int k = 0; k < 3; k++)
            ws_[k] = w[k];
        tg_[0] = base;
        tg_[1] = v0;
        tg_[2] = v1;
        tc_    = 0;
    }

    /** Pick the voice that is not the current one; cut it if it is still fading. */
    int FreeVoice()
    {
        const int vi = cur_voice_ == 0 ? 1 : 0;
        if(v_[vi].active) // an older voice is still fading: cut it (very rare)
        {
            v_[vi].active = false;
            ws_[1 + vi]   = 0.0f;
        }
        return vi;
    }

    /** Start playing `reg` (position worked out for "now") and fade to it. */
    void PlayRegion(const Region &reg, bool ping_pong = false, int semitones = 0, bool reverse = true)
    {
        const int vi = FreeVoice();
        Voice    &v  = v_[vi];
        v            = Voice();
        v.active     = true;
        v.r          = reg;
        int64_t u    = (hist_abs_ - reg.t0) % reg.L;
        if(u < 0)
            u += reg.L;
        v.u  = u;
        v.pp = ping_pong;
        v.rev = reverse;
        v.rate = std::pow(2.0, static_cast<double>(semitones) / 12.0);
        const int64_t cap = static_cast<int64_t>(cap_);
        v.s_idx = ((reg.S % cap) + cap) % cap;
        BeginTransition(0.0f, vi == 0 ? 1.0f : 0.0f, vi == 1 ? 1.0f : 0.0f);
        cur_voice_ = vi;
    }

    void GoBase()
    {
        BeginTransition(1.0f, 0.0f, 0.0f);
        cur_voice_ = -1;
        cur_       = kBase;
    }

    Region LoopRegion() const
    {
        Region r;
        r.S  = win_start_;
        r.L  = loop_len_;
        r.t0 = loop_t0_;
        return r;
    }

    /** The whole 2-bar window, but starting at slice `slice` (0..15 of the
     *  window) at line time `line_t`. */
    Region RotatedRegion(int slice, int64_t line_t) const
    {
        Region r;
        r.S    = win_start_;
        r.L    = loop_len_;
        r.t0   = line_t;
        r.wrap = true;
        r.rot  = static_cast<int64_t>(slice) * eighth_;
        return r;
    }

    Region SliceRegion(int j, int64_t line_t) const
    {
        Region r;
        r.S  = win_start_ + static_cast<int64_t>(bank_ * looper::kSlicesPerBank + j) * eighth_;
        r.L  = eighth_;
        r.t0 = line_t; // every slice starts on the line it was triggered on
        return r;
    }

    // ----- B6 -----
    /** Every block: notes B6 presses/releases; the effect itself switches on or off on a
     *  1/8 line (ApplyFxLine). Ignored unless slice mode is on and running; a B6 that is
     *  down when that is not true stays ignored until released. */
    void NoteFx(bool held, bool can)
    {
        const bool pressed  = held && !fx_prev_;
        const bool released = !held && fx_prev_;
        fx_prev_            = held;
        fx_held_            = held;
        if(released)
            fx_ign_ = false;
        if(!can || !mode_on_)
        {
            fx_ign_   = held;
            fx_press_ = false;
            if(fx_on_)
            {
                fx_on_ = false;
                Emit(SliceEventType::PitchOff);
            }
            return;
        }
        if(pressed && !fx_ign_)
            fx_press_ = true; // remembered even if released before the line
    }

    /** A 1/8 line: B6 on if it is down (or was pressed since the last line: a tap gives
     *  one eighth), otherwise off. */
    void ApplyFxLine()
    {
        const bool want = !fx_ign_ && (fx_held_ || fx_press_);
        fx_press_       = false;
        if(want != fx_on_)
        {
            fx_on_ = want;
            Emit(want ? SliceEventType::PitchOn : SliceEventType::PitchOff);
        }
    }

    // ----- buttons -----
    /** Every block: notes presses (does NOT act on them: that happens at the
     *  next 1/8 line in ApplyLine). Transport stopped ends slice mode at once. */
    void NoteButtons(uint32_t held, bool can)
    {
        const uint32_t pressed = held & ~prev_held_;
        prev_held_             = held;
        held_now_              = held;

        if(!can)
        {
            want_on_ = false;
            if(mode_on_)
                ModeOff(); // immediately
            press_mask_ = 0;
            pend_slice_ = -1;
            nstack_     = 0;
            return;
        }
        if(!mode_on_)
        {
            // Buttons that are down when the mode starts count as already held:
            // they do nothing until released and pressed again.
            press_mask_ = 0;
            pend_slice_ = -1;
            return;
        }
        for(uint32_t i = 0; i < looper::kSlicesPerBank; i++)
            if(pressed & (1u << i))
            {
                press_mask_ |= 1u << i;
                pend_slice_ = static_cast<int8_t>(i);
            }
    }

    void RemoveFromStack(uint32_t idx)
    {
        size_t j = 0;
        for(size_t i = 0; i < nstack_; i++)
            if(stack_[i] != idx)
                stack_[j++] = stack_[i];
        nstack_ = j;
    }

    void ModeOff()
    {
        mode_on_ = false;
        GoBase();
        // The crossfade out ends X frames from now; the recording resumes after it
        // (the frozen audio is still being read until then).
        write_from_abs_ = hist_abs_ + static_cast<int64_t>(xfade_) + 1;
        nstack_         = 0;
        pend_slice_     = -1;
        press_mask_     = 0;
        slice_fx_       = false;
        if(fx_on_)
        {
            fx_on_ = false;
            Emit(SliceEventType::PitchOff);
        }
        Emit(SliceEventType::ModeOff);
    }

    /** Try to start slice mode with the line at absolute time `T`. */
    void ModeOn(int64_t T, int64_t line_idx, const SliceBlockInfo &info)
    {
        const int64_t X     = static_cast<int64_t>(xfade_);
        const int64_t E     = static_cast<int64_t>(std::lround(info.samples_per_bar
                                                                / static_cast<float>(looper::kSliceQuantizeDen)));
        const int64_t N     = E * static_cast<int64_t>(looper::kSlicesPerBank * looper::kSliceBanks);
        if(E < 2 * X)
        {
            want_on_ = false;
            Emit(SliceEventType::IgnoredTooShort);
            return;
        }
        if(N + X > static_cast<int64_t>(cap_))
        {
            want_on_ = false;
            Emit(SliceEventType::IgnoredTooSlow);
            return;
        }
        // Not a whole bar of recorded audio behind this line yet (just after Start
        // or just after leaving slice mode)? Keep waiting: B7 stays pending and
        // we try again at the next bar line. (Never freeze silence.)
        // (write_from_abs_ = where the recording (re)started; not frozen here.)
        if(T - N < write_from_abs_ || T - N < 0)
            return;
        want_on_    = true;
        mode_on_    = true;
        eighth_     = E;
        loop_len_   = N;
        win_start_  = T - N;
        loop_t0_    = T;
        loop_idx0_  = line_idx;
        // Freeze: keep recording until the line, then stop. Positions older
        // than the last unfreeze gap are not valid audio (silence).
        valid_from_     = write_from_abs_;
        freeze_end_abs_ = T;
        write_from_abs_ = kNever;
        bank_           = 0; // always start on the older bar
        want_bank_      = 0;
        nstack_         = 0;
        press_mask_     = 0;
        pend_slice_     = -1;
        PlayRegion(LoopRegion());
        cur_ = kLoop;
        Emit(SliceEventType::ModeOn);
    }

    /** A 1/8 line is `xfade_` samples away (we are at the sample where the
     *  crossfade has to start). Act on the mode switch, the bank and the buttons. */
    void ApplyLine(const SliceBlockInfo &info, int k)
    {
        const int64_t T = hist_abs_ + static_cast<int64_t>(xfade_);
        // Which eighth-note line is this, counted from Start?
        const float   lf  = info.samples_per_bar / static_cast<float>(looper::kSliceQuantizeDen);
        const int64_t idx = static_cast<int64_t>(info.bar) * looper::kSliceQuantizeDen
                            + static_cast<int64_t>(std::lround(
                                (info.pos_in_bar - info.offset_samples + static_cast<float>(k)
                                 + static_cast<float>(xfade_)) / lf));

        // 0. B6 (switches on/off on this 1/8 line).
        if(mode_on_)
            ApplyFxLine();

        // 1. Mode switch.
        if(want_on_ && !mode_on_)
        {
            // Only on a BAR line (0.5.2): the frozen window then starts on beat 1.
            if(idx % static_cast<int64_t>(looper::kSliceQuantizeDen) == 0)
                ModeOn(T, idx, info);
            return; // (buttons that are down now are ignored until released)
        }
        if(!want_on_ && mode_on_)
        {
            ModeOff();
            return;
        }
        if(!mode_on_)
            return;

        // 2. Bank switch.
        if(want_bank_ != bank_)
        {
            bank_ = want_bank_;
            Emit(SliceEventType::Bank, static_cast<uint32_t>(bank_));
        }

        // 3. Keep the full loop in time with the clock: every window (8 eighths
        //    after the line the mode started on) restart it on the line.
        const int64_t win_lines = static_cast<int64_t>(looper::kSlicesPerBank * looper::kSliceBanks);
        const int64_t phase = ((idx - loop_idx0_) % win_lines + win_lines) % win_lines;
        const bool    loop_line = phase == 0;
        if(loop_line)
            loop_t0_ = T;

        // 4. Buttons: presses since the last line go on the stack, buttons that
        //    are up come off it. A tap (pressed and released between two lines)
        //    is pushed now and removed at the next line. At the BAR line (the loop
        //    end) a button that was already held before this line is CHOKED: it comes
        //    off the stack and, since only a new press puts a button on it, stays off
        //    until the button is released and pressed again.
        for(uint32_t i = 0; i < looper::kSlicesPerBank; i++)
        {
            const uint32_t bit = 1u << i;
            if(press_mask_ & bit)
            {
                RemoveFromStack(i);
                stack_[nstack_++] = static_cast<uint8_t>(i);
            }
            else if(!(held_now_ & bit) || loop_line)
                RemoveFromStack(i);
        }
        press_mask_ = 0;
        pend_slice_ = -1;

        const int want = nstack_ > 0 ? stack_[nstack_ - 1] : -1;
        if(want >= 0)
        {
            if(want != cur_)
            {
                PlayRegion(SliceRegion(want, T), fx_on_, info.fx_semitones, info.fx_reverse);
                slice_fx_   = fx_on_;
                slice_semis_ = info.fx_semitones;
                slice_rev_   = info.fx_reverse;
                cur_        = want;
                rot_slice_  = bank_ * static_cast<int>(looper::kSlicesPerBank) + want;
                Emit(SliceEventType::SliceOn, static_cast<uint32_t>(want));
            }
            else if(slice_fx_ != fx_on_
                    || (fx_on_ && (slice_semis_ != info.fx_semitones || slice_rev_ != info.fx_reverse)))
            {
                // B6 came on / went off, or the pitch knob / reverse switch changed, while the
                // slice is held: restart it on this line (pitched <-> normal repeat, new pitch).
                PlayRegion(SliceRegion(want, T), fx_on_, info.fx_semitones, info.fx_reverse);
                slice_fx_    = fx_on_;
                slice_semis_ = info.fx_semitones;
                slice_rev_   = info.fx_reverse;
            }
        }
        else if(loop_line && cur_ != kLoop)
        {
            // The bar line (loop end) with nothing held (a held slice was choked, or the
            // last one was just released, or a rotated loop is playing): back to the plain
            // loop from beat 1, in time.
            PlayRegion(LoopRegion());
            cur_ = kLoop;
            Emit(SliceEventType::BackToLoop);
        }
        else if(cur_ >= 0)
        {
            // Last slice button released: loop the whole window, carrying on
            // from the slice AFTER the one that was playing (0.5.3). It plays on
            // until the next bar line, where it resets to beat 1 (above).
            rot_slice_ = (rot_slice_ + 1) % static_cast<int>(win_lines);
            PlayRegion(RotatedRegion(rot_slice_, T));
            cur_        = kRotated;
            Emit(SliceEventType::BackToLoop);
        }
        else if(loop_line) // cur_ == kLoop
        {
            PlayRegion(LoopRegion()); // re-aligned to the clock (crossfaded)
        }
    }

    void Publish()
    {
        pub_on_       = mode_on_;
        pub_changing_ = want_on_ != mode_on_;
        pub_bank_     = static_cast<int8_t>(want_bank_);
        pub_playing_  = mode_on_ && cur_ >= kLoop ? static_cast<int8_t>(cur_) : static_cast<int8_t>(-1);
        pub_pending_  = pend_slice_;
        pub_pitch_on_      = fx_on_;
        pub_pitch_pending_ = fx_press_;
    }

    // recording
    float  *rl_ = nullptr, *rr_ = nullptr;
    size_t  cap_ = 1;
    size_t  xfade_ = 240;
    int64_t hist_abs_       = 0;      // frames seen so far = absolute sample number of "now"
    size_t  wi_             = 0;      // write position in the buffer (= hist_abs_ modulo cap_)
    int64_t freeze_end_abs_ = kNever; // recording stops here while frozen
    int64_t write_from_abs_ = 0;      // recording resumes here after unfreezing
    int64_t valid_from_     = 0;      // audio before this position is not valid

    // frozen window
    bool    mode_on_    = false;
    bool    want_on_    = false;
    int     bank_       = 0;
    int     want_bank_  = 0;
    int64_t eighth_     = 1;
    int64_t loop_len_   = 1;
    int64_t win_start_  = 0;
    int64_t loop_t0_    = 0;
    int64_t loop_idx0_  = 0;
    int     cur_        = kBase;
    int     rot_slice_  = 0; // window slice (0..15) the rotated loop starts at

    // voices and crossfade
    Voice  v_[2];
    int    cur_voice_ = -1;
    float  ws_[3] = {1, 0, 0}; // weights at the start of the fade: normal, voice 0, voice 1
    float  tg_[3] = {1, 0, 0}; // weights at the end of the fade
    size_t tc_    = 240;       // samples into the fade

    // buttons
    uint32_t prev_held_   = 0;
    uint32_t held_now_    = 0;
    uint32_t press_mask_  = 0;
    int8_t   pend_slice_  = -1;
    uint8_t  stack_[looper::kSlicesPerBank] = {};
    size_t   nstack_      = 0;
    int64_t  last_line_abs_ = -kNever;

    // events + status
    static constexpr uint32_t kEvSize = 16;
    SliceEvent ev_[kEvSize];
    uint32_t   ev_head_ = 0;
    uint32_t   ev_tail_ = 0;
    volatile bool   pub_on_       = false;
    volatile bool   pub_changing_ = false;
    volatile int8_t pub_bank_     = 0;
    volatile int8_t pub_playing_  = -1;
    volatile int8_t pub_pending_  = -1;
    volatile bool   pub_pitch_on_      = false;
    volatile bool   pub_pitch_pending_ = false;

    // B6
    bool fx_prev_  = false;
    bool fx_held_  = false;
    bool fx_press_ = false;
    bool fx_ign_   = false;
    bool fx_on_    = false;
    bool slice_fx_ = false; // the held slice is playing as a ping-pong
    int  slice_semis_ = 0;  // ... at this many semitones
    bool slice_rev_   = true; // ... with / without the backwards half
};
