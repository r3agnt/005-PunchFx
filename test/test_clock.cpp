// Host-side test for src/engine/clock_tracker.h and looper_config.h.
// Built with the Mac's own compiler (not the ARM one): `make -C test`.
// It fakes a DAW sending MIDI clock and checks what the tracker reports.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../src/engine/clock_tracker.h"

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, ...)                                  \
    do                                                    \
    {                                                     \
        g_checks++;                                       \
        if(!(cond))                                       \
        {                                                 \
            g_fails++;                                    \
            std::printf("  FAIL line %d: ", __LINE__);    \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
        }                                                 \
    } while(0)

static const float  kSr    = 48000.f;
static const size_t kBlock = 48; // libDaisy default block size

/** Fake audio engine + fake DAW. Time is in samples. */
struct Sim
{
    ClockTracker  clock;
    RtEventQueue  queue;
    uint64_t      now = 0;       // start of the next block
    uint32_t      ev_counts[8] = {}; // how often each ClockEvent bit was seen
    uint32_t      all_events   = 0;

    Sim() { clock.Init(kSr); }

    /** Run one audio block. Messages "sent" before it are picked up first. */
    void Block()
    {
        clock.Advance(queue, kBlock);
        const uint32_t e = clock.TakeEvents();
        all_events |= e;
        for(int b = 0; b < 8; b++)
            if(e & (1u << b))
                ev_counts[b]++;
        now += kBlock;
    }

    void RunUntil(uint64_t t)
    {
        while(now < t)
            Block();
    }
};

static uint32_t Count(const Sim &s, ClockEvent e)
{
    for(int b = 0; b < 8; b++)
        if(e == (1u << b))
            return s.ev_counts[b];
    return 0;
}

/** Send `n` ticks at `bpm` starting at time t0, with +/- jitter samples
 *  of random timing error. Returns the time just after the last tick. */
static uint64_t SendTicks(Sim &s, uint64_t t0, int n, double bpm, int jitter, unsigned seed = 1)
{
    std::srand(seed);
    const double period = 60.0 * kSr / (24.0 * bpm);
    uint64_t     t_last = t0;
    for(int i = 0; i < n; i++)
    {
        double ideal = static_cast<double>(t0) + i * period;
        double j     = jitter ? (std::rand() % (2 * jitter + 1)) - jitter : 0;
        uint64_t t   = static_cast<uint64_t>(ideal + j);
        if(t < s.now)
            t = s.now;
        s.RunUntil(t); // the message arrives at time t ...
        s.queue.Push(RtEvent::Tick); // ... and is picked up by the next block
        t_last = t;
    }
    return t_last;
}

static void Settle(Sim &s) { s.RunUntil(s.now + 4 * kBlock); }

// ---------------------------------------------------------------------
static void TestBasicCounting()
{
    std::printf("basic counting at 120 BPM\n");
    Sim s;
    s.queue.Push(RtEvent::Start);
    SendTicks(s, 1000, 385, 120.0, 0); // ticks 0..384 = 4 bars + downbeat of bar 5
    Settle(s);

    CHECK(Count(s, kEvStart) == 1, "start count %u", Count(s, kEvStart));
    CHECK(Count(s, kEvBar) == 5, "bar events %u (want 5)", Count(s, kEvBar));
    CHECK(Count(s, kEvBeat) == 17, "beat events %u (want 17)", Count(s, kEvBeat));
    CHECK(Count(s, kEvEighth) == 33, "eighth events %u (want 33)", Count(s, kEvEighth));
    CHECK(Count(s, kEvClockFound) == 1, "clock found %u", Count(s, kEvClockFound));
    CHECK(Count(s, kEvClockLost) == 0, "clock lost %u", Count(s, kEvClockLost));

    ClockStatus st = s.clock.GetStatus();
    CHECK(st.running && st.present, "running/present");
    CHECK(st.bar == 4 && st.beat == 0 && st.eighth == 0, "status bar %u beat %u eighth %u",
          st.bar, st.beat, st.eighth);

    const float bpm = s.clock.Bpm();
    CHECK(std::fabs(bpm - 120.f) < 0.3f, "bpm %.3f", bpm);
    const float spb = s.clock.SamplesPerBar();
    CHECK(std::fabs(spb - 96000.f) < 96000.f * 0.002f, "samples per bar %.1f", spb);
}

