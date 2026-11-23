// Twisted: SUB - DREADNOUGHT's SH-101-style mono bass (Jamie / TwistedGenetics), written again in float for the
// Tracker+ synth SDK. It follows DREADNOUGHT's SUB design, value ranges and its 13 factory sounds; the DSP code here is
// new (none of DREADNOUGHT's third-party GPL code is used).
//
// The SH-101's voice: one oscillator with a saw, a pulse (its width by hand, moved by its own triangle: PWM) and a
// square sub an octave or two down (or the 25 % pulse two down), plus noise, each on the mixer; a resonant 4-pole
// low-pass (a zero-delay-feedback ladder that self-oscillates in tune) with envelope amount and key tracking; the
// envelope (and, here, a filter envelope of its own), or the amp on the gate; portamento OFF / AUTO / ON; accent (a
// loud note: velocity above 100) opening the filter and lifting the level. Then the jungle part, after the amp:
// DRIVE (SOFT, HARD, FOLD, RECT), CRUSH, TONE, and the clean-sub SPLIT: below the split frequency the signal goes
// round the dirt untouched. Plus an LFO on the cutoff (for the WOBBLE preset and friends).

#include "config.h"
#ifdef FEATURE_SYNTH

#include "SubSynthVoice.h"
#include "TwistedPresetVoice.h"

#include <array>
#include <cstdio>

