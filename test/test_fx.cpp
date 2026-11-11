// Host-side test for layer 2 (src/engine/fx/): shared stage, bit crusher, button/latch/quantise logic,
// crossfades. Run with `make -C test`.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../src/engine/fx/fx_engine.h"
#include "../src/engine/fx/fx_common.h"
#include "../src/engine/fx/fx_bitcrush.h"
#include "../src/engine/fx/fx_layer.h"
#include "../src/engine/fx/fx_reverb_bus.h"

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

static const float  kSr = 48000.0f;
static const double kPi = 3.14159265358979323846;
static const size_t kBlock = 48;

/** A test fx: output = input * factor (0 = silence). Lets us see exactly when an fx is on. */
struct Scale : public fx::Engine
{
    float factor;
    explicit Scale(float f) : factor(f) {}
    void        Init(float) override {}
    void        Reset() override {}
    void        Process(const float* il, const float* ir, float* ol, float* orr, size_t n, const float*) override
    {
        for(size_t i = 0; i < n; i++)
            ol[i] = il[i] * factor, orr[i] = ir[i] * factor;
    }
    const char* KnobName(int) const override { return nullptr; }
    float       KnobReal(int, float v) const override { return v; }
    const char* KnobUnit(int) const override { return ""; }
};

static float Rms(const std::vector<float>& v, size_t a, size_t b)
{
    double s = 0;
    for(size_t i = a; i < b; i++)
        s += static_cast<double>(v[i]) * v[i];
    return static_cast<float>(std::sqrt(s / static_cast<double>(b - a)));
}

static std::vector<float> Sine(double f, size_t n, double amp = 0.5)
{
    std::vector<float> v(n);
    for(size_t i = 0; i < n; i++)
        v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * f * static_cast<double>(i) / kSr));
    return v;
}

static std::vector<float> RunStage(fx::SlotSettings s, const std::vector<float>& in, const std::vector<float>& fxsig)
{
    fx::Stage st;
    st.Init(kSr);
    std::vector<float> out(in.size());
    for(size_t t = 0; t + kBlock <= in.size(); t += kBlock)
    {
        float l[kBlock];
        st.Process(&in[t], &in[t], &fxsig[t], &fxsig[t], l, l, kBlock, s);
        for(size_t i = 0; i < kBlock; i++)
            out[t + i] = l[i];
    }
    return out;
}

static void TestStage()
{
    std::printf("shared stage\n");
    const auto in = Sine(1000, 48000);
    fx::SlotSettings s = fx::DefaultSettings();
    auto o = RunStage(s, in, in);
    CHECK(std::fabs(Rms(o, 4800, 48000) / Rms(in, 4800, 48000) - 1.0f) < 0.001f, "defaults pass the fx through unchanged");

    s.v[fx::kGain] = 1.0f;
    o = RunStage(s, in, in);
    CHECK(std::fabs(20 * std::log10(Rms(o, 4800, 48000) / Rms(in, 4800, 48000)) - 12.0f) < 0.05f, "gain knob full = +12 dB");
    s.v[fx::kGain] = 0.0f;
    o = RunStage(s, in, in);
    CHECK(std::fabs(20 * std::log10(Rms(o, 4800, 48000) / Rms(in, 4800, 48000)) + 12.0f) < 0.05f, "gain knob zero = -12 dB");

    s = fx::DefaultSettings();
    s.v[fx::kWet] = 0.0f;
    std::vector<float> silent(in.size(), 0.0f);
    o = RunStage(s, in, silent);
    CHECK(std::fabs(Rms(o, 4800, 48000) / Rms(in, 4800, 48000) - 1.0f) < 0.001f, "wet 0%% = only the dry signal");
    s.v[fx::kWet] = 0.5f;
    o = RunStage(s, in, silent);
    CHECK(std::fabs(Rms(o, 4800, 48000) / Rms(in, 4800, 48000) - 0.5f) < 0.001f, "wet 50%% of silence + dry = half the dry");

    // Hipass: knob 0.6 = 20*100^0.6 = 317 Hz. 100 Hz must drop a lot, 5 kHz must pass.
    s = fx::DefaultSettings();
    s.v[fx::kHp] = 0.6f;
    auto lo = Sine(100, 48000), hi = Sine(5000, 48000);
    CHECK(Rms(RunStage(s, lo, lo), 4800, 48000) < 0.2f * Rms(lo, 4800, 48000), "hipass cuts 100 Hz");
    CHECK(std::fabs(Rms(RunStage(s, hi, hi), 4800, 48000) / Rms(hi, 4800, 48000) - 1.0f) < 0.02f, "hipass passes 5 kHz");
    // The filter acts on the whole mix: with wet 0 the dry signal is filtered too.
    s.v[fx::kWet] = 0.0f;
    CHECK(Rms(RunStage(s, lo, silent), 4800, 48000) < 0.2f * Rms(lo, 4800, 48000), "hipass also filters the dry part of the mix");
    // No start-up thump: a steady level fed to a fresh hipass gives (nearly) nothing out at once.
    s.v[fx::kWet] = 1.0f;
    std::vector<float> dc(4800, 0.5f);
    auto od = RunStage(s, dc, dc);
    float first = 0;
    for(size_t i = 0; i < 200; i++)
        first = std::max(first, std::fabs(od[i]));
    CHECK(first < 0.01f, "hipass starts without a thump (%.4f)", first);
    // Lopass: knob 0.4 = 200*100^0.4 = 1264 Hz.
    s = fx::DefaultSettings();
    s.v[fx::kLp] = 0.4f;
    CHECK(Rms(RunStage(s, hi, hi), 4800, 48000) < 0.2f * Rms(hi, 4800, 48000), "lopass cuts 5 kHz");
    CHECK(std::fabs(Rms(RunStage(s, lo, lo), 4800, 48000) / Rms(lo, 4800, 48000) - 1.0f) < 0.02f, "lopass passes 100 Hz");
}

