// Twisted: JUNO - DREADNOUGHT's Juno-60 / 106-style polysynth (Jamie / TwistedGenetics) as a Tracker+ synth engine.
// Each Tracker voice is one Juno voice, made from DREADNOUGHT's JUNO code (its own float DSP): one DCO with a saw and a
// pulse (on / off each; the pulse's width moved by the LFO, by hand or by the envelope), a square sub an octave down
// and noise, RANGE 16' 8' 4' and the LFO on the pitch; the HPF (BOOST, FLAT, 2, 3); a resonant 4-pole low-pass that
// self-oscillates in tune, with the envelope (+ or inverted), the LFO, the keys and the velocity on its cutoff; ONE
// ADSR for the filter and the amp, or the amp on the gate; one triangle LFO with its DELAY (FREE or KEY, free or
// synced to the tempo); and the CHORUS I, II and I+II, two delay lines swept in opposite directions, left and right.
//
// The HPF and the chorus are linear, so running them per voice (the chorus's sweep from one clock for all voices)
// sounds as the Juno's single ones do. The LFO's DELAY counts from each voice's own key.

#include "config.h"
#ifdef FEATURE_SYNTH

#include "JunoSynthVoice.h"
#include "TwistedPresetVoice.h"
#include "Platform.h"
#include "Tracker.h"

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
    JP_RANGE, JP_SAW, JP_PULSE, JP_SUB, JP_PWM, JP_PWMSRC, JP_NOISE, JP_DLFO,
    JP_CUT, JP_RES, JP_FENV, JP_FKEY, JP_HPF, JP_FPOL, JP_FLFO, JP_FVEL,
    JP_ATK, JP_DEC, JP_SUS, JP_REL, JP_VCA, JP_VVEL,
    JP_LRATE, JP_LDELAY, JP_LSYNC, JP_LMODE,
    JP_CHORUS, JP_LEVEL, JP_TRANS,
    JP_N
};

enum class JunoParamId : int
{
    kPreset = 0,
    kRange, kSaw, kPulse, kSub, kPwm, kPwmSrc, kNoise, kDcoLfo,
    kCut, kRes, kEnv, kKey, kHpf, kEnvPol, kFiltLfo, kFiltVel,
    kAtk, kDec, kSus, kRel, kVca, kAmpVel,
    kLfoRate, kLfoDelay, kLfoSync, kLfoMode,
    kChorus, kLevel, kTrans,
};
static_assert(static_cast<int>(JunoParamId::kTrans) == JP_N, "one parameter per value");

constexpr const char* kNames[JP_N] = {
    "Range", "Saw", "Pulse", "Sub", "PWM", "PWM Source", "Noise", "DCO LFO",
    "Cutoff", "Resonance", "Env Amount", "Key Track", "HPF", "Env Polarity", "Filter LFO", "Filter Velocity",
    "Attack", "Decay", "Sustain", "Release", "VCA", "Amp Velocity",
    "LFO Rate", "LFO Delay", "LFO Sync", "LFO Mode", "Chorus", "Level", "Transpose",
};
constexpr int kMin[JP_N] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -24 };
constexpr int kMax[JP_N] = { 2, 1, 1, 127, 127, 2, 127, 127, 127, 127, 127, 127, 3, 1, 127, 127,
                             127, 127, 127, 127, 1, 127, 127, 127, 12, 1, 3, 127, 24 };
constexpr int16_t kInit[JP_N] = { 1, 1, 0, 0, 0, 0, 0, 0, 100, 0, 0, 64, 1, 0, 0, 0,
                                  0, 40, 127, 30, 0, 30, 60, 0, 0, 0, 0, 100, 0 };
constexpr uint8_t kDiv[12] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 }; // a synced LFO: sixteenths a cycle
constexpr const char* kDivNames[12] = { "1/16", "1/8", "1/8D", "1/4", "1/4D", "1/2", "1/2D", "1 Bar", "1.5 Bars",
                                        "2 Bars", "3 Bars", "4 Bars" };

