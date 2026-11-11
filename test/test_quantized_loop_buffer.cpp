// Host-side test for src/engine/quantized_loop_buffer.h (with the real ClockTracker).
// Run with `make -C test`. A fake DAW sends MIDI clock; a test signal goes
// in; we check what comes out: timing, level and CLICKS at every transition.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>
#include "../src/engine/clock_tracker.h"
#include "../src/engine/quantized_loop_buffer.h"

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, ...)                               \
    do                                                 \
    {                                                  \
        g_checks++;                                    \
        if(!(cond))                                    \
        {                                              \
            g_fails++;                                 \
            std::printf("  FAIL line %d: ", __LINE__); \
            std::printf(__VA_ARGS__);                  \
            std::printf("\n");                         \
        }                                              \
    } while(0)

static const double kSr    = 48000.0;
static const size_t kBlock = 48;
static const double kPi    = 3.14159265358979323846;
static const uint64_t kBar = 96000; // samples per bar at 120 BPM

// ---------------------------------------------------------------- rig
enum
{
    A_MIDI_START,
    A_MIDI_STOP,
    A_CLOCK_ON,
    A_CLOCK_OFF,
    A_CMD,
    A_KNOB,
    A_BPM,
    A_PPM
};

struct Act
{
    uint64_t t;
    int      kind;
    double   v;
};

struct Rig
{
    ClockTracker       clock;
    RtEventQueue       rtq;
    QuantizedLoopBuffer         eng;
    std::vector<float> bl, br;
    std::function<float(uint64_t)> input;
    std::vector<Act>   acts;
    size_t             act_i = 0;
    std::vector<float> in, out;
    struct Ev
    {
        uint64_t  t;
        LoopEvent e;
    };
    std::vector<Ev> evs;
    uint64_t        now      = 0;
    bool            clock_on = false;
    double          next_tick = 0;
    double          bpm = 120, ppm = 0;
    uint32_t        knob = 1;

    explicit Rig(size_t cap = looper::kLoopCapacityFrames) : bl(cap), br(cap)
    {
        clock.Init(static_cast<float>(kSr));
        eng.Init(bl.data(), br.data(), cap, static_cast<float>(kSr));
    }

    void At(uint64_t t, int kind, double v = 0) { acts.push_back({t, kind, v}); }
    void Cmd(uint64_t t, LoopCmd c) { At(t, A_CMD, static_cast<double>(c)); }

    void Run(uint64_t until)
    {
        std::stable_sort(acts.begin(), acts.end(), [](const Act &a, const Act &b) { return a.t < b.t; });
        while(now < until)
        {
            std::vector<LoopCmd> cmds;
            while(act_i < acts.size() && acts[act_i].t <= now)
            {
                const Act &a = acts[act_i++];
                switch(a.kind)
                {
                    case A_MIDI_START: rtq.Push(RtEvent::Start); break;
                    case A_MIDI_STOP: rtq.Push(RtEvent::Stop); break;
                    case A_CLOCK_ON:
                        clock_on  = true;
                        next_tick = static_cast<double>(now);
                        break;
                    case A_CLOCK_OFF: clock_on = false; break;
                    case A_CMD: cmds.push_back(static_cast<LoopCmd>(static_cast<int>(a.v))); break;
                    case A_KNOB: knob = static_cast<uint32_t>(a.v); break;
                    case A_BPM: bpm = a.v; break;
                    case A_PPM: ppm = a.v; break;
                }
            }
            if(clock_on)
            {
                const double period = 60.0 * kSr / (24.0 * bpm) * (1.0 + ppm * 1e-6);
                while(next_tick <= static_cast<double>(now))
                {
                    rtq.Push(RtEvent::Tick);
                    next_tick += period;
                }
            }
            clock.Advance(rtq, kBlock);
            const uint32_t    ev = clock.TakeEvents();
            const ClockStatus st = clock.GetStatus();
            LoopBlockInfo     info;
            info.clock_present   = st.present;
            info.running         = st.running;
            info.bar_start       = (ev & kEvBar) != 0;
            info.bar             = st.bar;
            info.samples_per_bar = clock.SamplesPerBar();

            eng.SetLoopBars(knob);
            for(LoopCmd c : cmds)
                eng.Command(c, info);

            float il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
            for(size_t i = 0; i < kBlock; i++)
            {
                il[i] = ir[i] = input(now + i);
            }
            eng.Process(info, il, ir, ol, orr, kBlock);
            for(size_t i = 0; i < kBlock; i++)
            {
                in.push_back(il[i]);
                out.push_back(ol[i]);
            }
            LoopEvent e;
            while(eng.PopEvent(e))
                evs.push_back({now, e});
            now += kBlock;
        }
    }