static void TestBitCrush()
{
    std::printf("bit crusher\n");
    fx::BitCrush bc;
    bc.Init(kSr);
    bc.smooth_flanger = false; // check the crush maths on their own first
    const auto in = Sine(300, 4800);
    const size_t N = 4800;
    std::vector<float> o(N), r(N);
    // knobs: 0 pre lopass (1 = off), 1 noise, 2 bit depth, 3 sample rate
    float knobs[4] = {1.0f, 0.0f, 1.0f, 0.0f}; // 1 bit, no noise, no hold
    bc.Process(in.data(), in.data(), o.data(), r.data(), N, knobs);
    bool on_grid = true;
    for(float x : o)
        if(std::fabs(x - std::round(x)) > 1e-4f)
            on_grid = false;
    CHECK(on_grid, "1-bit output only takes whole steps (-1, 0, 1)");

    knobs[2] = 0.0f; // 12 bits
    bc.Process(in.data(), in.data(), o.data(), r.data(), N, knobs);
    float worst = 0;
    for(size_t i = 0; i < N; i++)
        worst = std::max(worst, std::fabs(o[i] - in[i]));
    CHECK(worst < 0.0003f, "12 bits is nearly transparent (error %g)", worst);

    // bit depth: the knob must make an audible difference in the first half of its travel too
    CHECK(fx::BitCrush::Bits(0.5f) < 6.0f && fx::BitCrush::Bits(0.25f) < 9.0f, "depth curve: %.1f bits at half, %.1f at a quarter", fx::BitCrush::Bits(0.5f), fx::BitCrush::Bits(0.25f));

    // noise: 0 = none; more noise = more difference from a clean 12-bit signal, and it scales with the crush
    float err[3];
    const float noise_k[3] = {0.0f, 0.5f, 1.0f};
    for(int t = 0; t < 3; t++)
    {
        knobs[1] = noise_k[t];
        knobs[2] = 0.0f;
        bc.Reset();
        bc.Process(in.data(), in.data(), o.data(), r.data(), N, knobs);
        double e = 0;
        for(size_t i = 0; i < N; i++)
            e += (o[i] - in[i]) * (o[i] - in[i]);
        err[t] = static_cast<float>(std::sqrt(e / N));
    }
    CHECK(err[0] < 0.0003f && err[1] > 0.002f && err[2] > err[1], "noise knob adds noise (%.5f, %.5f, %.5f)", err[0], err[1], err[2]);
    knobs[2] = 0.7f; // a crushed signal gets more noise than a clean one
    knobs[1] = 1.0f;
    bc.Reset();
    bc.Process(in.data(), in.data(), o.data(), r.data(), N, knobs);
    double e2 = 0;
    for(size_t i = 0; i < N; i++)
        e2 += (o[i] - in[i]) * (o[i] - in[i]);
    CHECK(std::sqrt(e2 / N) > 2.0 * err[2], "noise is stronger at a lower bit depth");

    // sample hold
    knobs[1] = 0.0f;
    knobs[2] = 0.0f;
    knobs[3] = 1.0f; // hold 256
    bc.Reset();
    bc.Process(in.data(), in.data(), o.data(), r.data(), N, knobs);
    bool held = true;
    for(size_t i = 0; i < N; i++)
        if(o[i] != o[(i / 256) * 256])
            held = false;
    CHECK(held, "sample rate knob full = each value held for 256 samples");
    CHECK(fx::BitCrush::Hold(0.0f) == 1 && fx::BitCrush::Hold(0.5f) == 16 && fx::BitCrush::Hold(1.0f) == 256, "sample rate steps by octaves (1, 16, 256)");

    // pre lopass: knob 0.4 = 1264 Hz cuts 5 kHz, passes 100 Hz
    knobs[0] = 0.4f;
    knobs[3] = 0.0f;
    const auto hi = Sine(5000, N), lo = Sine(100, N);
    bc.Reset();
    bc.Process(hi.data(), hi.data(), o.data(), r.data(), N, knobs);
    const float r_hi = Rms(o, 480, N) / Rms(hi, 480, N);
    bc.Reset();
    bc.Process(lo.data(), lo.data(), o.data(), r.data(), N, knobs);
    const float r_lo = Rms(o, 480, N) / Rms(lo, 480, N);
    CHECK(r_hi < 0.2f && std::fabs(r_lo - 1.0f) < 0.03f, "pre lopass cuts 5 kHz (%.3f) and passes 100 Hz (%.3f)", r_hi, r_lo);

    // mono + flanger
    fx::BitCrush mono;
    mono.Init(kSr);
    mono.smooth_flanger = false;
    float k2[4] = {1.0f, 0.0f, 0.0f, 0.0f}; // clean: 12 bits, full rate
    std::vector<float> zero(N, 0.0f), ol(N), orr(N);
    mono.Process(in.data(), zero.data(), ol.data(), orr.data(), N, k2);
    float diff = 0;
    for(size_t i = 0; i < N; i++)
        diff = std::max(diff, std::fabs(ol[i] - orr[i]));
    CHECK(diff == 0.0f && Rms(ol, 480, N) > 0.1f, "the crusher is mono: a left-only input comes out equally on both sides");

    fx::BitCrush sm;
    sm.Init(kSr);
    const auto loud = Sine(300, 48000);
    std::vector<float> fl(48000), fr(48000);
    sm.Process(loud.data(), loud.data(), fl.data(), fr.data(), 48000, k2);
    // the two sides move differently (stereo width), but neither is far from the other in level
    float width = 0;
    for(size_t i = 4800; i < 48000; i++)
        width = std::max(width, std::fabs(fl[i] - fr[i]));
    CHECK(width > 0.02f, "left and right differ a little (width %.3f)", width);
    // stable on noise: no blow-up with the feedback
    std::vector<float> nz(48000);
    unsigned           seed = 12345;
    for(auto& x : nz)
    {
        seed = seed * 1664525u + 1013904223u;
        x    = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.9f;
    }
    sm.Reset();
    sm.Process(nz.data(), nz.data(), fl.data(), fr.data(), 48000, k2);
    const float level = Rms(fl, 4800, 48000) / Rms(nz, 4800, 48000);
    CHECK(level > 0.7f && level < 1.3f, "mild flanger keeps the level about the same on noise (x%.2f)", level);
    float peak = 0;
    for(float x : fl)
        peak = std::max(peak, std::fabs(x));
    CHECK(peak < 1.6f, "flanger stays stable on noise (peak %.2f)", peak);
    // smooth sweep: no clicks (biggest sample step of a 300 Hz sine stays close to the plain sine's)
    sm.Reset();
    sm.Process(loud.data(), loud.data(), fl.data(), fr.data(), 48000, k2);
    float step_max = 0;
    for(size_t i = 4801; i < 48000; i++)
        step_max = std::max(step_max, std::fabs(fl[i] - fl[i - 1]));
    const float plain = static_cast<float>(2 * kPi * 300 / kSr * 0.5);
    CHECK(step_max < plain * 1.6f, "flanger sweep makes no clicks (step %.4f vs %.4f)", step_max, plain);
}