namespace polysynth
{
namespace
{
using namespace twisted;
using namespace twisted::dsp;

enum : int
{
    SP_SAW, SP_PULSE, SP_SUB, SP_NOISE, SP_PW, SP_PWM, SP_PWMR, SP_SUBM,
    SP_CUT, SP_RES, SP_ENV, SP_KEY, SP_DRIVE, SP_DTYPE, SP_CRUSH, SP_TONE,
    SP_ATK, SP_DEC, SP_SUS, SP_REL, SP_GLIDE, SP_GMODE, SP_ACC, SP_VCA, SP_LEVEL, SP_TRANS, SP_SPLIT,
    // Twisted's own
    SP_FA, SP_FD, SP_FS, SP_FR, SP_LSHAPE, SP_LRATE, SP_LDEPTH, SP_LKEY, SP_VEL,
    SP_COUNT
};

enum class SubParamId : int
{
    kPreset = 0,
    kSaw, kPulse, kSub, kNoise, kPw, kPwm, kPwmRate, kSubMode,
    kCut, kRes, kEnv, kKey, kDrive, kDriveType, kCrush, kTone,
    kAtk, kDec, kSus, kRel, kGlide, kPorta, kAccent, kVca, kLevel, kTrans, kSplit,
    kFiltAtk, kFiltDec, kFiltSus, kFiltRel, kLfoShape, kLfoRate, kLfoDepth, kLfoKey, kVelocity,
};
static_assert(static_cast<int>(SubParamId::kVelocity) == SP_COUNT, "one parameter per value");

constexpr const char* kNames[SP_COUNT] = {
    "Saw", "Pulse", "Sub", "Noise", "Pulse Width", "PWM", "PWM Rate", "Sub Mode",
    "Cutoff", "Resonance", "Env Amount", "Key Track", "Drive", "Drive Type", "Crush", "Tone",
    "Attack", "Decay", "Sustain", "Release", "Glide", "Portamento", "Accent", "VCA", "Level", "Transpose",
    "Clean Sub Split", "Filter Attack", "Filter Decay", "Filter Sustain", "Filter Release", "LFO Shape",
    "LFO Rate", "LFO > Cutoff", "LFO Key Reset", "Velocity",
};
constexpr int kMin[SP_COUNT] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -36, 0,
                                 0, 0, 0, 0, 0, 0, -64, 0, 0 };
constexpr int kMax[SP_COUNT] = { 127, 127, 127, 127, 127, 127, 127, 2, 127, 127, 63, 127, 127, 3, 127, 127, 127, 127,
                                 127, 127, 127, 2, 127, 1, 127, 36, 127, 127, 127, 127, 127, 5, 127, 63, 1, 127 };

struct Preset
{
    const char* name;
    int8_t v[SP_SPLIT + 1];
};
// DREADNOUGHT's SUB sounds
//          SAW PUL SUB NOI  PW PWM PWR SBM  CUT RES ENV KEY  DRV TYP CRU TON  ATK DEC SUS REL  GLI GMO ACC VCA LEV TRN SPL
constexpr std::array<Preset, 13> kPresets = { {
  { "Dread", { 40, 90, 110, 0, 30, 0, 0, 0, 48, 30, 6, 40, 70, 0, 0, 90, 0, 70, 110, 30, 40, 1, 60, 0, 100, -24, 70 } },
  { "Donk", { 0, 120, 60, 0, 0, 0, 0, 0, 56, 90, 30, 64, 40, 1, 0, 110, 0, 40, 30, 20, 0, 0, 90, 0, 127, -12, 60 } },
  { "Roller", { 30, 0, 127, 0, 0, 0, 0, 0, 38, 10, 4, 30, 30, 0, 0, 70, 2, 80, 120, 34, 30, 1, 40, 0, 127, -24, 90 } },
  { "Wobble", { 110, 60, 90, 0, 40, 60, 50, 0, 50, 70, 6, 50, 80, 0, 0, 100, 0, 70, 120, 30, 20, 1, 50, 0, 100, -24, 70 } },
  { "Acid 101", { 127, 0, 30, 0, 0, 0, 0, 0, 44, 104, 40, 64, 50, 0, 0, 120, 0, 50, 20, 30, 50, 1, 110, 0, 127, -12, 0 } },
  { "808 Sub", { 0, 0, 127, 0, 0, 0, 0, 0, 20, 0, 8, 40, 90, 2, 0, 90, 0, 90, 0, 60, 45, 2, 30, 0, 100, -24, 100 } },
  { "Growl", { 120, 100, 80, 10, 60, 70, 30, 0, 62, 60, 8, 50, 120, 1, 0, 80, 0, 70, 120, 30, 20, 1, 60, 0, 60, -24, 80 } },
  { "Square Sub", { 0, 100, 127, 0, 0, 0, 0, 2, 52, 20, 4, 40, 20, 0, 0, 100, 0, 70, 120, 30, 0, 0, 40, 1, 120, -12, 0 } },
  { "Punch", { 100, 0, 100, 0, 0, 0, 0, 0, 40, 50, 28, 64, 50, 0, 0, 110, 0, 30, 0, 10, 0, 0, 80, 1, 100, -24, 60 } },
  { "Tech Sub", { 80, 80, 110, 0, 50, 20, 40, 0, 58, 40, 8, 50, 70, 3, 0, 90, 0, 70, 120, 30, 20, 1, 50, 0, 70, -24, 110 } },
  { "Crushed", { 60, 110, 100, 0, 20, 30, 20, 0, 60, 50, 8, 50, 40, 0, 70, 90, 0, 70, 120, 30, 20, 1, 50, 0, 100, -24, 80 } },
  { "Rave Stab", { 127, 90, 40, 30, 30, 40, 70, 0, 70, 60, 28, 64, 60, 0, 0, 127, 0, 50, 20, 40, 0, 0, 70, 0, 120, 0, 0 } },
  { "Init", { 100, 0, 0, 0, 0, 0, 0, 0, 127, 0, 0, 0, 0, 0, 0, 127, 0, 64, 127, 20, 0, 0, 0, 0, 100, -24, 0 } },
} };

class SubSynthVoice : public PresetVoice<SubSynthVoice, SubParamId>
{
  public:
    static constexpr int kValues = SP_COUNT;
    static const char* valueName(int k) { return kNames[k]; }
    static int valueMin(int k) { return kMin[k]; }
    static int valueMax(int k) { return kMax[k]; }
    static int presetCount() { return static_cast<int>(kPresets.size()); }
    static const char* presetName(int i) { return kPresets[i].name; }
    static int presetValue(int i, int k)
    {
        const Preset& pr = kPresets[i];
        switch (k)
        {
            case SP_FA: return pr.v[SP_ATK]; // the filter envelope starts as the amp's (the 101's one envelope)
            case SP_FD: return pr.v[SP_DEC];
            case SP_FS: return pr.v[SP_SUS];
            case SP_FR: return pr.v[SP_REL];
            case SP_LSHAPE: return 0;
            case SP_LRATE: return 50;
            case SP_LDEPTH: return 0;
            case SP_LKEY: return 0;
            case SP_VEL: return 64;
            default: return pr.v[k];
        }
    }
    static std::string valueText(int k, int v)
    {
        static const char* const kSubModes[3] = { "-1 Oct", "-2 Oct", "-2 Oct 25%" };
        static const char* const kTypes[4] = { "Soft", "Hard", "Fold", "Rect" };
        static const char* const kPorta[3] = { "Off", "Auto", "On" };
        static const char* const kShapes[6] = { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H" };
        switch (k)
        {
            case SP_SUBM: return kSubModes[std::clamp(v, 0, 2)];
            case SP_DTYPE: return kTypes[std::clamp(v, 0, 3)];
            case SP_GMODE: return kPorta[std::clamp(v, 0, 2)];
            case SP_VCA: return v ? "Gate" : "Envelope";
            case SP_LSHAPE: return kShapes[std::clamp(v, 0, 5)];
            case SP_LKEY: return v ? "On" : "Off (Free)";
            case SP_PW: return intText(50 - v * 45 / 127) + " %";
            case SP_CUT: return hzText(cutoffHz(static_cast<float>(v)));
            case SP_ATK:
            case SP_DEC:
            case SP_REL:
            case SP_FA:
            case SP_FD:
            case SP_FR: return msText(v);
            case SP_GLIDE: return v ? msText(v) : "Off";
            case SP_CRUSH: return v ? intText(v * 100 / 127) + " %" : "Off";
            case SP_SPLIT: return v ? intText(40 + v * 260 / 127) + " Hz" : "Off";
            case SP_PWMR:
            case SP_LRATE:
            {
                char b[16];
                snprintf(b, sizeof(b), "%.2f Hz", static_cast<double>(lfoHz(v)));
                return b;
            }
            case SP_ENV:
            case SP_LDEPTH: return intText(v, true);
            case SP_TRANS: return intText(v + 24, true) + " st"; // 0 plays the note entered (the presets sit 2 octaves down)
            default: return intText(v * 100 / 127) + " %";
        }
    }

    SubSynthVoice()
    {
        playMode = PlayMode::kLegato;
        recompute();
    }

    void onValuesChanged() {}

    bool isActive() final { return active || ampNow > 0.0f || ampNext > 0.0f; }

    void onNoteOn(const InstrumentVoiceContext&, note_t note, float velocity, bool isLegatoStep) final
    {
        vel = velocity;
        acc = velocity > 100.0f / 127.0f; // a loud note is an accent
        noteNum = static_cast<int>(note);
        target = static_cast<float>(note) + static_cast<float>(p[SP_TRANS] + 24);
        const bool glide = p[SP_GLIDE] && (p[SP_GMODE] == 2 || (p[SP_GMODE] == 1 && isLegatoStep && gate));
        if (!glide || !active)
            pitch = target;
        if (p[SP_LKEY])
        {
            lfoPh = 0.0f;
            lfoHold = randBipolar();
        }
        if (isLegatoStep && gate && active && stage)
        {
            gate = true; // legato: no new attack
            return;
        }
        if (!active)
        {
            ph = 0.5f; // the saw from its zero crossing
            sph = 0.0f;
            ladder.reset(0.002f);
        }
        gate = active = true;
        stage = fstage = 1;
    }

    void onNoteOff(const InstrumentVoiceContext&, note_t) final
    {
        gate = false;
        if (stage)
            stage = 3;
        if (fstage)
            fstage = 3;
    }

    void renderVoiceAudio(const InstrumentVoiceContext&,
                          [[maybe_unused]] std::optional<AudioBufferStereoRef> inputBuffer,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef>) final
    {
        auto out = outputBuffer[0];
        size_t i = 0;
        while (i < out.size())
        {
            if (ctl == 0)
            {
                control();
                ctl = kCtl;
            }
            const size_t len = std::min<size_t>(out.size() - i, static_cast<size_t>(ctl));
            if (!active && ampNow <= 0.0f && ampNext <= 0.0f)
                std::fill(out.begin() + i, out.begin() + i + len, 0.0f);
            else
                renderBlock(&out[i], static_cast<int>(len));
            ctl -= static_cast<int>(len);
            i += len;
        }
    }

    static void createMetadata(const Patch& patch, Patch::Metadata& metadata);

  private:
    static constexpr int kCtl = 32;
    bool gate = false, active = false, acc = false;
    float vel = 1.0f;
    int noteNum = 60;
    float pitch = 36.0f, target = 36.0f;
    uint8_t stage = 0, fstage = 0;
    float env = 0.0f, fenv = 0.0f, gamp = 0.0f;
    float ampNow = 0.0f, ampNext = 0.0f;
    int ctl = 0;
    float ph = 0.0f, sph = 0.0f, pwPh = 0.0f;
    float lfoPh = 0.0f, lfoHold = 0.0f, lfoVal = 0.0f;
    dsp::Ladder ladder;
    float lo1 = 0.0f, lo2 = 0.0f, tone = 0.0f, hold = 0.0f, crPh = 0.0f, dc = 0.0f;
    uint32_t rng = 0x101101u;
    // per tick
    float inc = 0.0f, sinc = 0.0f, pw = 0.5f, comp = 1.0f;

    float randBipolar()
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return static_cast<float>(static_cast<int32_t>(rng)) / 2147483648.0f;
    }