    /** nth event of a type (nullptr if none). */
    const Ev *Find(LoopEventType t, int nth = 0) const
    {
        for(const auto &e : evs)
            if(e.e.type == t && nth-- == 0)
                return &e;
        return nullptr;
    }
    int Count(LoopEventType t) const
    {
        int c = 0;
        for(const auto &e : evs)
            c += e.e.type == t;
        return c;
    }
};

// ---------------------------------------------------------------- signals
static std::function<float(uint64_t)> Sine(double f, double amp = 0.5)
{
    return [=](uint64_t n) { return static_cast<float>(amp * std::sin(2.0 * kPi * f * static_cast<double>(n) / kSr)); };
}
static float Ref(double f, uint64_t n) { return Sine(f)(n); }

/** Low-passed noise (deterministic). Called once per sample, in order. */
static std::function<float(uint64_t)> Noise()
{
    uint32_t seed = 12345;
    float    lp   = 0.0f;
    return [=](uint64_t) mutable {
        seed = seed * 1664525u + 1013904223u;
        const float white = (static_cast<float>(seed >> 8) / 8388608.0f) - 1.0f; // -1..1
        lp += 0.25f * (white - lp);
        return lp * 1.5f;
    };
}

/** Piecewise sine: frequency changes at the given sample times. */
static std::function<float(uint64_t)> Segments(std::vector<std::pair<uint64_t, double>> segs)
{
    return [=](uint64_t n) {
        double f = segs.front().second;
        for(const auto &s : segs)
            if(n >= s.first)
                f = s.second;
        return static_cast<float>(0.5 * std::sin(2.0 * kPi * f * static_cast<double>(n) / kSr));
    };
}

// ---------------------------------------------------------------- measures
static double MaxStep(const std::vector<float> &y, size_t from, size_t to)
{
    double m = 0;
    for(size_t i = from + 1; i < to && i < y.size(); i++)
        m = std::max(m, std::fabs(static_cast<double>(y[i]) - y[i - 1]));
    return m;
}
static double MaxDiff(const std::vector<float> &a, const std::function<float(uint64_t)> &ref,
                      size_t from, size_t to)
{
    double m = 0;
    for(size_t i = from; i < to && i < a.size(); i++)
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - ref(i)));
    return m;
}
static double MaxDiffVec(const std::vector<float> &a, const std::vector<float> &b, size_t from, size_t to)
{
    double m = 0;
    for(size_t i = from; i < to && i < a.size() && i < b.size(); i++)
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
    return m;
}
/** Lowest and highest ratio of rms(out)/rms(in) over short windows. */
static void RmsRatio(const std::vector<float> &o, const std::vector<float> &x, size_t from,
                     size_t to, size_t win, double &lo, double &hi)
{
    lo = 1e9;
    hi = 0;
    for(size_t s = from; s + win < to && s + win < o.size(); s += 48)
    {
        double a = 0, b = 0;
        for(size_t i = s; i < s + win; i++)
        {
            a += static_cast<double>(o[i]) * o[i];
            b += static_cast<double>(x[i]) * x[i];
        }
        if(b < 1e-9)
            continue;
        const double r = std::sqrt(a / b);
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
}
/** Largest sample-to-sample step a clean sine of this frequency could make. */
static double SineStep(double f, double amp = 0.5) { return 2.0 * kPi * f / kSr * amp; }

static bool Near(uint64_t a, uint64_t b, uint64_t tol) { return a > b ? a - b <= tol : b - a <= tol; }

// ---------------------------------------------------------------- tests
static void TestReader()
{
    std::printf("LoopReader\n");
    const size_t       N = 4000;
    std::vector<float> l(N), r(N);
    for(size_t i = 0; i < N; i++)
        l[i] = r[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 300.0 * i / kSr));
    LoopReader rd;
    rd.Init(240);
    std::vector<float> o;
    float              a, b;
    for(int i = 0; i < 1000; i++)
    {
        rd.Read(l.data(), r.data(), N, a, b);
        o.push_back(a);
    }
    rd.JumpTo(2500); // different phase: must fade, not click
    for(int i = 0; i < 1000; i++)
    {
        rd.Read(l.data(), r.data(), N, a, b);
        o.push_back(a);
    }
    CHECK(MaxStep(o, 0, o.size()) < 1.5 * SineStep(300), "jump click: step %.4f vs %.4f",
          MaxStep(o, 0, o.size()), SineStep(300));
    // run off the end: it must wrap by itself without reading outside the buffer
    LoopReader r2;
    r2.Init(240);
    std::vector<float> o2;
    for(int i = 0; i < 20000; i++)
    {
        r2.Read(l.data(), r.data(), 1000, a, b); // only 1000 frames valid
        o2.push_back(a);
    }
    CHECK(r2.Position() < 1000 + 240 + 2, "reader stays in range (%zu)", r2.Position());
    // valid == 0 is safe
    r2.Read(l.data(), r.data(), 0, a, b);
    CHECK(a == 0.f && b == 0.f, "empty buffer reads silence");
}

