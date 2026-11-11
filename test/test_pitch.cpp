// Host-side test for src/engine/pitch_fx.h (the B6 pitch-down stage). Run with `make -C test`.
// Checks: the pitch of the output is exactly 2^(-steps/12) of the input, the dry signal
// passes unchanged when not engaged, engaging/releasing makes no click, and the level stays.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../src/engine/pitch_fx.h"

static int g_checks = 0, g_fails = 0;
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

static const double kSr = 48000.0, kPi = 3.14159265358979323846;
static const size_t kBlock = 48;

/** Run `secs` of a sine through the stage; `engaged_from` = first sample where it is engaged. */
static std::vector<float> Run(double f, int steps, bool engaged, size_t total, size_t engaged_from = 0, size_t release_at = (size_t)-1)
{
    static PitchFx fx;
    fx.Init(static_cast<float>(kSr));
    std::vector<float> out;
    for(size_t t = 0; t < total; t += kBlock)
    {
        float l[kBlock], r[kBlock];
        for(size_t i = 0; i < kBlock; i++)
            l[i] = r[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * f * static_cast<double>(t + i) / kSr));
        const bool eng = engaged && t >= engaged_from && t < release_at;
        fx.Process(l, r, kBlock, steps, eng, -1);
        for(size_t i = 0; i < kBlock; i++)
            out.push_back(l[i]);
    }
    return out;
}

/** Frequency of the strongest spectral peak within +/-10% of `guess` (scans in 0.25 Hz steps
 *  over samples a..b). More reliable than counting zero crossings, which the small
 *  amplitude dips where the two heads cross-fade would disturb. */
static double Freq(const std::vector<float> &y, size_t a, size_t b, double guess)
{
    double best = 0, bf = 0;
    for(double f = guess * 0.9; f <= guess * 1.1; f += 0.25)
    {
        double re = 0, im = 0;
        const double w = 2.0 * kPi * f / kSr;
        for(size_t i = a; i < b; i++)
        {
            re += y[i] * std::cos(w * static_cast<double>(i));
            im += y[i] * std::sin(w * static_cast<double>(i));
        }
        const double m = re * re + im * im;
        if(m > best)
        {
            best = m;
            bf   = f;
        }
    }
    return bf;
}

static void TestPitchExact()
{
    std::printf("every step 1..12 gives 2^(-steps/12) of the input frequency\n");
    // A pure tone is shifted by exactly the ratio when the head hop (kPitchHopFrames) holds a
    // whole number of its cycles (the hop then changes the phase by whole cycles). For other
    // tones the hop adds a small fractional-cycle phase step, i.e. a detune of up to about
    // +/-0.5 cycle per hop (at most ~25 Hz at the deepest drop here), which music does not show.
    const double aligned = kSr / static_cast<double>(looper::kPitchHopFrames) * 10.0; // 1000 Hz: one hop = 10 cycles
    for(int s = 1; s <= 12; s++)
    {
        const double exp = aligned * std::pow(2.0, -s / 12.0);
        const auto   y   = Run(aligned, s, true, 48000);
        const double f   = Freq(y, 12000, 48000, exp);
        CHECK(std::fabs(f / exp - 1.0) < 0.004, "aligned tone, steps %d: %.1f Hz (expected %.1f)", s, f, exp);
        const double exp2 = 1234.567 * std::pow(2.0, -s / 12.0);
        const auto   y2   = Run(1234.567, s, true, 48000);
        const double f2   = Freq(y2, 12000, 48000, exp2);
        CHECK(std::fabs(f2 / exp2 - 1.0) < 0.06, "1234.567 Hz tone, steps %d: %.1f Hz (expected %.1f)", s, f2, exp2);
    }
}

static void TestBypass()
{
    std::printf("not engaged (or 0 steps): the input passes unchanged\n");
    const auto in = Run(440.0, 0, false, 9600);
    const auto a  = Run(440.0, 5, false, 9600);
    const auto b  = Run(440.0, 0, true, 9600);
    double     da = 0, db = 0;
    for(size_t i = 0; i < in.size(); i++)
    {
        const double x = 0.5 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kSr);
        da             = std::max(da, std::fabs(a[i] - x));
        db             = std::max(db, std::fabs(b[i] - x));
    }
    CHECK(da < 1e-6 && db < 1e-6, "not engaged %.7f, zero steps %.7f", da, db);
}

static void TestClicksAndLevel()
{
    std::printf("engaging and releasing makes no click; the shifted level stays\n");
    for(double f : {220.0, 1234.567})
    {
        const auto   y    = Run(f, 3, true, 96000, 20000, 60000);
        const double step = [&] { double m = 0; for(size_t i = 1; i < y.size(); i++) m = std::max(m, (double)std::fabs(y[i] - y[i - 1])); return m; }();
        const double lim  = 2.0 * kPi * f / kSr * 0.5 * 1.3 + 2.5 / 240.0;
        CHECK(step <= lim, "f=%.0f max step %.4f vs %.4f", f, step, lim);
        double peak = 0;
        for(size_t i = 30000; i < 55000; i++)
            peak = std::max(peak, (double)std::fabs(y[i]));
        CHECK(peak > 0.45 && peak < 0.56, "f=%.0f shifted peak %.3f (input 0.5)", f, peak);
    }
}