    static void stepEnv(float& e, uint8_t& st, int a, int d, float s, int r)
    {
        switch (st)
        {
            case 1:
                e += (1.25f - e) * tickCoef(std::max(a, 14), kCtl); // never quicker than ~3 ms: a sharper start clicks
                if (e >= 1.0f)
                {
                    e = 1.0f;
                    st = 2;
                }
                break;
            case 2: e += (s - e) * tickCoef(d, kCtl); break;
            case 3:
                e -= e * tickCoef(r, kCtl);
                if (e < 1.0f / 4096.0f)
                {
                    e = 0.0f;
                    st = 0;
                }
                break;
            default: break;
        }
    }

    void control()
    {
        stepEnv(env, stage, p[SP_ATK], p[SP_DEC], p[SP_SUS] / 127.0f, p[SP_REL]);
        stepEnv(fenv, fstage, p[SP_FA], p[SP_FD], p[SP_FS] / 127.0f, p[SP_FR]);
        gamp += ((gate ? 1.0f : 0.0f) - gamp) * tickCoef(14, kCtl); // GATE: declicked
        const float a = p[SP_VCA] ? gamp : env;
        if (!stage && (!p[SP_VCA] || gamp < 1.0f / 4096.0f))
            active = false;

        if (pitch != target)
        {
            const float d = target - pitch;
            if (!p[SP_GLIDE] || std::fabs(d) < 0.008f)
                pitch = target;
            else
                pitch += d * tickCoef(p[SP_GLIDE], kCtl);
        }

        ampNow = ampNext;
        float an = a * 0.787f;
        if (acc)
            an += an * p[SP_ACC] / 254.0f; // accent: louder by up to half again
        an *= 1.0f - (p[SP_VEL] / 127.0f) * (1.0f - vel);
        ampNext = active ? an : 0.0f;

        inc = noteToInc(pitch);
        sinc = inc * (p[SP_SUBM] == 0 ? 0.5f : 0.25f);

        // the PWM triangle and the pulse's width (3 % .. 97 %)
        pwPh = wrap01(pwPh + lfoHz(p[SP_PWMR]) * kCtl / kSr);
        const float tri = pwPh < 0.5f ? 4.0f * pwPh - 1.0f : 3.0f - 4.0f * pwPh;
        pw = std::clamp(0.5f - 0.45f * p[SP_PW] / 127.0f + 0.248f * tri * p[SP_PWM] / 127.0f, 0.03f, 0.97f);

        // the LFO on the cutoff
        const float nextPh = lfoPh + lfoHz(p[SP_LRATE]) * kCtl / kSr;
        if (nextPh >= 1.0f)
            lfoHold = randBipolar();
        lfoPh = wrap01(nextPh);
        switch (p[SP_LSHAPE])
        {
            case 1: lfoVal = lfoPh < 0.5f ? 4.0f * lfoPh - 1.0f : 3.0f - 4.0f * lfoPh; break;
            case 2: lfoVal = 2.0f * lfoPh - 1.0f; break;
            case 3: lfoVal = 1.0f - 2.0f * lfoPh; break;
            case 4: lfoVal = lfoPh < 0.5f ? 1.0f : -1.0f; break;
            case 5: lfoVal = lfoHold; break;
            default: lfoVal = sdk::fastSine(lfoPh); break;
        }

        // the cutoff: CUT, the filter envelope (with accent), the keys, accent, the LFO (steps of 0..127)
        const float steps = static_cast<float>(p[SP_CUT]) +
                            fenv * (static_cast<float>(p[SP_ENV]) + (acc ? p[SP_ACC] / 6.0f : 0.0f)) +
                            (noteNum - 60) * 1.168f * p[SP_KEY] / 127.0f + (acc ? p[SP_ACC] * 8.0f / 256.0f : 0.0f) +
                            lfoVal * p[SP_LDEPTH];
        const float k = p[SP_RES] * 4.25f / 127.0f;
        ladder.set(cutoffHz(steps), k);
        comp = 1.0f + k * 0.25f; // a little of the passband kept at high resonance
    }