static void TestPeriodicExact()
{
    std::printf("record + playback, exactly periodic input (output must equal input)\n");
    Rig rig;
    rig.input = Sine(440);
    rig.At(0, A_CLOCK_ON);
    rig.At(0, A_MIDI_START);
    rig.Cmd(24000, LoopCmd::ArmToggle);
    rig.Run(8 * kBar);

    const auto *armed = rig.Find(LoopEventType::Armed);
    const auto *rec   = rig.Find(LoopEventType::RecordStart);
    const auto *play  = rig.Find(LoopEventType::PlaybackStart);
    CHECK(armed && armed->e.a == 1 && armed->e.b == 2, "armed event (bars %u, start bar %u)",
          armed ? armed->e.a : 0, armed ? armed->e.b : 0);
    CHECK(rec && Near(rec->t, kBar, kBlock), "record starts at bar 2 (t=%llu)", rec ? (unsigned long long)rec->t : 0);
    CHECK(play && Near(play->t, 2 * kBar, kBlock), "playback starts at bar 3 (t=%llu)", play ? (unsigned long long)play->t : 0);
    CHECK(play && Near(play->e.b, kBar, kBlock), "loop length %u", play ? play->e.b : 0);
    CHECK(rig.Count(LoopEventType::Refused) == 0, "no refusals");
    CHECK(rig.eng.State() == LoopState::Playing, "state is playing");
    CHECK(MaxDiff(rig.out, Sine(440), 0, rig.out.size()) < 1e-5,
          "out must equal in everywhere: %.6f", MaxDiff(rig.out, Sine(440), 0, rig.out.size()));
}

static void TestSeamClicks()
{
    std::printf("loop seam with a phase mismatch (worst case for clicks and level)\n");
    for(double f : {437.125, 437.0 + 1.0 / 3.0})
    {
        Rig rig;
        rig.input = Sine(f);
        rig.At(0, A_CLOCK_ON);
        rig.At(0, A_MIDI_START);
        rig.Cmd(24000, LoopCmd::ArmToggle);
        rig.Run(8 * kBar);
        const double bound = SineStep(f);
        const double step  = MaxStep(rig.out, 0, rig.out.size());
        CHECK(step <= 1.6 * bound, "f=%.3f max step %.4f vs allowed %.4f", f, step, 1.6 * bound);
        double lo, hi;
        RmsRatio(rig.out, rig.in, 2 * kBar + 2000, rig.out.size(), 240, lo, hi);
        CHECK(lo > 0.6 && hi < 1.15, "f=%.3f level ratio %.2f..%.2f", f, lo, hi);
    }
}

static void TestFourBars()
{
    std::printf("4-bar loop starts on the 4-bar grid\n");
    Rig rig;
    rig.input = Sine(440);
    rig.At(0, A_CLOCK_ON);
    rig.At(0, A_MIDI_START);
    rig.At(0, A_KNOB, 4);
    rig.Cmd(2 * kBar + 1000, LoopCmd::ArmToggle); // during bar 3
    rig.Run(10 * kBar);
    const auto *armed = rig.Find(LoopEventType::Armed);
    const auto *rec   = rig.Find(LoopEventType::RecordStart);
    const auto *play  = rig.Find(LoopEventType::PlaybackStart);
    CHECK(armed && armed->e.a == 4 && armed->e.b == 5, "armed: bars %u start bar %u (want 4 / 5)",
          armed ? armed->e.a : 0, armed ? armed->e.b : 0);
    CHECK(rec && Near(rec->t, 4 * kBar, kBlock), "record at bar 5");
    CHECK(play && Near(play->t, 8 * kBar, kBlock), "playback at bar 9");
    CHECK(play && Near(play->e.b, 4 * kBar, kBlock), "length 4 bars");
    CHECK(MaxDiff(rig.out, Sine(440), 0, rig.out.size()) < 1e-5, "periodic: out == in");
}