/** Drives a Layer in blocks of 48 with a fake clock. Output goes to `out`. */
struct Rig
{
    fx::Layer         layer;
    fx::SlotSettings  settings[fx::kNumSlots];
    Scale             mute{0.0f}, half{0.5f};
    fx::BlockInfo     info;
    std::vector<float> in, out;
    size_t            t = 0;

    Rig(bool running)
    {
        for(auto& s : settings)
            s = fx::DefaultSettings();
        layer.Init(kSr, settings);
        layer.SetEngine(0, &mute); // slot 0 = silences the signal
        layer.SetEngine(1, &half); // slot 1 = halves it
        info.running         = running;
        info.samples_per_bar = 88615.0f; // 130 BPM
        info.pos_in_bar      = 100.0f;
        info.buttons_active  = true;
        in                   = Sine(440, 48000 * 3);
    }
    void Blocks(size_t count)
    {
        for(size_t b = 0; b < count; b++)
        {
            float l[kBlock], r[kBlock];
            layer.Process(info, &in[t], &in[t], l, r, kBlock);
            for(size_t i = 0; i < kBlock; i++)
                out.push_back(l[i]);
            t += kBlock;
            info.pos_in_bar += kBlock;
        }
    }
    /** Sample index of the first output that differs from the input by more than 1e-6 */
    long FirstChange(size_t from = 0) const
    {
        for(size_t i = from; i < out.size(); i++)
            if(std::fabs(out[i] - in[i]) > 1e-6f)
                return static_cast<long>(i);
        return -1;
    }
};

