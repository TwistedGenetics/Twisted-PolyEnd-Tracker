// Twisted: REESE - the Reese bass engine from DREADNOUGHT (Jamie / TwistedGenetics), written again
// for the Tracker+ synth SDK in single-precision float. It follows DREADNOUGHT's design, presets and
// value ranges; the DSP code here is new (none of DREADNOUGHT's third-party GPL code is used), so it
// sits under this firmware's own licence.
//
// Two or more oscillators detuned against each other (their sum is a comb filter that moves up the
// spectrum as they drift in and out of phase - the Casio CZ's LINE 1+1', the 1988 original), then, in
// the order producers use: a crossover taking the low end off the oscillators (they cancel each other
// there and the bottom pumps), drive, a 4-pole zero-delay-feedback ladder low-pass, a slowly swept
// notch comb, and a clean mono sine sub on the fundamental added last. Mono, with glide.
//
// The patch is a PRESET (the 10 DREADNOUGHT REESE sounds) plus offsets: every knob moves its value
// away from the preset's, and shows the value it ends up at. Changing PRESET changes the sound at
// once while keeping your tweaks.

#include "config.h"
#ifdef FEATURE_SYNTH

#include "ReeseSynthVoice.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace polysynth
{

namespace
{

enum class ReeseParamId : int
{
    kPreset = 0,
    kOsc,
    kDetune,
    kBeat,
    kSub,
    kCutoff,
    kResonance,
    kDrive,
    kNotch,
    kAttack,
    kDecay,
    kSustain,
    kRelease,
    kFilterEnv,
    kGlide,
    kGlideMode,
    kLfoRate,
    kLfoDepth,
    kTranspose,
    kVelocity,
    kVolume,
    // v1.7 (appended: saved patches keep their parameter ids)
    kFilterAttack,
    kFilterDecay,
    kFilterSustain,
    kFilterRelease,
    kLfoShape,
    kLfoRetrig,
    kDistType,
    kDistAmount,
    kChorusMix,
    kChorusRate,
    kChorusDepth,
};

// the DREADNOUGHT patch values (0..127 unless noted)
enum
{
    RP_OSC, RP_DTN, RP_BEAT, RP_SUB, RP_CUT, RP_RES, RP_DRV, RP_NTCH,
    RP_ATK, RP_DEC, RP_SUS, RP_REL, RP_FENV, RP_GLIDE, RP_LRATE, RP_LFLT, RP_TRANS,
    RP_FA, RP_FD, RP_FS, RP_FR, // the filter envelope (a preset starts it as the amp's A, D, no sustain, the amp's R)
    RP_COUNT
};
enum
{
    RO_2SAW, RO_3SAW, RO_5SAW, RO_7SAW, RO_SAWSQ, RO_2SIN, RO_N
};

constexpr int kMinV[RP_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -64, 0, 0, -64, -36, 0, 0, 0, 0 };
constexpr int kMaxV[RP_COUNT] = { RO_N - 1, 127, 7, 127, 127, 127, 127, 127, 127,
                                  127, 127, 127, 63, 127, 127, 63, 36, 127, 127, 127, 127 };

struct ReesePreset
{
    const char* name;
    int8_t v[RP_TRANS + 1];
};

// DREADNOUGHT's REESE presets: OSC DTN BEAT SUB CUT RES DRV NTCH ATK DEC SUS REL FENV GLIDE LRATE LFLT TRANS
constexpr std::array<ReesePreset, 10> kPresets = { {
  { "Classic", { RO_2SAW, 70, 0, 80, 68, 18, 24, 0, 0, 70, 120, 30, 0, 36, 0, 0, -24 } },
  { "OG Reese", { RO_2SIN, 93, 0, 0, 76, 8, 46, 0, 0, 70, 124, 28, 0, 40, 0, 0, -24 } },
  { "Hollow", { RO_2SAW, 80, 0, 80, 70, 20, 30, 80, 0, 70, 120, 30, 0, 36, 0, 0, -24 } },
  { "Techstep", { RO_SAWSQ, 90, 0, 76, 79, 30, 96, 50, 0, 80, 116, 28, 12, 30, 40, 14, -24 } },
  { "Neuro", { RO_5SAW, 110, 0, 80, 74, 52, 120, 70, 0, 80, 110, 26, 24, 0, 80, 34, -24 } },
  { "Sync", { RO_2SAW, 70, 3, 80, 68, 20, 30, 40, 0, 70, 120, 30, 0, 36, 0, 0, -24 } },
  { "Dark", { RO_3SAW, 50, 0, 96, 52, 24, 40, 30, 0, 70, 124, 34, 0, 40, 0, 0, -24 } },
  { "Sub Reese", { RO_2SAW, 40, 0, 110, 56, 10, 20, 0, 0, 70, 124, 28, 0, 36, 0, 0, -24 } },
  { "Hoover", { RO_7SAW, 127, 0, 30, 96, 10, 60, 40, 0, 80, 110, 38, 0, 60, 0, 0, -12 } },
  { "Init", { RO_2SAW, 40, 0, 60, 100, 0, 0, 0, 0, 64, 127, 20, 0, 0, 0, 0, -24 } },
} };

constexpr std::array<const char*, RO_N> kOscNames = { "2 Saw", "3 Saw", "5 Saw", "7 Saw", "Saw+Sq", "2 Sine" };
// a preset's value of a patch item; the filter envelope follows the preset's amp envelope
inline int presetBase(const ReesePreset& pr, int rp)
{
    switch (rp)
    {
        case RP_FA: return pr.v[RP_ATK];
        case RP_FD: return pr.v[RP_DEC];
        case RP_FS: return 0;
        case RP_FR: return pr.v[RP_REL];
        default: return pr.v[rp];
    }
}

constexpr std::array<const char*, 6> kLfoShapeNames = { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H" };
constexpr std::array<const char*, 4> kDistNames = { "Soft", "Hard", "Fold", "Crush" };

constexpr std::array<const char*, 8> kBeatNames = { "Off", "1/1", "1/2", "1/4", "1/8", "1/16", "1/4T", "1/8T" };
constexpr uint8_t kBeat4[8] = { 0, 1, 2, 4, 8, 16, 6, 12 }; // the beat in quarter notes x 4

// where each oscillator sits in the spread (-1..1) and its gain
constexpr uint8_t kOscCount[RO_N] = { 2, 3, 5, 7, 2, 2 };
constexpr float kOscPos[RO_N][7] = {
    { -1.0f, 1.0f },
    { 0.0f, -1.0f, 1.0f },
    { 0.0f, 0.34f, -0.30f, 1.0f, -0.97f },
    { 0.0f, 0.19f, -0.17f, 0.58f, -0.56f, 0.98f, -1.0f },
    { -1.0f, 1.0f },
    { -1.0f, 1.0f },
};
constexpr float kOscGain[RO_N] = { 0.70f, 0.58f, 0.40f, 0.29f, 0.70f, 0.80f };

constexpr int kCtl = 32;               // control rate: envelopes, glide, drift once every 32 samples
constexpr int kCombLen = 256;
constexpr float kSr = sdk::kSampleRate;

// times 1 ms .. 10 s, log over 0..127 (DREADNOUGHT's envelope / glide scale)
inline float valueToMs(int v)
{
    return std::pow(1.075254f, static_cast<float>(std::clamp(v, 0, 127)));
}
// one-pole coefficient per control tick that covers ~99 % of the way in that time
inline float tickCoef(int v)
{
    const float ms = valueToMs(v);
    return std::min(1.0f, 1.0f - std::exp(-4.6f * kCtl / (ms * 0.001f * kSr)));
}
inline float cutoffHz(float steps) // 0..127: 30 Hz .. 16 kHz, log
{
    return 30.0f * std::pow(16000.0f / 30.0f, std::clamp(steps, 0.0f, 127.0f) / 127.0f);
}
inline float lfoHz(int v) // 0.05 .. 20 Hz, log
{
    return 0.05f * std::pow(400.0f, static_cast<float>(std::clamp(v, 0, 127)) / 127.0f);
}
inline float softClip(float x) // a cheap tanh
{
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
inline float polyBlep(float t, float dt)
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt)
    {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
inline float wrap01(float p)
{
    return p - std::floor(p);
}
inline float saw(float ph, float dt)
{
    return (2.0f * ph - 1.0f) - polyBlep(ph, dt);
}

int presetIndex(const Patch& patch)
{
    auto v = patch.getParamValue(static_cast<int>(ReeseParamId::kPreset));
    return v ? std::clamp<int>(static_cast<int>(std::lround(*v)), 0, kPresets.size() - 1) : 0;
}

// a value as it ends up: the preset's plus the knob's offset
int finalValue(const Patch& patch, int rp, float offset)
{
    return std::clamp<int>(presetBase(kPresets[presetIndex(patch)], rp) + static_cast<int>(std::lround(offset)),
                           kMinV[rp],
                           kMaxV[rp]);
}

std::string msText(int v)
{
    const float ms = valueToMs(v);
    char b[16];
    if (ms < 1000.0f)
        snprintf(b, sizeof(b), "%d ms", static_cast<int>(ms + 0.5f));
    else
        snprintf(b, sizeof(b), "%.1f s", static_cast<double>(ms / 1000.0f));
    return b;
}

} // namespace

namespace
{

class ReeseSynthVoice : public sdk::CustomSynthVoice<ReeseSynthVoice, ReeseParamId>
{
  public:
    ReeseSynthVoice()
    {
        playMode = PlayMode::kLegato;
        recompute();
    }

    bool isActive() final { return active || ampNow > 0.0f || ampNext > 0.0f; }

    void onNoteOn(const InstrumentVoiceContext& /*ctx*/, note_t note, float velocity, bool isLegatoStep) final
    {
        vel = velocity;
        target = static_cast<float>(note) + static_cast<float>(p[RP_TRANS] + 24) + transpose;
        if (lfoRetrig) // the LFO restarts on every key press
        {
            lfoPh = 0.0f;
            lfoHold = static_cast<float>(static_cast<int32_t>(nextRand() >> 16) - 32768) / 32768.0f;
        }
        const bool slide = p[RP_GLIDE] > 0 && active && (isLegatoStep ? gate : glideAlways);
        if (slide)
        {
            gate = true;
            astage = (astage == 3) ? 2 : astage; // back from release without a new attack
            fstage = (fstage == 3) ? 2 : fstage;
            return;
        }
        pitch = target;
        if (!active || astage == 0)
        {
            // a fresh note: spread the phases, the sub and the filters from zero
            for (int j = 0; j < 7; j++)
                ph[j] = static_cast<float>(nextRand() & 0xFFFF) / 65536.0f;
            subPh = 0.0f;
            for (int j = 0; j < 4; j++)
                lad[j] = 0.0f;
            for (int j = 0; j < 4; j++)
                xo[j] = 0.0f;
            std::fill(std::begin(comb), std::end(comb), 0.0f);
        }
        gate = true;
        active = true;
        astage = 1;
        fstage = 1;
    }

    void onNoteOff(const InstrumentVoiceContext& /*ctx*/, note_t /*note*/) final
    {
        gate = false;
        if (astage)
            astage = 3;
        if (fstage)
            fstage = 3;
    }

    void renderVoiceAudio(const InstrumentVoiceContext& ctx,
                          [[maybe_unused]] std::optional<AudioBufferStereoRef> inputBuffer,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef> /*auxOutBuffer*/) final
    {
        auto out = outputBuffer[0];
        auto outR = outputBuffer[1];
        bpm = ctx.tempo > 1.0f ? ctx.tempo : 120.0f;

        size_t i = 0;
        const size_t n = out.size();
        while (i < n)
        {
            if (ctl == 0)
            {
                control();
                ctl = kCtl;
            }
            const size_t len = std::min<size_t>(n - i, static_cast<size_t>(ctl));
            if (!active && ampNow <= 0.0f && ampNext <= 0.0f)
            {
                std::fill(out.begin() + i, out.begin() + i + len, 0.0f);
                std::fill(outR.begin() + i, outR.begin() + i + len, 0.0f);
            }
            else
            {
                renderBlock(&out[i], &outR[i], static_cast<int>(len));
            }
            ctl -= static_cast<int>(len);
            i += len;
        }
    }

    static void createMetadata(const Patch& patch, Patch::Metadata& metadata);

  private:
    // the knobs
    int preset = 0;
    float off[RP_COUNT] = {};
    int oscSel = 0;  // 0: the preset's
    int beatSel = 0; // 0: the preset's
    bool glideAlways = false;
    float transpose = 0.0f;
    float velSens = 0.5f;
    float volume = 1.0f;

    // the values in use, after preset + offsets
    int p[RP_COUNT] = {};

    // the voice
    bool gate = false, active = false;
    float vel = 1.0f;
    float pitch = 36.0f, target = 36.0f;
    uint8_t astage = 0, fstage = 0;
    float aenv = 0.0f, fenv = 0.0f;
    float ampNow = 0.0f, ampNext = 0.0f;
    int ctl = 0;
    float bpm = 120.0f;
    float ph[7] = {};
    float subPh = 0.0f;
    float drift[7] = {}, dvel[7] = {};
    float lfoPh = 0.0f;
    float lad[4] = {};
    float xo[4] = {};
    float comb[kCombLen] = {};
    uint32_t combW = 0;
    uint32_t combPh = 0;
    uint32_t rng = 0x5EE5Eu;

    // LFO shape, distortion, chorus
    int lfoShape = 0;
    bool lfoRetrig = false;
    float lfoHold = 0.0f;
    int distType = 0;
    float distAmount = 0.0f;
    static constexpr int kChorusLen = 1024;
    float chorus[kChorusLen] = {};
    uint32_t chorusW = 0;
    float chorusPh = 0.0f, chorusInc = 0.0f;
    float chorusMix = 0.0f, chorusDepth = 0.5f;

    inline float chorusTap(float d) const
    {
        const int di = static_cast<int>(d);
        const float fr = d - di;
        const float a = chorus[(chorusW - di) & (kChorusLen - 1)];
        const float b = chorus[(chorusW - di - 1) & (kChorusLen - 1)];
        return a + (b - a) * fr;
    }

    // per tick
    float inc[7] = {};
    float subInc = 0.0f;
    float ladG = 0.5f, ladK = 0.0f;
    float xoA1 = 0.0f, xoA2 = 0.0f, xoA3 = 0.0f;
    bool xover = false;

    uint32_t nextRand()
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    }

    void recompute()
    {
        const ReesePreset& pr = kPresets[std::clamp(preset, 0, static_cast<int>(kPresets.size()) - 1)];
        for (int k = 0; k < RP_COUNT; k++)
        {
            p[k] = std::clamp<int>(presetBase(pr, k) + static_cast<int>(std::lround(off[k])), kMinV[k], kMaxV[k]);
        }
        if (oscSel > 0)
            p[RP_OSC] = oscSel - 1;
        if (beatSel > 0)
            p[RP_BEAT] = beatSel - 1;
        p[RP_TRANS] = pr.v[RP_TRANS];
    }

    // envelopes, glide, drift, pitch and filter coefficients: once every kCtl samples
    void control()
    {
        const float sus = p[RP_SUS] / 127.0f;
        // amplitude: attack to an overshoot target (an analog curve), decay to sustain, release
        switch (astage)
        {
            case 1:
                aenv += (1.25f - aenv) * tickCoef(p[RP_ATK]);
                if (aenv >= 1.0f)
                {
                    aenv = 1.0f;
                    astage = 2;
                }
                break;
            case 2: aenv += (sus - aenv) * tickCoef(p[RP_DEC]); break;
            case 3:
                aenv -= aenv * tickCoef(p[RP_REL]);
                if (aenv < 1.0f / 4096.0f)
                {
                    aenv = 0.0f;
                    astage = 0;
                    active = false;
                }
                break;
            default: break;
        }
        // the filter envelope: its own attack, decay, sustain and release
        switch (fstage)
        {
            case 1:
                fenv += (1.25f - fenv) * tickCoef(p[RP_FA]);
                if (fenv >= 1.0f)
                {
                    fenv = 1.0f;
                    fstage = 2;
                }
                break;
            case 2: fenv += (p[RP_FS] / 127.0f - fenv) * tickCoef(p[RP_FD]); break;
            case 3: fenv -= fenv * tickCoef(p[RP_FR]); break;
            default: break;
        }
        // glide
        if (pitch != target)
        {
            const float d = target - pitch;
            if (p[RP_GLIDE] == 0 || std::fabs(d) < 0.004f)
                pitch = target;
            else
                pitch += d * tickCoef(p[RP_GLIDE]);
        }
        // drift: a twice-smoothed random walk per oscillator, pulled back to centre (~1.5 cents)
        const int mode = p[RP_OSC];
        const int no = kOscCount[mode];
        for (int j = 0; j < no; j++)
        {
            const float rr = static_cast<float>(static_cast<int32_t>(nextRand() >> 20) - 2048);
            dvel[j] += rr * 30.0f / 128.0f - dvel[j] / 16.0f;
            drift[j] += dvel[j] / 32.0f - drift[j] / 128.0f;
        }
        // amplitude ramp over the tick
        ampNow = ampNext;
        float a = aenv * 0.787f;
        a *= 1.0f - velSens * (1.0f - vel);
        ampNext = a;

        // oscillator increments
        const float baseHz = 440.0f * sdk::fastPow2((pitch - 69.0f) / 12.0f);
        const float baseInc = baseHz / kSr;
        subInc = baseInc;
        const int beat = p[RP_BEAT];
        if (beat > 0)
        {
            // the pair beats at a note value of the tempo on every note: a fixed offset in Hz
            const float halfHz = std::clamp(bpm, 20.0f, 300.0f) * kBeat4[beat] / 480.0f;
            const float d = drift[0] / 17.0f / 65536.0f;
            for (int j = 0; j < no; j++)
                inc[j] = baseInc * (1.0f + d) + halfHz * kOscPos[mode][j] / kSr;
        }
        else
        {
            const float spread = p[RP_DTN] * p[RP_DTN] * 100.0f / (127.0f * 127.0f); // total, cents
            for (int j = 0; j < no; j++)
            {
                const float cents = spread * 0.5f * kOscPos[mode][j];
                inc[j] = baseInc * (sdk::fastPow2(cents / 1200.0f) + drift[j] / 17.0f / 65536.0f);
            }
        }
        // the filter LFO (a sine) and the cutoff
        const float nextPh = lfoPh + lfoHz(p[RP_LRATE]) * kCtl / kSr;
        if (nextPh >= 1.0f) // a new cycle: S&H takes a new value
            lfoHold = static_cast<float>(static_cast<int32_t>(nextRand() >> 16) - 32768) / 32768.0f;
        lfoPh = wrap01(nextPh);
        float shape = 0.0f;
        switch (lfoShape)
        {
            case 1: shape = lfoPh < 0.5f ? 4.0f * lfoPh - 1.0f : 3.0f - 4.0f * lfoPh; break; // triangle
            case 2: shape = 2.0f * lfoPh - 1.0f; break;                                       // saw up
            case 3: shape = 1.0f - 2.0f * lfoPh; break;                                       // saw down
            case 4: shape = lfoPh < 0.5f ? 1.0f : -1.0f; break;                               // square
            case 5: shape = lfoHold; break;                                                   // sample & hold
            default: shape = sdk::fastSine(lfoPh); break;
        }
        const float lfo = p[RP_LFLT] ? shape * p[RP_LFLT] * 2.0f : 0.0f;
        const float steps = static_cast<float>(p[RP_CUT]) + fenv * p[RP_FENV] + lfo;
        const float g = std::tan(3.14159265f * std::min(cutoffHz(steps), 18000.0f) / kSr);
        ladG = std::min(g / (1.0f + g), 0.75f);
        ladK = p[RP_RES] * 4.13f / 127.0f;
        // the crossover: two Butterworth high-passes at ~118 Hz (24 dB) when the sub is on
        xover = p[RP_SUB] > 0 && mode != RO_2SIN;
        if (xover)
        {
            const float gx = std::tan(3.14159265f * 118.0f / kSr), kx = 1.41421356f;
            xoA1 = 1.0f / (1.0f + gx * (gx + kx));
            xoA2 = gx * xoA1;
            xoA3 = gx * xoA2;
        }
    }

    inline float svfHp(float x, float& ic1, float& ic2) const
    {
        const float v3 = x - ic2;
        const float v1 = xoA1 * ic1 + xoA2 * v3;
        const float v2 = ic2 + xoA2 * ic1 + xoA3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return x - 1.41421356f * v1 - v2;
    }

    // a 4-pole zero-delay-feedback ladder (Zavalishin's topology), the input through a tanh
    inline float ladder(float x)
    {
        const float G = ladG, k = ladK;
        float in = std::clamp(x, -4.0f, 4.0f) * (1.0f + k * 0.5f);
        float S = G * lad[0] + lad[1];
        S = G * S + lad[2];
        S = G * S + lad[3];
        S *= (1.0f - G);
        const float G4 = G * G * G * G;
        const float y = (G4 * in + S) / (1.0f + k * G4);
        float u = 2.0f * softClip((in - k * y) * 0.5f);
        for (int j = 0; j < 4; j++)
        {
            const float v = G * (u - lad[j]);
            u = v + lad[j];
            lad[j] = u + v;
        }
        return u;
    }

    // the distortion after the filter (the sub stays clean)
    inline float distort(float x) const
    {
        if (distAmount <= 0.0f)
            return x;
        const float g = 1.0f + distAmount * 15.0f;
        float w;
        switch (distType)
        {
            case 1: w = std::clamp(x * g, -0.9f, 0.9f); break;                     // hard clip
            case 2: w = sdk::fastSine(0.25f * softClip(x * g * 0.5f) * 2.0f); break; // wavefold
            case 3:                                                                // bit crush
            {
                const float steps = std::pow(2.0f, 12.0f - distAmount * 9.0f);
                w = std::round(softClip(x * (1.0f + distAmount * 3.0f)) * steps) / steps;
                break;
            }
            default: w = softClip(x * g); break;                                  // soft (tube-ish)
        }
        // keep the level about the same as the amount goes up
        w *= (distType <= 1) ? 1.0f / (1.0f + distAmount * 1.4f) : 1.0f / (1.0f + distAmount * 0.65f);
        const float mix = std::min(1.0f, distAmount * 4.0f);
        return x + (w - x) * mix;
    }

    void renderBlock(float* out, float* outR, int len)
    {
        const int mode = p[RP_OSC];
        const int no = kOscCount[mode];
        const float gain = kOscGain[mode];
        const float drv = static_cast<float>(p[RP_DRV]);
        const float driveGain = 1.0f + drv * 96.0f / 4096.0f;
        const bool hard = p[RP_DRV] > 64;
        const float hol = p[RP_NTCH] / 127.0f;
        const float sub = p[RP_SUB] / 127.0f;
        const uint32_t crate = 8000u + static_cast<uint32_t>(p[RP_NTCH]) * 520u;
        const int pos0 = kCtl - ctl;
        const float da = (ampNext - ampNow) / kCtl;

        for (int i = 0; i < len; i++)
        {
            float x = 0.0f;
            for (int j = 0; j < no; j++)
            {
                const float dt = inc[j];
                float o;
                if (mode == RO_2SIN)
                    o = sdk::fastSine(ph[j]);
                else if (mode == RO_SAWSQ && j == 1)
                    o = 0.5f * (saw(ph[j], dt) - saw(wrap01(ph[j] + 0.5f), dt));
                else
                    o = saw(ph[j], dt);
                x += o * gain;
                ph[j] += dt;
                if (ph[j] >= 1.0f)
                    ph[j] -= 1.0f;
            }
            x *= 0.5f;
            if (xover)
            {
                x = svfHp(x, xo[0], xo[1]);
                x = svfHp(x, xo[2], xo[3]);
            }
            x *= driveGain;
            if (hard)
            {
                const float c = softClip(x);
                x = c + (x - c) * 0.25f;
            }
            float y = ladder(x);
            if (hol > 0.0f)
            {
                // the notch comb: a 0.5 .. 4.5 ms delay swept by a slow triangle, subtracted
                uint32_t tri = combPh >> 15;
                tri = tri < 65536u ? tri : 131071u - tri;
                const float d = 24.0f + tri * (176.0f / 65536.0f);
                const int di = static_cast<int>(d);
                const float fr = d - di;
                const float a = comb[(combW - di) & (kCombLen - 1)];
                const float b = comb[(combW - di - 1) & (kCombLen - 1)];
                comb[combW & (kCombLen - 1)] = y;
                combW++;
                y -= (a + (b - a) * fr) * hol;
                combPh += crate;
            }
            y = distort(y);
            const float amp = ampNow + da * static_cast<float>(pos0 + i);
            const float r = std::clamp(y, -1.83f, 1.83f) * amp * 0.49f * volume;
            // the chorus: two delay taps swept a quarter cycle apart, left and right
            float l = r, rr = r;
            if (chorusMix > 0.0f)
            {
                chorus[chorusW & (kChorusLen - 1)] = r;
                const float baseD = 0.007f * kSr, swing = chorusDepth * 0.006f * kSr;
                const float dl = baseD + swing * (0.5f + 0.5f * sdk::fastSine(chorusPh));
                const float dr = baseD + swing * (0.5f + 0.5f * sdk::fastSine(chorusPh + 0.25f));
                const float wl = chorusTap(dl), wr = chorusTap(dr);
                chorusW++;
                chorusPh += chorusInc;
                if (chorusPh >= 1.0f)
                    chorusPh -= 1.0f;
                l = r * (1.0f - 0.5f * chorusMix) + wl * chorusMix * 0.8f;
                rr = r * (1.0f - 0.5f * chorusMix) + wr * chorusMix * 0.8f;
            }
            // the sub: a clean mono sine on the fundamental, added last
            float sb = 0.0f;
            if (sub > 0.0f)
                sb = sdk::fastSine(subPh) * sub * amp * 0.49f * volume;
            subPh += subInc;
            if (subPh >= 1.0f)
                subPh -= 1.0f;
            out[i] = l + sb;
            outR[i] = rr + sb;
        }
        for (int j = no; j < 7; j++)
            ph[j] = wrap01(ph[j] + inc[0] * len);
    }

  public:
    // handlers
    static void setPreset(ReeseSynthVoice& v, float x)
    {
        v.preset = std::clamp<int>(static_cast<int>(std::lround(x)), 0, kPresets.size() - 1);
        v.recompute();
    }
    static void setOffset(ReeseSynthVoice& v, int rp, float x)
    {
        v.off[rp] = x;
        v.recompute();
    }
    static void setOsc(ReeseSynthVoice& v, float x)
    {
        v.oscSel = std::clamp<int>(static_cast<int>(std::lround(x)), 0, RO_N);
        v.recompute();
    }
    static void setBeat(ReeseSynthVoice& v, float x)
    {
        v.beatSel = std::clamp<int>(static_cast<int>(std::lround(x)), 0, 8);
        v.recompute();
    }
    static void setGlideMode(ReeseSynthVoice& v, float x) { v.glideAlways = std::lround(x) != 0; }
    static void setTranspose(ReeseSynthVoice& v, float x) { v.transpose = std::round(x); }
    static void setVelocity(ReeseSynthVoice& v, float x) { v.velSens = std::clamp(x, 0.0f, 1.0f); }
    static void setVolume(ReeseSynthVoice& v, float x) { v.volume = sdk::dbToAmplitude(x); }
    static void setLfoShape(ReeseSynthVoice& v, float x) { v.lfoShape = std::clamp<int>(std::lround(x), 0, 5); }
    static void setLfoRetrig(ReeseSynthVoice& v, float x) { v.lfoRetrig = std::lround(x) != 0; }
    static void setDistType(ReeseSynthVoice& v, float x) { v.distType = std::clamp<int>(std::lround(x), 0, 3); }
    static void setDistAmount(ReeseSynthVoice& v, float x) { v.distAmount = std::clamp(x, 0.0f, 1.0f); }
    static void setChorusMix(ReeseSynthVoice& v, float x) { v.chorusMix = std::clamp(x, 0.0f, 1.0f); }
    static void setChorusRate(ReeseSynthVoice& v, float x) { v.chorusInc = std::clamp(x, 0.05f, 8.0f) / kSr; }
    static void setChorusDepth(ReeseSynthVoice& v, float x) { v.chorusDepth = std::clamp(x, 0.0f, 1.0f); }
};

// an offset knob: moves a preset value, shows where it ends up
template<int RP>
Patch::Metadata::Param offsetParam(const char* name, uint8_t cc, std::string (*text)(int))
{
    (void)text;
    return Patch::Metadata::Param{
        .displayName = name,
        .defaultValue = 0.0f,
        .minValue = static_cast<Patch::paramvalue_short_t>(-127),
        .maxValue = static_cast<Patch::paramvalue_short_t>(127),
        .midiCC = cc,
        .valueFormatter =
          [](const Patch& patch, Patch::paramvalue_t value) -> std::string
        {
            const int v = finalValue(patch, RP, value);
            char b[24];
            switch (RP)
            {
                case RP_DTN:
                    snprintf(b, sizeof(b), "%d cents", v * v * 100 / (127 * 127));
                    return b;
                case RP_CUT:
                {
                    const float hz = cutoffHz(static_cast<float>(v));
                    if (hz < 1000.0f)
                        snprintf(b, sizeof(b), "%d Hz", static_cast<int>(hz + 0.5f));
                    else
                        snprintf(b, sizeof(b), "%.1f kHz", static_cast<double>(hz / 1000.0f));
                    return b;
                }
                case RP_ATK:
                case RP_DEC:
                case RP_REL:
                case RP_FA:
                case RP_FD:
                case RP_FR: return msText(v);
                case RP_GLIDE:
                    if (v == 0)
                        return "Off";
                    return msText(v);
                case RP_LRATE:
                    snprintf(b, sizeof(b), "%.2f Hz", static_cast<double>(lfoHz(v)));
                    return b;
                case RP_SUS:
                case RP_FS:
                case RP_SUB:
                case RP_RES:
                case RP_DRV:
                case RP_NTCH:
                    if (RP == RP_NTCH && v == 0)
                        return "Off";
                    snprintf(b, sizeof(b), "%d %%", v * 100 / 127);
                    return b;
                default:
                    snprintf(b, sizeof(b), "%+d", v);
                    return b;
            }
        },
    };
}

} // namespace

void ReeseSynthVoice::createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
{
    using namespace polysynth::sdk;
    using P = ReeseParamId;

    metadata.params.resize(enumutil::EnumMax<ReeseParamId>::value + 1);
    metadata.params.shrink_to_fit();
    if (paramHandlers.empty())
    {
        paramHandlers.resize(enumutil::EnumMax<ReeseParamId>::value + 1);
    }

    ParamHelper<ReeseSynthVoice> helper(metadata, paramHandlers);

    helper.addParam(
      *P::kPreset,
      {
        .displayName = "Preset",
        .defaultValue = 0,
        .minValue = 0,
        .maxValue = static_cast<Patch::paramvalue_short_t>(kPresets.size() - 1),
        .isEnum = true,
        .midiCC = *MidiCCs::kOscParam0,
        .valueFormatter = [](const Patch&, Patch::paramvalue_t value)
        { return std::string(kPresets[std::clamp<int>(std::lround(value), 0, kPresets.size() - 1)].name); },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setPreset(v, *val); });

    helper.addParam(
      *P::kOsc,
      {
        .displayName = "Oscillators",
        .defaultValue = 0,
        .minValue = 0,
        .maxValue = RO_N,
        .isEnum = true,
        .midiCC = static_cast<uint8_t>(*MidiCCs::kOscParam0 + 1),
        .valueFormatter = [](const Patch& patch, Patch::paramvalue_t value) -> std::string
        {
            const int sel = std::clamp<int>(std::lround(value), 0, RO_N);
            if (sel == 0)
                return std::string("Preset (") + kOscNames[kPresets[presetIndex(patch)].v[RP_OSC]] + ")";
            return kOscNames[sel - 1];
        },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOsc(v, *val); });

    helper.addParam(*P::kDetune,
                    offsetParam<RP_DTN>("Detune", *MidiCCs::kOscParam0 + 2, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_DTN, *val); });

    helper.addParam(
      *P::kBeat,
      {
        .displayName = "Beat Sync",
        .defaultValue = 0,
        .minValue = 0,
        .maxValue = 8,
        .isEnum = true,
        .midiCC = static_cast<uint8_t>(*MidiCCs::kOscParam0 + 3),
        .valueFormatter = [](const Patch& patch, Patch::paramvalue_t value) -> std::string
        {
            const int sel = std::clamp<int>(std::lround(value), 0, 8);
            if (sel == 0)
                return std::string("Preset (") + kBeatNames[kPresets[presetIndex(patch)].v[RP_BEAT]] + ")";
            return kBeatNames[sel - 1];
        },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setBeat(v, *val); });

    helper.addParam(*P::kSub,
                    offsetParam<RP_SUB>("Sub", *MidiCCs::kOscParam0 + 4, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_SUB, *val); });
    helper.addParam(*P::kCutoff,
                    offsetParam<RP_CUT>("Cutoff", *MidiCCs::kFilterCutoff, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_CUT, *val); });
    helper.addParam(*P::kResonance,
                    offsetParam<RP_RES>("Resonance", *MidiCCs::kFilterResonance, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_RES, *val); });
    helper.addParam(*P::kDrive,
                    offsetParam<RP_DRV>("Drive", *MidiCCs::kOscParam0 + 5, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_DRV, *val); });
    helper.addParam(*P::kNotch,
                    offsetParam<RP_NTCH>("Notch Sweep", *MidiCCs::kOscParam0 + 6, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_NTCH, *val); });
    helper.addParam(*P::kAttack,
                    offsetParam<RP_ATK>("Attack", *MidiCCs::kAmpEnvAttack, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_ATK, *val); });
    helper.addParam(*P::kDecay,
                    offsetParam<RP_DEC>("Decay", *MidiCCs::kAmpEnvDecay, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_DEC, *val); });
    helper.addParam(*P::kSustain,
                    offsetParam<RP_SUS>("Sustain", *MidiCCs::kAmpEnvSustain, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_SUS, *val); });
    helper.addParam(*P::kRelease,
                    offsetParam<RP_REL>("Release", *MidiCCs::kAmpEnvRelease, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_REL, *val); });
    helper.addParam(*P::kFilterEnv,
                    offsetParam<RP_FENV>("Filter Env", *MidiCCs::kFilterEnvAmount, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_FENV, *val); });
    helper.addParam(*P::kGlide,
                    offsetParam<RP_GLIDE>("Glide", *MidiCCs::kOscParam0 + 7, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_GLIDE, *val); });

    helper.addParam(
      *P::kGlideMode,
      {
        .displayName = "Glide Mode",
        .defaultValue = 1,
        .minValue = 0,
        .maxValue = 1,
        .isEnum = true,
        .midiCC = static_cast<uint8_t>(*MidiCCs::kOscParam0 + 8),
        .valueFormatter = [](const Patch&, Patch::paramvalue_t value)
        { return std::string(std::lround(value) ? "Always" : "Legato"); },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setGlideMode(v, *val); });

    helper.addParam(*P::kLfoRate,
                    offsetParam<RP_LRATE>("LFO Rate", *MidiCCs::kLfo1Frequency, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_LRATE, *val); });
    helper.addParam(*P::kLfoDepth,
                    offsetParam<RP_LFLT>("LFO > Cutoff", *MidiCCs::kFilterLfoAmount, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_LFLT, *val); });

    helper.addParam(
      *P::kTranspose,
      {
        .displayName = "Transpose",
        .defaultValue = 0,
        .minValue = -36,
        .maxValue = 36,
        .midiCC = static_cast<uint8_t>(*MidiCCs::kOscParam0 + 9),
        .valueFormatter = createSemitoneFormatter(),
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setTranspose(v, *val); });

    helper.addParam(
      *P::kVelocity,
      {
        .displayName = "Velocity",
        .defaultValue = 0.5f,
        .minValue = 0,
        .maxValue = 1,
        .midiCC = *MidiCCs::kVelocityVolume,
        .valueFormatter = createPercentFormatter(),
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setVelocity(v, *val); });

    helper.addParam(
      *P::kVolume,
      {
        .displayName = "Gain",
        .defaultValue = 0.0f,
        .minValue = -30,
        .maxValue = 6,
        .valueFormatter = [](const Patch&, Patch::paramvalue_t v)
        {
            char b[16];
            snprintf(b, sizeof(b), "%.1f dB", static_cast<double>(v));
            return std::string(b);
        },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setVolume(v, *val); });

    // v1.7: the filter envelope, LFO shape and retrigger, distortion, chorus
    helper.addParam(*P::kFilterAttack,
                    offsetParam<RP_FA>("Filter Attack", *MidiCCs::kFilterEnvAttack, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_FA, *val); });
    helper.addParam(*P::kFilterDecay,
                    offsetParam<RP_FD>("Filter Decay", *MidiCCs::kFilterEnvDecay, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_FD, *val); });
    helper.addParam(*P::kFilterSustain,
                    offsetParam<RP_FS>("Filter Sustain", *MidiCCs::kFilterEnvSustain, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_FS, *val); });
    helper.addParam(*P::kFilterRelease,
                    offsetParam<RP_FR>("Filter Release", *MidiCCs::kFilterEnvRelease, nullptr),
                    [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setOffset(v, RP_FR, *val); });
    helper.addParam(
      *P::kLfoShape,
      {
        .displayName = "LFO Shape",
        .defaultValue = 0,
        .minValue = 0,
        .maxValue = 5,
        .isEnum = true,
        .midiCC = static_cast<uint8_t>(*MidiCCs::kOscParam0 + 10),
        .valueFormatter = [](const Patch&, Patch::paramvalue_t value)
        { return std::string(kLfoShapeNames[std::clamp<int>(std::lround(value), 0, 5)]); },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setLfoShape(v, *val); });
    helper.addParam(
      *P::kLfoRetrig,
      {
        .displayName = "LFO Key Reset",
        .defaultValue = 0,
        .minValue = 0,
        .maxValue = 1,
        .isEnum = true,
        .midiCC = static_cast<uint8_t>(*MidiCCs::kOscParam0 + 11),
        .valueFormatter = [](const Patch&, Patch::paramvalue_t value)
        { return std::string(std::lround(value) ? "On" : "Off (Free)"); },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setLfoRetrig(v, *val); });
    helper.addParam(
      *P::kDistType,
      {
        .displayName = "Dist Type",
        .defaultValue = 0,
        .minValue = 0,
        .maxValue = 3,
        .isEnum = true,
        .valueFormatter = [](const Patch&, Patch::paramvalue_t value)
        { return std::string(kDistNames[std::clamp<int>(std::lround(value), 0, 3)]); },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setDistType(v, *val); });
    helper.addParam(
      *P::kDistAmount,
      {
        .displayName = "Distortion",
        .defaultValue = 0.0f,
        .minValue = 0,
        .maxValue = 1,
        .valueFormatter = createPercentFormatter(),
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setDistAmount(v, *val); });
    helper.addParam(
      *P::kChorusMix,
      {
        .displayName = "Chorus",
        .defaultValue = 0.0f,
        .minValue = 0,
        .maxValue = 1,
        .valueFormatter = createPercentFormatter(),
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setChorusMix(v, *val); });
    helper.addParam(
      *P::kChorusRate,
      {
        .displayName = "Chorus Rate",
        .defaultValue = 0.6f,
        .minValue = 0.05f,
        .maxValue = 8,
        .editCurve = Param::Curve::kExponential,
        .valueFormatter = createFrequencyFormatter(),
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setChorusRate(v, *val); });
    helper.addParam(
      *P::kChorusDepth,
      {
        .displayName = "Chorus Depth",
        .defaultValue = 0.5f,
        .minValue = 0,
        .maxValue = 1,
        .valueFormatter = createPercentFormatter(),
      },
      [](auto& v, Patch::optional_paramvalue_t val) { ReeseSynthVoice::setChorusDepth(v, *val); });

    using EditorElement = Patch::Metadata::EditorLayout::EditorElement;
    auto e = [](P id) { return EditorElement{ .type = EditorElement::Type::kParam, .id = *id }; };

    metadata.layoutFlat = Patch::Metadata::EditorLayout{
        .sections = {
          { .name = "Oscillators", .shortName = "Osc",
            .elements = { e(P::kPreset), e(P::kOsc), e(P::kDetune), e(P::kBeat), e(P::kSub) } },
          { .name = "Filter", .shortName = "Filter",
            .elements = { e(P::kCutoff), e(P::kResonance), e(P::kDrive), e(P::kNotch), e(P::kFilterEnv) } },
          { .name = "Filter Envelope", .shortName = "Filt Env",
            .elements = { e(P::kFilterAttack), e(P::kFilterDecay), e(P::kFilterSustain), e(P::kFilterRelease) } },
          { .name = "Amp Envelope", .shortName = "Amp Env",
            .elements = { e(P::kAttack), e(P::kDecay), e(P::kSustain), e(P::kRelease) } },
          { .name = "LFO", .shortName = "LFO",
            .elements = { e(P::kLfoShape), e(P::kLfoRate), e(P::kLfoDepth), e(P::kLfoRetrig) } },
          { .name = "Glide Voice", .shortName = "Glide",
            .elements = { e(P::kGlide), e(P::kGlideMode) } },
          { .name = "Effects FX", .shortName = "FX",
            .elements = { e(P::kDistType), e(P::kDistAmount), e(P::kChorusMix), e(P::kChorusRate),
                          e(P::kChorusDepth) } },
          { .name = "Voice", .shortName = "Voice",
            .elements = { e(P::kTranspose), e(P::kVelocity), e(P::kVolume) } },
        },
    };
    metadata.layout23 = metadata.layoutFlat;

    metadata.initDefaultMacros = [](auto& macros)
    {
        macros[0] = Patch::Metadata::Macro{ .displayName = "Cutoff",
                                            .modifiers = { { *P::kCutoff, 0.5f } },
                                            .bipolar = true };
        macros[1] = Patch::Metadata::Macro{ .displayName = "Detune",
                                            .modifiers = { { *P::kDetune, 0.5f } },
                                            .bipolar = true };
        macros[2] = Patch::Metadata::Macro{ .displayName = "Notch Sweep",
                                            .modifiers = { { *P::kNotch, 0.5f } } };
        macros[3] = Patch::Metadata::Macro{ .displayName = "Drive",
                                            .modifiers = { { *P::kDrive, 0.5f } } };
        macros[4] = Patch::Metadata::Macro{ .displayName = "Resonance",
                                            .modifiers = { { *P::kResonance, 0.4f } } };
        macros[5] = Patch::Metadata::Macro{ .displayName = "Wobble",
                                            .modifiers = { { *P::kLfoDepth, 0.4f }, { *P::kLfoRate, 0.3f } } };
    };

    metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kReeseSynth);
    metadata.outputChannels = Patch::Metadata::OutputChannels::kStereo; // the chorus
    metadata.maxVoices = 1;
}

std::unique_ptr<InstrumentType> createReeseSynth()
{
    return std::make_unique<InstrumentType>(
      InstrumentType{ .name = "REESE",
                      .createMetadata = &ReeseSynthVoice::createMetadata,
                      .createVoice = [](int /*voiceIdx*/) -> std::unique_ptr<InstrumentVoice>
                      { return std::make_unique<ReeseSynthVoice>(); } });
}

} // namespace polysynth

#endif
