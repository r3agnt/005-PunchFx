// Host-side test for src/engine/slice_loop_buffer.h (slice mode, B7), with the real
// ClockTracker and, for the "slices recorded into the loop" test, the real
// QuantizedLoopBuffer. Run with `make -C test`.
//
// Names: QUANTIZED LOOP BUFFER = the global looper (B1/B2).
//        SLICE LOOP BUFFER     = the frozen bar of slice mode (B7).
//
// Like test_beat.cpp the clock runs at 138.89 BPM, where a tick is exactly 864
// samples, so every position is a whole number of samples:
//   one bar = 82944, one eighth (E) = 10368, the frozen window (N) = 8 E = 1 bar.
// The input is hash noise, so any misalignment shows up as a large error.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>
#include "../src/engine/clock_tracker.h"
#include "../src/engine/quantized_loop_buffer.h"
#include "../src/engine/slice_loop_buffer.h"

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

static const double  kSr    = 48000.0;
static const size_t  kBlock = 48;
static const double  kPi    = 3.14159265358979323846;
static const int64_t kBarS  = 82944;
static const int64_t kE     = kBarS / 8; // one eighth = one slice = one 1/8 line
static const int64_t kN     = 8 * kE;    // the frozen window (1 bar, 0.5.2)
static const int64_t kX     = 240;       // crossfade (5 ms)

enum
{
    A_MIDI_START,
    A_MIDI_STOP,
    A_CLOCK_ON,
    A_HOLD,     // v = button index * 2 + (1 = press, 0 = release)
    A_CMD,      // v = SliceCmd
    A_OFFSET,   // v = offset in samples
    A_LOOP_CMD, // v = LoopCmd
    A_BPM,
    A_SEMIS, // pitch knob (whole semitones)
    A_REV    // reverse switch (1 = ping-pong, 0 = forward only)
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
    SliceLoopBuffer    sl;
    QuantizedLoopBuffer ql;
    std::vector<float> sbl, sbr, ll, lr;
    bool               use_loop;
    std::function<float(uint64_t)> input;
    std::vector<Act>   acts;
    size_t             act_i = 0;
    std::vector<float> in, out, perf; // perf = what the Quantized loop buffer is given
    struct Ev
    {
        uint64_t   t;
        SliceEvent e;
    };
    struct LEv
    {
        uint64_t  t;
        LoopEvent e;
    };
    std::vector<Ev>  evs;
    std::vector<LEv> levs;
    uint64_t         now       = 0;
    bool             clock_on  = false;
    double           next_tick = 0;
    double           bpm       = 120000.0 / 864.0;
    uint32_t         held      = 0;
    float            offset    = 0;
    int              semis     = looper::kSliceFxSemitones; // pitch knob value
    bool             reverse   = true;                      // the engine default in tests: ping-pong (new tests switch it)

    explicit Rig(bool with_loop = false)
        : sbl(looper::kHistoryCapacityFrames), sbr(looper::kHistoryCapacityFrames), use_loop(with_loop)
    {
        clock.Init(static_cast<float>(kSr));
        sl.Init(sbl.data(), sbr.data(), sbl.size(), static_cast<float>(kSr));
        if(with_loop)
        {
            ll.resize(looper::kLoopCapacityFrames);
            lr.resize(looper::kLoopCapacityFrames);
            ql.Init(ll.data(), lr.data(), ll.size(), static_cast<float>(kSr));
        }
    }
    void At(uint64_t t, int kind, double v = 0) { acts.push_back({t, kind, v}); }
    void Press(uint64_t t, int b) { At(t, A_HOLD, b * 2 + 1); }
    void Release(uint64_t t, int b) { At(t, A_HOLD, b * 2); }
    void Mode(uint64_t t) { At(t, A_CMD, static_cast<double>(SliceCmd::ToggleMode)); }
    void Bank(uint64_t t) { At(t, A_CMD, static_cast<double>(SliceCmd::ToggleBank)); }

    void Run(uint64_t until)
    {
        std::stable_sort(acts.begin(), acts.end(), [](const Act &a, const Act &b) { return a.t < b.t; });
        while(now < until)
        {
            std::vector<LoopCmd>  lcmds;
            std::vector<SliceCmd> scmds;
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
                    case A_HOLD:
                    {
                        const int idx = static_cast<int>(a.v) / 2;
                        if(static_cast<int>(a.v) % 2)
                            held |= 1u << idx;
                        else
                            held &= ~(1u << idx);
                        break;
                    }
                    case A_CMD: scmds.push_back(static_cast<SliceCmd>(static_cast<int>(a.v))); break;
                    case A_OFFSET: offset = static_cast<float>(a.v); break;
                    case A_LOOP_CMD: lcmds.push_back(static_cast<LoopCmd>(static_cast<int>(a.v))); break;
                    case A_BPM: bpm = a.v; break;
                    case A_SEMIS: semis = static_cast<int>(a.v); break;
                    case A_REV: reverse = a.v != 0; break;
                }
            }
            if(clock_on)
            {
                const double period = std::round(60.0 * kSr / (24.0 * bpm) * 1000.0) / 1000.0;
                while(next_tick <= static_cast<double>(now))
                {
                    rtq.Push(RtEvent::Tick);
                    next_tick += period;
                }
            }
            clock.Advance(rtq, kBlock);
            const uint32_t    cev = clock.TakeEvents();
            const ClockStatus st  = clock.GetStatus();

            SliceBlockInfo si;
            si.running         = st.running;
            si.bar             = clock.BarAtBlockStart();
            si.pos_in_bar      = clock.PositionAtBlockStart(kBlock);
            si.samples_per_bar = clock.SamplesPerBar();
            si.held_mask       = held;
            si.offset_samples  = offset;
            si.pitch_held      = (held >> 8) & 1u; // test button index 8 = B6
            si.fx_semitones    = semis;
            si.fx_reverse      = reverse;
            const bool can     = si.running && si.samples_per_bar > 0.0f;
            for(SliceCmd c : scmds)
                sl.Command(c, can);

