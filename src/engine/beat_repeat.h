#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include "looper_config.h"

/** beat_repeat.h : classic quantised beat repeat (stutter).
 *
 *  Plain C++ (no libDaisy) so it is tested on the Mac in test/.
 *  Design and rules: docs/DESIGN_LOOPER.md.
 *
 *  How it works:
 *   - The input is always being written into a 2-bar "history" ring.
 *   - While a stutter button (buttons 9-16 = 1 bar, 1/2, 3/8, 1/4, 1/8, 1/16,
 *     1/32, 1/64) is held, the engine repeats the slice that has just finished.
 *     Since 0.4.5 every press AND every release is quantised to a common
 *     1/16-bar line (looper::kQuantizeDen): the engine waits for the next line,
 *     then repeats the last slice-length of audio that ends on that line.
 *     The slice length is still the button's own division. A tap shorter than
 *     the wait still starts at the line and ends at the following line.
 *   - Every change (live -> stutter, the loop point of each repeat, stutter ->
 *     live, switching divisions) is a 5 ms linear crossfade scheduled to END
 *     exactly on the 1/16 line, so the first hit of each repeat is at full
 *     level.
 *   - Each division has its own "slot": its slice is copied, a bit at a time,
 *     out of the history into the slot, so it survives after the history has
 *     been overwritten (a 1-bar stutter can be held for ever).
 *   - Several buttons held (flip between them): the most recently pressed one
 *     wins. A division that has not been captured yet during this hold
 *     captures its slice at the next 1/16 line. A division that has already been
 *     captured is NOT captured again: at the next line its stored slice is used,
 *     started at the position it would have reached had it been repeating
 *     since it was captured (so it is in time). Releasing the newest button falls
 *     back to the older held one the same way. Releasing the last button returns
 *     to live (at the next line) and throws all stored slices away.
 *   - The input offset moves the slice grid against the clock (audio that
 *     arrives later than the MIDI clock = positive offset).
 *   - Needs the transport to be running and a known tempo; otherwise presses
 *     are ignored.
 *
 *  Grid: the 1/16 lines are every 1/16 of a bar counted from the start of
 *  the bar (plus the offset). A slice is no longer tied to its own division's
 *  grid: it ends on whichever line the press was quantised to, and then repeats
 *  with its own length (every division is a whole number of 1/16s except 1/32
 *  and 1/64, which are shorter than the line spacing).
 *
 *  Threading: runs in the audio callback. Held buttons are passed in as a
 *  bit mask each block; events and status are read back.
 */

enum class BeatEventType : uint8_t
{
    Waiting,           // button pressed, waiting for the next 1/16 line (div)
    On,                // stutter started with a fresh slice (div)
    OnStored,          // switched to a slice stored earlier in this hold (div)
    Off,               // stutter stopped, live signal again
    IgnoredNotRunning, // press ignored: transport stopped / tempo unknown (div)
    IgnoredTooShort,   // press ignored: slice shorter than 10 ms (div)
    IgnoredTooSlow,    // press ignored: tempo below looper::kMinBpm, slice does not fit its slot (div)
    PitchOn,           // B6 pitch effect switched on (at a 1/16 line)
    PitchOff,          // B6 pitch effect switched off (at a 1/16 line)
};

struct BeatEvent
{
    BeatEventType type = BeatEventType::Off;
    uint32_t      div  = 0; // division index 0..7 (see looper::kDivNum / kDivDen)
};

/** What the clock and buttons are doing in this audio block. */
struct BeatBlockInfo
{
    bool     running         = false; // transport running
    uint32_t bar             = 0;     // bars since Start, at the START of the block
    float    pos_in_bar      = 0.0f;  // samples since the bar began, at the START of the block
    float    samples_per_bar = 0.0f;  // tempo estimate (0 = unknown)
    uint32_t held_mask       = 0;     // bit i = button for division i is down
    float    offset_samples  = 0.0f;  // input offset in samples (+ = audio later than clock)
    bool     pitch_held      = false; // B6 is down (pitch-down effect, see PitchOn())
};