static void TestArmBeforeStart()
{
    std::printf("arm while the transport is stopped, then press Play\n");
    Rig rig;
    rig.input = Sine(440);
    rig.At(0, A_CLOCK_ON);
    rig.Cmd(30000, LoopCmd::ArmToggle);
    rig.At(2 * kBar, A_MIDI_START);
    rig.Run(5 * kBar);
    const auto *armed = rig.Find(LoopEventType::Armed);
    const auto *rec   = rig.Find(LoopEventType::RecordStart);
    const auto *play  = rig.Find(LoopEventType::PlaybackStart);
    CHECK(armed && armed->e.b == 0, "armed 'at Start'");
    CHECK(rec && Near(rec->t, 2 * kBar, kBlock), "recording starts with the transport (t=%llu)", rec ? (unsigned long long)rec->t : 0);
    CHECK(play && Near(play->t, 3 * kBar, kBlock), "playback one bar later");
}

static void TestSourceClearRearm()
{
    std::printf("B3 switch, clear, arm again from idle\n");
    Rig rig;
    // 440 Hz is recorded. The live input then changes to 300 Hz (hidden while the loop is heard), then 220 Hz.
    rig.input = Segments({{0, 440}, {194000, 300}, {674000, 220}});
    rig.At(0, A_CLOCK_ON);
    rig.At(0, A_MIDI_START);
    rig.Cmd(24000, LoopCmd::ArmToggle);        // loop of 440 plays from 192000
    rig.Cmd(250000, LoopCmd::ToggleSource);    // -> live (300)
    rig.Cmd(300000, LoopCmd::ToggleSource);    // -> loop (440)
    rig.Cmd(400000, LoopCmd::Clear);           // -> live (300), idle
    rig.Cmd(500000, LoopCmd::ArmToggle);       // record 300 from bar 7, plays from bar 8
    rig.Run(850000);

    CHECK(MaxDiff(rig.out, Sine(440), 193000, 249000) < 1e-5, "loop (440) heard before the first switch");
    CHECK(MaxDiff(rig.out, Segments({{0, 300}}), 250500, 299000) < 1e-5, "live (300) heard after B3");
    CHECK(rig.Count(LoopEventType::SourceInput) == 1 && rig.Count(LoopEventType::SourceBuffer) == 1, "source events");
    CHECK(MaxDiff(rig.out, Sine(440), 300500, 399000) < 1e-5, "loop back after B3, still in time");
    CHECK(MaxDiff(rig.out, Segments({{0, 300}}), 400500, 499000) < 1e-5, "live after clear");
    CHECK(rig.Count(LoopEventType::Cleared) == 1, "cleared event");
    const auto *rec2  = rig.Find(LoopEventType::RecordStart, 1);
    const auto *play2 = rig.Find(LoopEventType::PlaybackStart, 1);
    CHECK(rec2 && Near(rec2->t, 6 * kBar, kBlock), "second recording at bar 7");
    CHECK(play2 && Near(play2->t, 7 * kBar, kBlock), "second playback at bar 8");
    CHECK(MaxDiff(rig.out, Sine(300), 7 * kBar + 500, 850000) < 1e-5, "new loop (300) heard while live is 220");
    const double step = MaxStep(rig.out, 0, rig.out.size());
    CHECK(step <= 1.6 * SineStep(440), "no clicks anywhere: step %.4f vs %.4f", step, 1.6 * SineStep(440));
}