            float il[kBlock], ir[kBlock], sll[kBlock], slr[kBlock], bw[kBlock];
            float pl[kBlock], pr[kBlock], ol[kBlock], orr[kBlock];
            for(size_t i = 0; i < kBlock; i++)
                il[i] = ir[i] = input(now + i);
            sl.Process(si, il, ir, sll, slr, bw, kBlock);
            // (the same mixing as main.cpp)
            for(size_t i = 0; i < kBlock; i++)
            {
                pl[i] = bw[i] * il[i] + sll[i];
                pr[i] = bw[i] * ir[i] + slr[i];
            }
            if(use_loop)
            {
                LoopBlockInfo li;
                li.clock_present   = st.present;
                li.running         = st.running;
                li.bar_start       = (cev & kEvBar) != 0;
                li.bar             = st.bar;
                li.samples_per_bar = clock.SamplesPerBar();
                ql.SetLoopBars(1);
                for(LoopCmd c : lcmds)
                    ql.Command(c, li);
                float ql_l[kBlock], ql_r[kBlock];
                ql.Process(li, pl, pr, ql_l, ql_r, kBlock);
                for(size_t i = 0; i < kBlock; i++)
                {
                    ol[i]  = bw[i] * ql_l[i] + sll[i];
                    orr[i] = bw[i] * ql_r[i] + slr[i];
                }
                LoopEvent le;
                while(ql.PopEvent(le))
                    levs.push_back({now, le});
            }
            else
                for(size_t i = 0; i < kBlock; i++)
                {
                    ol[i]  = pl[i];
                    orr[i] = pr[i];
                }
            for(size_t i = 0; i < kBlock; i++)
            {
                in.push_back(il[i]);
                perf.push_back(pl[i]);
                out.push_back(ol[i]);
            }
            SliceEvent e;
            while(sl.PopEvent(e))
                evs.push_back({now, e});
            now += kBlock;
        }
    }
    int Count(SliceEventType t) const
    {
        int c = 0;
        for(const auto &e : evs)
            c += e.e.type == t;
        return c;
    }
};

// ---------------------------------------------------------------- signals / measures
static float HashNoise(uint64_t n)
{
    uint32_t x = static_cast<uint32_t>(n * 2654435761u) ^ static_cast<uint32_t>(n >> 32);
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    x ^= x >> 16;
    return static_cast<float>(x) / 4294967296.0f - 0.5f;
}
static std::function<float(uint64_t)> Noise() { return HashNoise; }
static std::function<float(uint64_t)> Sine(double f)
{
    return [=](uint64_t n) { return static_cast<float>(0.5 * std::sin(2.0 * kPi * f * static_cast<double>(n) / kSr)); };
}
static double SineStep(double f) { return 2.0 * kPi * f / kSr * 0.5; }

static double MaxStep(const std::vector<float> &y)
{
    double m = 0;
    for(size_t i = 1; i < y.size(); i++)
        m = std::max(m, std::fabs(static_cast<double>(y[i]) - y[i - 1]));
    return m;
}
static double MaxDiffShift(const std::vector<float> &o, int64_t o0, const std::vector<float> &x, int64_t x0, int64_t len)
{
    double m = 0;
    for(int64_t k = 0; k < len; k++)
    {
        const int64_t a = o0 + k, b = x0 + k;
        if(a < 0 || b < 0 || a >= static_cast<int64_t>(o.size()) || b >= static_cast<int64_t>(x.size()))
            return 1e9;
        m = std::max(m, std::fabs(static_cast<double>(o[a]) - x[b]));
    }
    return m;
}

/** The 1/8 line that a command or press at time `t` is quantised to (where the
 *  crossfade ENDS; it starts X earlier, never before the action itself). */
static int64_t Line(uint64_t t, int64_t offset = 0);
/** The BAR line that a B7 press at time `t` waits for (0.5.2: B7 ON only switches on a bar line). */
static int64_t BarLine(uint64_t t, int64_t offset = 0)
{
    const int64_t tp = ((static_cast<int64_t>(t) + static_cast<int64_t>(kBlock) - 1) / static_cast<int64_t>(kBlock)) * static_cast<int64_t>(kBlock);
    const int64_t k  = (tp + kX - offset + kBarS - 1) / kBarS;
    return k * kBarS + offset;
}
static int64_t Line(uint64_t t, int64_t offset)
{
    const int64_t tp = ((static_cast<int64_t>(t) + static_cast<int64_t>(kBlock) - 1) / static_cast<int64_t>(kBlock)) * static_cast<int64_t>(kBlock);
    const int64_t k  = (tp + kX - offset + kE - 1) / kE;
    return k * kE + offset;
}

/** Does out[t] equal the frozen window played as a loop (window starts at W =
 *  T - N, loop started on line T) for t in [t0, t1)? Skips the last X of every
 *  wrap, where the loop point is crossfaded. */
static bool PlaysLoop(const Rig &r, int64_t T, int64_t t0, int64_t t1, double tol = 1e-5)
{
    for(int64_t t = t0; t < t1; t++)
    {
        int64_t u = (t - T) % kN;
        if(u < 0)
            u += kN;
        if(u >= kN - kX)
            continue;
        if(std::fabs(static_cast<double>(r.out[t]) - r.in[T - kN + u]) > tol)
        {
            std::printf("    loop: sample %lld differs (u=%lld)\n", (long long)t, (long long)u);
            return false;
        }
    }
    return true;
}

/** Does out[t] play the whole window as a loop that STARTS at slice `slice` on
 *  line R (the 0.5.1 release behaviour)? Skips the last X before each window wrap. */
