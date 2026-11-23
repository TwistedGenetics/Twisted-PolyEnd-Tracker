// Twisted: CRAFTY - Crafty Synth (Jamie / TwistedGenetics), the Modal CRAFTsynth 2.0-style wavetable
// synth from the M-VAVE FM-1, as a Tracker+ / Mini synth engine. The engine is Crafty Synth's own C code
// (core/synth/crafty, built by core/libs/crafty), so it sounds as the FM-1 does: 8 oscillators on two
// wavetable banks, 16 oscillator modifiers, SPREAD unison / chords, 12 filter types, three envelopes,
// two LFOs, the 8-slot mod matrix, the arp-seq, glide, distortion, delay, reverb, chorus, phaser,
// EraCrush and OTT. Mono, as the CRAFTsynth.
//
// The patch is PRESET (the 64 factory presets) plus the rest: every continuous knob moves the preset's
// value and shows where it ends up; every stepped one (banks, modes, types, destinations) starts on
// "Preset" and can pick a value of its own. Changing PRESET changes the sound and keeps your changes.
//
// Each instance needs ~190 KB (wavetables and effect memory). Up to four live in a pool in external
// RAM, taken from the sample memory (see SAMPLE_MEMORY_SIZE in mtStructs.h).

#include "config.h"
#ifdef FEATURE_SYNTH

#include "CraftySynthVoice.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"
#include <cr_section_macros.h>