static void TestQuantisedStartStop()
{
    std::printf("button -> quantised start / stop\n");
    const double spl = 88615.0 / 16.0; // one 1/16 of the bar (5538 samples)
    {
        Rig r(true);
        r.Blocks(3);                // clean start
        r.info.raw_mask = 1u << 0;  // hold slot 0 (mutes)
        r.Blocks(200);
        const long first = r.FirstChange();
        // pos at sample 0 is 100; the first line after the press is at pos = spl
        const long expect = static_cast<long>(std::ceil(spl - 100.0));
        CHECK(first >= expect - 1 && first <= expect + 2, "fx starts on the 1/16 line (got %ld, expected %ld)", first, expect);
        CHECK(Rms(r.out, r.out.size() - 480, r.out.size()) < 1e-6f, "fx is fully on a little later");
        // release; it should stop at the next line
        r.info.raw_mask = 0;
        const size_t before = r.out.size();
        r.Blocks(200);
        // find first sample after release where output returns to dry: scan for rms > 0
        size_t back = 0;
        for(size_t i = before; i < r.out.size(); i++)
            if(std::fabs(r.out[i]) > 1e-4f)
            {
                back = i;
                break;
            }
        const double pos_at_release = 100.0 + static_cast<double>(before);
        const double next_line      = std::ceil(pos_at_release / spl) * spl;
        const long   want           = static_cast<long>(next_line - pos_at_release) + static_cast<long>(before);
        CHECK(back > 0 && std::labs(static_cast<long>(back) - want) < 20, "fx ends on the next 1/16 line (got %zu, expected ~%ld)", back, want);
    }
    {
        Rig r(true);
        r.Blocks(3);
        r.info.raw_mask = 1u << 0; // a tap: down for 3 blocks only (3 ms)
        r.Blocks(3);
        r.info.raw_mask = 0;
        r.Blocks(400);
        long first = r.FirstChange();
        CHECK(first > 0, "a short tap still starts the fx at the line");
        // and it stops at the following line
        size_t back = 0;
        for(size_t i = static_cast<size_t>(first) + 2000; i < r.out.size(); i++)
            if(std::fabs(r.out[i]) > 1e-4f)
            {
                back = i;
                break;
            }
        CHECK(back > 0 && static_cast<double>(back - static_cast<size_t>(first)) < spl + 300, "tap lasts about one 1/16 (%zu samples)", back - static_cast<size_t>(first));
    }
    {
        Rig r(false); // no clock: at once
        r.info.raw_mask = 1u << 0;
        r.Blocks(20);
        CHECK(Rms(r.out, 480, 960) < 1e-6f, "no clock: fx starts at once");
    }
}

static void TestLatchAndLayer()
{
    std::printf("latch, newest wins, layer gating\n");
    Rig r(false);
    r.info.shift    = true;
    r.info.raw_mask = 1u << 0; // Shift + slot 0
    r.Blocks(10);
    r.info.raw_mask = 0;
    r.info.shift    = false;
    r.Blocks(20);
    CHECK(Rms(r.out, r.out.size() - 480, r.out.size()) < 1e-6f, "Shift + button latches the fx on after release");
    CHECK(r.layer.GetStatus().latch == 0 && r.layer.GetStatus().edit == 0, "latch and edit slot are slot 0");

    // layer deselected: presses ignored, latch keeps running
    r.info.buttons_active = false;
    r.info.raw_mask       = 1u << 1;
    r.Blocks(10);
    CHECK(r.layer.GetStatus().edit == 0, "buttons ignored while layer 2 is not selected");
    CHECK(Rms(r.out, r.out.size() - 480, r.out.size()) < 1e-6f, "latched fx keeps running on another layer");
    r.info.buttons_active = true;
    r.info.raw_mask       = 0;
    r.Blocks(10);

    // hold slot 1 over the latch: newest wins; release goes back to the latch
    r.info.raw_mask = 1u << 1;
    r.Blocks(20);
    const float rms_half = Rms(r.out, r.out.size() - 480, r.out.size()) / Rms(r.in, r.out.size() - 480, r.out.size());
    CHECK(std::fabs(rms_half - 0.5f) < 0.01f, "held button overrides the latch (ratio %.3f)", rms_half);
    CHECK(r.layer.GetStatus().edit == 1, "edit slot follows the last button");
    r.info.raw_mask = 0;
    r.Blocks(20);
    CHECK(Rms(r.out, r.out.size() - 480, r.out.size()) < 1e-6f, "after release the latch is back");

    // Shift + same button again = off
    r.info.shift    = true;
    r.info.raw_mask = 1u << 0;
    r.Blocks(10);
    r.info.raw_mask = 0;
    r.info.shift    = false;
    r.Blocks(20);
    const float ratio = Rms(r.out, r.out.size() - 480, r.out.size()) / Rms(r.in, r.out.size() - 480, r.out.size());
    CHECK(std::fabs(ratio - 1.0f) < 0.01f, "Shift + button again turns the latch off");

    // unbuilt slot
    r.info.raw_mask = 1u << 5;
    r.Blocks(5);
    fx::Event e;
    bool      saw = false;
    while(r.layer.PopEvent(e))
        if(e.type == fx::EventType::NotBuilt && e.slot == 5)
            saw = true;
    CHECK(saw, "pressing a slot with no fx gives a 'not built' event");
    CHECK(r.layer.GetStatus().edit == 0, "an unbuilt slot does not become the edit slot");
}