/** The point of 0.6.1: with a repeat start signalled (jump_at), the hit at the start of each repeat
 *  comes out ON the beat (within a couple of samples), not ~20 ms late, at full level. */
static void TestTightAttack()
{
    std::printf("a click at the start of each repeat stays on the beat (no flam)\n");
    static PitchFx fx;
    fx.Init(static_cast<float>(kSr));
    const int64_t L   = 5184; // repeat length (1/16 at 138.9 BPM)
    const int64_t X   = 240;
    const size_t  tot = static_cast<size_t>(8 * L + 2000);
    std::vector<float> out;
    for(size_t t = 0; t < tot; t += kBlock)
    {
        float l[kBlock], r[kBlock];
        int   jump_at = -1;
        int   steps   = 0;
        for(size_t i = 0; i < kBlock; i++)
        {
            const int64_t n = static_cast<int64_t>(t + i);
            // a click at every repeat start (k * L), a little noise-free body after it
            const int64_t m = n % L;
            const float   v = m == 0 ? 0.9f : (m < 200 ? 0.2f * std::sin(0.3f * m) : 0.0f);
            l[i] = r[i] = v;
            // the loop crossfade of repeat k starts X before k*L
            if((n + X) % L == 0 && n + X >= 2 * L)
                jump_at = static_cast<int>(i);
        }
        // repeat index = number of loop crossfades started so far (1 from the 2nd repeat on)
        steps = static_cast<int>((static_cast<int64_t>(t) + static_cast<int64_t>(kBlock) - 1 + X) / L) - 1;
        if(steps < 0)
            steps = 0;
        // (steps is computed for the end of the block, as BeatRepeat::RepeatIndex() would be)
        fx.Process(l, r, kBlock, steps, true, jump_at);
        for(size_t i = 0; i < kBlock; i++)
            out.push_back(l[i]);
    }
    for(int k = 3; k <= 7; k++)
    {
        // find the largest sample within +/- 60 samples of k * L
        int64_t best = -1;
        float   bv   = 0;
        for(int64_t n = k * L - 60; n < k * L + 60; n++)
            if(std::fabs(out[static_cast<size_t>(n)]) > bv)
            {
                bv   = std::fabs(out[static_cast<size_t>(n)]);
                best = n;
            }
        CHECK(std::llabs(best - k * L) <= 3 && bv > 0.6f, "repeat %d: click at %+lld samples from the beat, level %.2f", k, (long long)(best - k * L), bv);
    }
}

/** Slow repeats (1/4 bar) held for a long time (big drop): a click in the MIDDLE of the repeat must
 *  not come out more than about kPitchMaxDelayFrames late. */
static void TestSlowRepeatStaysInTime()
{
    std::printf("slow repeat, deep drop: clicks in the middle of the repeat are at most ~12 ms late (10 ms + hop crossfade)\n");
    const int64_t L = 20736; // 1/4 bar at 138.9 BPM
    const int64_t X = 240;
    for(int steps : {2, 5, 8, 12})
    {
        static PitchFx fx;
        fx.Init(static_cast<float>(kSr));
        std::vector<float> out;
        const size_t tot = static_cast<size_t>(4 * L);
        for(size_t t = 0; t < tot; t += kBlock)
        {
            float l[kBlock], r[kBlock];
            int   jump_at = -1;
            for(size_t i = 0; i < kBlock; i++)
            {
                const int64_t n = static_cast<int64_t>(t + i);
                const int64_t m = n % L;
                l[i] = r[i] = (m % 1500 == 0 && m > 0) ? 0.9f : 0.0f; // clicks every 1500 samples
                if((n + X) % L == 0 && n >= L)
                    jump_at = static_cast<int>(i);
            }
            fx.Process(l, r, kBlock, steps, true, jump_at);
            for(size_t i = 0; i < kBlock; i++)
                out.push_back(l[i]);
        }
        int    bad = 0, found = 0, total = 0;
        int    worst = 0;
        for(int k = 2; k <= 3; k++)
            for(int64_t m = 1500; m < L - 1500; m += 1500)
            {
                const int64_t pos = k * L + m;
                int64_t best = -1;
                float   bv   = 0;
                for(int64_t n = pos - 5; n < pos + 600; n++)
                    if(std::fabs(out[static_cast<size_t>(n)]) > bv)
                    {
                        bv   = std::fabs(out[static_cast<size_t>(n)]);
                        best = n;
                    }
                total++;
                // Lowering the pitch while keeping the length means skipping some of the audio
                // (about half of it at -12), so a 1-sample click can fall in a skipped part and
                // vanish. Only the clicks that do come through are checked for lag.
                if(bv < 0.15f)
                    continue;
                found++;
                const int lag = static_cast<int>(best - pos);
                worst         = std::max(worst, lag);
                if(lag > static_cast<int>(looper::kPitchMaxDelayFrames) + 130) // + the drift during the 5 ms hop crossfade
                    bad++;
            }
        CHECK(bad == 0 && found * 3 >= total, "steps %d: %d of %d clicks late (worst lag %d samples), %d came through", steps, bad, total, worst, found);
    }
}

int main()
{
    TestSlowRepeatStaysInTime();
    TestTightAttack();
    TestPitchExact();
    TestBypass();
    TestClicksAndLevel();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