static void TestRerecord()
{
    std::printf("re-record while a loop is playing\n");
    Rig rig;
    rig.input = Segments({{0, 440}, {194000, 300}, {386000, 220}});
    rig.At(0, A_CLOCK_ON);
    rig.At(0, A_MIDI_START);
    rig.Cmd(24000, LoopCmd::ArmToggle);
    rig.Cmd(250000, LoopCmd::ArmToggle); // re-arm: new take starts at bar 4 (t=288000)
    rig.Run(600000);

    CHECK(rig.Count(LoopEventType::ReArmed) == 1, "re-armed event");
    CHECK(MaxDiff(rig.out, Sine(440), 193000, 287000) < 1e-5, "old loop keeps playing until the new cycle");
    const auto *rec2 = rig.Find(LoopEventType::RecordStart, 1);
    CHECK(rec2 && Near(rec2->t, 3 * kBar, kBlock), "re-record starts at bar 4");
    CHECK(MaxDiff(rig.out, Segments({{0, 300}}), 288500, 383000) < 1e-5, "live signal heard while re-recording");
    CHECK(MaxDiff(rig.out, Sine(300), 384500, 560000) < 1e-5, "new loop (300) plays after the take");
    const double step = MaxStep(rig.out, 0, rig.out.size());
    CHECK(step <= 1.6 * SineStep(440), "no clicks: step %.4f vs %.4f", step, 1.6 * SineStep(440));

    // arm then cancel: nothing changes
    Rig r2;
    r2.input = Sine(440);
    r2.At(0, A_CLOCK_ON);
    r2.At(0, A_MIDI_START);
    r2.Cmd(24000, LoopCmd::ArmToggle);
    r2.Cmd(60000, LoopCmd::ArmToggle);
    r2.Run(3 * kBar);
    CHECK(r2.Count(LoopEventType::ArmCancelled) == 1 && r2.Count(LoopEventType::RecordStart) == 0, "arm cancelled");
    CHECK(r2.eng.State() == LoopState::Idle, "idle after cancel");
}

static void TestTransport()
{
    std::printf("transport stop during recording and during playback\n");
    {
        Rig rig;
        rig.input = Sine(437.125);
        rig.At(0, A_CLOCK_ON);
        rig.At(0, A_MIDI_START);
        rig.Cmd(24000, LoopCmd::ArmToggle);
        rig.At(150000, A_MIDI_STOP);
        rig.Run(2 * kBar);
        const auto *ab = rig.Find(LoopEventType::RecordAborted);
        CHECK(ab && ab->e.a == kAbortTransportStopped, "recording aborted by Stop");
        CHECK(rig.eng.State() == LoopState::Idle, "idle after abort");
        CHECK(MaxDiffVec(rig.out, rig.in, 0, rig.out.size()) < 1e-6, "output is always the live signal");
    }
    {
        Rig rig;
        rig.input = Segments({{0, 440}, {194000, 300}});
        rig.At(0, A_CLOCK_ON);
        rig.At(0, A_MIDI_START);
        rig.Cmd(24000, LoopCmd::ArmToggle);
        rig.At(250000, A_MIDI_STOP);
        rig.At(330000, A_MIDI_START);
        rig.Run(520000);
        CHECK(rig.Count(LoopEventType::Held) == 1, "held event");
        CHECK(MaxDiff(rig.out, Segments({{0, 300}}), 251000, 329000) < 1e-5, "live signal while stopped");
        const uint64_t ts = 330000; // first tick after Start = bar 1
        double         m  = 0;
        for(uint64_t n = ts + 500; n < ts + 90000; n++)
            m = std::max(m, std::fabs(static_cast<double>(rig.out[n]) - Ref(440, n - ts)));
        CHECK(m < 1e-5, "loop restarts from its beginning at bar 1 after Start: err %.6f", m);
        const double step = MaxStep(rig.out, 0, rig.out.size());
        CHECK(step <= 1.6 * SineStep(440), "no clicks: %.4f vs %.4f", step, 1.6 * SineStep(440));
    }
}

static void TestDrift()
{
    std::printf("DAW clock running fast / slow against the audio clock\n");
    for(double ppm : {100.0, -100.0, 300.0})
    {
        for(int kind = 0; kind < 2; kind++)
        {
            // Both signals: no clicks. (Level is not checked here: with drift the seam phase
            // is arbitrary, and mixing two uncorrelated signals dips up to 3 dB for 5 ms.)
            Rig rig;
            rig.input = kind == 0 ? Sine(437.125) : Noise();
            rig.At(0, A_PPM, ppm);
            rig.At(0, A_CLOCK_ON);
            rig.At(0, A_MIDI_START);
            rig.At(0, A_KNOB, 2);
            rig.Cmd(24000, LoopCmd::ArmToggle);
            rig.Run(14 * kBar);
            const double in_step = MaxStep(rig.in, 0, rig.in.size());
            const double step    = MaxStep(rig.out, 0, rig.out.size());
            CHECK(step <= 1.25 * in_step, "ppm %.0f %s clicks: %.4f vs input %.4f", ppm,
                  kind ? "noise" : "sine", step, in_step);
            CHECK(rig.eng.State() == LoopState::Playing && rig.Count(LoopEventType::PlaybackStart) == 1,
                  "still playing");
        }
    }
}

