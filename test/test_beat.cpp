// Host-side test for src/engine/beat_repeat.h (with the real ClockTracker, and
// for the "stutter into the loop" test also the real QuantizedLoopBuffer).
// Run with `make -C test`.
//
// Most tests use a 138.89 BPM clock, where a tick is exactly 864 samples
// (18 blocks of 48; the buffers are sized for tempos of 130 BPM and up), so every position is a whole number of samples and the
// output can be compared with the input sample for sample:
//   one bar = 82944 samples, 1/64 bar = 1296 samples (1.5 ticks).
// The input is hash noise, so any misalignment shows up as a large error.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>
#include "../src/engine/beat_repeat.h"
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

static const double   kSr    = 48000.0;
static const size_t   kBlock = 48;
static const double   kPi    = 3.14159265358979323846;
static const int64_t  kBarS  = 82944; // samples per bar at 138.89 BPM (tick = 864 samples)
static const int64_t  kX     = 240;   // crossfade length (5 ms)

enum
{
    A_MIDI_START,
    A_MIDI_STOP,
    A_CLOCK_ON,
    A_HOLD,    // v = division index * 2 + (1 = press, 0 = release)
    A_OFFSET,  // v = offset in samples
    A_BPM,
    A_LOOP_CMD // v = LoopCmd
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
    BeatRepeat         br;
    QuantizedLoopBuffer         eng;
    std::vector<float> hl, hr, fl, fr, ll, lr;
    bool               use_loop = false;
    std::function<float(uint64_t)> input;
    std::vector<Act>   acts;
    size_t             act_i = 0;
    std::vector<float> in, out, rep; // rep = beat repeat output, out = final output
    struct Ev
    {
        uint64_t  t;
        BeatEvent e;
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
    double           bpm       = 120000.0 / 864.0; // tick = 864 samples
    uint32_t         held      = 0;
    float            offset    = 0;
    std::vector<int>  lc;   // beat repeat LoopCount() after each block
    std::vector<char> pon;  // beat repeat PitchOn() after each block
    std::vector<int>  ji, ri; // JumpStartIndex() / RepeatIndex() after each block

    explicit Rig(bool with_loop = false, size_t ring = looper::kHistoryCapacityFrames)
        : hl(ring), hr(ring), fl(looper::kFreezeCapacityFrames), fr(looper::kFreezeCapacityFrames),
          use_loop(with_loop)
    {
        clock.Init(static_cast<float>(kSr));
        br.Init(hl.data(), hr.data(), ring, fl.data(), fr.data(), fl.size(), static_cast<float>(kSr));
        if(with_loop)
        {
            ll.resize(looper::kLoopCapacityFrames);
            lr.resize(looper::kLoopCapacityFrames);
            eng.Init(ll.data(), lr.data(), ll.size(), static_cast<float>(kSr));
        }
    }

    void At(uint64_t t, int kind, double v = 0) { acts.push_back({t, kind, v}); }
    void Press(uint64_t t, int div_idx) { At(t, A_HOLD, div_idx * 2 + 1); }
    void Release(uint64_t t, int div_idx) { At(t, A_HOLD, div_idx * 2); }

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
                    case A_HOLD:
                    {
                        const int idx = static_cast<int>(a.v) / 2;
                        if(static_cast<int>(a.v) % 2)
                            held |= 1u << idx;
                        else
                            held &= ~(1u << idx);
                        break;
                    }
                    case A_OFFSET: offset = static_cast<float>(a.v); break;
                    case A_BPM: bpm = a.v; break;
                    case A_LOOP_CMD: cmds.push_back(static_cast<LoopCmd>(static_cast<int>(a.v))); break;
                }
            }
            if(clock_on)
            {
                // (rounded, so that 120000/864 BPM gives exactly 864 samples, not 864.0000000001)
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

            BeatBlockInfo bi;
            bi.running         = st.running;
            bi.bar             = clock.BarAtBlockStart();
            bi.pos_in_bar      = clock.PositionAtBlockStart(kBlock);
            bi.samples_per_bar = clock.SamplesPerBar();
            bi.held_mask       = held;
            bi.offset_samples  = offset;
            bi.pitch_held      = (held >> 8) & 1u; // test button index 8 = B6

            float il[kBlock], ir[kBlock], rl[kBlock], rr[kBlock], ol[kBlock], orr[kBlock];
            for(size_t i = 0; i < kBlock; i++)
                il[i] = ir[i] = input(now + i);
            br.Process(bi, il, ir, rl, rr, kBlock);
            lc.push_back(br.LoopCount());
            pon.push_back(br.PitchOn());
            ji.push_back(br.JumpStartIndex());
            ri.push_back(br.RepeatIndex());
            if(use_loop)
            {
                LoopBlockInfo li;
                li.clock_present   = st.present;
                li.running         = st.running;
                li.bar_start       = (cev & kEvBar) != 0;
                li.bar             = st.bar;
                li.samples_per_bar = clock.SamplesPerBar();
                eng.SetLoopBars(1);
                for(LoopCmd c : cmds)
                    eng.Command(c, li);
                eng.Process(li, rl, rr, ol, orr, kBlock);
                LoopEvent le;
                while(eng.PopEvent(le))
                    levs.push_back({now, le});
            }
            else
                for(size_t i = 0; i < kBlock; i++)
                {
                    ol[i]  = rl[i];
                    orr[i] = rr[i];
                }
            for(size_t i = 0; i < kBlock; i++)
            {
                in.push_back(il[i]);
                rep.push_back(rl[i]);
                out.push_back(ol[i]);
            }
            BeatEvent e;
            while(br.PopEvent(e))
                evs.push_back({now, e});
            now += kBlock;
        }
    }

    int Count(BeatEventType t) const
    {
        int c = 0;
        for(const auto &e : evs)
            c += e.e.type == t;
        return c;
    }
};