struct Preset
{
    const char* name;
    int16_t kv[40]; // (value, setting) pairs over INIT, -1 ends
};
// DREADNOUGHT's JUNO sounds
constexpr std::array<Preset, 17> kPresets = { {
  { "Init", { -1 } },
  { "Juno Pad", { JP_PULSE, 1, JP_PWM, 70, JP_CUT, 70, JP_RES, 20, JP_FENV, 30, JP_ATK, 70, JP_DEC, 80, JP_SUS, 100,
                  JP_REL, 85, JP_LRATE, 45, JP_LDELAY, 30, JP_CHORUS, 2, -1 } },
  { "Brass", { JP_CUT, 45, JP_RES, 15, JP_FENV, 75, JP_FKEY, 80, JP_ATK, 25, JP_DEC, 60, JP_SUS, 80, JP_REL, 40,
               JP_CHORUS, 1, -1 } },
  { "Strings", { JP_SAW, 0, JP_PULSE, 1, JP_PWM, 90, JP_LRATE, 55, JP_CUT, 85, JP_HPF, 2, JP_ATK, 75, JP_DEC, 60,
                 JP_SUS, 120, JP_REL, 85, JP_CHORUS, 2, -1 } },
  { "106 Bass", { JP_RANGE, 0, JP_SAW, 0, JP_PULSE, 1, JP_PWM, 40, JP_PWMSRC, 1, JP_SUB, 127, JP_CUT, 35, JP_RES, 40,
                  JP_FENV, 60, JP_DEC, 55, JP_SUS, 0, JP_REL, 20, JP_HPF, 0, -1 } },
  { "Organ", { JP_SAW, 0, JP_PULSE, 1, JP_SUB, 100, JP_CUT, 110, JP_VCA, 1, JP_REL, 10, JP_LEVEL, 75, JP_CHORUS, 1, -1 } },
  { "Rave Stab", { JP_PULSE, 1, JP_PWM, 60, JP_PWMSRC, 1, JP_CUT, 60, JP_RES, 70, JP_FENV, 70, JP_DEC, 50,
                   JP_SUS, 20, JP_REL, 35, JP_CHORUS, 3, -1 } },
  { "Pluck", { JP_CUT, 40, JP_RES, 30, JP_FENV, 80, JP_FVEL, 40, JP_DEC, 45, JP_SUS, 0, JP_REL, 45, JP_CHORUS, 2, -1 } },
  { "Piano", { JP_PULSE, 1, JP_PWM, 30, JP_PWMSRC, 1, JP_CUT, 55, JP_FENV, 60, JP_FKEY, 100, JP_DEC, 85, JP_SUS, 0,
               JP_REL, 60, JP_VVEL, 80, JP_CHORUS, 1, -1 } },
  { "Sub Bass", { JP_RANGE, 0, JP_SAW, 0, JP_SUB, 127, JP_CUT, 40, JP_DEC, 60, JP_REL, 15, JP_HPF, 0, -1 } },
  { "Acid Poly", { JP_CUT, 30, JP_RES, 95, JP_FENV, 85, JP_DEC, 40, JP_SUS, 10, JP_REL, 25, -1 } },
  { "Glass", { JP_RANGE, 2, JP_SAW, 0, JP_PULSE, 1, JP_PWM, 100, JP_PWMSRC, 2, JP_HPF, 3, JP_CUT, 95, JP_RES, 60,
               JP_ATK, 5, JP_DEC, 90, JP_SUS, 30, JP_REL, 90, JP_CHORUS, 3, -1 } },
  { "Dark Pad", { JP_SUB, 60, JP_CUT, 35, JP_RES, 55, JP_FLFO, 40, JP_LRATE, 20, JP_LDELAY, 60, JP_ATK, 90,
                  JP_REL, 95, JP_CHORUS, 2, -1 } },
  { "Noise Sweep", { JP_SAW, 0, JP_NOISE, 127, JP_CUT, 50, JP_RES, 90, JP_FLFO, 90, JP_LRATE, 30, JP_ATK, 60,
                     JP_REL, 80, JP_CHORUS, 2, -1 } },
  { "Whistle", { JP_SAW, 0, JP_CUT, 60, JP_RES, 127, JP_FKEY, 127, JP_ATK, 40, JP_REL, 50, JP_CHORUS, 1, -1 } },
  { "Hooverish", { JP_RANGE, 0, JP_PULSE, 1, JP_PWM, 110, JP_LRATE, 80, JP_DLFO, 30, JP_LDELAY, 40, JP_CUT, 80,
                   JP_RES, 30, JP_FENV, 20, JP_CHORUS, 3, -1 } },
  { "Jungle Stab", { JP_PULSE, 1, JP_PWM, 50, JP_PWMSRC, 1, JP_SUB, 80, JP_CUT, 70, JP_RES, 35, JP_FENV, 55,
                     JP_DEC, 45, JP_SUS, 0, JP_REL, 30, JP_HPF, 2, JP_CHORUS, 1, -1 } },
} };

