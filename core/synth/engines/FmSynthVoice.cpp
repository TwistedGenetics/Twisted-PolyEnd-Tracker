// Twisted: DX7 - DREADNOUGHT's six-operator DX7-style FM synth (Jamie / TwistedGenetics) as a Tracker+ synth engine.
// The engine is DREADNOUGHT's FM code (core/synth/dreadnought/dn_fm.c), itself a C port of the core of Dexed's MSFA
// (Copyright 2012 Google Inc., 2013-2017 Pascal Gauthier and the Dexed authors, Apache License 2.0): the operators and
// 32 algorithms, the DX7 envelopes, keyboard scaling, velocity, the LFO, the pitch envelope and feedback, as Dexed
// plays them. Each Tracker voice runs one engine with one voice.
//
// Sounds: DREADNOUGHT's 10 factory voices, then the DX7 banks in the SD card's /DX7 folder (32-voice .syx files, up to
// 8 banks), read in the file manager's idle time. The knobs move the chosen voice: BRIGHT (the modulators' levels),
// TIME (every envelope rate), ATTACK and RELEASE (the carriers'), plus the algorithm, feedback, the LFO and transpose.

#include "config.h"
#ifdef FEATURE_SYNTH

#include "FmSynthVoice.h"
#include "DxBanks.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"
#include "Platform.h"
#include "Tracker.h"
#include <cr_section_macros.h>