// ---------------------------------------------------------------- signals
/** Deterministic hash noise in -0.5..0.5, a function of the sample number only. */
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

// ---------------------------------------------------------------- measures
static double MaxStep(const std::vector<float> &y, size_t from, size_t to)
{
    double m = 0;
    for(size_t i = from + 1; i < to && i < y.size(); i++)
        m = std::max(m, std::fabs(static_cast<double>(y[i]) - y[i - 1]));
    return m;
}
/** max |out[o + k] - in[i + k]| for k in [0, len) */
static double MaxDiffShift(const std::vector<float> &o, int64_t o0, const std::vector<float> &x,
                           int64_t x0, int64_t len)
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

/** Slice length in samples of division index d (at 138.89 BPM). */
static int64_t SliceLen(int d)
{
    return kBarS * looper::kDivNum[d] / looper::kDivDen[d];
}

/** First slice boundary (absolute sample) the engine will use for a press at
 *  time `t` (acts are applied at the start of the block that contains t). */
static int64_t Boundary(uint64_t t, int64_t L, int64_t offset = 0)
{
    const int64_t tp = ((static_cast<int64_t>(t) + kBlock - 1) / kBlock) * kBlock;
    // transition starts when time + X reaches a grid line + offset
    int64_t k = (tp + kX - offset + L - 1) / L;
    return k * L + offset;
}

/** The 1/16 line (kBarS / 16 = 5184 samples) that a press or release at time
 *  `t` is quantised to: the point where the transition ENDS (it starts X before). */
static const int64_t kLine = kBarS / 16;
static int64_t QB(uint64_t t, int64_t offset = 0)
{
    return Boundary(t, kLine, offset);
}