static bool PlaysRotated(const Rig &r, int64_t T, int slice, int64_t R, int64_t t0, int64_t t1, double tol = 1e-5)
{
    for(int64_t t = t0; t < t1; t++)
    {
        int64_t p = (slice * kE + (t - R)) % kN;
        if(p < 0)
            p += kN;
        if(p >= kN - kX)
            continue;
        if(std::fabs(static_cast<double>(r.out[t]) - r.in[T - kN + p]) > tol)
        {
            std::printf("    rotated: sample %lld differs (p=%lld)\n", (long long)t, (long long)p);
            return false;
        }
    }
    return true;
}

/** Does out[t] repeat slice `slice` (0..15 of the window), started on line L0,
 *  for t in [t0, t1)? Skips the last X of every repeat (the loop crossfade). */
static bool PlaysSlice(const Rig &r, int64_t T, int slice, int64_t L0, int64_t t0, int64_t t1, double tol = 1e-5)
{
    const int64_t S = T - kN + slice * kE;
    for(int64_t t = t0; t < t1; t++)
    {
        int64_t u = (t - L0) % kE;
        if(u < 0)
            u += kE;
        if(u >= kE - kX)
            continue;
        if(std::fabs(static_cast<double>(r.out[t]) - r.in[S + u]) > tol)
        {
            std::printf("    slice %d: sample %lld differs (u=%lld)\n", slice, (long long)t, (long long)u);
            return false;
        }
    }
    return true;
}

/** Does out[t] play slice `slice` (0..7) as the B6 ping-pong from line L0: pitched up 3 semitones, forward
 *  through the first 16th (read faster), then backwards through the same piece, every eighth? The position
 *  in the slice is tri(phase) * 2^(3/12) with tri = the triangle of the phase inside the eighth. */
static bool PlaysPingPong(const Rig &r, int64_t T, int slice, int64_t L0, int64_t t0, int64_t t1, int semis = looper::kSliceFxSemitones, double tol = 1e-4)
{
    const int64_t S    = T - kN + slice * kE;
    const double  rate = std::pow(2.0, semis / 12.0);
    for(int64_t t = t0; t < t1; t++)
    {
        int64_t phi = (t - L0) % kE;
        if(phi < 0)
            phi += kE;
        const int64_t tri = phi <= kE / 2 ? phi : kE - phi;
        const double  x   = static_cast<double>(tri) * rate;
        const int64_t i0  = static_cast<int64_t>(std::floor(x));
        const double  e   = r.in[S + i0] + (r.in[S + i0 + 1] - r.in[S + i0]) * (x - static_cast<double>(i0));
        if(std::fabs(static_cast<double>(r.out[t]) - e) > tol)
        {
            std::printf("    ping-pong slice %d: sample %lld differs (phase %lld)\n", slice, (long long)t, (long long)phi);
            return false;
        }
    }
    return true;
}

/** Does out[t] play slice `slice` as the B6 FORWARD-only pitched repeat from line L0: position = phase * 2^(semis/12)
 *  inside every eighth, restarted on every 1/8 line? Skips the last X of each eighth (the crossfade at the repeat). */
static bool PlaysForward(const Rig &r, int64_t T, int slice, int64_t L0, int64_t t0, int64_t t1, int semis = looper::kSliceFxSemitones, double tol = 1e-4)
{
    const int64_t S    = T - kN + slice * kE;
    const double  rate = std::pow(2.0, semis / 12.0);
    for(int64_t t = t0; t < t1; t++)
    {
        int64_t phi = (t - L0) % kE;
        if(phi < 0)
            phi += kE;
        if(phi >= kE - kX)
            continue;
        const double  x  = static_cast<double>(phi) * rate;
        const int64_t i0 = static_cast<int64_t>(std::floor(x));
        const double  e  = r.in[S + i0] + (r.in[S + i0 + 1] - r.in[S + i0]) * (x - static_cast<double>(i0));
        if(std::fabs(static_cast<double>(r.out[t]) - e) > tol)
        {
            std::printf("    forward slice %d: sample %lld differs (phase %lld)\n", slice, (long long)t, (long long)phi);
            return false;
        }
    }
    return true;
}

static void Start(Rig &r)
{
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
}

// ---------------------------------------------------------------- tests
static void TestEnterLoopExit()
{
    std::printf("B7: at the next BAR line the frozen bar (beat 1 = B9) plays as a loop; B7 again returns to the normal signal\n");
    for(int variant = 0; variant < 4; variant++)
    {
        Rig r;
        r.input = Noise();
        Start(r);
        const uint64_t tp = 3 * kBarS + 12345 + variant * 2500;
        const int64_t  T  = BarLine(tp);
        r.Mode(tp);
        const uint64_t tx = T + 2 * kN + 777 + variant * 1111;
        const int64_t  TX = Line(tx);
        r.Mode(tx);
        r.Run(TX + 3 * kBarS);

        CHECK(T % kBarS == 0 && T >= static_cast<int64_t>(tp) + kX, "v%d: bar line %lld", variant, (long long)T);
        CHECK(MaxDiffShift(r.out, 0, r.in, 0, T - kX - 1) < 1e-6, "v%d: normal signal until the line", variant);
        CHECK(PlaysLoop(r, T, T, TX - kX), "v%d: the frozen window loops in time (several passes)", variant);
        // the loop starts on beat 1: the first sample after the line is the first sample of the bar that just ended
        CHECK(std::fabs(static_cast<double>(r.out[T + 5]) - r.in[T - kBarS + 5]) < 1e-6, "v%d: loop starts on beat 1", variant);
        // the loop is audio from exactly 2 bars before (in time with the beat)
        CHECK(MaxDiffShift(r.out, T + 100, r.in, T + 100 - kN, kN - kX - 200) < 1e-5, "v%d: first pass = the input 1 bar earlier", variant);
        CHECK(MaxDiffShift(r.out, TX + 2, r.in, TX + 2, 2 * kBarS) < 1e-6, "v%d: normal signal again after the exit line", variant);
        CHECK(r.Count(SliceEventType::ModeOn) == 1 && r.Count(SliceEventType::ModeOff) == 1, "v%d: events", variant);
        bool on_ok = false;
        for(const auto &e : r.evs)
            if(e.e.type == SliceEventType::ModeOn)
            {
                const int64_t dt = (T - kX) - static_cast<int64_t>(e.t);
                on_ok            = dt >= 0 && dt < static_cast<int64_t>(kBlock);
            }
        CHECK(on_ok, "v%d: ModeOn is logged when the crossfade starts", variant);
    }
}