static void TestJitterAndTempos()
{
    std::printf("tempo estimate with timing jitter\n");
    const double bpms[] = {60.0, 90.0, 120.0, 133.3, 174.0};
    for(double bpm : bpms)
    {
        Sim s;
        s.queue.Push(RtEvent::Start);
        SendTicks(s, 1000, 300, bpm, 30, 7); // +/- 30 samples (0.6 ms)
        Settle(s);
        const float est = s.clock.Bpm();
        CHECK(std::fabs(est - bpm) < bpm * 0.004, "bpm %.1f estimated as %.3f", bpm, est);
    }
}

static void TestTempoChange()
{
    std::printf("tempo change 120 -> 100\n");
    Sim s;
    s.queue.Push(RtEvent::Start);
    uint64_t t = SendTicks(s, 1000, 200, 120.0, 0);
    // continue the clock at 100 BPM: next tick one 100-BPM period later
    const double p100 = 60.0 * kSr / (24.0 * 100.0);
    SendTicks(s, t + static_cast<uint64_t>(p100), 40, 100.0, 0); // ~1.5 beats... 40 ticks
    Settle(s);
    const float est = s.clock.Bpm();
    CHECK(std::fabs(est - 100.f) < 2.0f, "bpm after change %.2f (want ~100)", est);
}

static void TestStopStart()
{
    std::printf("stop, start and reset\n");
    Sim s;
    s.queue.Push(RtEvent::Start);
    uint64_t t = SendTicks(s, 1000, 150, 120.0, 0); // 1.5 bars
    Settle(s);
    CHECK(s.clock.GetStatus().bar == 1, "bar %u before stop", s.clock.GetStatus().bar);

    s.queue.Push(RtEvent::Stop);
    s.Block();
    CHECK(!s.clock.GetStatus().running, "should not be running after stop");
    CHECK(Count(s, kEvStop) == 1, "stop count");
    const uint32_t bars_before = Count(s, kEvBar);

    // clock keeps ticking after stop: nothing should be counted
    t = SendTicks(s, t + 1000, 100, 120.0, 0);
    Settle(s);
    CHECK(Count(s, kEvBar) == bars_before, "bars counted while stopped");
    CHECK(s.clock.GetStatus().present, "clock should still be present");

    // start again: next tick is position 0
    s.queue.Push(RtEvent::Start);
    s.Block();
    CHECK(!s.clock.GetStatus().running, "start waits for the next tick");
    SendTicks(s, t + 1000, 1, 120.0, 0);
    Settle(s);
    ClockStatus st = s.clock.GetStatus();
    CHECK(st.running && st.bar == 0 && st.beat == 0 && st.eighth == 0,
          "after restart bar %u beat %u eighth %u", st.bar, st.beat, st.eighth);
    CHECK(Count(s, kEvBar) == bars_before + 1, "downbeat event on restart");
}

static void TestTicksBeforeStart()
{
    std::printf("ticks before Start\n");
    Sim s;
    SendTicks(s, 1000, 100, 120.0, 0);
    Settle(s);
    ClockStatus st = s.clock.GetStatus();
    CHECK(st.present && !st.running, "present %d running %d", st.present, st.running);
    CHECK(Count(s, kEvBar) == 0 && Count(s, kEvBeat) == 0, "no beats before Start");
    CHECK(s.clock.Bpm() > 119.f && s.clock.Bpm() < 121.f, "bpm known before Start: %.2f", s.clock.Bpm());
}

static void TestContinueIgnored()
{
    std::printf("Continue is ignored\n");
    Sim s;
    s.queue.Push(RtEvent::Start);
    uint64_t t = SendTicks(s, 1000, 50, 120.0, 0);
    s.queue.Push(RtEvent::Continue);
    SendTicks(s, t + 1000, 46, 120.0, 0); // 50 + 46 = 96 ticks -> tick index 95
    Settle(s);
    CHECK(Count(s, kEvContinueIgnrd) == 1, "continue flagged");
    ClockStatus st = s.clock.GetStatus();
    CHECK(st.running && st.bar == 0 && st.eighth == 7, "bar %u eighth %u (want 0 / 7)", st.bar, st.eighth);
}

