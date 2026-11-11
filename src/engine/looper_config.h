#pragma once

#include <cstddef>
#include <cstdint>

/** looper_config.h : the fixed numbers the looper is built around.
 *
 *  Plain C++ (no libDaisy), so the Mac test app in test/ can use it too.
 *  To allow longer loops or slower tempos later, change kMaxLoopBars,
 *  kMinBpm and the kLoopBarOptions list: every buffer size is calculated
 *  from them.
 */
namespace looper {

// ----- Time signature: 4/4 only -----
constexpr uint32_t kBeatsPerBar    = 4;
constexpr uint32_t kTicksPerBeat   = 24;  // MIDI clock is 24 ticks per beat
constexpr uint32_t kTicksPerEighth = 12;
constexpr uint32_t kTicksPerBar    = kTicksPerBeat * kBeatsPerBar; // 96
constexpr uint32_t kEighthsPerBar  = 8;

// ----- Limits (these decide how much memory the buffers need) -----
constexpr uint32_t kMaxLoopBars = 8;     // longest loop
constexpr float    kMinBpm      = 130.0f; // slowest tempo the buffers are sized for (see docs/DESIGN_LOOPER.md, "Minimum tempo and memory")
constexpr uint32_t kHistoryBars = 2;     // beat-repeat history length

/** Loop lengths (in bars) that knob 2 chooses between, shortest first.
 *  The knob range is split into equal zones, one per entry. */
constexpr uint32_t kLoopBarOptions[]  = {1, 2, 4, 8};
constexpr size_t   kNumLoopBarOptions = sizeof(kLoopBarOptions) / sizeof(kLoopBarOptions[0]);
static_assert(kLoopBarOptions[kNumLoopBarOptions - 1] <= kMaxLoopBars,
              "kLoopBarOptions must not exceed kMaxLoopBars");

/** Knob 0..1 -> number of bars in one loop cycle. */
inline uint32_t LoopBarsFromControl(float v)
{
    if(v < 0.f)
        v = 0.f;
    size_t i = static_cast<size_t>(v * static_cast<float>(kNumLoopBarOptions));
    if(i >= kNumLoopBarOptions)
        i = kNumLoopBarOptions - 1;
    return kLoopBarOptions[i];
}

// ----- Beat repeat -----
/** Beat repeat slice lengths as a fraction of one bar (numerator / denominator),
 *  on buttons 9-16 in this order: 1 bar, 1/2, 3/8, 1/4, 1/8, 1/16, 1/32, 1/64.
 *  3/8 of a bar = a dotted quarter note (1.5 beats). Its grid runs on across
 *  bar lines (a slice starts every 1.5 beats, counted from bar 1 beat 1), so it
 *  lines up with the bar again every 3 bars. */
constexpr uint32_t kDivNum[]     = {1, 1, 3, 1, 1, 1, 1, 1};
constexpr uint32_t kDivDen[]     = {1, 2, 8, 4, 8, 16, 32, 64};
constexpr size_t   kNumDivisions = sizeof(kDivNum) / sizeof(kDivNum[0]);
static_assert(sizeof(kDivDen) / sizeof(kDivDen[0]) == kNumDivisions,
              "kDivNum and kDivDen must have the same length");

/** Beat repeat presses and releases are quantised to 1/kQuantizeDen of a bar
 *  (16 = sixteenth notes in 4/4). A button press waits for the next such line
 *  before the stutter starts (since 0.4.5); the slice length is still the
 *  button's own division. */
constexpr uint32_t kQuantizeDen = 16;

// ----- Pitch-down effect on the beat repeat (B6, 0.6.0; src/engine/pitch_fx.h) -----
/** While B6 is held, every repeat of the held slice is one more semitone lower
 *  than the one before (the 1st repeat is normal), down to kPitchMaxSteps. */
constexpr int kPitchStepSemitones = 1;
constexpr int kPitchMaxSteps      = 12; // never lower than one octave
/** A pitch head that has fallen this many frames behind the live signal hops forward
 *  by kPitchHopFrames with a 5 ms crossfade, so a slow repeat never lags the beat by
 *  more than kPitchMaxDelayFrames. (Each repeat starts a new head 1 frame behind, so
 *  short repeats never get here. Bigger values = smoother tone but more lag in slow
 *  repeats; 1536 / 1024 was the 0.6.1 setting.) */
constexpr size_t kPitchMaxDelayFrames = 480; // 10 ms: the most a head may lag the beat
constexpr size_t kPitchHopFrames      = 480; // a hop recovers all of it (back to about 1 frame behind)

/** B6 in SLICE mode (0.7.2): the held slice plays kSliceFxSemitones higher, forward for the
 *  first 16th of the eighth and BACKWARDS for the second 16th (a "ping-pong"), repeating every
 *  eighth. The slice is a recording, so it is simply read faster (like tape); reading forward
 *  then back over the same piece keeps the length of every repeat exactly one eighth. */
constexpr int kSliceFxSemitones = 3; // the default (power-up) pitch; the pitch knob (K4) sets it, -12..+12
constexpr int kSliceFxMinSemitones = -12;
constexpr int kSliceFxMaxSemitones = 12;

/** Pitch knob (K4) 0..1 -> whole semitones -12..+12. */
inline int SlicePitchFromControl(float v)
{
    if(v < 0.0f)
        v = 0.0f;
    if(v > 1.0f)
        v = 1.0f;
    const float st = static_cast<float>(kSliceFxMinSemitones)
                     + v * static_cast<float>(kSliceFxMaxSemitones - kSliceFxMinSemitones);
    return static_cast<int>(st + (st < 0.0f ? -0.5f : 0.5f));
}

// ----- Slice mode (B7): the Slice loop buffer -----
/** Slice mode freezes the last kSliceBanks bars and cuts them into eighth notes. */
constexpr uint32_t kSliceBanks        = 1; // 0.5.2: 1 bar = 1 bank of 8 slices (2 would give 2 bars = 2 banks)
constexpr uint32_t kSlicesPerBank     = 8; // buttons 9-16
/** The slice presses/releases, B6 and B7 OFF are quantised to 1/kSliceQuantizeDen of
 *  a bar (8 = eighth notes). B7 ON waits for the next BAR line (0.5.2). */
constexpr uint32_t kSliceQuantizeDen  = 8;
static_assert(kSlicesPerBank * kSliceBanks == kSliceQuantizeDen * kSliceBanks,
              "the frozen window is kSliceBanks bars of eighth notes");
static_assert(kSlicesPerBank == kNumDivisions, "slice buttons are buttons 9-16");

/** Text for the log: "1 bar", "1/2", "3/8" ... */
inline const char *DivisionLabel(size_t i)
{
    static const char *const kLabels[kNumDivisions]
        = {"1 bar", "1/2", "3/8", "1/4", "1/8", "1/16", "1/32", "1/64"};
    return i < kNumDivisions ? kLabels[i] : "?";
}

/** Input offset (Shift + K3): how far slices are shifted against the clock.
 *  Positive = the audio arrives later than the MIDI clock. */
constexpr float kMaxOffsetMs = 20.0f;

/** Knob 0..1 -> offset in ms (centre 0.5 = 0 ms). */
inline float OffsetMsFromControl(float v)
{
    if(v < 0.0f)
        v = 0.0f;
    if(v > 1.0f)
        v = 1.0f;
    return (v - 0.5f) * 2.0f * kMaxOffsetMs;
}

/** Number of audio frames (one frame = left + right sample) needed to hold
 *  `bars` bars at the slowest supported tempo. */
constexpr size_t FramesForBars(uint32_t bars, float sample_rate)
{
    return static_cast<size_t>(static_cast<float>(bars * kBeatsPerBar) * 60.0f
                               / kMinBpm * sample_rate)
           + 1;
}

// ----- Click protection -----
constexpr float kCrossfadeMs = 5.0f;  // length of every fade / crossfade
constexpr float kTailMs      = 20.0f; // extra audio recorded past the loop end

constexpr size_t XfadeFrames(float sample_rate)
{
    return static_cast<size_t>(kCrossfadeMs * 0.001f * sample_rate);
}
constexpr size_t TailFrames(float sample_rate)
{
    return static_cast<size_t>(kTailMs * 0.001f * sample_rate);
}

/** The loop buffer is a fixed array sized for this sample rate (the Seed
 *  runs at 48 kHz). A higher rate would not fit the longest loops, and
 *  arming those is refused. */
constexpr float  kBufferSampleRate = 48000.0f;
constexpr size_t kLoopCapacityFrames
    = FramesForBars(kMaxLoopBars, kBufferSampleRate) + TailFrames(kBufferSampleRate);

/** Spare frames kept in the beat repeat buffers (50 ms at 48 kHz): room for
 *  the input offset and the crossfades. */
constexpr size_t kHistoryMarginFrames = 2400;
/** The history ring holds kHistoryBars bars at the slowest tempo. */
constexpr size_t kHistoryCapacityFrames
    = FramesForBars(kHistoryBars, kBufferSampleRate) + kHistoryMarginFrames;

/** Every division keeps its own stored copy of its slice while a stutter
 *  button is held (so you can flip between them). One slot is as long as that
 *  slice at the slowest tempo, plus the margin. */
constexpr size_t SliceCapacityFrames(size_t i)
{
    return FramesForBars(1, kBufferSampleRate) * kDivNum[i] / kDivDen[i] + kHistoryMarginFrames;
}
/** Total frames of all slots (sum of the first n slots). */
constexpr size_t FreezeFramesUpTo(size_t n)
{
    return n == 0 ? 0 : FreezeFramesUpTo(n - 1) + SliceCapacityFrames(n - 1);
}
constexpr size_t kFreezeCapacityFrames = FreezeFramesUpTo(kNumDivisions);
/** Frames copied into a stored-slice slot per audio block (keeps each block fast).
 *  0.4.4: lowered from 2048 to 256. 2048 frames of SDRAM copying per 1 ms block
 *  probably overran the audio block on the Seed (glitch on the first pass of the
 *  long divisions). 256 frames per block is still about 5x faster than real time,
 *  so even a 1-bar slice is fully copied long before its first repeat ends. */
constexpr size_t kCopyChunkFrames = 256;

/** Bytes for a stereo float buffer of that many frames. */
inline size_t BytesForFrames(size_t frames)
{
    return frames * 2 * sizeof(float);
}

} // namespace looper