static void TestNoClicks()
{
    std::printf("no clicks when an fx starts, switches or stops\n");
    Rig r(false);
    r.Blocks(5);
    r.info.raw_mask = 1u << 1; // halve
    r.Blocks(30);
    r.info.raw_mask = 1u << 0; // switch to mute
    r.Blocks(30);
    r.info.raw_mask = 0;       // stop
    r.Blocks(30);
    float worst = 0;
    for(size_t i = 1; i < r.out.size(); i++)
        worst = std::max(worst, std::fabs(r.out[i] - r.out[i - 1]));
    const float normal = static_cast<float>(2 * kPi * 440 / kSr * 0.5);
    CHECK(worst < normal * 1.6f, "biggest step %.4f vs %.4f for the plain sine", worst, normal);
}

/** A simple stand-in reverb for the Mac tests: one echo (100 ms) that feeds back by the "time" setting.
 *  out = 0.5 * in + echo. With feedback below 1 the tail dies away. */
struct EchoCore : public fx::ReverbCore
{
    static const size_t kLen = 4800;
    float               bl[kLen] = {}, br[kLen] = {};
    size_t              pos = 0;
    float               fb  = 0.5f;
    void  Init(float) override {}
    void  SetFeedback(float f) override { fb = f; }
    void  SetToneHz(float) override {}
    void  Process(float il, float ir, float* ol, float* orr) override
    {
        const float el = bl[pos], er = br[pos];
        bl[pos] = il + fb * el;
        br[pos] = ir + fb * er;
        pos     = (pos + 1) % kLen;
        *ol     = 0.5f * il + el;
        *orr    = 0.5f * ir + er;
    }
};