static void TestSlices()
{
    std::printf("slice buttons: repeat that eighth on the next 1/8 line, release: the bar loops from that slice\n");
    for(int variant = 0; variant < 4; variant++)
    {
        Rig r;
        r.input = Noise();
        Start(r);
        const uint64_t tm = 3 * kBarS + 1000;
        const int64_t  T  = BarLine(tm);
        r.Mode(tm);
        const int      b  = 2 + variant;                 // button index 2..5 = slice 2..5 of bank 1
        const uint64_t tp = T + 3 * kE + 1500 + variant * 1700;
        const int64_t  L0 = Line(tp);
        r.Press(tp, b);
        const uint64_t tr = L0 + 2 * kE + 900 + variant * 600;
        const int64_t  R  = Line(tr);
        r.Release(tr, b);
        r.Run(R + kN + 4 * kE);

        CHECK(PlaysLoop(r, T, T, L0 - kX), "v%d: loop before the press line", variant);
        CHECK(PlaysSlice(r, T, b, L0, L0, R - kX), "v%d: slice %d repeats from the press line (as long as it is held)", variant, b);
        CHECK(R > static_cast<int64_t>(tr) && R % kE == 0, "v%d: release line %lld", variant, (long long)R);
        // On release the bar carries on from the slice after it, until the bar line (0.8.0) ...
        const int64_t B = T + kN;
        CHECK(PlaysRotated(r, T, (b + 1) % 8, R, R + 2, B - kX), "v%d: after release the bar carries on from the slice after %d until the bar line", variant, b);
        // ... where it resets to beat 1: the plain loop, in time
        CHECK(PlaysLoop(r, T, B + 2, R + kN + 4 * kE), "v%d: at the bar line the loop restarts from beat 1", variant);
        CHECK(r.Count(SliceEventType::SliceOn) == 1 && r.Count(SliceEventType::BackToLoop) == 2, "v%d: events (release, bar-line reset)", variant);
    }
}

static void TestTap()
{
    std::printf("a tap shorter than the wait still plays one eighth\n");
    Rig r;
    r.input = Noise();
    Start(r);
    const uint64_t tm = 3 * kBarS + 1000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    const uint64_t tp = T + 2 * kE + 1000;
    const int64_t  L0 = Line(tp);
    r.Press(tp, 4);
    r.Release(tp + 192, 4);
    r.Run(L0 + 4 * kE);
    CHECK(PlaysSlice(r, T, 4, L0, L0, L0 + kE - kX), "one eighth of slice 5 starting on the line");
    CHECK(PlaysRotated(r, T, 5, L0 + kE, L0 + kE + 2, L0 + 3 * kE), "then the bar carries on with slice 6 (slice 5 is not played twice)");
    CHECK(r.Count(SliceEventType::SliceOn) == 1 && r.Count(SliceEventType::BackToLoop) == 1, "events");
}

static void TestBank()
{
    std::printf("Shift + B7: one bank only (1 bar), so it is ignored and logged\n");
    Rig r;
    r.input = Noise();
    Start(r);
    const uint64_t tm = 3 * kBarS + 1000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    const uint64_t tb = T + kE + 500;
    r.Bank(tb);
    const uint64_t tp = tb + 2 * kE + 700;
    const int64_t  L0 = Line(tp);
    r.Press(tp, 7); // button 16 = the last eighth of the bar
    r.Release(L0 + 2 * kE, 7);
    r.Run(L0 + 6 * kE);
    CHECK(PlaysSlice(r, T, 7, L0, L0, L0 + 2 * kE - kX), "button 16 plays the last eighth of the bar");
    CHECK(r.Count(SliceEventType::IgnoredOneBank) == 1 && r.Count(SliceEventType::Bank) == 0, "bank switch ignored (1 bar only)");
    // Shift + B7 outside the mode does nothing
    Rig q;
    q.input = Noise();
    Start(q);
    q.Bank(50000);
    q.Run(100000);
    CHECK(q.Count(SliceEventType::IgnoredNoMode) == 1 && q.Count(SliceEventType::Bank) == 0, "Shift + B7 outside the mode is ignored");
    CHECK(MaxDiffShift(q.out, 0, q.in, 0, static_cast<int64_t>(q.in.size())) < 1e-6, "output stayed normal");
}

static void TestFlipBetweenHeld()
{
    std::printf("several buttons: newest wins, releasing it falls back to the older held one\n");
    Rig r;
    r.input = Noise();
    Start(r);
    const uint64_t tm = 3 * kBarS + 1000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    const uint64_t tpA = T + 100;
    const int64_t  LA  = Line(tpA);
    r.Press(tpA, 1); // slice 1
    const uint64_t tpB = LA + kE + 3000;
    const int64_t  LB  = Line(tpB);
    r.Press(tpB, 6); // slice 6 (newest)
    const uint64_t trB = LB + kE + 50;
    const int64_t  RB  = Line(trB);
    r.Release(trB, 6); // -> slice 1 again
    const uint64_t trA = RB + kE + 2222;
    const int64_t  RA  = Line(trA);
    r.Release(trA, 1); // -> loop
    r.Run(RA + 3 * kE);
    CHECK(PlaysSlice(r, T, 1, LA, LA, LB - kX), "slice 2 until slice 7 takes over");
    CHECK(PlaysSlice(r, T, 6, LB, LB, RB - kX), "slice 7 (newest wins)");
    CHECK(PlaysSlice(r, T, 1, RB, RB + 2, RA - kX), "release the newest: slice 2 again (older held button)");
    CHECK(PlaysRotated(r, T, 2, RA, RA + 2, T + kN - kX), "all up: the bar carries on from slice 3 (until the bar line)");
}

