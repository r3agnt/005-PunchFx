#pragma once

#include <cstddef>
#include <cstdint>
#include "looper_config.h"
#include "spsc_queue.h"

/** clock_tracker.h : follows MIDI clock and transport (start / stop).
 *
 *  Plain C++ (no libDaisy), so it is tested on the Mac in test/.
 *
 *  How it fits together:
 *   1. The MIDI receive code (main.cpp) calls RtEventQueue::Push() for every
 *      clock tick / Start / Stop / Continue message. That is all it does.
 *   2. The audio callback calls ClockTracker::Advance() once per block. It
 *      takes the queued messages, updates the song position and moves time
 *      forward by one block of samples.
 *   3. Anyone can read GetStatus(), Bpm(), PositionInBarSamples() etc.
 *
 *  Timing: a message is stamped with the start of the audio block that
 *  picks it up, so each stamp can be up to one block (about 1 ms) late.
 *  The tempo is measured over a whole bar of ticks, so that error averages
 *  out; the position inside a bar is "last tick + samples since then", so
 *  it never accumulates.
 *
 *  Transport rules (as agreed):
 *   - Start: the next clock tick is beat 1 of bar 1 (position 0).
 *   - Stop: counting stops.
 *   - Continue and Song Position Pointer are ignored for now.
 *   - No ticks for half a second = "clock lost"; counting stops.
 */

/** The MIDI real-time messages we care about. */
enum class RtEvent : uint8_t
{
    Tick,
    Start,
    Stop,
    Continue
};

/** Passes real-time messages from the MIDI code to the audio callback. */
using RtEventQueue = SpscQueue<RtEvent, 64>;

/** Things that happened since the last TakeEvents() (for logging). */
enum ClockEvent : uint32_t
{
    kEvStart          = 1u << 0, // Start message received
    kEvStop           = 1u << 1, // Stop message received
    kEvContinueIgnrd  = 1u << 2, // Continue message received (ignored)
    kEvClockFound     = 1u << 3, // first tick after silence
    kEvClockLost      = 1u << 4, // ticks stopped arriving
    kEvEighth         = 1u << 5, // a new eighth note began
    kEvBeat           = 1u << 6, // a new beat began
    kEvBar            = 1u << 7, // a new bar began
};

/** A copy of the song position, safe to read from another context.
 *  bar, beat and eighth are 0-based (the log prints them 1-based). */
struct ClockStatus
{
    bool     present = false; // clock ticks are arriving
    bool     running = false; // transport is running (Start received)
    uint32_t bar     = 0;     // bars since Start
    uint32_t beat    = 0;     // 0..3 within the bar
    uint32_t eighth  = 0;     // 0..7 within the bar
};