static void TestRefusals()
{
    std::printf("refusals and limits\n");
    {   // no clock
        Rig rig;
        rig.input = Sine(440);
        rig.Cmd(1000, LoopCmd::ArmToggle);
        rig.Cmd(2000, LoopCmd::ToggleSource);
        rig.Run(10000);
        const auto *r0 = rig.Find(LoopEventType::Refused, 0);
        const auto *r1 = rig.Find(LoopEventType::Refused, 1);
        CHECK(r0 && r0->e.a == kRefuseNoClock, "B2 with no clock refused");
        CHECK(r1 && r1->e.a == kRefuseNoLoop, "B3 with no loop refused");
        CHECK(rig.eng.State() == LoopState::Idle, "still idle");
    }
    {   // B2 while recording
        Rig rig;
        rig.input = Sine(440);
        rig.At(0, A_CLOCK_ON);
        rig.At(0, A_MIDI_START);
        rig.Cmd(24000, LoopCmd::ArmToggle);
        rig.Cmd(120000, LoopCmd::ArmToggle);
        rig.Run(3 * kBar);
        const auto *r = rig.Find(LoopEventType::Refused);
        CHECK(r && r->e.a == kRefuseRecording, "B2 while recording refused");
        CHECK(rig.Count(LoopEventType::PlaybackStart) == 1, "recording was not disturbed");
    }
    {   // tempo too slow for 8 bars (100 BPM: the buffers are sized for 130), but 150 BPM fits
        Rig rig;
        rig.input = Sine(440);
        rig.At(0, A_BPM, 100);
        rig.At(0, A_CLOCK_ON);
        rig.At(0, A_KNOB, 8);
        rig.Cmd(150000, LoopCmd::ArmToggle);
        rig.Run(160000);
        const auto *r = rig.Find(LoopEventType::Refused);
        CHECK(r && r->e.a == kRefuseTooSlow, "8 bars at 100 BPM refused");
        Rig ok;
        ok.input = Sine(440);
        ok.At(0, A_BPM, 150);
        ok.At(0, A_CLOCK_ON);
        ok.At(0, A_KNOB, 8);
        ok.Cmd(150000, LoopCmd::ArmToggle);
        ok.Run(160000);
        CHECK(ok.Find(LoopEventType::Armed) != nullptr, "8 bars at 150 BPM accepted");
    }
    {   // buffer fills up: recording is aborted cleanly
        Rig rig(60000);
        rig.input = Sine(437.125);
        rig.At(0, A_CLOCK_ON);
        rig.Cmd(5000, LoopCmd::ArmToggle); // tempo not measured yet, so not refused
        rig.At(10000, A_MIDI_START);
        rig.Run(3 * kBar);
        const auto *ab = rig.Find(LoopEventType::RecordAborted);
        CHECK(ab && ab->e.a == kAbortTooLong, "overflow aborts recording");
        CHECK(rig.eng.State() == LoopState::Idle, "idle after overflow");
        CHECK(MaxDiffVec(rig.out, rig.in, 0, rig.out.size()) < 1e-6, "output stayed the live signal");
    }
}

static void TestKnobDuringPlayback()
{
    std::printf("knob 2 does not change a playing loop\n");
    Rig rig;
    rig.input = Sine(440);
    rig.At(0, A_CLOCK_ON);
    rig.At(0, A_MIDI_START);
    rig.Cmd(24000, LoopCmd::ArmToggle);
    rig.At(250000, A_KNOB, 8);
    rig.Run(6 * kBar);
    CHECK(rig.eng.GetStatus().bars == 1, "status bars %u (want 1)", rig.eng.GetStatus().bars);
    CHECK(MaxDiff(rig.out, Sine(440), 0, rig.out.size()) < 1e-5, "loop unaffected");
    CHECK(rig.Count(LoopEventType::PlaybackStart) == 1, "no extra recordings");
}

int main()
{
    TestReader();
    TestPeriodicExact();
    TestSeamClicks();
    TestFourBars();
    TestArmBeforeStart();
    TestSourceClearRearm();
    TestRerecord();
    TestTransport();
    TestDrift();
    TestRefusals();
    TestKnobDuringPlayback();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