static void TestNewSliceDuringRotatedLoop()
{
    std::printf("a new slice during the rotated loop: repeats, then the loop restarts from THAT slice\n");
    Rig r;
    r.input = Noise();
    Start(r);
    const uint64_t tm = 3 * kBarS + 1000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    const uint64_t tpA = T + 3 * kE + 100;
    const int64_t  LA  = Line(tpA);
    r.Press(tpA, 6); // button 15 = slice 7 (the loop then wraps past the window end)
    const uint64_t trA = LA + kE + 50;
    const int64_t  RA  = Line(trA);
    r.Release(trA, 6);
    const uint64_t tpB = RA + 5 * kE + 321;
    const int64_t  LB  = Line(tpB);
    r.Press(tpB, 1); // slice 2
    const uint64_t trB = LB + 2 * kE + 10;
    const int64_t  RB  = Line(trB);
    r.Release(trB, 1);
    r.Run(T + 2 * kN + 2 * kE);
    const int64_t B = T + kN;
    CHECK(PlaysRotated(r, T, 7, RA, RA + 2, B - kX), "loop from slice 8 until the bar line");
    CHECK(PlaysLoop(r, T, B + 2, LB - kX), "bar line: back to the plain loop from beat 1");
    CHECK(PlaysSlice(r, T, 1, LB, LB, RB - kX), "new slice repeats");
    CHECK(PlaysRotated(r, T, 2, RB, RB + 2, T + 2 * kN - kX), "loop restarts from the new slice (until the next bar line)");
}

static void TestPingPong()
{
    std::printf("B6 in slice mode: the slice plays 3 semitones up, forward through the first 16th then backwards, every eighth\n");
    for(int variant = 0; variant < 2; variant++) // 0: B6 first, then the slice; 1: slice first, B6 later, B6 off later
    {
        Rig r;
        r.input = Noise();
        Start(r);
        const uint64_t tm = 3 * kBarS + 1000;
        const int64_t  T  = BarLine(tm);
        r.Mode(tm);
        const int      b  = 3;
        int64_t        L0, P6, R;
        if(variant == 0)
        {
            const uint64_t tf = T + 2 * kE + 100;
            P6                = Line(tf);
            r.Press(tf, 8);
            const uint64_t tp = T + 4 * kE + 1500;
            L0                = Line(tp);
            r.Press(tp, b);
            const uint64_t tr = L0 + kE + 600;
            R                 = Line(tr);
            r.Release(tr, b);
            r.Run(R + 4 * kE);
            CHECK(PlaysLoop(r, T, T, L0 - kX), "v0: loop unchanged with B6 on and no slice held");
            CHECK(PlaysPingPong(r, T, b, L0, L0, R - kX), "v0: slice %d plays as a ping-pong (2 eighths, no seams)", b);
            CHECK(PlaysRotated(r, T, b + 1, R, R + 2, T + kN - kX), "v0: release: the bar carries on from slice %d at normal pitch", b + 1);
            CHECK(r.Count(SliceEventType::PitchOn) == 1 && r.Count(SliceEventType::PitchOff) == 1 - 1, "v0: one PitchOn");
        }
        else
        {
            const uint64_t tp = T + kE + 1500;
            L0                = Line(tp);
            r.Press(tp, b);
            const uint64_t tf = L0 + 700; // B6 comes on while the slice is held
            P6                = Line(tf);
            r.Press(tf, 8);
            const uint64_t tf2 = P6 + kE + 300; // and goes off again, slice still held
            const int64_t  P7  = Line(tf2);
            r.Release(tf2, 8);
            const uint64_t tr = P7 + kE + 900;
            R                 = Line(tr);
            r.Release(tr, b);
            r.Run(R + 4 * kE);
            CHECK(PlaysSlice(r, T, b, L0, L0, P6 - kX), "v1: normal repeat until B6 comes on");
            CHECK(PlaysPingPong(r, T, b, P6, P6 + 2, P7 - kX), "v1: ping-pong from the next eighth line after B6 is pressed");
            CHECK(PlaysSlice(r, T, b, P7, P7 + 2, R - kX), "v1: normal repeat again after B6 goes off");
            CHECK(PlaysRotated(r, T, b + 1, R, R + 2, T + kN - kX), "v1: release as usual");
            CHECK(r.Count(SliceEventType::PitchOn) == 1 && r.Count(SliceEventType::PitchOff) == 1, "v1: one PitchOn, one PitchOff");
        }
        bool on_ok = false;
        for(const auto &e : r.evs)
            if(e.e.type == SliceEventType::PitchOn)
                on_ok = std::llabs((P6 - kX) - static_cast<int64_t>(e.t)) < static_cast<int64_t>(kBlock);
        CHECK(on_ok, "v%d: B6 switches on at its 1/8 line (the crossfade start)", variant);
    }
    // B6 without slice mode does nothing; B6 pressed before slice mode starts stays ignored until released
    Rig q;
    q.input = Noise();
    Start(q);
    q.Press(3 * kBarS + 100, 8);
    q.Mode(3 * kBarS + 1000);
    q.Release(5 * kBarS, 8);
    q.Run(6 * kBarS);
    CHECK(q.Count(SliceEventType::PitchOn) == 0, "B6 held before slice mode started does nothing");
}