/** A copy of the state for the LEDs. -1 = none, otherwise 0..7 (division index). */
struct BeatStatus
{
    int8_t active        = -1;    // repeating now
    int8_t pending       = -1;    // waiting for the next boundary
    bool   pitch_on      = false; // B6 effect is on
    bool   pitch_pending = false; // B6 pressed, waiting for its 1/16 line
};

class BeatRepeat
{
  public:
    /** `hist_*` = history ring (hn frames), `frz_*` = pool for the stored slices
     *  (fn frames, at least looper::kFreezeCapacityFrames). The ring is cleared
     *  here, so stale memory is never heard. */
    void Init(float *hist_l, float *hist_r, size_t hn,
              float *frz_l, float *frz_r, size_t fn, float sample_rate)
    {
        hl_    = hist_l;
        hr_    = hist_r;
        hn_    = hn;
        fl_    = frz_l;
        fr_    = frz_r;
        fn_    = fn;
        xfade_ = looper::XfadeFrames(sample_rate);
        if(xfade_ < 1)
            xfade_ = 1;
        for(size_t i = 0; i < hn_; i++)
        {
            hl_[i] = 0.0f;
            hr_[i] = 0.0f;
        }
        hist_abs_ = 0;
        // Carve the pool into one region per division.
        size_t off = 0;
        for(size_t d = 0; d < looper::kNumDivisions; d++)
        {
            slot_[d]     = Slot();
            slot_off_[d] = off;
            slot_cap_[d] = looper::SliceCapacityFrames(d);
            off += slot_cap_[d];
        }
        if(off > fn_) // pool too small: no slot fits, every press is refused as "too short"
            for(size_t d = 0; d < looper::kNumDivisions; d++)
                slot_cap_[d] = 0;
        v_[0] = SliceVoice();
        v_[1] = SliceVoice();
        cur_  = -1;
        for(int k = 0; k < 3; k++)
        {
            ws_[k] = k == 0 ? 1.0f : 0.0f;
            tg_[k] = ws_[k];
        }
        tc_         = xfade_;
        pend_       = -1;
        active_div_ = -1;
        pitch_prev_ = pitch_held_ = pitch_press_ = pitch_ign_ = pitch_on_ = false;
        loops_      = 0;
        prev_held_  = 0;
        ign_mask_   = 0;
        press_mask_ = 0;
        last_line_abs_ = kNoLine;
        nstack_     = 0;
        ev_head_    = 0;
        ev_tail_    = 0;
        Publish();
    }

    void Process(const BeatBlockInfo &info,
                 const float *inl, const float *inr,
                 float *outl, float *outr, size_t n)
    {
        const bool     can = info.running && info.samples_per_bar > 0.0f;
        const uint32_t raw = info.held_mask & ((1u << looper::kNumDivisions) - 1u);
        NoteButtons(raw, can);
        NoteFx(info.pitch_held, can);
        jump_idx_ = -1;

        // Is the next 1/16 line inside this block? Presses and releases only
        // take effect on a line. (The crossfade must END on the line, so we
        // look for the moment `xfade_` samples before it.)
        int ev_k = -1;
        if(can)
        {
            const float bar = info.samples_per_bar;
            const float lf  = bar / static_cast<float>(looper::kQuantizeDen);
            float       g   = std::fmod(info.pos_in_bar - info.offset_samples
                                        + static_cast<float>(xfade_), bar);
            if(g < 0.0f)
                g += bar;
            const float r  = std::fmod(g, lf);
            const float dt = r <= 0.001f ? 0.0f : lf - r;
            if(dt < static_cast<float>(n))
            {
                ev_k = static_cast<int>(dt);
                // Never act twice on the same line (float rounding at a block edge).
                if(hist_abs_ + ev_k - last_line_abs_ < static_cast<int64_t>(lf * 0.5f))
                    ev_k = -1;
            }
        }

        for(size_t i = 0; i < n; i++)
        {
            const float pl = inl[i];
            const float pr = inr[i];

            // Record the input into the history (before reading, so reads
            // of "just now" are valid).
            const size_t wi = static_cast<size_t>(hist_abs_ % static_cast<int64_t>(hn_));
            hl_[wi]         = pl;
            hr_[wi]         = pr;

            if(static_cast<int>(i) == ev_k)
            {
                last_line_abs_ = hist_abs_;
                ApplyLine(info, raw);
                ApplyFxLine();
            }

            blk_i_ = static_cast<int>(i);
            float w[3];
            Weights(w);
            float ol  = w[0] * pl;
            float orr = w[0] * pr;
            for(int vi = 0; vi < 2; vi++)
            {
                if(!v_[vi].active)
                    continue;
                float vl, vr;
                ReadVoice(v_[vi], vl, vr);
                ol += w[1 + vi] * vl;
                orr += w[1 + vi] * vr;
            }
            outl[i] = ol;
            outr[i] = orr;

            if(tc_ < xfade_)
                tc_++;
            else
                for(int vi = 0; vi < 2; vi++)
                    if(v_[vi].active && tg_[1 + vi] == 0.0f)
                        v_[vi].active = false;
            hist_abs_++;
        }
        CopyStep();
        Publish();
    }