int presetValueOf(int i, int k)
{
    const Preset& pr = kPresets[i];
    int v = kInit[k];
    for (int j = 0; j + 1 < 40 && pr.kv[j] >= 0; j += 2)
        if (pr.kv[j] == k)
            v = pr.kv[j + 1];
    return v;
}

// one clock for every voice (the chorus's sweep and a free LFO stay in step across voices)
#ifdef TWISTED_HOST_TEST
double nowSeconds()
{
    static double t = 0.0;
    t += 128.0 / 44100.0;
    return t;
}
#else
double nowSeconds()
{
    static uint32_t last = 0;
    static double acc = 0.0;
    const uint32_t us = Tracker::getInstance()->getPlatform()->getTimer()->micros();
    acc += static_cast<double>(us - last) * 1e-6;
    last = us;
    return acc;
}
#endif

class JunoSynthVoice : public PresetVoice<JunoSynthVoice, JunoParamId>
{
  public:
    static constexpr int kValues = JP_N;
    static const char* valueName(int k) { return kNames[k]; }
    static int valueMin(int k) { return kMin[k]; }
    static int valueMax(int k) { return kMax[k]; }
    static int presetCount() { return static_cast<int>(kPresets.size()); }
    static const char* presetName(int i) { return kPresets[i].name; }
    static int presetValue(int i, int k) { return presetValueOf(i, k); }
    static std::string valueText(int k, int v)
    {
        static const char* const kRange[3] = { "16'", "8'", "4'" };
        static const char* const kSrc[3] = { "LFO", "Manual", "Envelope" };
        static const char* const kHpf[4] = { "Boost", "Flat", "2", "3" };
        static const char* const kCh[4] = { "Off", "I", "II", "I+II" };
        switch (k)
        {
            case JP_RANGE: return kRange[std::clamp(v, 0, 2)];
            case JP_SAW:
            case JP_PULSE: return v ? "On" : "Off";
            case JP_PWMSRC: return kSrc[std::clamp(v, 0, 2)];
            case JP_HPF: return kHpf[std::clamp(v, 0, 3)];
            case JP_FPOL: return v ? "Inverted" : "Normal";
            case JP_VCA: return v ? "Gate" : "Envelope";
            case JP_LSYNC: return v ? kDivNames[std::clamp(v - 1, 0, 11)] : "Off (Rate)";
            case JP_LMODE: return v ? "Key" : "Free";
            case JP_CHORUS: return kCh[std::clamp(v, 0, 3)];
            case JP_TRANS: return intText(v, true) + " st";
            case JP_CUT: return hzText(30.0f * std::exp2(v * 108.7f / 127.0f / 12.0f));
            case JP_ATK:
            case JP_DEC:
            case JP_REL: return msText(v);
            case JP_LRATE:
            {
                char b[16];
                snprintf(b, sizeof(b), "%.2f Hz", static_cast<double>(0.1f * std::exp2(v * 8.229f / 127.0f)));
                return b;
            }
            case JP_FKEY: return intText(v * 100 / 127) + " %";
            default: return intText(v);
        }
    }

    JunoSynthVoice()
    {
        static uint32_t seed = 0x2468ACEu;
        seed = seed * 1664525u + 1013904223u;
        nz = seed;
        recompute();
    }

    void onValuesChanged() {}

    bool isActive() final { return live; }

    void onNoteOn(const InstrumentVoiceContext&, note_t note, float velocity, bool) final
    {
        if (!live)
        {
            nz = nz * 1664525u + 1013904223u;
            ph = static_cast<float>(nz >> 8) / 16777216.0f; // the DCOs run free: no two voices in phase
            sph = ph * 0.5f;
            env = 0.0f;
            amp = 0.0f;
            G = -1.0f;
            s[0] = 1e-3f; // a breath in it: a ringing filter starts alone
            s[1] = s[2] = s[3] = 0.0f;
        }
        lt = 0;
        if (p[JP_LMODE])
            lph = 0.75f; // the triangle at 0, rising
        live = gate = true;
        noteNum = static_cast<int>(note);
        vel = std::max(1, static_cast<int>(std::lround(velocity * 127.0f)));
        st = 0;
    }

    void onNoteOff(const InstrumentVoiceContext&, note_t) final
    {
        gate = false;
        st = 2;
    }