static void TestForwardOnly()
{
    std::printf("reverse off: B6 plays the whole eighth forward at the raised pitch; the switch changes it on the next 1/8 line\n");
    Rig r;
    r.input = Noise();
    r.reverse = false;
    Start(r);
    const uint64_t tm = 3 * kBarS + 1000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    r.Press(T + 200, 8); // B6 on
    const int      b  = 2;
    const uint64_t tp = T + kE + 1500;
    const int64_t  L0 = Line(tp);
    r.Press(tp, b);
    const uint64_t tk = L0 + 2 * kE + 500; // the reverse switch goes on while the slice is held
    const int64_t  K  = Line(tk);
    r.At(tk, A_REV, 1);
    const uint64_t tk2 = K + kE + 500; // and off again
    const int64_t  K2  = Line(tk2);
    r.At(tk2, A_REV, 0);
    const uint64_t tr = K2 + 300;
    const int64_t  R  = Line(tr);
    r.Release(tr, b);
    r.Run(T + 2 * kN);
    CHECK(PlaysForward(r, T, b, L0, L0, K - kX), "forward only: 2 eighths, pitched +3, restarted every eighth");
    CHECK(PlaysPingPong(r, T, b, K, K + 2, K2 - kX), "reverse switched on: ping-pong from the next 1/8 line");
    CHECK(PlaysForward(r, T, b, K2, K2 + 2, R - kX), "reverse switched off: forward only again");
    // a down shift (knob at -7) reads LESS than an eighth of the recording
    Rig q;
    q.input = Noise();
    q.reverse = false;
    Start(q);
    q.At(0, A_SEMIS, -7);
    q.Mode(tm);
    q.Press(T + 200, 8);
    q.Press(tp, b);
    q.Release(L0 + 3 * kE + 300, b);
    q.Run(T + 2 * kN);
    CHECK(PlaysForward(q, T, b, L0, L0, L0 + 3 * kE - kX, -7), "forward only at -7 semitones");
    // forward-only has no clicks at the repeat: the step stays small on a sine
    Rig c;
    c.input = Sine(437.125);
    c.reverse = false;
    Start(c);
    c.Mode(tm);
    c.Press(T + 200, 8);
    c.Press(tp, b);
    c.Release(L0 + 4 * kE + 300, b);
    c.Run(T + 2 * kN);
    double m = 0;
    for(int64_t t = L0 + 2; t < L0 + 4 * kE; t++)
        m = std::max(m, std::fabs(static_cast<double>(c.out[t]) - c.out[t - 1]));
    CHECK(m <= 1.6 * SineStep(437.125) * std::pow(2.0, 3 / 12.0), "no click at the repeat (max step %.4f)", m);
}

static void TestBarLineChoke()
{
    std::printf("bar line: a held slice is choked and the loop restarts from beat 1; a press that lands on the bar line plays\n");
    Rig r;
    r.input = Noise();
    Start(r);
    const uint64_t tm = 3 * kBarS + 1000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    const int64_t B  = T + kN; // the bar line (end of the Slice loop buffer)
    const int     b  = 4;
    const uint64_t tp = T + 5 * kE + 800;
    const int64_t  L0 = Line(tp);
    r.Press(tp, b);                        // held across the bar line
    const uint64_t tr = B + 3 * kE + 600;
    const int64_t  R  = Line(tr);
    r.Release(tr, b);                      // released in the next bar: nothing to do
    const uint64_t tp2 = R + kE + 700;     // pressed again later: plays again
    const int64_t  L2  = Line(tp2);
    r.Press(tp2, b);
    // a press shortly BEFORE the next bar line lands ON it and plays; the older slice b (still held) is choked there
    const uint64_t tp3 = T + 2 * kN - kE / 2;
    const int64_t  B2  = T + 2 * kN;
    r.Press(tp3, 1);
    r.Run(B2 + 3 * kE);
    CHECK(PlaysSlice(r, T, b, L0, L0, B - kX), "slice %d repeats until the bar line", b);
    CHECK(PlaysLoop(r, T, B + 2, R + kE), "bar line: choked, plain loop from beat 1 (the button is still held: it stays choked)");
    CHECK(PlaysSlice(r, T, b, L2, L2, B2 - kX), "pressed again after the release: plays (until the next bar line)");
    CHECK(PlaysSlice(r, T, 1, B2, B2 + 2, B2 + 3 * kE - kX), "a press that lands on the bar line plays; the older held slice is choked");
    CHECK(r.Count(SliceEventType::SliceOn) == 3, "three slice starts");
}

static void TestPitchKnob()
{
    std::printf("pitch knob: the ping-pong pitch follows the knob (whole semitones, next 1/8 line)\n");
    Rig r;
    r.input = Noise();
    Start(r);
    const uint64_t tm = 3 * kBarS + 1000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    r.Press(T + 200, 8);                      // B6 on
    const int      b  = 2;
    const uint64_t tp = T + kE + 1500;
    const int64_t  L0 = Line(tp);
    r.Press(tp, b);
    const uint64_t tk = L0 + kE + 500;        // the knob moves to -5 semitones
    const int64_t  K  = Line(tk);
    r.At(tk, A_SEMIS, -5);
    const uint64_t tk2 = K + kE + 500;        // and to +12
    const int64_t  K2  = Line(tk2);
    r.At(tk2, A_SEMIS, 12);
    const uint64_t tr = K2 + kE + 300;
    const int64_t  R  = Line(tr);
    r.Release(tr, b);
    r.Run(T + 2 * kN);
    CHECK(PlaysPingPong(r, T, b, L0, L0, K - kX, 3), "starts at +3 (the default)");
    CHECK(PlaysPingPong(r, T, b, K, K + 2, K2 - kX, -5), "knob at -5: from the next 1/8 line");
    CHECK(PlaysPingPong(r, T, b, K2, K2 + 2, R - kX, 12), "knob at +12");
    CHECK(looper::SlicePitchFromControl(0.0f) == -12 && looper::SlicePitchFromControl(1.0f) == 12
              && looper::SlicePitchFromControl(0.625f) == 3 && looper::SlicePitchFromControl(0.5f) == 0,
          "knob 0..1 -> -12..+12, 0.625 = +3");
}