    bool PopEvent(BeatEvent &e)
    {
        if(ev_head_ == ev_tail_)
            return false;
        e        = ev_[ev_tail_];
        ev_tail_ = (ev_tail_ + 1) % kEvSize;
        return true;
    }

    BeatStatus GetStatus() const
    {
        BeatStatus s;
        s.active  = pub_active_;
        s.pending = pub_pending_;
        s.pitch_on      = pub_pitch_on_;
        s.pitch_pending = pub_pitch_pending_;
        return s;
    }

    int64_t HistoryPosition() const { return hist_abs_; }

    /** B6 pitch effect (audio thread). PitchOn() = the effect is on (B6 held,
     *  quantised to the 1/16 line, also when no slice is repeating: it is then
     *  "armed" for the next one). LoopCount() = how many times the held slice
     *  has wrapped round since it started (0 during its first play); the pitch
     *  stage lowers the sound by this many semitones. Repeating() = a slice is
     *  repeating (not live). */
    bool PitchOn() const { return pitch_on_; }
    int  LoopCount() const { return loops_; }
    /** The repeat number the sound is in, counting a repeat as soon as its loop
     *  crossfade has started (= LoopCount() + 1 while the voice is crossfading back
     *  to the start of the slice). This is what the pitch stage wants: the new
     *  repeat's pitch applies from the start of that crossfade. */
    int RepeatIndex() const
    {
        return loops_ + ((active_div_ >= 0 && cur_ >= 0 && v_[cur_].jumping) ? 1 : 0);
    }
    /** Sample index (inside the last Process() call) where the repeat that is
     *  heard started its loop crossfade (the crossfade ends exactly on the loop
     *  point), or -1 if that did not happen in the block. */
    int JumpStartIndex() const { return jump_idx_; }
    bool Repeating() const { return active_div_ >= 0; }

  private:
    /** The stored copy of one division's slice. The slice is the audio from
     *  ring position `S_abs` to `S_abs + L` (absolute sample numbers); the copy
     *  also includes `X` samples of pre-roll before `S_abs` (needed for the
     *  crossfade at the loop point). */
    enum class SlotState : uint8_t
    {
        Empty,   // nothing captured in this hold
        Copying, // captured; still being copied out of the history ring
        Ready    // complete copy in the slot buffer
    };
    struct Slot
    {
        SlotState state    = SlotState::Empty;
        int64_t   S_abs    = 0;
        uint32_t  L        = 0;
        int64_t   copy_pos = 0; // next ring position to copy
    };

    /** One repeating slice. It reads the slot's slice in "slot coordinates":
     *  index 0 = pre-roll start, the slice itself is S .. S+L-1 with S = X.
     *  Until the slot copy is complete it reads the history ring instead
     *  (same samples, so the switch is inaudible). */
    struct SliceVoice
    {
        bool     active  = false;
        bool     use_buf = false; // reading the slot buffer (else the ring)
        bool     jumping = false;
        int      slot    = 0;
        int64_t  S       = 0;
        int64_t  a       = 0; // read position
        int64_t  b       = 0; // position being faded in during a loop jump
        uint32_t L       = 0;
        uint32_t X       = 1;
        uint32_t jc      = 0; // samples into the jump
    };