static void TestReverbBus()
{
    std::printf("reverb bus\n");
    {
        // input diffusion: an allpass keeps the level and smears a click into a dense burst
        static fx::Allpass ap;
        ap.Init(142, 0.7f);
        double e_in = 0, e_out = 0;
        unsigned seed = 7;
        for(int i = 0; i < 48000; i++)
        {
            seed    = seed * 1664525u + 1013904223u;
            float x = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.5f;
            const float y = ap.Process(x);
            e_in += static_cast<double>(x) * x;
            e_out += static_cast<double>(y) * y;
        }
        CHECK(std::fabs(std::sqrt(e_out / e_in) - 1.0) < 0.02, "an allpass keeps the level (x%.3f)", std::sqrt(e_out / e_in));

        static fx::ReverbBus bus;
        static EchoCore       core;
        core = EchoCore();
        core.fb = 0.0f;
        bus.Init(kSr, &core);
        fx::SlotSettings s = fx::DefaultSettings();
        s.v[0] = 0.0f;
        s.v[2] = 0.0f; // no pre-delay
        // one click in: with diffusion the output is a spread-out burst, not one spike; energy is kept
        auto Burst = [&](bool diffuse, float& peak, int& nonzero, double& energy) {
            bus.Init(kSr, &core);
            bus.SetDiffusion(diffuse);
            core = EchoCore();
            core.fb = 0.0f;
            std::vector<float> in(2400, 0.0f);
            in[0] = 1.0f;
            peak = 0;
            nonzero = 0;
            energy  = 0;
            for(size_t t = 0; t < in.size(); t += kBlock)
            {
                float l[kBlock], r[kBlock];
                bus.Process(&in[t], &in[t], l, r, kBlock, s, true);
                for(size_t i = 0; i < kBlock; i++)
                {
                    const float x = l[i] / fx::ReverbBus::kMakeup;
                    peak          = std::max(peak, std::fabs(x));
                    nonzero += std::fabs(x) > 0.01f ? 1 : 0;
                    energy += static_cast<double>(x) * x;
                }
            }
        };
        float peak_off, peak_on;
        int   nz_off, nz_on;
        double en_off, en_on;
        Burst(false, peak_off, nz_off, en_off);
        Burst(true, peak_on, nz_on, en_on);
        CHECK(nz_on > 10 * nz_off && peak_on < 0.7f * peak_off, "diffusion spreads a click (%d samples above 1%% vs %d, peak %.2f vs %.2f)", nz_on, nz_off, peak_on, peak_off);
        CHECK(std::fabs(en_on / en_off - 1.0) < 0.1, "...and keeps its energy (x%.2f)", en_on / en_off);
    }
    {
        // Time knob: scaled by tail length, in even (geometric) steps, and back-checked against the feedback maths
        using B = fx::ReverbBus;
        CHECK(std::fabs(B::TailSeconds(0.0f) - 0.4f) < 1e-4f && std::fabs(B::TailSeconds(1.0f) - 12.0f) < 1e-3f, "time knob: 0.4 s .. 12 s");
        CHECK(std::fabs(B::TailSeconds(0.5f) - std::sqrt(0.4f * 12.0f)) < 1e-3f, "time knob: the middle is the geometric middle (%.2f s)", B::TailSeconds(0.5f));
        const float r1 = B::TailSeconds(0.6f) / B::TailSeconds(0.5f), r2 = B::TailSeconds(0.9f) / B::TailSeconds(0.8f);
        CHECK(std::fabs(r1 - r2) < 1e-3f, "equal knob steps give equal tail-length ratios");
        for(float v = 0.0f; v <= 1.0f; v += 0.1f)
        {
            const float fb = B::Feedback(v);
            const float db60 = fb > 0.0f ? B::kMeanDelayS / B::TailSeconds(v) * 3.0f + std::log10(fb) : 99.0f; // = 0 if consistent
            CHECK(fb > 0.2f && fb < 0.97f && std::fabs(db60) < 1e-3f, "feedback %.3f for knob %.1f (tail %.2f s)", fb, v, B::TailSeconds(v));
        }
        CHECK(std::fabs(B::ToneHz(0.5678f) - 4686.0f) < 5.0f, "default tone is 4686 Hz (%.0f)", B::ToneHz(0.5678f));
        CHECK(std::fabs(B::PreDelayMs(1.0f) - 100.0f) < 1e-3f && B::PreDelayMs(0.0f) == 0.0f, "pre-delay 0 .. 100 ms");
        fx::SlotSettings d = fx::DefaultSettings();
        fx::ReverbSlot().SetDefaults(d);
        CHECK(d.v[2] == 0.0f && std::fabs(B::ToneHz(d.v[1]) - 4686.0f) < 5.0f, "defaults: pre-delay 0, tone 4686 Hz");
    }
    {
        // pre-delay: an impulse comes out of the reverb only after the pre-delay
        static EchoCore core;
        static fx::ReverbBus bus;
        bus.Init(kSr, &core);
        bus.SetDiffusion(false); // exact arrival time
        fx::SlotSettings s = fx::DefaultSettings();
        s.v[0] = 0.0f; // time
        s.v[2] = 1.0f; // pre-delay 100 ms (the maximum) = 4800 samples
        std::vector<float> in(9600, 0.0f), zl(9600), zr(9600);
        in[0] = 1.0f;
        for(int pass = 0; pass < 200; pass++) // let the pre-delay glide settle (it glides per block)
        {
            float a[kBlock] = {}, b[kBlock], c[kBlock];
            bus.Process(a, a, b, c, kBlock, s, true);
        }
        // flush: fresh bus state is not needed; measure the delay of the impulse
        float impulse_at = -1;
        for(size_t t = 0; t < 9600; t += kBlock)
        {
            float l[kBlock], r[kBlock];
            bus.Process(&in[t], &in[t], l, r, kBlock, s, true);
            for(size_t i = 0; i < kBlock; i++)
                if(impulse_at < 0 && std::fabs(l[i]) > 0.2f)
                    impulse_at = static_cast<float>(t + i);
        }
        CHECK(std::fabs(impulse_at - 4800.0f) < 3.0f, "pre-delay 100 ms: impulse arrives at sample %.0f", impulse_at);
    }
    {
        // the bus switches itself off after the tail has died, and is silent while off
        static EchoCore core;
        static fx::ReverbBus bus;
        bus.Init(kSr, &core);
        fx::SlotSettings s = fx::DefaultSettings();
        s.v[0] = 0.0f; // fast decay (feedback 0.5)
        s.v[2] = 0.0f;
        const auto in = Sine(440, 4800);
        for(size_t t = 0; t + kBlock <= 4800; t += kBlock)
        {
            float l[kBlock], r[kBlock];
            bus.Process(&in[t], &in[t], l, r, kBlock, s, true);
        }
        CHECK(bus.Running(), "bus runs while fed");
        std::vector<float> tail;
        const float        zero[kBlock] = {};
        for(size_t t = 0; t < 48000 * 4; t += kBlock)
        {
            float l[kBlock], r[kBlock];
            bus.Process(zero, zero, l, r, kBlock, s, false);
            if(t < 4800)
                for(size_t i = 0; i < kBlock; i++)
                    tail.push_back(l[i]);
        }
        CHECK(Rms(tail, 0, tail.size()) > 0.05f, "the tail keeps ringing after the input stops");
        CHECK(!bus.Running(), "the bus switches itself off once the tail has died");
        float l[kBlock], r[kBlock];
        bus.Process(zero, zero, l, r, kBlock, s, false);
        float m = 0;
        for(float x : l)
            m = std::max(m, std::fabs(x));
        CHECK(m == 0.0f, "output is exactly silent while the bus is off");
    }
}