static void TestIgnoredAndStop()
{
    std::printf("stopped transport: B7 is ignored; Stop ends slice mode at once; buttons held when it starts are ignored\n");
    {
        Rig r;
        r.input = Noise();
        r.At(0, A_CLOCK_ON); // clock but no Start
        r.Mode(40000);
        r.Run(120000);
        CHECK(r.Count(SliceEventType::IgnoredNotRunning) == 1 && r.Count(SliceEventType::ModeOn) == 0, "ignored");
        CHECK(MaxDiffShift(r.out, 0, r.in, 0, static_cast<int64_t>(r.in.size())) < 1e-6, "normal signal");
    }
    {
        Rig r;
        r.input = Noise();
        Start(r);
        const uint64_t tm = 3 * kBarS + 1000;
        const int64_t  T  = BarLine(tm);
        r.Mode(tm);
        const int64_t ts = T + kBarS + 300;
        r.At(ts, A_MIDI_STOP);
        r.Run(T + 3 * kBarS);
        CHECK(r.Count(SliceEventType::ModeOff) == 1, "Stop ends slice mode");
        CHECK(MaxDiffShift(r.out, ts + 400, r.in, ts + 400, kBarS) < 1e-6, "normal signal after Stop");
    }
    {
        Rig r;
        r.input = Noise();
        Start(r);
        r.Press(3 * kBarS, 3); // already held when the mode starts
        const uint64_t tm = 3 * kBarS + 1000;
        const int64_t  T  = BarLine(tm);
        r.Mode(tm);
        r.Release(T + 3 * kE, 3);
        r.Run(T + 6 * kE);
        CHECK(r.Count(SliceEventType::SliceOn) == 0, "a button held before the mode started does not play");
        CHECK(PlaysLoop(r, T, T, T + 6 * kE), "still the loop");
    }
}

static void TestUnfreeze()
{
    std::printf("after B7 off the recording resumes; B7 again soon after WAITS until a whole bar is recorded (never plays silence)\n");
    Rig r;
    r.input = Noise();
    Start(r);
    const uint64_t tm = 3 * kBarS + 1000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    const uint64_t tx = T + 2 * kE + 100;
    const int64_t  TX = Line(tx);
    r.Mode(tx);
    const uint64_t tm2 = TX + 2 * kE + 400;
    r.Mode(tm2);
    r.Run(T + 4 * kBarS);
    // The first bar line after tm2 is T + kBarS: the bar before it (from T) is partly
    // not recorded (frozen until TX), so B7 waits for T + 2 bars, whose bar is all recorded.
    const int64_t T2 = T + 2 * kBarS;
    CHECK(MaxDiffShift(r.out, TX + 2 * kX, r.in, TX + 2 * kX, T2 - kX - TX - 4 * kX) < 1e-6, "live signal is heard while B7 waits");
    CHECK(PlaysLoop(r, T2, T2, T2 + kBarS + 2 * kE), "frozen bar plays from beat 1, no silence");
    CHECK(r.Count(SliceEventType::ModeOn) == 2 && r.Count(SliceEventType::ModeOff) == 1, "events");

    // Right after Start: the bar before the first bar line is audio recorded since power-up, so it plays.
    Rig q;
    q.input = Noise();
    q.At(0, A_CLOCK_ON);
    q.At(40000, A_MIDI_START);
    q.Mode(40000 + 1000);
    q.Run(40000 + 4 * kBarS);
    CHECK(q.Count(SliceEventType::ModeOn) == 1, "B7 right after Start switches at the first bar line");
}

static void TestOffset()
{
    std::printf("input offset shifts the 1/8 lines like the beat repeat\n");
    for(int off : {960, -960, 300})
    {
        Rig r;
        r.input = Noise();
        Start(r);
        r.At(0, A_OFFSET, off);
        const uint64_t tm = 3 * kBarS + 7000;
        const int64_t  T  = BarLine(tm, off);
        r.Mode(tm);
        const uint64_t tp = T + 2 * kE + 3000;
        const int64_t  L0 = Line(tp, off);
        r.Press(tp, 3);
        r.Release(L0 + kE + 100, 3);
        r.Run(L0 + 4 * kE);
        CHECK((T - off) % kE == 0, "offset %d: the freeze line is on the shifted grid", off);
        CHECK(PlaysSlice(r, T, 3, L0, L0, L0 + kE - kX), "offset %d: slice starts on the shifted line", off);
    }
}

static void TestDrift()
{
    std::printf("the loop is re-aligned to the clock every bar (clock 300 ppm slower than the audio)\n");
    Rig r;
    r.input = Noise();
    Start(r);
    const uint64_t tm = 3 * kBarS + 1000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    r.At(T + 100, A_BPM, 120000.0 / 864.0 * (1.0 - 300e-6)); // the DAW is now slightly slower
    const int64_t span = 20 * kBarS;
    r.Run(T + span + kN);
    // The bar line moves later by ~span*300ppm; the loop follows it, so its start is
    // within a few samples of the (later) clock lines, never a growing error.
    // Find where the loop's first sample (= in[T - N]) reappears near T + 8 N.
    const int64_t guess = T + 8 * kN;
    int64_t       best  = -1;
    double        best_d = 1e9;
    for(int64_t c = guess - 700; c < guess + 700; c++)
    {
        const double d = std::fabs(static_cast<double>(r.out[c + 50]) - r.in[T - kN + 50])
                       + std::fabs(static_cast<double>(r.out[c + 51]) - r.in[T - kN + 51]);
        if(d < best_d)
        {
            best_d = d;
            best   = c;
        }
    }
    const int64_t expect_shift = static_cast<int64_t>(std::lround(8.0 * kN * 300e-6));
    CHECK(best_d < 1e-5, "found the loop start again (%.5f)", best_d);
    CHECK(std::llabs((best - guess) - expect_shift) <= 64, "the loop start follows the slower clock by %lld samples (expected about %lld)", (long long)(best - guess), (long long)expect_shift);
    CHECK(MaxStep(r.out) < 1.0, "no big jumps");
}