static void TestClockLostAndFound()
{
    std::printf("clock lost and found\n");
    Sim s;
    s.queue.Push(RtEvent::Start);
    uint64_t t = SendTicks(s, 1000, 100, 120.0, 0);
    s.RunUntil(t + 24000 + 2 * kBlock); // 0.5 s of silence
    CHECK(Count(s, kEvClockLost) == 1, "lost %u", Count(s, kEvClockLost));
    CHECK(!s.clock.GetStatus().present && !s.clock.GetStatus().running, "present/running after loss");
    CHECK(s.clock.Bpm() == 0.f, "bpm cleared");

    s.RunUntil(s.now + 48000);
    CHECK(Count(s, kEvClockLost) == 1, "lost reported only once");
    SendTicks(s, s.now + 1000, 30, 120.0, 0);
    Settle(s);
    CHECK(Count(s, kEvClockFound) == 2, "found again %u", Count(s, kEvClockFound));
    CHECK(s.clock.GetStatus().present, "present again");
    CHECK(!s.clock.GetStatus().running, "needs a new Start after clock loss");
}

static void TestPosition()
{
    std::printf("position inside the bar\n");
    Sim s;
    s.queue.Push(RtEvent::Start);
    // run 3 bars, sampling position every block; it must rise smoothly and
    // wrap once per bar
    const double period = 60.0 * kSr / (24.0 * 120.0);
    uint64_t     next   = 1000;
    int          sent   = 0;
    float        prev   = 0.f;
    int          wraps  = 0;
    float        spb    = 0.f;
    for(int blk = 0; blk < 3 * 96000 / 48 + 400; blk++)
    {
        while(sent < 3 * 96 && next <= s.now)
        {
            s.queue.Push(RtEvent::Tick);
            sent++;
            next = 1000 + static_cast<uint64_t>(sent * period);
        }
        s.Block();
        if(s.clock.GetStatus().running && s.clock.SamplesPerBar() > 0.f)
        {
            spb         = s.clock.SamplesPerBar();
            const float p = s.clock.PositionInBarSamples();
            CHECK(p >= 0.f && p <= spb + 1.f, "position %.1f out of range", p);
            if(p + spb * 0.5f < prev)
                wraps++;
            else
                CHECK(p + 100.f >= prev, "position went backwards: %.1f -> %.1f", prev, p);
            prev = p;
        }
    }
    CHECK(wraps == 2, "wraps %d (want 2: bars 2 and 3 begin)", wraps);
}

static void TestQueue()
{
    std::printf("event queue\n");
    RtEventQueue q;
    int          pushed = 0;
    for(int i = 0; i < 100; i++)
        if(q.Push(RtEvent::Tick))
            pushed++;
    CHECK(pushed == 63, "pushed %d (want 63)", pushed);
    CHECK(q.Dropped() == 37, "dropped %u", q.Dropped());
    RtEvent e;
    int     popped = 0;
    while(q.Pop(e))
        popped++;
    CHECK(popped == 63, "popped %d", popped);
    CHECK(q.Push(RtEvent::Start) && q.Pop(e) && e == RtEvent::Start, "reuse after empty");
}

static void TestConfig()
{
    std::printf("loop length control\n");
    using namespace looper;
    CHECK(LoopBarsFromControl(0.0f) == 1, "0.0");
    CHECK(LoopBarsFromControl(0.2499f) == 1, "0.2499");
    CHECK(LoopBarsFromControl(0.25f) == 2, "0.25");
    CHECK(LoopBarsFromControl(0.5f) == 4, "0.5");
    CHECK(LoopBarsFromControl(0.75f) == 8, "0.75");
    CHECK(LoopBarsFromControl(1.0f) == 8, "1.0");
    CHECK(LoopBarsFromControl(-1.0f) == 1, "negative");
    CHECK(LoopBarsFromControl(5.0f) == 8, "above 1");

    // memory: 8 bars at 130 BPM at 48 kHz = 14.77 s
    CHECK(kMinBpm == 130.0f, "minimum tempo is 130 BPM");
    const size_t frames = FramesForBars(kMaxLoopBars, kSr);
    CHECK(frames > 708000 && frames < 710000, "frames %zu", frames);
    CHECK(BytesForFrames(frames) < 6u * 1024 * 1024, "loop buffer fits in 6 MB");
    CHECK(BytesForFrames(kLoopCapacityFrames + kHistoryCapacityFrames + kFreezeCapacityFrames) < 10u * 1000 * 1000,
          "all looper buffers together stay under 10 MB");
}

int main()
{
    TestBasicCounting();
    TestJitterAndTempos();
    TestTempoChange();
    TestStopStart();
    TestTicksBeforeStart();
    TestContinueIgnored();
    TestClockLostAndFound();
    TestPosition();
    TestQueue();
    TestConfig();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
