#pragma once

#include <cstddef>
#include <cstdint>
#include "looper_config.h"

/** quantized_loop_buffer.h : the quantised audio looper (ARM -> REC -> PLAYBACK).
 *
 *  Plain C++ (no libDaisy) so it is tested on the Mac in test/.
 *  Design and rules: docs/DESIGN_LOOPER.md.
 *
 *  What it does, in short:
 *   - IDLE: the performance signal passes straight through.
 *   - B1 -> ARMED: waits for the next loop cycle (a bar that is a multiple of
 *     the chosen length, counted from transport Start).
 *   - RECORDING: copies the performance signal into the buffer. Output is
 *     still the live signal.
 *   - At the next cycle: PLAYING. The loop restarts every cycle, re-aligned
 *     to the clock, and is crossfaded in from the live signal.
 *   - B2 switches the output between loop and live signal (loop keeps
 *     running silently so it stays in time). B1 while playing arms a
 *     re-record. Shift+B1 clears.
 *
 *  Click protection: every change of what is heard is a 5 ms linear
 *  crossfade. The loop seam is crossfaded too: 20 ms of audio is recorded
 *  PAST the loop end (the "tail") and blended into the loop start.
 *
 *  Threading: everything here runs in the audio callback. Button presses are
 *  passed in as commands (via a queue) and read back through GetStatus() /
 *  PopEvent().
 */

enum class LoopCmd : uint8_t
{
    ArmToggle,    // B1
    Clear,        // Shift + B1
    ToggleSource, // B2
};

enum class LoopState : uint8_t
{
    Idle,
    Armed,       // waiting for the next cycle, no loop yet
    Recording,
    Playing,
    PlayingArmed // a loop is playing and a re-record is armed
};

/** What the clock is doing in this audio block (built by main.cpp). */
struct LoopBlockInfo
{
    bool     clock_present   = false; // MIDI clock ticks are arriving
    bool     running         = false; // transport is running
    bool     bar_start       = false; // a new bar began at the start of this block
    uint32_t bar             = 0;     // bar index since Start (0-based)
    float    samples_per_bar = 0.0f;  // tempo estimate (0 = not known yet)
};

enum class LoopEventType : uint8_t
{
    Armed,         // a = cycle bars, b = start bar (1-based, 0 = when transport starts)
    ReArmed,       // same, while a loop is playing
    ArmCancelled,
    RecordStart,   // a = bars
    PlaybackStart, // a = bars, b = loop length in samples
    RecordAborted, // a = AbortReason
    Cleared,
    SourceBuffer,  // output is now the loop
    SourceInput,   // output is now the live signal
    Held,          // transport stopped, loop paused
    Refused        // a = RefuseReason
};

enum AbortReason : uint32_t
{
    kAbortTransportStopped = 0,
    kAbortTooLong          = 1,
};

enum RefuseReason : uint32_t
{
    kRefuseNoClock   = 0, // B1: no MIDI clock
    kRefuseTooSlow   = 1, // B1: tempo too slow for this loop length
    kRefuseRecording = 2, // B1: already recording
    kRefuseNoLoop    = 3, // B2: no loop to switch to
};

struct LoopEvent
{
    LoopEventType type = LoopEventType::Cleared;
    uint32_t      a    = 0;
    uint32_t      b    = 0;
};

/** A copy of the state for the LEDs (safe to read from another context). */
struct LoopStatus
{
    LoopState state           = LoopState::Idle;
    bool      buffer_audible  = false; // the loop is what you hear
    bool      held            = false; // loop paused because the transport stopped
    uint32_t  bars            = 1;     // cycle length in use (knob when idle)
};

/** Reads the loop with a crossfaded jump: when told to jump, the old
 *  position fades out while the new one fades in, so nothing clicks. */
class LoopReader
{
  public:
    void Init(size_t xfade_frames)
    {
        step_ = 1.0f / static_cast<float>(xfade_frames > 0 ? xfade_frames : 1);
        Reset();
    }

    /** Start at the beginning with no fade. */
    void Reset()
    {
        a_       = 0;
        b_       = 0;
        j_       = 0.0f;
        jumping_ = false;
    }

    /** Fade from the current position to `pos`. */
    void JumpTo(size_t pos)
    {
        if(jumping_)
            a_ = b_; // a jump was still fading: finish it instantly (rare)
        b_       = pos;
        j_       = 0.0f;
        jumping_ = true;
    }