/** A Rig with the reverb on slot 0 (bus) and a stand-in core. Input = 0.4 s of sine, then silence. */
struct BusRig
{
    EchoCore         core;
    fx::ReverbBus    bus;
    fx::ReverbSlot   slot;
    fx::Layer        layer;
    fx::SlotSettings settings[fx::kNumSlots];
    Scale            half{0.5f};
    fx::BlockInfo    info;
    std::vector<float> in, out;
    size_t           t = 0;
    explicit BusRig(bool diffuse = false)
    {
        for(auto& s : settings)
            s = fx::DefaultSettings();
        layer.Init(kSr, settings);
        bus.Init(kSr, &core);
        bus.SetDiffusion(diffuse);
        layer.SetReverbBus(&bus, 0);
        layer.SetEngine(0, &slot);
        layer.SetEngine(1, &half);
        info.running        = false;
        info.buttons_active = true;
        in                  = Sine(440, 48000 * 6);
        for(size_t i = 19200; i < in.size(); i++)
            in[i] = 0.0f;
        settings[0].v[0] = 0.0f; // fast decay
        settings[0].v[2] = 0.0f; // no pre-delay
    }
    void Blocks(size_t count)
    {
        for(size_t b = 0; b < count; b++)
        {
            float l[kBlock], r[kBlock];
            layer.Process(info, &in[t], &in[t], l, r, kBlock);
            for(size_t i = 0; i < kBlock; i++)
                out.push_back(l[i]);
            t += kBlock;
        }
    }
};