static void TestClicks()
{
    std::printf("no clicks at any transition (sine input, worst-case phases)\n");
    for(double f : {437.125, 1234.567})
    {
        Rig r;
        r.input = Sine(f);
        Start(r);
        r.Mode(3 * kBarS + 1000);
        r.Press(3 * kBarS + 40001, 3);
        r.Press(4 * kBarS + 7, 5);
        r.Release(4 * kBarS + 30000, 5);
        r.Bank(4 * kBarS + 50000);
        r.Press(5 * kBarS + 4321, 0);
        r.Release(5 * kBarS + 80000, 0);
        r.Release(5 * kBarS + 90000, 3);
        r.Press(5 * kBarS + 20000, 8); // B6: pitched ping-pong
        r.Release(5 * kBarS + 60000, 8);
        r.Press(6 * kBarS + 2000, 8);
        r.Press(6 * kBarS + 9000, 4);
        r.Release(6 * kBarS + 40000, 4);
        r.Release(6 * kBarS + 50000, 8);
        r.Mode(7 * kBarS + 12345);
        r.Run(9 * kBarS);
        const double step = MaxStep(r.out);
        CHECK(step <= 1.6 * SineStep(f), "f=%.1f max step %.4f vs allowed %.4f", f, step, 1.6 * SineStep(f));
    }
}

static void TestIntoLoop()
{
    std::printf("slice mode output is recorded into the Quantized loop buffer, and heard over a playing loop\n");
    Rig r(true);
    r.input = Noise();
    Start(r);
    const uint64_t tm = 3 * kBarS + 2000;
    const int64_t  T  = BarLine(tm);
    r.Mode(tm);
    r.At(3 * kBarS + 6000, A_LOOP_CMD, static_cast<double>(LoopCmd::ArmToggle)); // records bar 4, plays bar 5+
    const uint64_t tp = 4 * kBarS + 5 * kE + 1000; // a slice press while recording
    r.Press(tp, 2);
    r.Release(tp + 2 * kE, 2);
    const uint64_t tx = 6 * kBarS + 3000;
    const int64_t  TX = Line(tx);
    r.Mode(tx);
    r.Run(10 * kBarS);

    const LoopEvent *rec = nullptr, *play = nullptr;
    uint64_t         trec = 0, tplay = 0;
    for(const auto &e : r.levs)
    {
        if(e.e.type == LoopEventType::RecordStart && !rec)
        {
            rec  = &e.e;
            trec = e.t;
        }
        if(e.e.type == LoopEventType::PlaybackStart && !play)
        {
            play  = &e.e;
            tplay = e.t;
        }
    }
    CHECK(rec && play, "the loop recorded and played");
    if(rec && play)
    {
        const int64_t len = static_cast<int64_t>(play->b);
        CHECK(len > kBarS - 100 && len < kBarS + 100, "loop length %lld", (long long)len);
        // 1. While slice mode is on and the loop plays, you hear slice mode (the frozen window), not the loop.
        // (after the slice button is released it loops the 2 bars from slice 3: 0.5.1)
        const int64_t RL  = Line(tp + 2 * kE);
        const int64_t th0 = std::max<int64_t>(static_cast<int64_t>(tplay), RL) + 2;
        // (the slice is released ON the bar line, so slice mode goes back to the plain loop from beat 1: 0.8.0)
        CHECK(PlaysLoop(r, T, th0, TX - kX - 100), "slice mode is heard over the playing Quantized loop buffer");
        // 2. What the Quantized loop buffer was given while recording is the slice mode signal,
        //    including the slice that was pressed.
        const int64_t L0 = Line(tp);
        const int64_t S  = T - kN + 2 * kE;
        double        d  = 0;
        for(int64_t t = L0; t < L0 + kE - kX; t++)
            d = std::max(d, std::fabs(static_cast<double>(r.perf[t]) - r.in[S + (t - L0)]));
        CHECK(d < 1e-5 && L0 > static_cast<int64_t>(trec) && L0 + kE < static_cast<int64_t>(trec) + len,
              "the slice press is inside the recorded bar (diff %.6f)", d);
        // 3. After slice mode ends, the Quantized loop buffer plays back exactly what it recorded.
        const int64_t after = TX + 3 * len / 2 + 1000;
        const int64_t k0    = (after - static_cast<int64_t>(tplay)) % len;
        const double  d2    = MaxDiffShift(r.out, after + 300, r.perf, static_cast<int64_t>(trec) + k0 + 300,
                                           std::min<int64_t>(len - k0 - 700, 20000));
        CHECK(d2 < 1e-5, "after slice mode the loop plays what was recorded (max diff %.6f)", d2);
    }
}

int main()
{
    TestEnterLoopExit();
    TestSlices();
    TestTap();
    TestBank();
    TestFlipBetweenHeld();
    TestPingPong();
    TestForwardOnly();
    TestBarLineChoke();
    TestPitchKnob();
    TestNewSliceDuringRotatedLoop();
    TestIgnoredAndStop();
    TestUnfreeze();
    TestOffset();
    TestDrift();
    TestClicks();
    TestIntoLoop();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