class ClockTracker
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        timeout_     = static_cast<uint64_t>(sample_rate * 0.5f);
        now_         = 0;
        last_tick_   = 0;
        since_tick_  = 0;
        n_           = 0;
        head_        = 0;
        present_     = false;
        running_     = false;
        start_pend_  = false;
        tick_index_  = 0;
        events_      = 0;
        Publish();
    }

    /** Call once per audio block, at the start of the block.
     *  `block_size` = number of samples in the block. */
    void Advance(RtEventQueue &queue, size_t block_size)
    {
        RtEvent e;
        while(queue.Pop(e))
        {
            switch(e)
            {
                case RtEvent::Tick: OnTick(); break;
                case RtEvent::Start:
                    start_pend_ = true;
                    running_    = false;
                    events_ |= kEvStart;
                    break;
                case RtEvent::Stop:
                    start_pend_ = false;
                    running_    = false;
                    events_ |= kEvStop;
                    break;
                case RtEvent::Continue: events_ |= kEvContinueIgnrd; break;
            }
        }

        now_ += block_size;
        since_tick_ += static_cast<uint32_t>(block_size);

        // No ticks for a while: the clock has gone away.
        if(present_ && (now_ - last_tick_) > timeout_)
        {
            present_    = false;
            running_    = false;
            start_pend_ = false;
            n_          = 0;
            events_ |= kEvClockLost;
        }
        Publish();
    }

    /** Returns the events since the last call and clears them. */
    uint32_t TakeEvents()
    {
        const uint32_t e = events_;
        events_          = 0;
        return e;
    }

    ClockStatus GetStatus() const
    {
        ClockStatus s;
        s.present = pub_present_;
        s.running = pub_running_;
        s.bar     = pub_bar_;
        s.beat    = pub_beat_;
        s.eighth  = pub_eighth_;
        return s;
    }

    /** Samples between two clock ticks (0 until a tempo has been measured). */
    float SamplesPerTick() const { return TempoValid() ? Period() : 0.0f; }

    /** Samples in one bar (0 until a tempo has been measured). */
    float SamplesPerBar() const
    {
        return SamplesPerTick() * static_cast<float>(looper::kTicksPerBar);
    }

    /** Tempo in beats per minute (0 until measured, after one beat of ticks). */
    float Bpm() const
    {
        const float p = SamplesPerTick();
        if(p <= 0.0f)
            return 0.0f;
        return 60.0f * sample_rate_ / (static_cast<float>(looper::kTicksPerBeat) * p);
    }

    /** Where we are inside the current bar, in samples since the bar began
     *  (0 if not running). Estimated from the last tick plus the samples
     *  that have passed since. */
    float PositionInBarSamples() const
    {
        if(!running_ || !TempoValid())
            return 0.0f;
        const float p       = Period();
        const float in_tick = static_cast<float>(since_tick_) < p
                                  ? static_cast<float>(since_tick_)
                                  : p;
        return static_cast<float>(tick_index_ % looper::kTicksPerBar) * p + in_tick;
    }

    /** Position inside the bar (samples) at the START of the block that was
     *  just passed to Advance(). Advance() has already moved time forward by
     *  `block_size`, so this goes back by one block. */
    float PositionAtBlockStart(size_t block_size) const
    {
        if(!running_ || !TempoValid())
            return 0.0f;
        const float p = Period();
        float       s = since_tick_ >= block_size
                            ? static_cast<float>(since_tick_ - block_size)
                            : 0.0f;
        if(s > p)
            s = p;
        return static_cast<float>(tick_index_ % looper::kTicksPerBar) * p + s;
    }

    /** Bars since Start, at the START of the block that was just passed to
     *  Advance() (goes with PositionAtBlockStart). Counts on without limit
     *  (a 32-bit tick counter: about 44 million bars). */
    uint32_t BarAtBlockStart() const
    {
        return tick_index_ / looper::kTicksPerBar;
    }

  private:
    // ----- tempo measurement: timestamps of the last 97 ticks -----
    static constexpr size_t kHist   = looper::kTicksPerBar + 1;
    static constexpr size_t kShortW = looper::kTicksPerBeat;     // 1 beat
    static constexpr size_t kMinN   = looper::kTicksPerBeat + 1; // tempo valid

    bool TempoValid() const { return n_ >= kMinN; }

    /** Timestamp `back` ticks before the newest one. */
    uint64_t Stamp(size_t back) const
    {
        return hist_[(head_ + kHist - 1 - back) % kHist];
    }

    /** Average samples per tick over the last bar (or less, if fewer ticks). */
    float Period() const
    {
        if(n_ < 2)
            return 0.0f;
        size_t w = n_ - 1;
        if(w > looper::kTicksPerBar)
            w = looper::kTicksPerBar;
        return static_cast<float>(Stamp(0) - Stamp(w)) / static_cast<float>(w);
    }

    void OnTick()
    {
        if(!present_)
        {
            present_ = true;
            n_       = 0;
            events_ |= kEvClockFound;
        }

        hist_[head_] = now_;
        head_        = (head_ + 1) % kHist;
        if(n_ < kHist)
            n_++;
        last_tick_  = now_;
        since_tick_ = 0;

        // Tempo jumped (last beat differs from last bar by over 5%)?
        // Forget the older ticks so the new tempo shows up quickly.
        if(n_ > 2 * kShortW)
        {
            const float lng  = Period();
            const float shrt = static_cast<float>(Stamp(0) - Stamp(kShortW))
                               / static_cast<float>(kShortW);
            const float diff = shrt > lng ? shrt - lng : lng - shrt;
            if(diff > 0.05f * lng)
                n_ = kMinN;
        }

        if(start_pend_)
        {
            start_pend_ = false;
            running_    = true;
            tick_index_ = 0;
            FireBoundaries();
        }
        else if(running_)
        {
            tick_index_++;
            FireBoundaries();
        }
    }

    void FireBoundaries()
    {
        const uint32_t t = tick_index_ % looper::kTicksPerBar;
        if(t % looper::kTicksPerEighth == 0)
            events_ |= kEvEighth;
        if(t % looper::kTicksPerBeat == 0)
            events_ |= kEvBeat;
        if(t == 0)
            events_ |= kEvBar;
    }

    void Publish()
    {
        const uint32_t t = tick_index_ % looper::kTicksPerBar;
        pub_bar_         = tick_index_ / looper::kTicksPerBar;
        pub_beat_        = t / looper::kTicksPerBeat;
        pub_eighth_      = t / looper::kTicksPerEighth;
        pub_present_     = present_;
        pub_running_     = running_;
    }

    float    sample_rate_ = 48000.0f;
    uint64_t timeout_     = 24000;
    uint64_t now_         = 0;       // samples since Init
    uint64_t last_tick_   = 0;       // time of the newest tick
    uint32_t since_tick_  = 0;       // samples since the newest tick
    uint64_t hist_[kHist] = {};      // tick timestamps (ring)
    size_t   head_        = 0;       // next slot to write
    size_t   n_           = 0;       // how many stamps are valid
    bool     present_     = false;
    bool     running_     = false;
    bool     start_pend_  = false;   // Start received, waiting for first tick
    uint32_t tick_index_  = 0;       // ticks since the bar-1 downbeat tick
    uint32_t events_      = 0;

    // Copies for other contexts (written at the end of Advance()).
    volatile bool     pub_present_ = false;
    volatile bool     pub_running_ = false;
    volatile uint32_t pub_bar_     = 0;
    volatile uint32_t pub_beat_    = 0;
    volatile uint32_t pub_eighth_  = 0;
};