extern "C" {
#include "dreadnought/dn_fm.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace polysynth
{
namespace
{

// ------------------------------------------------------------------ the engines (one a Tracker voice), external RAM
constexpr int kPool = 24;
__NOINIT(EXTERNAL_RAM) dn_fm_t fmPool[kPool] __attribute__((aligned(8)));
bool fmPoolUsed[kPool] = {};

dn_fm_t* takeEngine()
{
    for (int i = 0; i < kPool; i++)
        if (!fmPoolUsed[i])
        {
            fmPoolUsed[i] = true;
            dn_fm_init(&fmPool[i]);
            fmPool[i].voices = 1;
            return &fmPool[i];
        }
    return nullptr;
}
void giveEngine(dn_fm_t* s)
{
    for (int i = 0; i < kPool; i++)
        if (&fmPool[i] == s)
            fmPoolUsed[i] = false;
}

// ------------------------------------------------------------------ the DX7 banks from the SD card
constexpr int kBanks = 8;
constexpr int kBankVoices = 32;
constexpr const char* kDxFolder = "/DX7";
__NOINIT(EXTERNAL_RAM) uint8_t dxBanks[kBanks][kBankVoices * DN_FM_PACKED] __attribute__((aligned(4)));
int dxBankCount = 0;      // banks loaded
int dxBankLoading = 0;    // (while scanning)
bool dxScanRequested = true;
int dxScanStage = 0;
char* dxNames[64] = {};
int dxNameCount = 0, dxNext = 0;

int factoryCount() { return static_cast<int>(dn_fm_factory_count()); }
int presetTotal() { return factoryCount() + kBanks * kBankVoices; }

// a preset's unpacked voice; false: an empty bank slot
bool presetVoice(int i, uint8_t* patch)
{
    if (i < factoryCount())
    {
        dn_fm_factory(static_cast<uint32_t>(i), patch);
        return true;
    }
    i -= factoryCount();
    const int bank = i / kBankVoices;
    if (bank >= dxBankCount)
    {
        dn_fm_patch_init(patch);
        return false;
    }
    dn_fm_unpack(&dxBanks[bank][(i % kBankVoices) * DN_FM_PACKED], patch);
    return true;
}

std::string presetText(int i)
{
    char b[32];
    if (i < factoryCount())
        return dn_fm_factory_name(static_cast<uint32_t>(i));
    const int j = i - factoryCount();
    const int bank = j / kBankVoices, slot = j % kBankVoices;
    if (bank >= dxBankCount)
    {
        snprintf(b, sizeof(b), "B%d-%02d (empty)", bank + 1, slot + 1);
        return b;
    }
    char name[11] = {};
    std::memcpy(name, &dxBanks[bank][slot * DN_FM_PACKED + 118], 10);
    for (char& c : name)
        if (c != 0 && (c < ' ' || c > '~'))
            c = ' ';
    snprintf(b, sizeof(b), "B%d-%02d %s", bank + 1, slot + 1, name);
    return b;
}

bool dxNameLess(const char* a, const char* b)
{
    for (;; a++, b++)
    {
        int x = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a, y = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (x != y || !x)
            return x < y;
    }
}

// a 32-voice bank: the sysex (F0 43 00 09 20 00, 4096 bytes, checksum, F7) or the bare 4096 bytes
bool loadBank(FileSystem* fs, const char* name, uint8_t* dest)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", kDxFolder, name);
    auto [isOpen, fh] = fs->openFile(path, FileSystem::FileOpenMode::kRead);
    if (!isOpen)
        return false;
    const size_t size = fs->getFileSize(fh);
    bool ok = false;
    uint8_t head[6];
    if (size >= 4104 && fs->readFile(head, 6, fh) == 6 && head[0] == 0xF0 && head[1] == 0x43 && head[3] == 0x09)
        ok = fs->readFile(dest, 4096, fh) == 4096;
    else if (size == 4096 && fs->seekFile(0, fh))
        ok = fs->readFile(dest, 4096, fh) == 4096;
    fs->closeFile(fh);
    return ok;
}

enum class FmParamId : int
{
    kPreset = 0,
    kBright,
    kTime,
    kAttack,
    kRelease,
    kAlgorithm,
    kFeedback,
    kLfoSpeed,
    kLfoDelay,
    kLfoPmd,
    kLfoAmd,
    kTranspose,
    kVolume,
};

class FmSynthVoice : public sdk::CustomSynthVoice<FmSynthVoice, FmParamId>
{
  public:
    FmSynthVoice()
      : s(takeEngine())
    {
        dxScanRequested = true; // pick up any new banks
        load();
    }
    ~FmSynthVoice() override
    {
        if (s)
            giveEngine(s);
    }

    bool isActive() final { return s != nullptr && dn_fm_sounding(s) != 0u; }

    void onNoteOn(const InstrumentVoiceContext&, note_t note, float velocity, bool) final
    {
        if (!s)
            return;
        if (stale)
            load();
        current = std::clamp(static_cast<int>(note) + transpose, 0, 127);
        dn_fm_note_on(s, static_cast<uint32_t>(current),
                      static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(velocity * 127.0f)), 1, 127)));
    }
    void onNoteOff(const InstrumentVoiceContext&, note_t) final
    {
        if (s)
            dn_fm_note_off(s, static_cast<uint32_t>(current));
    }

    void renderVoiceAudio(const InstrumentVoiceContext&,
                          [[maybe_unused]] std::optional<AudioBufferStereoRef> inputBuffer,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef>) final
    {
        auto out = outputBuffer[0];
        if (!s)
        {
            std::fill(out.begin(), out.end(), 0.0f);
            return;
        }
        int32_t lr[2 * 128];
        size_t done = 0;
        while (done < out.size())
        {
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(out.size() - done, 128));
            std::memset(lr, 0, sizeof(int32_t) * 2 * n);
            dn_fm_render(s, lr, n);
            for (uint32_t i = 0; i < n; i++)
                out[done + i] = static_cast<float>(lr[2 * i]) * (gain * 4.0f / 32768.0f); // (+12 dB: one voice
                                                                                            //  a Tracker voice)
            done += n;
        }
    }

    static void createMetadata(const Patch& patch, Patch::Metadata& metadata);

    static void setPreset(FmSynthVoice& v, float x)
    {
        v.preset = std::clamp<int>(std::lround(x), 0, presetTotal() - 1);
        v.load();
    }
    static void setMacro(FmSynthVoice& v, int which, float x)
    {
        const int8_t m = static_cast<int8_t>(std::clamp<int>(std::lround(x), -50, 50));
        switch (which)
        {
            case 0: v.macro.bright = m; break;
            case 1: v.macro.time = m; break;
            case 2: v.macro.attack = m; break;
            default: v.macro.release = m; break;
        }
        v.load();
    }
    static void setOverride(FmSynthVoice& v, int which, float x)
    {
        v.over[which] = static_cast<int>(std::lround(x));
        v.load();
    }
    static void setTranspose(FmSynthVoice& v, float x) { v.transpose = static_cast<int>(std::lround(x)); }
    static void setVolume(FmSynthVoice& v, float x) { v.gain = sdk::dbToAmplitude(x); }

    void markStale() { stale = true; }

  private:
    dn_fm_t* s = nullptr;
    int preset = 0;
    dn_fm_macro_t macro = {};
    int over[5] = {}; // algorithm (0 = the voice's, else 1..32), feedback, LFO speed, delay, PMD, AMD offsets
    int transpose = 0;
    float gain = 1.0f;
    int current = 60;
    bool stale = false;

    void load()
    {
        if (!s)
            return;
        uint8_t base[DN_FM_PATCH], patch[DN_FM_PATCH];
        presetVoice(preset, base);
        dn_fm_apply(base, &macro, patch);
        if (over[0] > 0)
            patch[DN_FMV_ALG] = static_cast<uint8_t>(std::clamp(over[0] - 1, 0, 31));
        patch[DN_FMV_FB] = static_cast<uint8_t>(std::clamp(patch[DN_FMV_FB] + over[1], 0, 7));
        patch[DN_FMV_LFS] = static_cast<uint8_t>(std::clamp(patch[DN_FMV_LFS] + over[2], 0, 99));
        patch[DN_FMV_LFD] = static_cast<uint8_t>(std::clamp(patch[DN_FMV_LFD] + over[3], 0, 99));
        patch[DN_FMV_LPMD] = static_cast<uint8_t>(std::clamp(patch[DN_FMV_LPMD] + over[4], 0, 99));
        patch[DN_FMV_LAMD] = static_cast<uint8_t>(std::clamp(patch[DN_FMV_LAMD] + overAmd, 0, 99));
        dn_fm_voice_load(s, patch);
        stale = false;
    }

  public:
    int overAmd = 0;
    static void setAmd(FmSynthVoice& v, float x)
    {
        v.overAmd = static_cast<int>(std::lround(x));
        v.load();
    }
};