    void renderVoiceAudio(const InstrumentVoiceContext& ctx,
                          [[maybe_unused]] std::optional<AudioBufferStereoRef> inputBuffer,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef>) final
    {
        auto outL = outputBuffer[0];
        auto outR = outputBuffer[1];
        bpm = ctx.tempo > 1.0f ? ctx.tempo : 120.0f;
        const double t0 = nowSeconds();
        size_t i0 = 0;
        while (i0 < outL.size())
        {
            const int len = static_cast<int>(std::min<size_t>(outL.size() - i0, kTick));
            if (!live)
            {
                std::fill(outL.begin() + i0, outL.end(), 0.0f);
                std::fill(outR.begin() + i0, outR.end(), 0.0f);
                return;
            }
            tick(&outL[i0], &outR[i0], len, t0 + static_cast<double>(i0) / kSr);
            i0 += static_cast<size_t>(len);
        }
    }

    static void createMetadata(const Patch& patch, Patch::Metadata& metadata);

  private:
    static constexpr int kTick = 32;
    static constexpr int kChorusLen = 256;

    bool live = false, gate = false;
    int noteNum = 60, vel = 100;
    uint8_t st = 0;
    float ph = 0.0f, sph = 0.0f;
    float env = 0.0f, amp = 0.0f, G = -1.0f;
    float s[4] = {};
    float lph = 0.0f;
    uint32_t lt = 0;
    uint32_t nz = 1;
    float bpm = 120.0f;
    float hlp = 0.0f;
    int16_t cb[kChorusLen] = {};
    uint32_t cw = 0;
    float clp[2] = {};

    static inline float sat(float u) // a soft clip (the ladder's input): u - 4u^3/27, flat past 1.5
    {
        if (u > 1.5f)
            return 1.0f;
        if (u < -1.5f)
            return -1.0f;
        return u - 0.148148f * u * u * u;
    }
    static inline float kOf(int v) { return tickCoef(v, kTick); }