    inline float drive(float x, float gain, int type)
    {
        const float d = x * gain;
        switch (type)
        {
            case 1: return std::clamp(d, -0.671f, 0.671f) * 1.5f;
            case 2: // a triangle fold: past +-T it turns back
            {
                constexpr float T = 0.61f;
                float u = std::fmod(d, 4.0f * T);
                if (u < 0.0f)
                    u += 4.0f * T;
                const float y = u < T ? u : (u < 3.0f * T ? 2.0f * T - u : u - 4.0f * T);
                return y * 1.5f;
            }
            case 3: // full-wave: an octave up and rasp, its DC taken out
            {
                const float r = std::fabs(softClip(d));
                dc += (r - dc) * (1.0f / 1024.0f);
                return (r - dc) * 2.0f;
            }
            default: return softClip(d);
        }
    }

    void renderBlock(float* out, int len)
    {
        const float lsaw = p[SP_SAW] * 100.0f / 16384.0f, lpul = p[SP_PULSE] * 100.0f / 16384.0f;
        const float lsub = p[SP_SUB] * 110.0f / 16384.0f, lnoi = p[SP_NOISE] * 90.0f / 16384.0f;
        const int dtype = p[SP_DTYPE];
        const float gain = 1.0f + p[SP_DRIVE] * p[SP_DRIVE] * 2.0f / 4096.0f;
        const int crush = p[SP_CRUSH];
        const float split = p[SP_SPLIT] ? (40.0f + p[SP_SPLIT] * 260.0f / 127.0f) * 6.2832f / kSr : 0.0f;
        const float level = p[SP_LEVEL] / 100.0f * 0.519f;
        float ta = 1.0f;
        if (p[SP_TONE] < 127)
        {
            const float g = std::tan(3.14159265f * cutoffHz(24.0f + p[SP_TONE] * 103.0f / 127.0f) / kSr);
            ta = std::min(1.0f, 2.0f * g / (1.0f + g));
        }
        const int pos0 = kCtl - ctl;
        const float da = (ampNext - ampNow) / kCtl;
        const float crushStep = crush ? std::exp2(static_cast<float>(crush * 11 / 127)) / 32768.0f : 0.0f;

        for (int i = 0; i < len; i++)
        {
            float x = 0.0f;
            if (lsaw > 0.0f)
                x += saw(ph, inc) * lsaw;
            if (lpul > 0.0f)
                x += pulse(ph, inc, pw) * lpul;
            if (lsub > 0.0f)
                x += pulse(sph, sinc, p[SP_SUBM] == 2 ? 0.25f : 0.5f) * lsub;
            x += randBipolar() * (lnoi > 0.0f ? lnoi : 1.0f / 2048.0f); // a -66 dB floor: the filter rings up from it
            ph += inc;
            if (ph >= 1.0f)
                ph -= 1.0f;
            sph += sinc;
            if (sph >= 1.0f)
                sph -= 1.0f;

            const float amp = ampNow + da * static_cast<float>(pos0 + i);
            float y = std::clamp(ladder.tick(x, comp), -2.44f, 2.44f) * amp;
            float lo = 0.0f;
            if (split > 0.0f) // the clean low end, round the dirt
            {
                lo1 += (y - lo1) * split;
                lo2 += (lo1 - lo2) * split;
                lo = lo2;
            }
            float hi = y - lo;
            if (p[SP_DRIVE] || dtype == 3)
                hi = drive(hi, gain, dtype);
            if (crush) // rate down (hold), then bits down
            {
                crPh += 1.0f / (1.0f + crush / 8);
                if (crPh >= 1.0f)
                {
                    crPh -= 1.0f;
                    hold = hi;
                }
                hi = std::floor(hold / crushStep) * crushStep;
            }
            if (ta < 1.0f)
            {
                tone += (hi - tone) * ta;
                hi = tone;
            }
            out[i] = std::clamp(std::clamp(hi + lo, -1.373f, 1.373f) * level, -1.0f, 1.0f);
        }
    }
};

} // namespace

