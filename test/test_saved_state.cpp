// Host-side test for src/engine/saved_state.h (what goes to flash). Run with `make -C test`.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include "../src/engine/saved_state.h"

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

static SavedState Make()
{
    SavedState s;
    std::memset(&s, 0, sizeof(s));
    s.magic        = SavedState::kMagic;
    s.input_offset = 0.5f;
    s.loop_length  = 0.5f;
    s.input_gain   = 1.0f / 3.0f;
    s.out_level    = 1.0f;
    for(auto &slot : s.fx)
        slot = fx::DefaultSettings();
    return s;
}

int main()
{
    std::printf("saved state (flash block)\n");
    SavedState a = Make();
    CHECK(a.IsValid(), "a normal state is valid");
    CHECK(sizeof(SavedState) < 1024, "small (%zu bytes)", sizeof(SavedState));

    // pretend flash: copy out, copy back
    unsigned char flash[sizeof(SavedState)];
    a.fx[6].v[2] = 0.123f;
    a.input_gain = 0.9f;
    std::memcpy(flash, &a, sizeof(a));
    SavedState b;
    std::memcpy(&b, flash, sizeof(b));
    CHECK(b == a && !(b != a) && b.IsValid(), "a state copied through 'flash' comes back identical");
    b.fx[6].v[2] = 0.2f;
    CHECK(b != a, "a changed value is seen as a change (so it gets saved)");

    // blank flash (all 0xFF) and zeros are rejected
    std::memset(flash, 0xFF, sizeof(flash));
    std::memcpy(&b, flash, sizeof(b));
    CHECK(!b.IsValid(), "blank (erased) flash is not valid");
    std::memset(&b, 0, sizeof(b));
    CHECK(!b.IsValid(), "all zeros is not valid");

    SavedState c = Make();
    c.magic ^= 0x100;
    CHECK(!c.IsValid(), "a different layout version is rejected");
    c = Make();
    c.fx[3].v[5] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!c.IsValid(), "NaN is rejected");
    c = Make();
    c.fx[14].v[7] = 1.5f;
    CHECK(!c.IsValid(), "a value above 1 is rejected");
    c = Make();
    c.loop_length = -0.1f;
    CHECK(!c.IsValid(), "a negative value is rejected");

    std::printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