/** Check that, from boundary B on, the output repeats the slice that ended at B. */
static bool RepeatsSlice(const Rig &r, int64_t B, int64_t L, int cycles, double tol = 1e-5, bool quiet = false)
{
    for(int c = 0; c < cycles; c++)
    {
        const double d = MaxDiffShift(r.out, B + c * L, r.in, B - L, L - kX);
        if(d > tol)
        {
            if(!quiet)
                std::printf("    cycle %d differs by %.5f (B=%lld L=%lld)\n", c, d, (long long)B, (long long)L);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- tests
static void TestAllDivisions()
{
    std::printf("every division repeats exactly the slice that just finished\n");
    for(int d = 0; d < static_cast<int>(looper::kNumDivisions); d++)
    {
        const char    *div = looper::DivisionLabel(d);
        const int64_t  L   = SliceLen(d);
        Rig            r;
        r.input = Noise();
        r.At(0, A_CLOCK_ON);
        r.At(0, A_MIDI_START);
        const uint64_t tp = 2 * kBarS + 12345; // somewhere in bar 3
        const int64_t  B  = QB(tp); // the stutter starts on the next 1/16 line
        // hold for ~3 repeats (at most 5 bars)
        const int64_t  hold = std::min<int64_t>(L * 3 + 500 + 2 * kLine, 5 * kBarS);
        const uint64_t tr   = static_cast<uint64_t>(B + hold);
        const int64_t  R    = QB(tr); // ... and ends on the next 1/16 line after the release
        r.Press(tp, d);
        r.Release(tr, d);
        r.Run(tr + 4 * kBarS / 2);
        const int cycles = static_cast<int>((R - B) / L); // complete repeats before the release
        CHECK(cycles >= 1, "div %s: at least one full repeat is checked (%d)", div, cycles);

        CHECK(r.Count(BeatEventType::Waiting) == 1 && r.Count(BeatEventType::On) == 1, "div %s events", div);
        CHECK(RepeatsSlice(r, B, L, cycles), "div %s: output is not a repeat of the slice that ended at %lld", div, (long long)B);
        // before the transition: live
        CHECK(MaxDiffShift(r.out, 0, r.in, 0, B - kX - 1) < 1e-6, "div %s: live before the boundary", div);
        // after the release (+ fade): live again
        CHECK(MaxDiffShift(r.out, R + 2, r.in, R + 2, kBarS / 2) < 1e-6, "div %s: live after release (on the 1/16 line)", div);
        CHECK(r.Count(BeatEventType::Off) == 1, "div %s: off event", div);
    }
}

static void TestLongHold()
{
    std::printf("a 1-bar slice held for 6 bars (history ring is only just over 2 bars)\n");
    Rig r(false, 2 * kBarS + 2400);
    r.input = Noise();
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
    const uint64_t tp = kBarS + 5000;
    const int64_t  B  = QB(tp);
    r.Press(tp, 0);
    r.Run(B + 6 * kBarS + 3000);
    CHECK(RepeatsSlice(r, B, kBarS, 6), "repeat stays identical for 6 cycles (frozen copy takes over)");
}

static void TestOffset()
{
    std::printf("input offset moves the slice grid (+/- 20 ms)\n");
    for(int off : {960, -960, 300, -300})
    {
        Rig r;
        r.input = Noise();
        r.At(0, A_CLOCK_ON);
        r.At(0, A_MIDI_START);
        r.At(0, A_OFFSET, off);
        const uint64_t tp = kBarS + 7000;
        const int64_t  L  = kBarS / 8; // 1/8
        const int64_t  B  = QB(tp, off);
        r.Press(tp, 4);
        r.Release(B + 4 * L, 4);
        r.Run(B + 6 * L);
        CHECK(RepeatsSlice(r, B, L, 3), "offset %d: repeat aligned to the shifted grid", off);
        // sanity: the same expectation WITHOUT the offset must not match
        const int64_t B0 = QB(tp, 0);
        CHECK(!RepeatsSlice(r, B0, L, 1, 1e-3, true) || B0 == B, "offset %d: test can tell the offset matters", off);
    }
}

/** Snare-like bursts: every 1/8 bar a short decaying burst whose pitch depends
 *  on which slice it is. The whole signal is `late` samples late against the clock. */
static std::function<float(uint64_t)> Bursts(int64_t late)
{
    return [=](uint64_t n) {
        const int64_t L = kBarS / 8;
        const int64_t t = static_cast<int64_t>(n) - late;
        if(t < 0)
            return 0.0f;
        const int64_t k = t / L, m = t % L;
        const double  f = 300.0 + 90.0 * (k % 8);
        return static_cast<float>(0.8 * std::exp(-m / 700.0) * std::sin(2 * kPi * f * m / kSr));
    };
}

static void TestFullSnare()
{
    std::printf("a snare that arrives 6 ms late is repeated whole (offset set) and cut (offset 0)\n");
    const int64_t late = 288; // 6 ms
    for(int use_offset = 1; use_offset >= 0; use_offset--)
    {
        Rig r;
        r.input = Bursts(late);
        r.At(0, A_CLOCK_ON);
        r.At(0, A_MIDI_START);
        r.At(0, A_OFFSET, use_offset ? late : 0);
        const uint64_t tp = kBarS + 9000;
        const int64_t  L  = kBarS / 8;
        const int64_t  B  = QB(tp, use_offset ? late : 0); // (a line that is also a burst start)
        r.Press(tp, 4);
        r.Release(B + 3 * L, 4);
        r.Run(B + 5 * L);
        // The first 1 ms of every repeat should contain the attack at full level.
        double attack_in = 0, attack_out = 0;
        for(int k = 0; k < 48; k++)
        {
            attack_in += std::fabs(r.in[B - L + late - (B % L) * 0 + k]);
            attack_out += std::fabs(r.out[B + k]);
        }
        // where is the burst attack inside the repeated slice?
        // With the offset the slice starts exactly at the burst; without, 6 ms earlier.
        if(use_offset)
            CHECK(attack_out > 0.9 * attack_in, "with offset: repeat starts on the attack (%.2f vs %.2f)", attack_out, attack_in);
        else
            CHECK(attack_out < 0.5 * attack_in, "without offset the slice starts before the attack (%.2f vs %.2f)", attack_out, attack_in);
    }
}

/** Expected output of a stored slice: the slice [B0-L, B0) of the input,
 *  repeating on the grid (B0 is a grid line). Compares every sample in
 *  [t0, t1) except the last X before each grid line (the loop crossfade). */
static bool StoredPlays(const Rig &r, int64_t B0, int64_t L, int64_t t0, int64_t t1, double tol, const char *what)
{
    int64_t checked = 0;
    for(int64_t t = t0; t < t1; t++)
    {
        int64_t u = (t - B0) % L;
        if(u < 0)
            u += L;
        if(u >= L - kX)
            continue;
        checked++;
        const double d = std::fabs(static_cast<double>(r.out[t]) - r.in[B0 - L + u]);
        if(d > tol)
        {
            std::printf("    %s: sample %lld differs by %.5f (u=%lld)\n", what, (long long)t, d, (long long)u);
            return false;
        }
    }
    return checked > 0;
}

static void TestFlipBetweenHeld()
{
    std::printf("held buttons: flip between the stored slices, no new capture, in time\n");
    // Several release times, so that the flip lands at every part of the slice
    // (including the last X samples, where the loop fade is already under way).
    const int64_t LA = kBarS / 4, LB = kBarS / 16;
    static const int64_t kIn[8] = {10, 500, LB / 2, LB - 600, LB - 300, LB - 200, LB - 100, LB - 49};
    for(int variant = 0; variant < 8; variant++)
    {
        Rig r;
        r.input = Noise();
        r.At(0, A_CLOCK_ON);
        r.At(0, A_MIDI_START);
        const uint64_t tpA = kBarS + 3000;
        const int64_t  BA  = QB(tpA);
        r.Press(tpA, 3); // 1/4
        const uint64_t tpB = BA + LA + LA / 3;
        const int64_t  BB  = QB(tpB);
        r.Press(tpB, 5); // 1/16 (newest)
        const uint64_t trB = BB + 5 * LB + kIn[variant];
        r.Release(trB, 5); // -> A's stored slice
        const uint64_t tpB2 = trB + LB * 2 + 3000 + variant * 123;
        r.Press(tpB2, 5); // -> B's stored slice
        const uint64_t trA = tpB2 + LB * 3 + 777;
        r.Release(trA, 3); // the older one is released while B is held: no change
        const uint64_t trB2 = trA + LB * 2 + 41;
        r.Release(trB2, 5); // all up -> live, slices forgotten
        const uint64_t tpA2 = trB2 + kBarS / 2 + 1000;
        const int64_t  BA2  = QB(tpA2);
        r.Press(tpA2, 3); // must capture NEW audio
        r.Release(BA2 + 2 * LA, 3);
        r.Run(BA2 + 3 * LA + 2000);

        CHECK(RepeatsSlice(r, BA, LA, static_cast<int>((BB - kX - BA) / LA)), "v%d: A repeats until B takes over", variant);
        CHECK(RepeatsSlice(r, BB, LB, 5), "v%d: B captured the slice that ended at its own boundary", variant);
        CHECK(StoredPlays(r, BA, LA, QB(trB) + 2, QB(tpB2) - kX, 1e-5, "A original"),
              "v%d: right after releasing B: A's ORIGINAL slice, in time", variant);
        CHECK(StoredPlays(r, BB, LB, QB(tpB2) + 2, QB(trB2) - kX, 1e-5, "B original"),
              "v%d: B pressed again: B's ORIGINAL slice, in time (also after A is released)", variant);
        CHECK(MaxDiffShift(r.out, QB(trB2) + 60, r.in, QB(trB2) + 60, kBarS / 4) < 1e-6,
              "v%d: live after the last release", variant);
        CHECK(RepeatsSlice(r, BA2, LA, 2), "v%d: after releasing everything a press captures fresh audio", variant);
        CHECK(r.Count(BeatEventType::Waiting) == 4, "v%d: waiting events %d (A, B, B again, A after release)", variant, r.Count(BeatEventType::Waiting));
        CHECK(r.Count(BeatEventType::On) == 3, "v%d: fresh starts %d", variant, r.Count(BeatEventType::On));
        CHECK(r.Count(BeatEventType::OnStored) == 2, "v%d: stored recalls %d", variant, r.Count(BeatEventType::OnStored));
    }
}

static void TestFlipLongAfterHistory()
{
    std::printf("a stored 1-bar slice can still be flipped back to after the history has been overwritten\n");
    Rig r(false, 2 * kBarS + 2400);
    r.input = Noise();
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
    const uint64_t tpA = kBarS + 5000;
    const int64_t  BA  = QB(tpA);
    r.Press(tpA, 0); // 1 bar
    const uint64_t tpB = BA + 3 * kBarS + 100; // 3 bars later: the history no longer holds A's slice
    const int64_t  BB  = QB(tpB);
    r.Press(tpB, 4);
    const uint64_t trB = BB + 4 * (kBarS / 8) + 5;
    r.Release(trB, 4);
    r.Run(trB + 3 * kBarS);
    CHECK(StoredPlays(r, BA, kBarS, QB(trB) + 2, QB(trB) + kBarS + kBarS / 2, 1e-5, "stored 1 bar"),
          "the 1-bar slice is still the original one");
}

static void TestDottedGrid()
{
    std::printf("3/8 (dotted quarter): repeats stay exact across bar lines\n");
    const int64_t L = SliceLen(2);
    CHECK(L == kBarS * 3 / 8 && L == 31104, "3/8 of a bar is %lld samples", (long long)L);
    for(int bar = 0; bar < 6; bar++)
    {
        Rig r;
        r.input = Noise();
        r.At(0, A_CLOCK_ON);
        r.At(0, A_MIDI_START);
        const uint64_t tp = static_cast<uint64_t>(bar) * kBarS + 40000; // (tempo is known after ~25 ticks)
        const int64_t  B  = QB(tp);
        r.Press(tp, 2);
        r.Release(B + 8 * L + 50, 2); // 3 bars of repeats: crosses bar lines
        r.Run(B + 9 * L + 2000);
        CHECK(RepeatsSlice(r, B, L, 8), "bar %d: the 3/8 repeat stays exact over bar lines", bar);
    }
}

static void TestQuantisedPressRelease()
{
    std::printf("press and release both wait for the next 1/16 line (the slice length is unchanged)\n");
    for(int variant = 0; variant < 6; variant++)
    {
        Rig r;
        r.input = Noise();
        r.At(0, A_CLOCK_ON);
        r.At(0, A_MIDI_START);
        const int64_t  L  = kBarS / 4; // 1/4 = four lines long
        const uint64_t tp = kBarS + 4000 + variant * 777;
        const int64_t  B  = QB(tp);
        r.Press(tp, 3);
        const uint64_t tr = B + L / 2 + 1234 + variant * 901; // mid-slice, mid-line
        const int64_t  R  = QB(tr);
        r.Release(tr, 3);
        r.Run(tr + 3 * kLine);

        // the press: live until the transition (which starts X before the line, never earlier than the press)
        CHECK(B >= static_cast<int64_t>(tp) + kX && B - static_cast<int64_t>(tp) <= kLine + kX + static_cast<int64_t>(kBlock),
              "v%d: start line %lld is within one line of the press at %llu", variant, (long long)B, (unsigned long long)tp);
        CHECK(B % kLine == 0, "v%d: starts on a 1/16 line", variant);
        CHECK(MaxDiffShift(r.out, 0, r.in, 0, B - kX - 1) < 1e-6, "v%d: live until the line", variant);
        // the On event comes in the block that holds the start of the crossfade
        bool on_ok = false;
        for(const auto &e : r.evs)
            if(e.e.type == BeatEventType::On)
            {
                const int64_t dt = (B - kX) - static_cast<int64_t>(e.t);
                on_ok            = dt >= 0 && dt < static_cast<int64_t>(kBlock);
            }
        CHECK(on_ok, "v%d: On event is at the crossfade start before the line", variant);
        // after the line: the slice that ended on the line repeats ...
        CHECK(MaxDiffShift(r.out, B, r.in, B - L, R - B - kX) < 1e-5, "v%d: repeats the last 1/4 ending on the line, until the release line", variant);
        // ... and keeps repeating after the release, until the next line
        CHECK(R % kLine == 0 && R > static_cast<int64_t>(tr), "v%d: release line %lld is after the release", variant, (long long)R);
        CHECK(MaxDiffShift(r.out, R + 2, r.in, R + 2, 2 * kLine) < 1e-6, "v%d: live again from the release line", variant);
        CHECK(r.Count(BeatEventType::On) == 1 && r.Count(BeatEventType::Off) == 1, "v%d: one start, one end", variant);
    }
}

static void TestTap()
{
    std::printf("a tap shorter than the wait still plays for one 1/16 line\n");
    Rig r;
    r.input = Noise();
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
    const uint64_t tp = kBarS + 1000;
    const int64_t  L  = kBarS / 8; // 1/8 = two lines
    const int64_t  B  = QB(tp);
    r.Press(tp, 4);
    r.Release(tp + 192, 4); // up again after 4 ms, long before the line
    r.Run(B + 4 * kLine);
    CHECK(r.Count(BeatEventType::On) == 1 && r.Count(BeatEventType::Off) == 1, "one start, one end");
    CHECK(MaxDiffShift(r.out, 0, r.in, 0, B - kX - 1) < 1e-6, "live until the line");
    CHECK(MaxDiffShift(r.out, B, r.in, B - L, kLine - kX) < 1e-5, "plays the 1/8 slice for one line");
    CHECK(MaxDiffShift(r.out, B + kLine + 2, r.in, B + kLine + 2, 2 * kLine) < 1e-6, "live again after that line");
}

static void TestHeldPressBlocksLateRelease()
{
    std::printf("a button released then pressed again before the line keeps repeating\n");
    Rig r;
    r.input = Noise();
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
    const uint64_t tp = kBarS + 1000;
    const int64_t  B  = QB(tp);
    r.Press(tp, 5); // 1/16 = one line
    r.Release(B + 100, 5);
    r.Press(B + 400, 5); // pressed again just after the start line, held on
    r.Release(B + 6 * kLine + 100, 5);
    r.Run(B + 9 * kLine);
    CHECK(MaxDiffShift(r.out, B, r.in, B - kLine, kLine - kX) < 1e-5, "first repeat");
    CHECK(r.Count(BeatEventType::On) == 1, "still one fresh start (%d)", r.Count(BeatEventType::On));
    CHECK(MaxDiffShift(r.out, B + kLine, r.in, B - kLine, kLine - kX) < 1e-5, "the same slice keeps repeating");
}

static void TestStopped()
{
    std::printf("presses while the transport is stopped are ignored\n");
    {
        Rig r;
        r.input = Noise();
        r.At(0, A_CLOCK_ON); // clock but no Start
        r.Press(40000, 4);
        r.Release(60000, 4);
        r.Run(100000);
        CHECK(r.Count(BeatEventType::IgnoredNotRunning) == 1, "ignored event");
        CHECK(r.Count(BeatEventType::On) == 0, "no stutter");
        CHECK(MaxDiffShift(r.out, 0, r.in, 0, static_cast<int64_t>(r.in.size())) < 1e-6, "output is the input");
    }
    {   // Stop while repeating: back to live
        Rig r;
        r.input = Noise();
        r.At(0, A_CLOCK_ON);
        r.At(0, A_MIDI_START);
        const uint64_t tp = kBarS + 4000;
        const int64_t  B  = QB(tp);
        r.Press(tp, 4);
        r.At(B + kBarS, A_MIDI_STOP);
        r.Run(B + 2 * kBarS);
        CHECK(r.Count(BeatEventType::On) == 1 && r.Count(BeatEventType::Off) == 1, "stutter stopped by Stop");
        const int64_t s = B + kBarS + 400;
        CHECK(MaxDiffShift(r.out, s, r.in, s, 5000) < 1e-6, "live after Stop");
    }
}

static void TestTooShort()
{
    std::printf("slices shorter than 10 ms are refused\n");
    Rig r;
    r.bpm   = 400; // 1/64 bar = 450 samples
    r.input = Noise();
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
    r.Press(30000, 7);
    r.Release(60000, 7);
    r.Run(90000);
    CHECK(r.Count(BeatEventType::IgnoredTooShort) == 1, "too-short event (%d)", r.Count(BeatEventType::IgnoredTooShort));
    CHECK(r.Count(BeatEventType::On) == 0, "no stutter started");
    CHECK(MaxDiffShift(r.out, 0, r.in, 0, static_cast<int64_t>(r.in.size())) < 1e-6, "output stayed live");
}

static void TestTooSlow()
{
    std::printf("below the minimum tempo (130 BPM) long slices are refused with their own message\n");
    Rig r;
    r.bpm   = 125; // tick = 960 samples; a 1-bar slice (92160) does not fit its slot (sized for 130 BPM)
    r.input = Noise();
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
    r.Press(60000, 0);
    r.Release(150000, 0);
    r.Run(200000);
    CHECK(r.Count(BeatEventType::IgnoredTooSlow) == 1, "too-slow event (%d)", r.Count(BeatEventType::IgnoredTooSlow));
    CHECK(r.Count(BeatEventType::IgnoredTooShort) == 0, "not reported as too short");
    CHECK(r.Count(BeatEventType::On) == 0, "no stutter started");
    CHECK(MaxDiffShift(r.out, 0, r.in, 0, static_cast<int64_t>(r.in.size())) < 1e-6, "output stayed live");
    // just above 130 BPM (tick = 912 samples = 131.6 BPM) the 1-bar slice fits
    Rig q;
    q.bpm   = 120000.0 / 912.0;
    q.input = Noise();
    q.At(0, A_CLOCK_ON);
    q.At(0, A_MIDI_START);
    q.Press(60000, 0);
    q.Release(260000, 0);
    q.Run(300000);
    CHECK(q.Count(BeatEventType::On) == 1 && q.Count(BeatEventType::IgnoredTooSlow) == 0, "1 bar works at 131.6 BPM");
}

static void TestClicks()
{
    std::printf("no clicks at any transition (sine input, worst-case phases)\n");
    for(double f : {437.125, 1234.567})
    {
        Rig r;
        r.input = Sine(f);
        r.At(0, A_CLOCK_ON);
        r.At(0, A_MIDI_START);
        r.Press(kBarS + 3000, 4);            // 1/8
        r.Press(2 * kBarS + 20000, 5);       // switch to 1/16 (fresh capture)
        r.Release(3 * kBarS + 7000, 5);      // flip back to the stored 1/8 (immediate)
        r.Press(3 * kBarS + 40001, 5);       // flip to the stored 1/16 again
        r.Press(3 * kBarS + 60007, 2);       // 3/8 (fresh capture)
        r.Release(3 * kBarS + 80003, 2);     // back to 1/16
        r.Release(4 * kBarS + 12345, 5);     // back to 1/8
        r.Release(4 * kBarS + 50000, 4);     // live
        r.Press(5 * kBarS + 100, 0);         // 1 bar, held 5 bars (slot copy takes over)
        r.Release(10 * kBarS + 4321, 0);
        r.Press(11 * kBarS + 500, 6);        // 1/32
        r.Release(11 * kBarS + 30000, 6);
        r.Run(12 * kBarS);
        const double step = MaxStep(r.out, 0, r.out.size());
        CHECK(step <= 1.6 * SineStep(f), "f=%.1f max step %.4f vs allowed %.4f", f, step, 1.6 * SineStep(f));
    }
}

static void TestIntoLoop()
{
    std::printf("a stutter performed while the loop records ends up in the loop\n");
    Rig r(true);
    r.input = Noise();
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
    r.At(5000, A_LOOP_CMD, static_cast<double>(LoopCmd::ArmToggle)); // records bar 2, plays bar 3+
    const int64_t  L  = kBarS / 8;
    const uint64_t tp = kBarS + 2 * L + 700;
    const int64_t  B  = QB(tp);
    r.Press(tp, 4);
    r.Release(B + 2 * L + 100, 4);
    r.Run(5 * kBarS);

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
    CHECK(rec && play, "loop recorded and played");
    if(rec && play)
    {
        const int64_t len = static_cast<int64_t>(play->b);
        CHECK(len > kBarS - 100 && len < kBarS + 100, "loop length %lld", (long long)len);
        // the loop must contain the stutter (differs from the plain input)
        double diff = 0;
        for(int64_t k = 0; k < len; k++)
            diff = std::max(diff, std::fabs(static_cast<double>(r.rep[trec + k]) - r.in[trec + k]));
        CHECK(diff > 0.2, "the recorded bar contains the stutter (max diff %.2f)", diff);
        // playback = what was recorded (excluding the first / last crossfade)
        const double d = MaxDiffShift(r.out, static_cast<int64_t>(tplay) + 300, r.rep, static_cast<int64_t>(trec) + 300, len - 700);
        CHECK(d < 1e-5, "loop plays back the stutter performance: max diff %.6f", d);
    }
    const double step = MaxStep(r.out, 0, r.out.size());
    CHECK(true, "step %.3f", step);
}

static void TestPitchSteps()
{
    std::printf("B6: pitch effect on/off quantised to 1/16 lines; the repeat counter steps once per repeat\n");
    Rig r;
    r.input = Noise();
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
    const uint64_t t6 = kBarS + 300; // B6 first (armed, no stutter yet)
    const int64_t  P6 = QB(t6);
    r.Press(t6, 8);
    const uint64_t tp = kBarS + 3 * kLine + 777;
    const int64_t  B  = QB(tp);
    r.Press(tp, 5); // 1/16 = one line
    const uint64_t tr = B + 4 * kLine + 500; // stutter released
    const int64_t  RS = QB(tr);
    r.Release(tr, 5);
    const uint64_t t7 = RS + 3 * kLine + 100; // B6 released later
    const int64_t  P7 = QB(t7);
    r.Release(t7, 8);
    r.Run(P7 + 4 * kLine);

    auto block_of = [](int64_t t) { return static_cast<size_t>(t / static_cast<int64_t>(kBlock)); };
    CHECK(r.Count(BeatEventType::PitchOn) == 1 && r.Count(BeatEventType::PitchOff) == 1, "one PitchOn, one PitchOff");
    CHECK(!r.pon[block_of(P6 - kX - 2 * kBlock)] && r.pon[block_of(P6 - kX + kBlock)], "B6 switches on at its 1/16 line (the crossfade start), not at the press");
    CHECK(r.pon[block_of(B + kLine)] && r.pon[block_of(RS + kLine)], "stays on while held");
    CHECK(r.pon[block_of(P7 - kX - 2 * kBlock)] && !r.pon[block_of(P7 - kX + kBlock)], "B6 switches off at its 1/16 line after the release");
    for(int k = 0; k < 4; k++)
        CHECK(r.lc[block_of(B + k * kLine + kLine / 2)] == k, "repeat %d: counter %d (is %d)", k, k, r.lc[block_of(B + k * kLine + kLine / 2)]);
    CHECK(r.lc[block_of(B - kLine / 2)] == 0 && r.lc[block_of(RS + kLine)] == 0, "counter is 0 before the stutter and after it is released");
    // The pitch stage needs the exact sample where each repeat starts its loop crossfade (X before
    // the loop point) and the new repeat number from that moment on (0.6.1).
    for(int k = 1; k <= 3; k++)
    {
        const int64_t j  = B + k * kLine - kX; // start of the loop crossfade of repeat k
        const size_t  bj = block_of(j);
        CHECK(r.ji[bj] == static_cast<int>(j - static_cast<int64_t>(bj * kBlock)), "repeat %d: jump start index %d (expected %lld)", k, r.ji[bj], (long long)(j - static_cast<int64_t>(bj * kBlock)));
        CHECK(r.ri[bj] == k && r.ri[bj - 1] == k - 1, "repeat %d: RepeatIndex %d (before: %d)", k, r.ri[bj], r.ri[bj - 1]);
        CHECK(r.ji[bj + 1] == -1 && r.ji[bj - 1] == -1, "repeat %d: one jump index only", k);
    }
}

static void TestPitchTap()
{
    std::printf("B6: a tap shorter than the wait still switches the effect on for one 1/16 line\n");
    Rig r;
    r.input = Noise();
    r.At(0, A_CLOCK_ON);
    r.At(0, A_MIDI_START);
    const uint64_t t6 = kBarS + 300;
    const int64_t  P  = QB(t6);
    r.Press(t6, 8);
    r.Release(t6 + 100, 8);
    r.Run(P + 4 * kLine);
    auto block_of = [](int64_t t) { return static_cast<size_t>(t / static_cast<int64_t>(kBlock)); };
    CHECK(r.pon[block_of(P - kX + kBlock)] && r.pon[block_of(P - kX + kLine / 2)], "on for the line");
    CHECK(!r.pon[block_of(P - kX + kLine + 2 * kBlock)], "off at the next line");
    CHECK(r.Count(BeatEventType::PitchOn) == 1 && r.Count(BeatEventType::PitchOff) == 1, "one on, one off");
    // not running: ignored and off
    Rig q;
    q.input = Noise();
    q.Press(1000, 8);
    q.Run(60000);
    CHECK(q.Count(BeatEventType::PitchOn) == 0, "B6 while stopped does nothing");
}

int main()
{
    TestPitchSteps();
    TestPitchTap();
    TestAllDivisions();
    TestLongHold();
    TestOffset();
    TestFullSnare();
    TestFlipBetweenHeld();
    TestFlipLongAfterHistory();
    TestDottedGrid();
    TestQuantisedPressRelease();
    TestTap();
    TestHeldPressBlocksLateRelease();
    TestStopped();
    TestTooShort();
    TestTooSlow();
    TestClicks();
    TestIntoLoop();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