    size_t Position() const { return a_; }

    /** Read one frame and advance. `valid` = number of usable frames in the
     *  buffer. Running off the end starts a jump back to the start. */
    void Read(const float *bl, const float *br, size_t valid, float &l, float &r)
    {
        if(valid == 0)
        {
            l = 0.0f;
            r = 0.0f;
            return;
        }
        if(a_ >= valid && !jumping_)
            JumpTo(0);
        const size_t ia = a_ < valid ? a_ : valid - 1;
        float        ol = bl[ia];
        float        orr = br[ia];
        a_++;
        if(jumping_)
        {
            const size_t ib = b_ < valid ? b_ : valid - 1;
            ol += (bl[ib] - ol) * j_;
            orr += (br[ib] - orr) * j_;
            b_++;
            j_ += step_;
            if(j_ >= 1.0f)
            {
                a_       = b_;
                jumping_ = false;
            }
        }
        l = ol;
        r = orr;
    }

  private:
    size_t a_       = 0;
    size_t b_       = 0;
    float  j_       = 0.0f;
    float  step_    = 1.0f;
    bool   jumping_ = false;
};

class QuantizedLoopBuffer
{
  public:
    /** `buf_l` / `buf_r` must each hold `capacity_frames` floats. */
    void Init(float *buf_l, float *buf_r, size_t capacity_frames, float sample_rate)
    {
        buf_l_  = buf_l;
        buf_r_  = buf_r;
        cap_    = capacity_frames;
        sr_     = sample_rate;
        xfade_  = looper::XfadeFrames(sample_rate);
        tail_   = looper::TailFrames(sample_rate);
        if(xfade_ < 1)
            xfade_ = 1;
        step_    = 1.0f / static_cast<float>(xfade_);
        max_rec_ = cap_ > tail_ ? cap_ - tail_ : 0;
        reader_.Init(xfade_);
        state_       = LoopState::Idle;
        src_buffer_  = true;
        held_        = false;
        reader_on_   = false;
        mix_         = 0.0f;
        mix_target_  = 0.0f;
        knob_bars_   = 4;
        arm_bars_    = 4;
        loop_bars_   = 4;
        rec_bars_    = 4;
        loop_len_    = 0;
        tail_valid_  = 0;
        tail_left_   = 0;
        ev_head_     = 0;
        ev_tail_     = 0;
        Publish();
    }

    /** The loop length chosen with the knob (latched when you arm). */
    void SetLoopBars(uint32_t bars) { knob_bars_ = bars > 0 ? bars : 1; }

    /** Apply a button command. Call before Process() in the same block. */
    void Command(LoopCmd cmd, const LoopBlockInfo &info)
    {
        switch(cmd)
        {
            case LoopCmd::ArmToggle: CmdArm(info); break;
            case LoopCmd::Clear: CmdClear(); break;
            case LoopCmd::ToggleSource: CmdSource(); break;
        }
        UpdateTarget();
        Publish();
    }

    /** Process one block. `in` is the performance signal (what you would
     *  hear if there were no loop); `out` gets what should be heard. */
    void Process(const LoopBlockInfo &info,
                 const float *inl, const float *inr,
                 float *outl, float *outr, size_t n)
    {
        BlockStart(info);

        bool overflow = false;
        for(size_t i = 0; i < n; i++)
        {
            const float pl = inl[i];
            const float pr = inr[i];

            // Record (or finish recording the tail past the loop end).
            if(state_ == LoopState::Recording)
            {
                if(rec_pos_ < max_rec_)
                {
                    buf_l_[rec_pos_] = pl;
                    buf_r_[rec_pos_] = pr;
                    rec_pos_++;
                }
                else
                    overflow = true;
            }
            else if(tail_left_ > 0)
            {
                buf_l_[tail_pos_] = pl;
                buf_r_[tail_pos_] = pr;
                tail_pos_++;
                tail_left_--;
                tail_valid_++;
            }

            // Loop playback (runs silently when the live signal is selected).
            float ll = 0.0f;
            float lr = 0.0f;
            if(reader_on_)
                reader_.Read(buf_l_, buf_r_, loop_len_ + tail_valid_, ll, lr);

            // Fade between live and loop.
            if(mix_ < mix_target_)
            {
                mix_ += step_;
                if(mix_ > mix_target_)
                    mix_ = mix_target_;
            }
            else if(mix_ > mix_target_)
            {
                mix_ -= step_;
                if(mix_ < mix_target_)
                    mix_ = mix_target_;
            }
            outl[i] = pl + (ll - pl) * mix_;
            outr[i] = pr + (lr - pr) * mix_;
        }

        if(overflow)
        {
            Abort(kAbortTooLong);
            UpdateTarget();
        }
        // Once the loop has faded out completely, stop reading it.
        if(!HasLoop() && mix_ <= 0.0f)
            reader_on_ = false;
        Publish();
    }