static void TestReverbLayer()
{
    std::printf("reverb in the layer (slot B1 + sends)\n");
    {
        BusRig r; // nothing pressed: untouched and the bus stays off
        r.Blocks(100);
        bool same = true;
        for(size_t i = 0; i < r.out.size(); i++)
            if(r.out[i] != r.in[i])
                same = false;
        CHECK(same && !r.layer.GetStatus().reverb_ringing, "no fx on: output equals input exactly and the reverb is off");
    }
    {
        BusRig r; // hold B1 with wet = 0: only the dry signal
        r.settings[0].v[fx::kWet] = 0.0f;
        r.info.raw_mask = 1u << 0;
        r.Blocks(200);
        float err = 0;
        for(size_t i = 4800; i < r.out.size(); i++)
            err = std::max(err, std::fabs(r.out[i] - r.in[i]));
        CHECK(err < 1e-4f, "reverb slot with wet 0%%: dry only (error %g)", err);
    }
    {
        BusRig r; // output gain K5 also applies to the dry while the reverb is on
        r.settings[0].v[fx::kWet]  = 0.0f;
        r.settings[0].v[fx::kGain] = 1.0f; // +12 dB
        r.info.raw_mask = 1u << 0;
        r.Blocks(150);
        const float on_ratio = Rms(r.out, 2400, 6000) / Rms(r.in, 2400, 6000);
        CHECK(std::fabs(20 * std::log10(on_ratio) - 12.0f) < 0.3f, "reverb slot on: output gain +12 dB also lifts the dry (%.1f dB)", 20 * std::log10(on_ratio));
    }
    {
        // K8 lopass on the reverb slot filters what goes INTO the reverb: the dry is untouched, the return is darker
        const float lp_knob = 0.4f; // 1264 Hz
        BusRig r;
        r.in = Sine(5000, 48000 * 6);
        for(size_t i = 19200; i < r.in.size(); i++)
            r.in[i] = 0.0f;
        r.settings[0].v[fx::kWet] = 0.0f; // no return: only the dry is heard
        r.settings[0].v[fx::kLp]  = lp_knob;
        r.info.raw_mask = 1u << 0;
        r.Blocks(150);
        const float dry_only = Rms(r.out, 2400, 6000) / Rms(r.in, 2400, 6000);
        CHECK(std::fabs(dry_only - 1.0f) < 0.02f, "reverb slot on, lopass low: the dry 5 kHz is NOT filtered (x%.2f)", dry_only);
        BusRig q; // wet 100%: the stand-in returns 0.5 * (what it is fed) at once
        q.in = r.in;
        q.settings[0].v[fx::kWet] = 1.0f;
        q.settings[0].v[fx::kLp]  = lp_knob;
        q.info.raw_mask = 1u << 0;
        q.Blocks(100);
        const float with_ret = Rms(q.out, 2400, 4800) / Rms(q.in, 2400, 4800);
        CHECK(with_ret < 1.3f, "the lopass cuts the 5 kHz going into the reverb: only the dry remains (x%.2f)", with_ret);
        BusRig lo; // the same lopass lets 100 Hz into the reverb: dry + return
        lo.in = Sine(100, 48000 * 6);
        for(size_t i = 19200; i < lo.in.size(); i++)
            lo.in[i] = 0.0f;
        lo.settings[0].v[fx::kWet] = 1.0f;
        lo.settings[0].v[fx::kLp]  = lp_knob;
        lo.info.raw_mask = 1u << 0;
        lo.Blocks(100);
        const float low_ret = Rms(lo.out, 2400, 4800) / Rms(lo.in, 2400, 4800);
        const float expect  = 1.0f + 0.5f * fx::ReverbBus::kMakeup;
        CHECK(std::fabs(low_ret - expect) < 0.2f, "100 Hz passes the lopass into the reverb (x%.2f, expected %.2f)", low_ret, expect);
    }
    {
        BusRig r; // hold B1 with wet = 100%: it is a SEND: dry stays at full level, the reverb is added on top
        r.settings[0].v[fx::kWet] = 1.0f;
        r.info.raw_mask = 1u << 0;
        r.Blocks(100); // 100 ms: the first 100 ms contain only the stand-in's direct 0.5*in part
        const float with_wet = Rms(r.out, 2400, 4800) / Rms(r.in, 2400, 4800);
        const float expect   = 1.0f + 0.5f * fx::ReverbBus::kMakeup; // dry + the stand-in's direct part
        CHECK(std::fabs(with_wet - expect) < 0.1f, "wet 100%%: dry stays and the reverb is added (x%.2f, expected %.2f)", with_wet, expect);
    }
    {
        BusRig r; // hold, then release: dry returns, the tail keeps ringing, then the bus turns itself off
        r.settings[0].v[fx::kWet] = 0.5f;
        r.info.raw_mask = 1u << 0;
        r.Blocks(300); // 0.3 s
        CHECK(r.layer.GetStatus().reverb_ringing, "reverb running while held");
        // letting go must not change the dry level: compare the output just before and just after the release
        const size_t release_at = r.out.size();
        r.info.raw_mask = 0;
        r.Blocks(450);  // input ends at 0.4 s; we are at ~0.75 s
        // (the input is still playing for 100 ms after the release: dry must be the same level as before it)
        const float before_rel = Rms(r.out, release_at - 2400, release_at - 480);
        const float after_rel  = Rms(r.out, release_at + 480 + 2400, release_at + 4800);
        CHECK(std::fabs(after_rel - before_rel) < 0.35f * before_rel, "letting go does not jump the level (%.3f -> %.3f)", before_rel, after_rel);
        CHECK(Rms(r.out, 24000 - 4800, 24000) > 0.01f, "tail rings after the input stopped and the button was released");
        r.Blocks(3000);
        CHECK(!r.layer.GetStatus().reverb_ringing, "reverb switched itself off later");
        float peak = 0;
        for(float x : r.out)
            peak = std::max(peak, std::fabs(x));
        CHECK(peak < 4.0f, "stays bounded (peak %.2f)", peak);
    }
    {
        BusRig r; // an fx with a send: B2 (halves) with send 1.0 feeds the reverb; send 0 does not
        r.info.raw_mask = 1u << 1;
        r.Blocks(300);
        CHECK(!r.layer.GetStatus().reverb_ringing, "send 0: the reverb stays off");
        BusRig q;
        q.settings[1].v[fx::kSendReverb] = 1.0f;
        q.settings[0].v[fx::kWet]        = 0.5f;
        q.info.raw_mask = 1u << 1;
        q.Blocks(300);
        CHECK(q.layer.GetStatus().reverb_ringing, "send 1: the reverb runs");
        // the insert's own output (half) plus the reverb: louder than half alone once the echo returns
        const float rms_send = Rms(q.out, 9600, 14400), rms_dry = Rms(r.out, 9600, 14400);
        CHECK(rms_send > rms_dry * 1.1f, "the send adds reverb on top of the fx output (%.3f vs %.3f)", rms_send, rms_dry);
    }
}

int main()
{
    TestStage();
    TestBitCrush();
    TestQuantisedStartStop();
    TestLatchAndLayer();
    TestNoClicks();
    TestReverbBus();
    TestReverbLayer();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