// the voice a knob moves, read back for the screen
int presetOfPatch(const Patch& patch)
{
    auto v = patch.getParamValue(0);
    return v ? std::clamp<int>(std::lround(*v), 0, presetTotal() - 1) : 0;
}
int baseValue(const Patch& patch, int index)
{
    uint8_t base[DN_FM_PATCH];
    presetVoice(presetOfPatch(patch), base);
    return base[index];
}

} // namespace

void FmSynthVoice::createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
{
    using namespace polysynth::sdk;
    using P = FmParamId;
    metadata.params.resize(enumutil::EnumMax<FmParamId>::value + 1);
    metadata.params.shrink_to_fit();
    if (paramHandlers.empty())
        paramHandlers.resize(enumutil::EnumMax<FmParamId>::value + 1);
    ParamHelper<FmSynthVoice> helper(metadata, paramHandlers);

    helper.addParam(
      *P::kPreset,
      {
        .displayName = "Voice",
        .defaultValue = 0,
        .minValue = 0,
        .maxValue = static_cast<Patch::paramvalue_short_t>(presetTotal() - 1),
        .isEnum = true,
        .valueFormatter = [](const Patch&, Patch::paramvalue_t value)
        { return presetText(std::clamp<int>(std::lround(value), 0, presetTotal() - 1)); },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setPreset(v, *val); });

    auto macroParam = [](const char* name) {
        return Patch::Metadata::Param{
            .displayName = name,
            .defaultValue = 0.0f,
            .minValue = -50,
            .maxValue = 50,
            .valueFormatter = [](const Patch&, Patch::paramvalue_t v) {
                char b[8];
                snprintf(b, sizeof(b), "%+d", static_cast<int>(std::lround(v)));
                return std::string(b);
            },
        };
    };
    helper.addParam(*P::kBright, macroParam("Bright"),
                    [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setMacro(v, 0, *val); });
    helper.addParam(*P::kTime, macroParam("Env Time"),
                    [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setMacro(v, 1, *val); });
    helper.addParam(*P::kAttack, macroParam("Attack"),
                    [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setMacro(v, 2, *val); });
    helper.addParam(*P::kRelease, macroParam("Release"),
                    [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setMacro(v, 3, *val); });

    helper.addParam(
      *P::kAlgorithm,
      {
        .displayName = "Algorithm",
        .defaultValue = 0,
        .minValue = 0,
        .maxValue = 32,
        .isEnum = true,
        .valueFormatter = [](const Patch& patch, Patch::paramvalue_t value) -> std::string
        {
            const int n = static_cast<int>(std::lround(value));
            char b[24];
            if (n <= 0)
                snprintf(b, sizeof(b), "Voice's (%d)", baseValue(patch, DN_FMV_ALG) + 1);
            else
                snprintf(b, sizeof(b), "%d", n);
            return b;
        },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setOverride(v, 0, *val); });

    // offsets from the voice's own value, shown as the value it ends up at
    auto offsetParam = [](const char* name, int range) {
        return Patch::Metadata::Param{
            .displayName = name,
            .defaultValue = 0.0f,
            .minValue = static_cast<Patch::paramvalue_short_t>(-range),
            .maxValue = static_cast<Patch::paramvalue_short_t>(range),
        };
    };
    auto withText = [](Patch::Metadata::Param p, int index, int top) {
        p.valueFormatter = [index, top](const Patch& patch, Patch::paramvalue_t v) {
            char b[8];
            snprintf(b, sizeof(b), "%d", std::clamp(baseValue(patch, index) + static_cast<int>(std::lround(v)), 0, top));
            return std::string(b);
        };
        return p;
    };
    helper.addParam(*P::kFeedback, withText(offsetParam("Feedback", 7), DN_FMV_FB, 7),
                    [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setOverride(v, 1, *val); });
    helper.addParam(*P::kLfoSpeed, withText(offsetParam("LFO Speed", 99), DN_FMV_LFS, 99),
                    [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setOverride(v, 2, *val); });
    helper.addParam(*P::kLfoDelay, withText(offsetParam("LFO Delay", 99), DN_FMV_LFD, 99),
                    [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setOverride(v, 3, *val); });
    helper.addParam(*P::kLfoPmd, withText(offsetParam("LFO Pitch Depth", 99), DN_FMV_LPMD, 99),
                    [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setOverride(v, 4, *val); });
    helper.addParam(*P::kLfoAmd, withText(offsetParam("LFO Amp Depth", 99), DN_FMV_LAMD, 99),
                    [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setAmd(v, *val); });

    helper.addParam(
      *P::kTranspose,
      {
        .displayName = "Transpose",
        .defaultValue = 0,
        .minValue = -36,
        .maxValue = 36,
        .valueFormatter = createSemitoneFormatter(),
      },
      [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setTranspose(v, *val); });
    helper.addParam(
      *P::kVolume,
      {
        .displayName = "Gain",
        .defaultValue = 0.0f,
        .minValue = -30,
        .maxValue = 12,
        .valueFormatter = [](const Patch&, Patch::paramvalue_t v)
        {
            char b[16];
            snprintf(b, sizeof(b), "%.1f dB", static_cast<double>(v));
            return std::string(b);
        },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { FmSynthVoice::setVolume(v, *val); });

    using EditorElement = Patch::Metadata::EditorLayout::EditorElement;
    auto e = [](P id) { return EditorElement{ .type = EditorElement::Type::kParam, .id = *id }; };
    metadata.layoutFlat = Patch::Metadata::EditorLayout{
        .sections = {
          { .name = "Voice", .shortName = "Voice",
            .elements = { e(P::kPreset), e(P::kAlgorithm), e(P::kFeedback), e(P::kTranspose), e(P::kVolume) } },
          { .name = "Shape Envelope", .shortName = "Shape",
            .elements = { e(P::kBright), e(P::kTime), e(P::kAttack), e(P::kRelease) } },
          { .name = "LFO", .shortName = "LFO",
            .elements = { e(P::kLfoSpeed), e(P::kLfoDelay), e(P::kLfoPmd), e(P::kLfoAmd) } },
        },
    };
    metadata.layout23 = metadata.layoutFlat;
    metadata.initDefaultMacros = [](auto& macros)
    {
        macros[0] = Patch::Metadata::Macro{ .displayName = "Bright", .modifiers = { { *P::kBright, 0.5f } },
                                            .bipolar = true };
        macros[1] = Patch::Metadata::Macro{ .displayName = "Env Time", .modifiers = { { *P::kTime, 0.5f } },
                                            .bipolar = true };
        macros[2] = Patch::Metadata::Macro{ .displayName = "Attack", .modifiers = { { *P::kAttack, 0.5f } },
                                            .bipolar = true };
        macros[3] = Patch::Metadata::Macro{ .displayName = "Release", .modifiers = { { *P::kRelease, 0.5f } },
                                            .bipolar = true };
        macros[4] = Patch::Metadata::Macro{ .displayName = "Vibrato", .modifiers = { { *P::kLfoPmd, 0.3f } } };
        macros[5] = Patch::Metadata::Macro{ .displayName = "Feedback", .modifiers = { { *P::kFeedback, 0.5f } },
                                            .bipolar = true };
    };
    metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kFmSynth);
    metadata.outputChannels = Patch::Metadata::OutputChannels::kMono;
    metadata.maxVoices = 6;
}