    /** Oldest log event, if any. */
    bool PopEvent(LoopEvent &e)
    {
        if(ev_head_ == ev_tail_)
            return false;
        e        = ev_[ev_tail_];
        ev_tail_ = (ev_tail_ + 1) % kEvSize;
        return true;
    }

    LoopStatus GetStatus() const
    {
        LoopStatus s;
        s.state          = static_cast<LoopState>(pub_state_);
        s.buffer_audible = pub_audible_;
        s.held           = pub_held_;
        s.bars           = pub_bars_;
        return s;
    }

    // Read-only details (used by the tests).
    LoopState State() const { return state_; }
    size_t    LoopLength() const { return loop_len_; }
    size_t    ReaderPosition() const { return reader_.Position(); }

  private:
    bool HasLoop() const
    {
        return state_ == LoopState::Playing || state_ == LoopState::PlayingArmed;
    }

    void Emit(LoopEventType t, uint32_t a = 0, uint32_t b = 0)
    {
        const uint32_t next = (ev_head_ + 1) % kEvSize;
        if(next == ev_tail_)
            return; // log queue full: drop
        ev_[ev_head_] = {t, a, b};
        ev_head_      = next;
    }

    void UpdateTarget()
    {
        mix_target_ = (HasLoop() && src_buffer_ && !held_) ? 1.0f : 0.0f;
    }

    /** 1-based number of the bar where recording would start (0 = when the
     *  transport starts). */
    static uint32_t StartBar(const LoopBlockInfo &info, uint32_t bars)
    {
        if(!info.running)
            return 0;
        if(info.bar_start && info.bar % bars == 0)
            return info.bar + 1;
        return ((info.bar / bars) + 1) * bars + 1;
    }

    void CmdArm(const LoopBlockInfo &info)
    {
        switch(state_)
        {
            case LoopState::Idle:
            case LoopState::Playing:
            {
                if(!info.clock_present)
                {
                    Emit(LoopEventType::Refused, kRefuseNoClock);
                    return;
                }
                const uint32_t bars = knob_bars_;
                if(info.samples_per_bar > 0.0f
                   && static_cast<float>(bars) * info.samples_per_bar
                          > static_cast<float>(max_rec_))
                {
                    Emit(LoopEventType::Refused, kRefuseTooSlow);
                    return;
                }
                arm_bars_ = bars;
                const bool playing = state_ == LoopState::Playing;
                state_ = playing ? LoopState::PlayingArmed : LoopState::Armed;
                Emit(playing ? LoopEventType::ReArmed : LoopEventType::Armed,
                     bars,
                     StartBar(info, bars));
                break;
            }
            case LoopState::Armed:
                state_ = LoopState::Idle;
                Emit(LoopEventType::ArmCancelled);
                break;
            case LoopState::PlayingArmed:
                state_ = LoopState::Playing;
                Emit(LoopEventType::ArmCancelled);
                break;
            case LoopState::Recording:
                Emit(LoopEventType::Refused, kRefuseRecording);
                break;
        }
    }

    void CmdClear()
    {
        if(state_ == LoopState::Idle)
            return;
        state_      = LoopState::Idle; // loop fades out to the live signal
        held_       = false;
        src_buffer_ = true;
        tail_left_  = 0;
        Emit(LoopEventType::Cleared);
    }

    void CmdSource()
    {
        if(!HasLoop())
        {
            Emit(LoopEventType::Refused, kRefuseNoLoop);
            return;
        }
        src_buffer_ = !src_buffer_;
        Emit(src_buffer_ ? LoopEventType::SourceBuffer : LoopEventType::SourceInput);
    }

    void Abort(AbortReason why)
    {
        state_     = LoopState::Idle;
        held_      = false;
        tail_left_ = 0;
        Emit(LoopEventType::RecordAborted, why);
    }