void SubSynthVoice::createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
{
    using namespace polysynth::sdk;
    using P = SubParamId;
    metadata.params.resize(enumutil::EnumMax<SubParamId>::value + 1);
    metadata.params.shrink_to_fit();
    if (paramHandlers.empty())
        paramHandlers.resize(enumutil::EnumMax<SubParamId>::value + 1);
    ParamHelper<SubSynthVoice> helper(metadata, paramHandlers);
    addPresetParams(metadata, helper);

    using EditorElement = Patch::Metadata::EditorLayout::EditorElement;
    auto e = [](P id) { return EditorElement{ .type = EditorElement::Type::kParam, .id = *id }; };
    metadata.layoutFlat = Patch::Metadata::EditorLayout{
        .sections = {
          { .name = "Oscillator", .shortName = "Osc",
            .elements = { e(P::kPreset), e(P::kSaw), e(P::kPulse), e(P::kSub), e(P::kNoise), e(P::kSubMode) } },
          { .name = "Pulse Width", .shortName = "PWM", .elements = { e(P::kPw), e(P::kPwm), e(P::kPwmRate) } },
          { .name = "Filter", .shortName = "Filter",
            .elements = { e(P::kCut), e(P::kRes), e(P::kEnv), e(P::kKey), e(P::kAccent) } },
          { .name = "Filter Envelope", .shortName = "Filt Env",
            .elements = { e(P::kFiltAtk), e(P::kFiltDec), e(P::kFiltSus), e(P::kFiltRel) } },
          { .name = "Amp Envelope", .shortName = "Amp Env",
            .elements = { e(P::kAtk), e(P::kDec), e(P::kSus), e(P::kRel), e(P::kVca) } },
          { .name = "LFO", .shortName = "LFO",
            .elements = { e(P::kLfoShape), e(P::kLfoRate), e(P::kLfoDepth), e(P::kLfoKey) } },
          { .name = "Drive FX", .shortName = "Drive",
            .elements = { e(P::kDrive), e(P::kDriveType), e(P::kCrush), e(P::kTone), e(P::kSplit) } },
          { .name = "Voice", .shortName = "Voice",
            .elements = { e(P::kGlide), e(P::kPorta), e(P::kTrans), e(P::kVelocity), e(P::kLevel) } },
        },
    };
    metadata.layout23 = metadata.layoutFlat;
    metadata.initDefaultMacros = [](auto& macros)
    {
        macros[0] = Patch::Metadata::Macro{ .displayName = "Cutoff", .modifiers = { { *P::kCut, 0.5f } },
                                            .bipolar = true };
        macros[1] = Patch::Metadata::Macro{ .displayName = "Resonance", .modifiers = { { *P::kRes, 0.4f } } };
        macros[2] = Patch::Metadata::Macro{ .displayName = "Env Amount", .modifiers = { { *P::kEnv, 0.4f } },
                                            .bipolar = true };
        macros[3] = Patch::Metadata::Macro{ .displayName = "Drive", .modifiers = { { *P::kDrive, 0.5f } } };
        macros[4] = Patch::Metadata::Macro{ .displayName = "Decay", .modifiers = { { *P::kDec, 0.4f },
                                                                                   { *P::kFiltDec, 0.4f } },
                                            .bipolar = true };
        macros[5] = Patch::Metadata::Macro{ .displayName = "Wobble", .modifiers = { { *P::kLfoDepth, 0.3f } } };
    };
    metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kSubSynth);
    metadata.outputChannels = Patch::Metadata::OutputChannels::kMono;
    metadata.maxVoices = 1;
}

std::unique_ptr<InstrumentType> createSubSynth()
{
    return std::make_unique<InstrumentType>(
      InstrumentType{ .name = "SUB",
                      .createMetadata = &SubSynthVoice::createMetadata,
                      .createVoice = [](int) -> std::unique_ptr<InstrumentVoice>
                      { return std::make_unique<SubSynthVoice>(); } });
}

} // namespace polysynth

#endif