extern "C" {
#include "crafty/cr_inst.h"
#include "crafty/cr_text.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace polysynth
{

namespace
{

enum class CraftyParamId : int
{
    kPreset = 0,
    kW1pos,
    kW2pos,
    kMix,
    kOmamt,
    kSpread,
    kW1bank,
    kW2bank,
    kOmmode,
    kTune,
    kFine,
    kOct,
    kGlide,
    kCut,
    kRes,
    kMorph,
    kFegamt,
    kFa,
    kFd,
    kFs,
    kFr,
    kAa,
    kAd,
    kAs,
    kAr,
    kAegamt,
    kMa,
    kMd,
    kMs,
    kMr,
    kL1rate,
    kL1shape,
    kL1mode,
    kL1sync,
    kL2rate,
    kL2shape,
    kL2mode,
    kL2sync,
    kDist,
    kDmix,
    kDtime,
    kDfb,
    kDsync,
    kVol,
    kDep0,
    kDep1,
    kDep2,
    kDep3,
    kDep4,
    kDep5,
    kDep6,
    kDep7,
    kDst0,
    kDst1,
    kDst2,
    kDst3,
    kDst4,
    kDst5,
    kDst6,
    kDst7,
    kArp,
    kTempo,
    kArprate,
    kBmode,
    kBamt,
    kFtype,
    kVmode,
    kVsize,
    kVdecay,
    kVmix,
    kVwarp,
    kVdens,
    kVmod,
    kChmix,
    kChrate,
    kPhmix,
    kPhrate,
    kEra,
    kCrush,
    kOtt,
};
static_assert(static_cast<int>(CraftyParamId::kOtt) == CR_NP, "one parameter per Crafty patch value");

// ------------------------------------------------------------------ the instance pool (external RAM)
constexpr int kPoolSize = 4;
__NOINIT(EXTERNAL_RAM) uint8_t craftyPool[kPoolSize][sizeof(cr_inst_t)] __attribute__((aligned(8)));
bool craftyPoolUsed[kPoolSize] = {};

cr_inst_t* takeInstance()
{
    for (int i = 0; i < kPoolSize; i++)
    {
        if (!craftyPoolUsed[i])
        {
            craftyPoolUsed[i] = true;
            auto* in = reinterpret_cast<cr_inst_t*>(craftyPool[i]);
            cr_inst_init(in);
            return in;
        }
    }
    return nullptr; // more Crafty voices than the pool holds: this one stays silent
}
void giveInstance(cr_inst_t* in)
{
    for (int i = 0; i < kPoolSize; i++)
    {
        if (reinterpret_cast<cr_inst_t*>(craftyPool[i]) == in)
            craftyPoolUsed[i] = false;
    }
}

// ------------------------------------------------------------------ stepped values
// how many choices a stepped parameter has (0: continuous) and the patch value of choice i
int choiceCount(int id)
{
    switch (CR_P[id].kind)
    {
        case CR_K_BANK: return id == CR_W2BANK ? CR_NBANK : CR_NBANK - 1; // factory banks (no user area)
        case CR_K_OMMODE: return CR_NOM;
        case CR_K_OCT: return 7;
        case CR_K_LSHAPE: return 5;
        case CR_K_LMODE: return 3;
        case CR_K_ONOFF: return 2;
        case CR_K_ARPRATE: return 6;
        case CR_K_DEST: return CR_ND;
        case CR_K_BMODE: return 5;
        case CR_K_FTYPE: return 12;
        case CR_K_VMODE: return 6;
        case CR_K_ERA: return 6;
        default: return 0;
    }
}
int choiceValue(int id, int i)
{
    switch (CR_P[id].kind)
    {
        case CR_K_BANK: return static_cast<int>(cr_bank_val(static_cast<uint32_t>(i), id == CR_W2BANK));
        case CR_K_OMMODE: return i * 8 + 4;
        case CR_K_LSHAPE: return (i * 127) / 4;
        case CR_K_LMODE: return i == 0 ? 20 : (i == 1 ? 63 : 105);
        case CR_K_ONOFF: return i ? 127 : 0;
        case CR_K_DEST: return i;
        default: return static_cast<int>(cr_band_val(static_cast<uint32_t>(i), static_cast<uint32_t>(choiceCount(id))));
    }
}
// a parameter's final patch value: the preset's, moved by an offset or replaced by a choice
int finalValue(int id, int presetValue, float knob)
{
    const int k = static_cast<int>(std::lround(knob));
    if (choiceCount(id) > 0)
        return k <= 0 ? presetValue : choiceValue(id, k - 1);
    return std::clamp(presetValue + k, 0, 127);
}

int presetOf(const Patch& patch)
{
    auto v = patch.getParamValue(static_cast<int>(CraftyParamId::kPreset));
    return v ? std::clamp<int>(static_cast<int>(std::lround(*v)), 0, CR_NFACTORY - 1) : 0;
}

// the FM-1's labels and value texts are kept as they are (capitals, big and clear on screen)
std::string titleCase(const char* s)
{
    return std::string(s);
}

// the screen name: the FM-1's label, with its page where the label alone is ambiguous
std::string paramName(int id)
{
    if (id >= CR_DEP0 && id <= CR_DEP7)
        return titleCase(CR_SRC_NAME[id - CR_DEP0]) + " DEPTH";
    if (id >= CR_DST0 && id <= CR_DST7)
        return titleCase(CR_SRC_NAME[id - CR_DST0]) + " DEST";
    const char* page = cr_param_page(static_cast<uint32_t>(id));
    std::string n = titleCase(CR_P[id].name);
    if (page[0] != 0 && std::string(CR_P[id].name).find(page) == std::string::npos)
        n = titleCase(page) + " " + n;
    return n;
}

std::string valueText(const Patch& patch, int id, float knob)
{
    cr_patch_t t;
    (void)cr_preset_get(nullptr, static_cast<uint32_t>(presetOf(patch)), &t);
    const int pv = t.p[id];
    const int fv = finalValue(id, pv, knob);
    t.p[id] = static_cast<uint8_t>(fv);
    char b[32] = {};
    cr_param_text(&t, static_cast<uint32_t>(id), b, 1, nullptr, nullptr);
    std::string s = titleCase(b);
    if (choiceCount(id) > 0 && std::lround(knob) <= 0)
        s = "PRESET (" + s + ")";
    return s;
}

} // namespace

namespace
{

class CraftySynthVoice : public sdk::CustomSynthVoice<CraftySynthVoice, CraftyParamId>
{
  public:
    CraftySynthVoice()
      : in(takeInstance())
    {
        playMode = PlayMode::kLegato;
        std::fill(std::begin(knob), std::end(knob), 0.0f);
    }
    ~CraftySynthVoice() override
    {
        if (in)
            giveInstance(in);
    }

    bool isActive() final { return in != nullptr && cr_synth_sounding(&in->syn); }

    void onMidiEvent(const InstrumentVoiceContext& ctx, const MidiEvent& event) override
    {
        if (in == nullptr)
            return;
        switch (event.type)
        {
            case MidiEvent::Type::kNoteOff:
                // Crafty keeps its own held-note stack (legato, glide): the note-off goes straight to
                // it, and the base class only keeps its note bookkeeping
                cr_inst_note(in, event.data.note.note, 0);
                suppress = true;
                sdk::CustomSynthVoice<CraftySynthVoice, CraftyParamId>::onMidiEvent(ctx, event);
                suppress = false;
                return;
            case MidiEvent::Type::kPitchBendChange:
            {
                const int v = std::clamp(event.data.pitchBendChange.value + 8192, 0, 16383);
                const uint8_t m[3] = { 0xE0, static_cast<uint8_t>(v & 0x7F), static_cast<uint8_t>(v >> 7) };
                cr_inst_midi(in, m, 3);
                return;
            }
            default: break;
        }
        sdk::CustomSynthVoice<CraftySynthVoice, CraftyParamId>::onMidiEvent(ctx, event);
    }

    void onNoteOn(const InstrumentVoiceContext& /*ctx*/, note_t note, float velocity, bool /*isLegatoStep*/) final
    {
        if (in == nullptr || suppress)
            return;
        const int v = std::clamp(static_cast<int>(std::lround(velocity * 127.0f)), 1, 127);
        cr_inst_note(in, note, static_cast<uint32_t>(v));
    }

    void onNoteOff(const InstrumentVoiceContext& /*ctx*/, note_t /*note*/) final {}

    void renderVoiceAudio(const InstrumentVoiceContext& ctx,
                          [[maybe_unused]] std::optional<AudioBufferStereoRef> inputBuffer,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef> /*auxOutBuffer*/) final
    {
        auto outL = outputBuffer[0];
        auto outR = outputBuffer[1];
        if (in == nullptr)
        {
            std::fill(outL.begin(), outL.end(), 0.0f);
            std::fill(outR.begin(), outR.end(), 0.0f);
            return;
        }
        cr_inst_transport(in, -1.0, ctx.tempo > 1.0f ? ctx.tempo : 120.0f, 0);
        float lr[2 * CR_INST_FRAMES];
        const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(outL.size()), CR_INST_FRAMES);
        cr_inst_render(in, lr, n);
        for (uint32_t i = 0; i < n; i++)
        {
            outL[i] = lr[2 * i];
            outR[i] = lr[2 * i + 1];
        }
    }

    static void createMetadata(const Patch& patch, Patch::Metadata& metadata);

    static void setPreset(CraftySynthVoice& v, float x)
    {
        v.preset = std::clamp<int>(static_cast<int>(std::lround(x)), 0, CR_NFACTORY - 1);
        if (v.in == nullptr)
            return;
        cr_inst_load(v.in, static_cast<uint32_t>(v.preset));
        // keep every change made on top of the preset
        for (int id = 0; id < CR_NP; id++)
        {
            if (std::lround(v.knob[id]) != 0)
                v.apply(id);
        }
    }
    static void setValue(CraftySynthVoice& v, int id, float x)
    {
        v.knob[id] = x;
        v.apply(id);
    }

  private:
    cr_inst_t* in = nullptr;
    int preset = 0;
    float knob[CR_NP];
    bool suppress = false;

    void apply(int id)
    {
        if (in == nullptr)
            return;
        cr_patch_t t;
        (void)cr_preset_get(nullptr, static_cast<uint32_t>(preset), &t);
        cr_inst_param(in, static_cast<uint32_t>(id), static_cast<uint32_t>(finalValue(id, t.p[id], knob[id])));
    }
};

template<int ID>
void addCraftyParam(sdk::ParamHelper<CraftySynthVoice>& helper)
{
    constexpr int id = ID - 1; // the Crafty patch value
    const int choices = choiceCount(id);
    Patch::Metadata::Param def{
        .displayName = paramName(id),
        .defaultValue = 0.0f,
        .minValue = static_cast<Patch::paramvalue_short_t>(choices > 0 ? 0 : -127),
        .maxValue = static_cast<Patch::paramvalue_short_t>(choices > 0 ? choices : 127),
        .isEnum = choices > 0,
        .midiCC = CR_P[id].cc,
        .valueFormatter = [](const Patch& patch, Patch::paramvalue_t value) { return valueText(patch, id, value); },
    };
    helper.addParam(ID, std::move(def), [](auto& v, Patch::optional_paramvalue_t val) {
        CraftySynthVoice::setValue(v, id, *val);
    });
}

template<int... I>
void addAllCraftyParams(sdk::ParamHelper<CraftySynthVoice>& helper, std::integer_sequence<int, I...>)
{
    (addCraftyParam<I + 1>(helper), ...);
}

} // namespace

void CraftySynthVoice::createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
{
    using namespace polysynth::sdk;
    using P = CraftyParamId;

    metadata.params.resize(enumutil::EnumMax<CraftyParamId>::value + 1);
    metadata.params.shrink_to_fit();
    if (paramHandlers.empty())
    {
        paramHandlers.resize(enumutil::EnumMax<CraftyParamId>::value + 1);
    }

    ParamHelper<CraftySynthVoice> helper(metadata, paramHandlers);

    helper.addParam(
      *P::kPreset,
      {
        .displayName = "Preset",
        .defaultValue = 0,
        .minValue = 0,
        .maxValue = static_cast<Patch::paramvalue_short_t>(CR_NFACTORY - 1),
        .isEnum = true,
        .valueFormatter = [](const Patch&, Patch::paramvalue_t value)
        {
            const uint32_t slot = static_cast<uint32_t>(std::clamp<int>(std::lround(value), 0, CR_NFACTORY - 1));
            cr_patch_t t;
            (void)cr_preset_get(nullptr, slot, &t);
            char label[8];
            cr_preset_label(slot, label);
            char name[13] = {};
            std::memcpy(name, t.name, 12);
            return std::string(label) + " " + titleCase(name);
        },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { CraftySynthVoice::setPreset(v, *val); });

    addAllCraftyParams(helper, std::make_integer_sequence<int, CR_NP>{});

    using EditorElement = Patch::Metadata::EditorLayout::EditorElement;
    auto e = [](int crId) { return EditorElement{ .type = EditorElement::Type::kParam, .id = crId + 1 }; };
    auto section = [&](const char* name, const char* shortName, std::initializer_list<int> ids)
    {
        Patch::Metadata::EditorLayout::EditorSection s{ .name = name, .shortName = shortName };
        for (int id : ids)
            s.elements.push_back(e(id));
        return s;
    };

    Patch::Metadata::EditorLayout layout;
    layout.sections.push_back(
      Patch::Metadata::EditorLayout::EditorSection{ .name = "Preset", .shortName = "Preset",
                                                    .elements = { EditorElement{ .type = EditorElement::Type::kParam,
                                                                                 .id = *P::kPreset },
                                                                  e(CR_VOL), e(CR_TUNE), e(CR_FINE), e(CR_OCT),
                                                                  e(CR_GLIDE) } });
    layout.sections.push_back(section("Oscillators", "Osc",
                                      { CR_W1BANK, CR_W1POS, CR_W2BANK, CR_W2POS, CR_MIX, CR_SPREAD }));
    layout.sections.push_back(section("Oscillator Mod", "Osc Mod", { CR_OMMODE, CR_OMAMT, CR_BMODE, CR_BAMT }));
    layout.sections.push_back(section("Filter", "Filter", { CR_FTYPE, CR_CUT, CR_RES, CR_MORPH, CR_FEGAMT }));
    layout.sections.push_back(section("Filter Envelope", "Filt Env", { CR_FA, CR_FD, CR_FS, CR_FR }));
    layout.sections.push_back(section("Amp Envelope", "Amp Env", { CR_AA, CR_AD, CR_AS, CR_AR, CR_AEGAMT }));
    layout.sections.push_back(section("Mod Envelope", "Mod Env", { CR_MA, CR_MD, CR_MS, CR_MR }));
    layout.sections.push_back(section("LFO 1", "LFO 1", { CR_L1RATE, CR_L1SHAPE, CR_L1MODE, CR_L1SYNC }));
    layout.sections.push_back(section("LFO 2", "LFO 2", { CR_L2RATE, CR_L2SHAPE, CR_L2MODE, CR_L2SYNC }));
    layout.sections.push_back(section("Modulation 1-4", "Mod 1-4",
                                      { CR_DST0, CR_DEP0, CR_DST1, CR_DEP1, CR_DST2, CR_DEP2, CR_DST3, CR_DEP3 }));
    layout.sections.push_back(section("Modulation 5-8", "Mod 5-8",
                                      { CR_DST4, CR_DEP4, CR_DST5, CR_DEP5, CR_DST6, CR_DEP6, CR_DST7, CR_DEP7 }));
    layout.sections.push_back(section("Arp Voice", "Arp", { CR_ARP, CR_TEMPO, CR_ARPRATE }));
    layout.sections.push_back(section("Distortion Delay FX", "Dist/Dly",
                                      { CR_DIST, CR_DMIX, CR_DTIME, CR_DFB, CR_DSYNC }));
    layout.sections.push_back(section("Reverb FX", "Reverb",
                                      { CR_VMODE, CR_VMIX, CR_VSIZE, CR_VDECAY, CR_VWARP, CR_VDENS, CR_VMOD }));
    layout.sections.push_back(section("Colour FX", "Colour",
                                      { CR_CHMIX, CR_CHRATE, CR_PHMIX, CR_PHRATE, CR_ERA, CR_CRUSH, CR_OTT }));
    metadata.layoutFlat = layout;
    metadata.layout23 = layout;

    metadata.initDefaultMacros = [](auto& macros)
    {
        macros[0] = Patch::Metadata::Macro{ .displayName = "Cutoff", .modifiers = { { CR_CUT + 1, 0.5f } },
                                            .bipolar = true };
        macros[1] = Patch::Metadata::Macro{ .displayName = "Wave Position",
                                            .modifiers = { { CR_W1POS + 1, 0.5f }, { CR_W2POS + 1, 0.5f } },
                                            .bipolar = true };
        macros[2] = Patch::Metadata::Macro{ .displayName = "Osc Mod", .modifiers = { { CR_OMAMT + 1, 0.5f } } };
        macros[3] = Patch::Metadata::Macro{ .displayName = "Resonance", .modifiers = { { CR_RES + 1, 0.4f } } };
        macros[4] = Patch::Metadata::Macro{ .displayName = "Distortion", .modifiers = { { CR_DIST + 1, 0.5f } } };
        macros[5] = Patch::Metadata::Macro{ .displayName = "Reverb", .modifiers = { { CR_VMIX + 1, 0.5f } } };
    };

    metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kCraftySynth);
    metadata.outputChannels = Patch::Metadata::OutputChannels::kStereo;
    metadata.maxVoices = 1;
}

std::unique_ptr<InstrumentType> createCraftySynth()
{
    return std::make_unique<InstrumentType>(
      InstrumentType{ .name = "CRAFTY",
                      .createMetadata = &CraftySynthVoice::createMetadata,
                      .createVoice = [](int /*voiceIdx*/) -> std::unique_ptr<InstrumentVoice>
                      { return std::make_unique<CraftySynthVoice>(); } });
}

} // namespace polysynth

#endif