    void StartRecording(uint32_t bar)
    {
        // A loop that was playing is replaced; it keeps fading out (reading
        // its old data) while the new recording starts at the buffer start.
        state_     = LoopState::Recording;
        rec_bars_  = arm_bars_;
        rec_start_ = bar;
        rec_pos_   = 0;
        tail_left_ = 0;
        held_      = false;
        Emit(LoopEventType::RecordStart, rec_bars_);
    }

    void EndRecording()
    {
        loop_len_   = rec_pos_;
        loop_bars_  = rec_bars_;
        tail_pos_   = loop_len_;
        tail_left_  = tail_;
        tail_valid_ = 0;
        state_      = LoopState::Playing;
        src_buffer_ = true;
        held_       = false;
        reader_.Reset();
        reader_on_ = true;
        Emit(LoopEventType::PlaybackStart,
             loop_bars_,
             static_cast<uint32_t>(loop_len_));
    }

    /** Called at every cycle boundary while a loop exists: restart the
     *  loop from its beginning (crossfaded) so it stays locked to the clock. */
    void Resync()
    {
        if(held_ || mix_ <= 0.0001f)
            reader_.Reset();
        else
            reader_.JumpTo(0);
        reader_on_ = true;
        held_      = false;
    }

    void BlockStart(const LoopBlockInfo &info)
    {
        if(!info.running)
        {
            if(state_ == LoopState::Recording)
                Abort(kAbortTransportStopped);
            else if(HasLoop() && !held_)
            {
                held_ = true;
                Emit(LoopEventType::Held);
            }
        }
        else if(info.bar_start)
        {
            const uint32_t bar = info.bar;
            if((state_ == LoopState::Armed || state_ == LoopState::PlayingArmed)
               && bar % arm_bars_ == 0)
                StartRecording(bar);
            else if(state_ == LoopState::Recording)
            {
                if(bar < rec_start_)
                    Abort(kAbortTransportStopped); // transport restarted
                else if(bar >= rec_start_ + rec_bars_)
                    EndRecording();
            }
            else if(HasLoop() && bar % loop_bars_ == 0)
                Resync();
        }
        UpdateTarget();
    }

    void Publish()
    {
        pub_state_   = static_cast<uint8_t>(state_);
        pub_audible_ = HasLoop() && src_buffer_ && !held_;
        pub_held_    = held_;
        switch(state_)
        {
            case LoopState::Armed: pub_bars_ = arm_bars_; break;
            case LoopState::Recording: pub_bars_ = rec_bars_; break;
            case LoopState::Playing:
            case LoopState::PlayingArmed: pub_bars_ = loop_bars_; break;
            default: pub_bars_ = knob_bars_; break;
        }
    }

    // buffers
    float *buf_l_    = nullptr;
    float *buf_r_    = nullptr;
    size_t cap_      = 0;
    size_t max_rec_  = 0;
    float  sr_       = 48000.0f;
    size_t xfade_    = 240;
    size_t tail_     = 960;
    float  step_     = 1.0f / 240.0f;

    // state
    LoopState  state_      = LoopState::Idle;
    bool       src_buffer_ = true;  // B2: true = loop, false = live signal
    bool       held_       = false; // transport stopped while a loop exists
    uint32_t   knob_bars_  = 4;
    uint32_t   arm_bars_   = 4;     // length chosen when armed
    uint32_t   rec_bars_   = 4;
    uint32_t   rec_start_  = 0;     // bar where recording started
    uint32_t   loop_bars_  = 4;     // length of the loop that exists
    size_t     rec_pos_    = 0;
    size_t     loop_len_   = 0;     // frames in the loop (without the tail)
    size_t     tail_pos_   = 0;
    size_t     tail_left_  = 0;
    size_t     tail_valid_ = 0;
    LoopReader reader_;
    bool       reader_on_  = false;
    float      mix_        = 0.0f;  // 0 = live signal, 1 = loop
    float      mix_target_ = 0.0f;

    // log events
    static constexpr uint32_t kEvSize = 16;
    LoopEvent ev_[kEvSize];
    uint32_t  ev_head_ = 0;
    uint32_t  ev_tail_ = 0;

    // copies for other contexts
    volatile uint8_t  pub_state_   = 0;
    volatile bool     pub_audible_ = false;
    volatile bool     pub_held_    = false;
    volatile uint32_t pub_bars_    = 1;
};