    void Emit(BeatEventType t, uint32_t div = 0)
    {
        const uint32_t next = (ev_head_ + 1) % kEvSize;
        if(next == ev_tail_)
            return;
        ev_[ev_head_] = {t, div};
        ev_head_      = next;
    }

    // ----- grid maths -----
    /** Length of one slice of division d in samples (float). */
    static float SliceSamples(const BeatBlockInfo &info, size_t d)
    {
        return info.samples_per_bar * static_cast<float>(looper::kDivNum[d])
               / static_cast<float>(looper::kDivDen[d]);
    }

    // ----- buttons -----
    /** Called every block with the raw button state. Notes new presses (and
     *  logs / flashes the LED) but does NOT act on them: that happens at the
     *  next 1/16 line in ApplyLine. Transport stopped is the exception: that
     *  goes back to live straight away. */
    void NoteButtons(uint32_t held, bool can)
    {
        const uint32_t pressed  = held & ~prev_held_;
        const uint32_t released = prev_held_ & ~held;
        prev_held_              = held;

        for(uint32_t i = 0; i < looper::kNumDivisions; i++)
        {
            const uint32_t bit = 1u << i;
            if(released & bit)
                ign_mask_ &= ~bit;
            if(pressed & bit)
            {
                if(can)
                {
                    press_mask_ |= bit; // remembered even if released before the line
                    pend_ = static_cast<int>(i);
                    Emit(BeatEventType::Waiting, i);
                }
                else
                {
                    ign_mask_ |= bit;
                    Emit(BeatEventType::IgnoredNotRunning, i);
                }
            }
        }

        if(!can)
        {
            // Transport stopped: everything goes back to live; buttons that
            // are still down stay ignored until released.
            ign_mask_ |= held;
            press_mask_ = 0;
            nstack_     = 0;
            pend_       = -1;
            if(active_div_ >= 0)
                Release();
            ClearSlots();
        }
    }

    /** B6 (every block): notes presses/releases; the effect itself switches on
     *  a 1/16 line (ApplyFxLine). Transport stopped switches it off at once, and
     *  a B6 that is down then stays ignored until released. */
    void NoteFx(bool held, bool can)
    {
        const bool pressed  = held && !pitch_prev_;
        const bool released = !held && pitch_prev_;
        pitch_prev_         = held;
        pitch_held_         = held;
        if(released)
            pitch_ign_ = false;
        if(!can)
        {
            pitch_ign_   = held;
            pitch_press_ = false;
            if(pitch_on_)
            {
                pitch_on_ = false;
                Emit(BeatEventType::PitchOff);
            }
            return;
        }
        if(pressed && !pitch_ign_)
            pitch_press_ = true; // remembered even if released before the line
    }

    /** A 1/16 line: B6 on if it is down (or was pressed since the last line, so a
     *  tap still gives one line), otherwise off. */
    void ApplyFxLine()
    {
        const bool want = !pitch_ign_ && (pitch_held_ || pitch_press_);
        pitch_press_    = false;
        if(want != pitch_on_)
        {
            pitch_on_ = want;
            Emit(want ? BeatEventType::PitchOn : BeatEventType::PitchOff);
        }
    }