std::unique_ptr<InstrumentType> createFmSynth()
{
    return std::make_unique<InstrumentType>(
      InstrumentType{ .name = "DX7",
                      .createMetadata = &FmSynthVoice::createMetadata,
                      .createVoice = [](int) -> std::unique_ptr<InstrumentVoice>
                      { return std::make_unique<FmSynthVoice>(); } });
}

// the /DX7 folder, read one bank per idle call
void dxService()
{
    FileSystem* fs = Tracker::getInstance()->getPlatform()->getFileSystem();
    if (fs == nullptr || fs->isAsyncReadBusy())
        return;
    if (dxScanStage == 0)
    {
        if (!dxScanRequested)
            return;
        dxScanRequested = false;
        if (!fs->exists(kDxFolder))
            fs->mkdir(0, kDxFolder);
        for (auto& n : dxNames)
            n = nullptr;
        const int found = fs->createFilesList(kDxFolder, 0, dxNames, 64, 4000, FileSystem::enListFileFilter_all);
        dxNameCount = found > 0 ? found : 0;
        std::sort(dxNames, dxNames + dxNameCount, dxNameLess);
        dxNext = 0;
        dxBankLoading = 0;
        dxScanStage = 1;
        return;
    }
    if (dxNext < dxNameCount && dxBankLoading < kBanks)
    {
        const char* n = dxNames[dxNext++];
        const size_t len = n ? strlen(n) : 0;
        const bool syx = len > 4 && (strcasecmp(n + len - 4, ".syx") == 0 || strcasecmp(n + len - 4, ".bin") == 0);
        if (syx && loadBank(fs, n, dxBanks[dxBankLoading]))
        {
            dxBankLoading++;
            if (dxBankLoading > dxBankCount)
                dxBankCount = dxBankLoading; // a bank shows as soon as it is in
        }
        return;
    }
    for (int i = 0; i < dxNameCount; i++)
    {
        delete[] dxNames[i];
        dxNames[i] = nullptr;
    }
    dxBankCount = dxBankLoading;
    dxScanStage = 0;
}

} // namespace polysynth

void twistedDxService()
{
    polysynth::dxService();
}

#endif