    void tick(float* outL, float* outR, int len, double t0)
    {
        const float rl = 1.0f / static_cast<float>(len);
        const float kres = p[JP_RES] * (4.6f / 127.0f);

        // the LFO: free (from the shared clock) or from this key (KEY), faded in after the key (DELAY)
        float hz;
        if (p[JP_LSYNC] > 0)
            hz = bpm * (4.0f / 60.0f) / kDiv[(p[JP_LSYNC] - 1) % 12];
        else
            hz = 0.1f * std::exp2(p[JP_LRATE] * (8.229f / 127.0f));
        if (p[JP_LMODE])
            lph = wrap01(lph + hz * kTick / kSr);
        else
            lph = static_cast<float>(std::fmod(t0 * hz, 1.0));
        float lfo = 4.0f * std::fabs(lph - 0.5f) - 1.0f;
        const float d = p[JP_LDELAY] / 127.0f;
        if (d > 0.0f) // DELAY: up to 3 s still, then as long fading in
        {
            const float sec = static_cast<float>(lt) * (kTick / kSr), dl = 3.0f * d * d;
            lfo *= std::clamp((sec - dl) / std::max(dl, 0.05f), 0.0f, 1.0f);
        }
        if (lt < 0x7FFFFFFFu)
            lt++;

        // the envelope (ONE, the Juno's)
        if (st == 0)
        {
            env += (1.3f - env) * kOf(p[JP_ATK]);
            if (env >= 1.0f)
            {
                env = 1.0f;
                st = 1;
            }
        }
        else if (st == 1)
            env += (p[JP_SUS] / 127.0f - env) * kOf(p[JP_DEC]);
        else
            env -= env * kOf(p[JP_REL]);

        // the level: the envelope or the gate, less for a soft note
        const float vv = p[JP_VVEL] / 127.0f, vf = 1.0f - vv * (1.0f - vel / 127.0f);
        float a1 = (p[JP_VCA] ? (gate ? 1.0f : 0.0f) : env) * vf;
        if (p[JP_VCA] && !gate)
            a1 = amp * 0.5f;
        if (!gate && (p[JP_VCA] ? amp < 1e-4f : env < 1e-4f))
        {
            live = false;
            std::fill(outL, outL + len, 0.0f);
            std::fill(outR, outR + len, 0.0f);
            return;
        }

        // the pitch: the note, TRANSPOSE, RANGE, the LFO
        float pitch = static_cast<float>(noteNum) + p[JP_TRANS] + 12.0f * (p[JP_RANGE] - 1);
        const float dl = p[JP_DLFO] / 127.0f;
        pitch += lfo * dl * dl * 7.0f;
        const float inc = noteToInc(pitch), sinc = inc * 0.5f;

        // the cutoff
        const float c = p[JP_CUT] * (108.7f / 127.0f) + p[JP_FENV] * (96.0f / 127.0f) * env * (p[JP_FPOL] ? -1.0f : 1.0f) +
                        p[JP_FLFO] * (36.0f / 127.0f) * lfo + (noteNum - 60.0f) * p[JP_FKEY] / 127.0f +
                        p[JP_FVEL] * (24.0f / 127.0f) * (vel - 64.0f) / 63.0f;
        const float f = std::clamp(30.0f * std::exp2(c / 12.0f), 20.0f, 18000.0f);
        const float g = std::tan(3.14159265f * f / kSr);
        const float G1 = g / (1.0f + g);
        if (G < 0.0f)
            G = G1;
        const float comp = 1.0f + 0.4f * kres;
        const float inv0 = 1.0f / (1.0f + kres * G * G * G * G), inv1 = 1.0f / (1.0f + kres * G1 * G1 * G1 * G1);
        float Gc = G, inv = inv0, a = amp;
        const float dG = (G1 - G) * rl, dinv = (inv1 - inv0) * rl, da = (a1 - amp) * rl;
        G = G1;
        amp = a1;

        // the DCO
        const float pwm = p[JP_PWM] * (0.46f / 127.0f);
        const float pw = 0.5f - pwm * (p[JP_PWMSRC] == 0 ? (lfo + 1.0f) * 0.5f : (p[JP_PWMSRC] == 1 ? 1.0f : env));
        const float sawl = p[JP_SAW] ? 0.5f : 0.0f, pul = p[JP_PULSE] ? 0.5f : 0.0f;
        const float subl = p[JP_SUB] * (0.5f / 127.0f), nzl = p[JP_NOISE] * (0.4f / 127.0f);

        // the HPF and the chorus
        static const float HA[4] = { 0.01415f, 0.0f, 0.0322f, 0.0975f };
        static const float CR[4] = { 0.0f, 0.513f, 0.863f, 9.75f }, CD[4] = { 0.0f, 80.0f, 80.0f, 9.0f };
        const int hp = p[JP_HPF] & 3, ch = p[JP_CHORUS] & 3;
        const float ha = HA[hp], og = p[JP_LEVEL] * (0.3f / 100.0f), cdep = CD[ch];
        double cph = std::fmod(t0 * CR[ch], 1.0);
        const double cstep = CR[ch] / kSr;

        for (int i = 0; i < len; i++)
        {
            float x = 0.0f;
            ph += inc;
            if (ph >= 1.0f)
                ph -= 1.0f;
            sph += sinc;
            if (sph >= 1.0f)
                sph -= 1.0f;
            if (sawl > 0.0f)
                x += sawl * saw(ph, inc);
            if (pul > 0.0f)
                x += pul * pulse(ph, inc, pw);
            if (subl > 0.0f)
                x += subl * pulse(sph, sinc, 0.5f);
            if (nzl > 0.0f)
            {
                nz = nz * 1664525u + 1013904223u;
                x += nzl * static_cast<float>(static_cast<int32_t>(nz)) * (1.0f / 2147483648.0f);
            }
            x *= comp;

            // the ladder
            const float b = 1.0f - Gc, G2 = Gc * Gc, G3 = G2 * Gc, G4 = G2 * G2;
            const float S = b * (G3 * s[0] + G2 * s[1] + Gc * s[2] + s[3]);
            const float y = (G4 * x + S) * inv;
            const float u = sat(x - kres * y);
            const float y0 = Gc * u + b * s[0];
            s[0] = y0 + y0 - s[0];
            const float y1 = Gc * y0 + b * s[1];
            s[1] = y1 + y1 - s[1];
            const float y2 = Gc * y1 + b * s[2];
            s[2] = y2 + y2 - s[2];
            const float y3 = Gc * y2 + b * s[3];
            s[3] = y3 + y3 - s[3];
            float v = y3 * a;
            Gc += dG;
            inv += dinv;
            a += da;

            if (hp != 1)
            {
                hlp += ha * (v - hlp);
                v = hp == 0 ? v + 0.6f * hlp : v - hlp;
            }
            float L = v, R = v;
            if (ch)
            {
                cb[cw & (kChorusLen - 1)] =
                  static_cast<int16_t>(v > 1.99f ? 32600 : (v < -1.99f ? -32600 : static_cast<int>(v * 16384.0f)));
                const float t = static_cast<float>(cph);
                cph += cstep;
                if (cph >= 1.0)
                    cph -= 1.0;
                const float tri2 = 4.0f * std::fabs(t - 0.5f) - 1.0f;
                const float dL = 155.0f + cdep * tri2, dR = 155.0f - cdep * tri2;
                const int il = static_cast<int>(dL), ir = static_cast<int>(dR);
                const float fl = dL - il, fr = dR - ir;
                const float wl = (cb[(cw - il) & (kChorusLen - 1)] * (1.0f - fl) + cb[(cw - il - 1) & (kChorusLen - 1)] * fl) /
                                 16384.0f;
                const float wr = (cb[(cw - ir) & (kChorusLen - 1)] * (1.0f - fr) + cb[(cw - ir - 1) & (kChorusLen - 1)] * fr) /
                                 16384.0f;
                clp[0] += 0.63f * (wl - clp[0]); // the BBD's darkness: ~7 kHz
                clp[1] += 0.63f * (wr - clp[1]);
                cw++;
                L = 0.72f * (v + clp[0]);
                R = 0.72f * (v + clp[1]);
            }
            outL[i] = L * og;
            outR[i] = R * og;
        }
        auto flush = [](float& z) { if (std::fabs(z) < 1e-20f) z = 0.0f; };
        for (auto& z : s)
            flush(z);
        flush(hlp);
        flush(clp[0]);
        flush(clp[1]);
    }
};

} // namespace

