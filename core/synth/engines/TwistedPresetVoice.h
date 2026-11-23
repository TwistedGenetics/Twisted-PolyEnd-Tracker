#pragma once
// Twisted: a shared base for the DREADNOUGHT engines (SUB, JUNO): a PRESET parameter plus one parameter per patch
// value. Wide-range values are offsets from the preset (the screen shows where they end up); values with few
// settings start on "PRESET (...)" and can pick their own. Changing PRESET keeps your changes on top of the new one.
//
// The engine class (Derived) supplies, as static members:
//   static constexpr int kValues;                 // patch values
//   static const char* valueName(int k);
//   static int valueMin(int k), valueMax(int k);
//   static int presetCount();
//   static const char* presetName(int i);
//   static int presetValue(int i, int k);
//   static std::string valueText(int k, int v);   // how a value reads on screen
// and a ParamIdEnum with kPreset = 0 and then exactly kValues named enumerators.
// The voice reads the values in use from p[k] and gets onValuesChanged() after any change.

#include "PolySynth/sdk/SynthVoiceSDK.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace polysynth::twisted
{

template<class Derived, class ParamIdEnum, int kMaxValues = 64>
class PresetVoice : public sdk::CustomSynthVoice<Derived, ParamIdEnum>
{
  public:
    using Base = sdk::CustomSynthVoice<Derived, ParamIdEnum>;

    // a value is a choice (not an offset) when it has only a few settings
    static constexpr int kChoiceMax = 8;
    static bool isChoice(int k) { return Derived::valueMax(k) - Derived::valueMin(k) < kChoiceMax; }

    static int presetIndexOf(const Patch& patch)
    {
        auto v = patch.getParamValue(0);
        return v ? std::clamp<int>(static_cast<int>(std::lround(*v)), 0, Derived::presetCount() - 1) : 0;
    }

    static int finalValue(int preset, int k, float knob)
    {
        const int pv = Derived::presetValue(preset, k);
        const int n = static_cast<int>(std::lround(knob));
        if (isChoice(k))
            return n <= 0 ? pv : std::clamp(Derived::valueMin(k) + n - 1, Derived::valueMin(k), Derived::valueMax(k));
        return std::clamp(pv + n, Derived::valueMin(k), Derived::valueMax(k));
    }

    static void setPreset(Derived& v, float x)
    {
        v.preset = std::clamp<int>(static_cast<int>(std::lround(x)), 0, Derived::presetCount() - 1);
        v.recompute();
    }
    static void setKnob(Derived& v, int k, float x)
    {
        v.knob[k] = x;
        v.recompute();
    }

    // the parameters: PRESET, then one per value (ids 1..kValues)
    static void addPresetParams(Patch::Metadata& metadata, sdk::ParamHelper<Derived>& helper)
    {
        (void)metadata;
        helper.addParam(
          0,
          Patch::Metadata::Param{
            .displayName = "Preset",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = static_cast<Patch::paramvalue_short_t>(Derived::presetCount() - 1),
            .isEnum = true,
            .valueFormatter = [](const Patch&, Patch::paramvalue_t value) {
                return std::string(Derived::presetName(
                  std::clamp<int>(static_cast<int>(std::lround(value)), 0, Derived::presetCount() - 1)));
            },
          },
          [](auto& v, Patch::optional_paramvalue_t val) { setPreset(v, *val); });
        addAll(helper, std::make_integer_sequence<int, Derived::kValues>{});
    }

  protected:
    int preset = 0;
    float knob[kMaxValues] = {};
    int p[kMaxValues] = {};

    void recompute()
    {
        for (int k = 0; k < Derived::kValues; k++)
            p[k] = finalValue(preset, k, knob[k]);
        static_cast<Derived*>(this)->onValuesChanged();
    }

  private:
    template<int K>
    static void addOne(sdk::ParamHelper<Derived>& helper)
    {
        const bool choice = isChoice(K);
        const int range = Derived::valueMax(K) - Derived::valueMin(K);
        helper.addParam(
          K + 1,
          Patch::Metadata::Param{
            .displayName = Derived::valueName(K),
            .defaultValue = 0.0f,
            .minValue = static_cast<Patch::paramvalue_short_t>(choice ? 0 : -range),
            .maxValue = static_cast<Patch::paramvalue_short_t>(choice ? range + 1 : range),
            .isEnum = choice,
            .valueFormatter =
              [](const Patch& patch, Patch::paramvalue_t value) {
                  const int pr = presetIndexOf(patch);
                  std::string s = Derived::valueText(K, finalValue(pr, K, value));
                  if (isChoice(K) && std::lround(value) <= 0)
                      s = "Preset (" + s + ")";
                  return s;
              },
          },
          [](auto& v, Patch::optional_paramvalue_t val) { setKnob(v, K, *val); });
    }
    template<int... K>
    static void addAll(sdk::ParamHelper<Derived>& helper, std::integer_sequence<int, K...>)
    {
        (addOne<K>(helper), ...);
    }
};

// ---- small float DSP pieces shared by the engines --------------------------------------------------------------
namespace dsp
{
constexpr float kSr = 44100.0f;

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
inline float wrap01(float p) { return p - std::floor(p); }
inline float saw(float ph, float dt) { return (2.0f * ph - 1.0f) - polyBlep(ph, dt); }
// a pulse high for the first w of the cycle
inline float pulse(float ph, float dt, float w)
{
    return (ph < w ? 1.0f : -1.0f) + polyBlep(ph, dt) - polyBlep(wrap01(ph - w + 1.0f), dt);
}
inline float noteToInc(float note) { return 440.0f * std::exp2((note - 69.0f) / 12.0f) / kSr; }
// times 1 ms .. 10 s over 0..127 (DREADNOUGHT's envelope scale)
inline float valueToMs(int v) { return std::pow(1.075254f, static_cast<float>(std::clamp(v, 0, 127))); }
// a one-pole coefficient per tick of n samples covering ~99 % of the way in that time
inline float tickCoef(int v, int n)
{
    return std::min(1.0f, 1.0f - std::exp(-4.6f * n / (valueToMs(v) * 0.001f * kSr)));
}
inline float cutoffHz(float steps) // 0..127: 30 Hz .. 16 kHz, log
{
    return 30.0f * std::pow(16000.0f / 30.0f, std::clamp(steps, 0.0f, 127.0f) / 127.0f);
}
inline float lfoHz(int v) // 0.05 .. 20 Hz, log
{
    return 0.05f * std::pow(400.0f, static_cast<float>(std::clamp(v, 0, 127)) / 127.0f);
}
inline std::string msText(int v)
{
    const float ms = valueToMs(v);
    char b[16];
    if (ms < 1000.0f)
        snprintf(b, sizeof(b), "%d ms", static_cast<int>(ms + 0.5f));
    else
        snprintf(b, sizeof(b), "%.1f s", static_cast<double>(ms / 1000.0f));
    return b;
}
inline std::string hzText(float hz)
{
    char b[16];
    if (hz < 1000.0f)
        snprintf(b, sizeof(b), "%d Hz", static_cast<int>(hz + 0.5f));
    else
        snprintf(b, sizeof(b), "%.1f kHz", static_cast<double>(hz / 1000.0f));
    return b;
}
inline std::string intText(int v, bool sign = false)
{
    char b[16];
    snprintf(b, sizeof(b), sign ? "%+d" : "%d", v);
    return b;
}

// a 4-pole zero-delay-feedback ladder (Zavalishin's topology), its input through a soft clip
struct Ladder
{
    float s[4] = {};
    float G = 0.5f, k = 0.0f;
    void set(float hz, float res01to4) // res: 0 .. ~4.3 (self-oscillates past 4)
    {
        const float g = std::tan(3.14159265f * std::min(hz, 18000.0f) / kSr);
        G = std::min(g / (1.0f + g), 0.75f);
        k = res01to4;
    }
    float tick(float x, float comp)
    {
        float in = std::clamp(x, -4.0f, 4.0f) * comp;
        float S = G * s[0] + s[1];
        S = G * S + s[2];
        S = G * S + s[3];
        S *= (1.0f - G);
        const float G4 = G * G * G * G;
        const float y = (G4 * in + S) / (1.0f + k * G4);
        float u = 2.0f * softClip((in - k * y) * 0.5f);
        for (int j = 0; j < 4; j++)
        {
            const float v = G * (u - s[j]);
            u = v + s[j];
            s[j] = u + v;
        }
        return u;
    }
    void reset(float seed = 0.0f)
    {
        s[0] = seed;
        s[1] = s[2] = s[3] = 0.0f;
    }
};
} // namespace dsp

} // namespace polysynth::twisted