    /** A 1/16 line is `xfade_` samples away (we are at the sample where the
     *  crossfade has to start). Act on the buttons now: presses since the last
     *  line go on the stack, buttons that are up come off it, then switch to
     *  whatever is wanted. A tap (pressed and released between two lines) is
     *  pushed this time and removed at the next line, so it repeats for one
     *  line at least. */
    void ApplyLine(const BeatBlockInfo &info, uint32_t held)
    {
        for(uint32_t i = 0; i < looper::kNumDivisions; i++)
        {
            const uint32_t bit = 1u << i;
            if(press_mask_ & bit)
            {
                RemoveFromStack(i); // (moves it to the top if it was already there)
                stack_[nstack_++] = static_cast<uint8_t>(i);
            }
            else if(!(held & bit))
                RemoveFromStack(i);
        }
        press_mask_ = 0;

        const int want = nstack_ > 0 ? stack_[nstack_ - 1] : -1;
        if(want < 0)
        {
            // All buttons are up: live again, and the stored slices go.
            pend_ = -1;
            if(active_div_ >= 0)
                Release();
            ClearSlots();
        }
        else if(want == active_div_)
        {
            pend_ = -1; // back to the one that is already repeating
        }
        else if(slot_[want].state != SlotState::Empty)
        {
            // Captured earlier in this hold: use that slice, in time.
            pend_ = -1;
            Recall(want);
        }
        else
        {
            pend_ = want;
            StartCapture(info);
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

    // ----- crossfades between live and the two voices -----
    void Weights(float w[3]) const
    {
        const float t = tc_ >= xfade_ ? 1.0f
                                      : static_cast<float>(tc_) / static_cast<float>(xfade_);
        for(int k = 0; k < 3; k++)
            w[k] = ws_[k] + (tg_[k] - ws_[k]) * t;
    }

    void BeginTransition(float live, float v0, float v1)
    {
        float w[3];
        Weights(w);
        for(int k = 0; k < 3; k++)
            ws_[k] = w[k];
        tg_[0] = live;
        tg_[1] = v0;
        tg_[2] = v1;
        tc_    = 0;
    }

    void Release()
    {
        BeginTransition(1.0f, 0.0f, 0.0f);
        active_div_ = -1;
        loops_      = 0;
        Emit(BeatEventType::Off);
    }

    /** Forget the stored slices (everything was released). Voices that are
     *  still fading out keep reading the slot buffers, whose contents stay
     *  until a new capture needs the same slot (StartCapture cuts such voices). */
    void ClearSlots()
    {
        for(size_t d = 0; d < looper::kNumDivisions; d++)
            slot_[d].state = SlotState::Empty;
    }

    /** Pick the voice that is not the current one; cut it if it is still fading. */
    int FreeVoice()
    {
        const int vi = cur_ == 0 ? 1 : 0;
        if(v_[vi].active) // an older voice is still fading: cut it (very rare)
        {
            v_[vi].active = false;
            ws_[1 + vi]   = 0.0f;
        }
        return vi;
    }

    void StartFade(int vi)
    {
        float t0 = 0.0f, t1 = 0.0f;
        (vi == 0 ? t0 : t1) = 1.0f;
        loops_ = 0; // a new slice starts: the pitch steps start again
        // the previous voice (if any) fades out
        BeginTransition(0.0f, vi == 0 ? t0 : 0.0f, vi == 1 ? t1 : 0.0f);
        cur_ = vi;
    }

    /** The 1/16 line is `xfade_` samples from now: capture the slice that ends
     *  on it (the last slice-length of audio) into the waiting division's slot
     *  and start repeating it. */
    void StartCapture(const BeatBlockInfo &info)
    {
        const size_t   d   = static_cast<size_t>(pend_);
        const uint32_t L   = static_cast<uint32_t>(SliceSamples(info, d) + 0.5f);
        const uint32_t X   = static_cast<uint32_t>(xfade_);
        if(L < 2 * X || static_cast<size_t>(L) + X > slot_cap_[d])
        {
            // Too short (under 10 ms), or too long for the slot (tempo below kMinBpm).
            Emit(L < 2 * X ? BeatEventType::IgnoredTooShort : BeatEventType::IgnoredTooSlow,
                 static_cast<uint32_t>(d));
            ign_mask_ |= 1u << d;
            RemoveFromStack(static_cast<uint32_t>(d));
            pend_ = -1;
            return;
        }
        // Any voice still reading this slot's old buffer must go first.
        for(int k = 0; k < 2; k++)
            if(v_[k].active && v_[k].slot == static_cast<int>(d))
            {
                v_[k].active = false;
                ws_[1 + k]   = 0.0f;
                tg_[1 + k]   = 0.0f;
            }
        const int vi = FreeVoice();

        Slot &s    = slot_[d];
        s.state    = SlotState::Copying;
        s.L        = L;
        s.S_abs    = hist_abs_ + static_cast<int64_t>(X) - static_cast<int64_t>(L);
        s.copy_pos = s.S_abs - static_cast<int64_t>(X);

        SliceVoice &v = v_[vi];
        v             = SliceVoice();
        v.active      = true;
        v.use_buf     = false;
        v.slot        = static_cast<int>(d);
        v.L           = L;
        v.X           = X;
        v.S           = X;
        v.a           = 0; // start in the pre-roll, so the fade ends at S

        StartFade(vi);
        active_div_ = static_cast<int>(d);
        Emit(BeatEventType::On, static_cast<uint32_t>(d));
        pend_ = -1;
    }

    /** Switch to the stored slice of division d now, at the position it would
     *  have reached if it had been repeating ever since it was captured. The
     *  slice's first repeat began at `S_abs + L`, so the position inside the
     *  slice is (now - S_abs) modulo L. */
    void Recall(int d)
    {
        const Slot    &s = slot_[d];
        const uint32_t X = static_cast<uint32_t>(xfade_);
        const int      vi = FreeVoice();

        const int64_t L = static_cast<int64_t>(s.L);
        int64_t       u = (hist_abs_ - s.S_abs) % L;
        if(u < 0)
            u += L;

        SliceVoice &v = v_[vi];
        v             = SliceVoice();
        v.active      = true;
        v.use_buf     = s.state == SlotState::Ready;
        v.slot        = d;
        v.L           = s.L;
        v.X           = X;
        v.S           = X;
        v.a           = static_cast<int64_t>(X) + u;
        // Already inside the last X samples of the slice? Then the fade back
        // to the start (the loop point) is already under way.
        const int64_t into_end = u - (static_cast<int64_t>(s.L) - static_cast<int64_t>(X));
        if(into_end >= 0)
        {
            v.jumping = true;
            v.jc      = static_cast<uint32_t>(into_end);
            v.b       = v.S - static_cast<int64_t>(X) + into_end;
        }

        StartFade(vi);
        active_div_ = d;
        Emit(BeatEventType::OnStored, static_cast<uint32_t>(d));
    }

    // ----- reading -----
    void Fetch(const SliceVoice &v, int64_t pos, float &l, float &r) const
    {
        int64_t       idx  = pos - (v.S - static_cast<int64_t>(v.X));
        const int64_t last = static_cast<int64_t>(v.L) + v.X - 1;
        if(idx < 0)
            idx = 0;
        if(idx > last)
            idx = last;
        if(v.use_buf)
        {
            const size_t k = slot_off_[v.slot] + static_cast<size_t>(idx);
            l              = fl_[k];
            r              = fr_[k];
            return;
        }
        FetchRing(slot_[v.slot].S_abs - static_cast<int64_t>(v.X) + idx, l, r);
    }

    void FetchRing(int64_t abs, float &l, float &r) const
    {
        if(abs < 0 || abs > hist_abs_ || hist_abs_ - abs >= static_cast<int64_t>(hn_))
        {
            l = 0.0f;
            r = 0.0f;
            return;
        }
        const size_t i = static_cast<size_t>(abs % static_cast<int64_t>(hn_));
        l              = hl_[i];
        r              = hr_[i];
    }

    void ReadVoice(SliceVoice &v, float &l, float &r)
    {
        const int64_t X = v.X;
        // Near the end of the slice: start fading to the beginning of the
        // pre-roll, so that at the end of the fade we are at S again.
        if(!v.jumping && v.a >= v.S + static_cast<int64_t>(v.L) - X)
        {
            v.b       = v.S - X;
            v.jc      = 0;
            v.jumping = true;
            if(active_div_ >= 0 && cur_ >= 0 && &v == &v_[cur_])
                jump_idx_ = blk_i_; // (for the pitch stage: a new repeat starts here)
        }
        float al, ar;
        Fetch(v, v.a, al, ar);
        if(v.jumping)
        {
            float bl, br;
            Fetch(v, v.b, bl, br);
            const float g = static_cast<float>(v.jc) / static_cast<float>(v.X);
            al += (bl - al) * g;
            ar += (br - ar) * g;
            v.b++;
            v.jc++;
            if(v.jc >= v.X)
            {
                v.a       = v.b - 1; // (incremented below)
                v.jumping = false;
                if(active_div_ >= 0 && cur_ >= 0 && &v == &v_[cur_])
                    loops_++; // the repeat that is heard has wrapped round once more
            }
        }
        v.a++;
        l = al;
        r = ar;
    }

    /** Copy the captured slices into their slot buffers, a chunk per block.
     *  Runs for every slot that is still Copying, even if the repeat has
     *  moved on to another division (it may be flipped back to). */
    void CopyStep()
    {
        size_t budget = looper::kCopyChunkFrames;
        for(size_t d = 0; d < looper::kNumDivisions && budget > 0; d++)
        {
            Slot &s = slot_[d];
            if(s.state != SlotState::Copying)
                continue;
            const int64_t first = s.S_abs - static_cast<int64_t>(xfade_);
            const int64_t end   = s.S_abs + static_cast<int64_t>(s.L);
            const int64_t limit = end < hist_abs_ ? end : hist_abs_; // only audio recorded so far
            while(s.copy_pos < limit && budget > 0)
            {
                float l, r;
                FetchRing(s.copy_pos, l, r);
                const size_t k = slot_off_[d] + static_cast<size_t>(s.copy_pos - first);
                fl_[k]         = l;
                fr_[k]         = r;
                s.copy_pos++;
                budget--;
            }
            if(s.copy_pos >= end)
            {
                s.state = SlotState::Ready;
                // Voices on this slot switch from the ring to the buffer: same
                // samples, so nothing changes in the sound.
                for(int vi = 0; vi < 2; vi++)
                    if(v_[vi].active && v_[vi].slot == static_cast<int>(d))
                        v_[vi].use_buf = true;
            }
        }
    }

    void Publish()
    {
        pub_active_  = static_cast<int8_t>(active_div_);
        pub_pending_ = static_cast<int8_t>(pend_);
        pub_pitch_on_      = pitch_on_;
        pub_pitch_pending_ = pitch_press_;
    }

    // buffers
    float  *hl_ = nullptr, *hr_ = nullptr;
    size_t  hn_ = 1;
    float  *fl_ = nullptr, *fr_ = nullptr;
    size_t  fn_ = 0;
    size_t  xfade_ = 240;
    int64_t hist_abs_ = 0; // frames written so far = absolute sample number of "now"

    // stored slices (one slot per division)
    Slot   slot_[looper::kNumDivisions];
    size_t slot_off_[looper::kNumDivisions] = {};
    size_t slot_cap_[looper::kNumDivisions] = {};

    // voices and crossfade
    SliceVoice v_[2];
    int        cur_ = -1;
    float      ws_[3] = {1, 0, 0}; // weights at the start of the fade: live, voice 0, voice 1
    float      tg_[3] = {1, 0, 0}; // weights at the end of the fade
    size_t     tc_    = 240;       // samples into the fade

    // buttons
    uint32_t prev_held_  = 0;
    uint32_t ign_mask_   = 0;
    uint32_t press_mask_ = 0; // presses seen since the last 1/16 line
    static constexpr int64_t kNoLine = -1000000000;
    int64_t  last_line_abs_ = kNoLine; // sample number of the last 1/16 line we acted on
    uint8_t  stack_[looper::kNumDivisions] = {};
    size_t   nstack_     = 0;
    int      pend_       = -1;
    int      active_div_ = -1;

    // events + status
    static constexpr uint32_t kEvSize = 16;
    BeatEvent ev_[kEvSize];
    uint32_t  ev_head_ = 0;
    uint32_t  ev_tail_ = 0;
    volatile int8_t pub_active_  = -1;
    volatile int8_t pub_pending_ = -1;
    volatile bool   pub_pitch_on_      = false;
    volatile bool   pub_pitch_pending_ = false;

    // B6 pitch effect
    bool pitch_prev_  = false;
    bool pitch_held_  = false;
    bool pitch_press_ = false;
    bool pitch_ign_   = false;
    bool pitch_on_    = false;
    int  loops_       = 0;
    int  jump_idx_    = -1; // sample index of the last loop-crossfade start in this block
    int  blk_i_       = 0;  // sample index inside the block being processed
};