void JunoSynthVoice::createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
{
    using namespace polysynth::sdk;
    using P = JunoParamId;
    metadata.params.resize(enumutil::EnumMax<JunoParamId>::value + 1);
    metadata.params.shrink_to_fit();
    if (paramHandlers.empty())
        paramHandlers.resize(enumutil::EnumMax<JunoParamId>::value + 1);
    ParamHelper<JunoSynthVoice> helper(metadata, paramHandlers);
    addPresetParams(metadata, helper);

    using EditorElement = Patch::Metadata::EditorLayout::EditorElement;
    auto e = [](P id) { return EditorElement{ .type = EditorElement::Type::kParam, .id = *id }; };
    metadata.layoutFlat = Patch::Metadata::EditorLayout{
        .sections = {
          { .name = "Oscillator", .shortName = "DCO",
            .elements = { e(P::kPreset), e(P::kRange), e(P::kSaw), e(P::kPulse), e(P::kSub), e(P::kNoise) } },
          { .name = "Pulse Width", .shortName = "PWM", .elements = { e(P::kPwm), e(P::kPwmSrc), e(P::kDcoLfo) } },
          { .name = "Filter", .shortName = "VCF",
            .elements = { e(P::kHpf), e(P::kCut), e(P::kRes), e(P::kEnv), e(P::kEnvPol), e(P::kKey) } },
          { .name = "Filter Modulation", .shortName = "VCF Mod", .elements = { e(P::kFiltLfo), e(P::kFiltVel) } },
          { .name = "Envelope", .shortName = "Env",
            .elements = { e(P::kAtk), e(P::kDec), e(P::kSus), e(P::kRel), e(P::kVca), e(P::kAmpVel) } },
          { .name = "LFO", .shortName = "LFO",
            .elements = { e(P::kLfoRate), e(P::kLfoDelay), e(P::kLfoSync), e(P::kLfoMode) } },
          { .name = "Chorus Voice", .shortName = "Chorus",
            .elements = { e(P::kChorus), e(P::kLevel), e(P::kTrans) } },
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
        macros[3] = Patch::Metadata::Macro{ .displayName = "Attack", .modifiers = { { *P::kAtk, 0.4f } },
                                            .bipolar = true };
        macros[4] = Patch::Metadata::Macro{ .displayName = "Release", .modifiers = { { *P::kRel, 0.4f } },
                                            .bipolar = true };
        macros[5] = Patch::Metadata::Macro{ .displayName = "PWM", .modifiers = { { *P::kPwm, 0.5f } } };
    };
    metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kJunoSynth);
    metadata.outputChannels = Patch::Metadata::OutputChannels::kStereo;
    metadata.maxVoices = 6;
}

std::unique_ptr<InstrumentType> createJunoSynth()
{
    return std::make_unique<InstrumentType>(
      InstrumentType{ .name = "JUNO",
                      .createMetadata = &JunoSynthVoice::createMetadata,
                      .createVoice = [](int) -> std::unique_ptr<InstrumentVoice>
                      { return std::make_unique<JunoSynthVoice>(); } });
}

} // namespace polysynth

#endif
