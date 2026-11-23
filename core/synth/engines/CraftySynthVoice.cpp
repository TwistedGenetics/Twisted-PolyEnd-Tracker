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
#include "CraftyTables.h"
#include "TwistedCrash.h"
#include "CraftyUser.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"
#include "Platform.h"
#include "Tracker.h"
#include <cr_section_macros.h>

extern "C" {
#include "crafty/cr_inst.h"
#include "crafty/cr_text.h"
#include "crafty/cr_user.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    kSubLvl, // 0.12: the sub oscillator
    kSubOct,
    kSubWave,
    kTable1File, // Twisted: a wavetable file in place of Bank 1 / Bank 2 (CraftyTables.cpp)
    kTable2File,
};
static_assert(static_cast<int>(CraftyParamId::kSubWave) == CR_NP, "one parameter per Crafty patch value");

// ------------------------------------------------------------------ the instance pool (external RAM)
constexpr int kPoolSize = 4;
__NOINIT(EXTERNAL_RAM) uint8_t craftyPool[kPoolSize][sizeof(cr_inst_t)] __attribute__((aligned(8)));
bool craftyPoolUsed[kPoolSize] = {};

// ------------------------------------------------------------------ the user area (user presets, user banks)
// The same bytes as the FM-1's flash and the plugins' "Crafty Synth.user" file. Filled from the SD card in
// the file manager's idle time (twistedCraftyService); until then CRAFTY has no user banks or presets.
__NOINIT(EXTERNAL_RAM) uint8_t craftyUser[CR_USER_BYTES] __attribute__((aligned(4)));
bool craftyUserReady = false;

const uint8_t* userArea()
{
    return craftyUserReady ? craftyUser : nullptr;
}

const char* userBankName(void* /*ctx*/, uint32_t u)
{
    const cr_ubank_t* b = cr_user_bank(userArea(), u);
    return b ? b->name : nullptr;
}

void craftyRequestScanImpl();

// Twisted: wavetable files. Each pooled instance has room for its two files' waves; the voice says which file it
// wants (fileWant), the main loop reads it once the choice has rested a moment (you can scroll through files).
__NOINIT(EXTERNAL_RAM) int16_t fileWaves[kPoolSize][2][5][CR_WT_N];
volatile int fileWant[kPoolSize][2] = {};
int fileLoaded[kPoolSize][2] = {};
int fileSeen[kPoolSize][2] = {};
uint32_t fileSeenMs[kPoolSize][2] = {};
constexpr uint32_t kFileRestMs = 150;

int poolIndexOf(const cr_inst_t* in)
{
    for (int i = 0; i < kPoolSize; i++)
        if (reinterpret_cast<const cr_inst_t*>(craftyPool[i]) == in)
            return i;
    return -1;
}

cr_inst_t* takeInstance()
{
    for (int i = 0; i < kPoolSize; i++)
    {
        if (!craftyPoolUsed[i])
        {
            auto* in = reinterpret_cast<cr_inst_t*>(craftyPool[i]);
            for (int k = 0; k < 2; k++)
                fileWant[i][k] = fileLoaded[i][k] = fileSeen[i][k] = 0;
            twCrashCrumb(kCrumbCraftyInstance);
            cr_inst_init(in);
            cr_inst_set_user(in, userArea());
            craftyPoolUsed[i] = true;
            craftyRequestScanImpl(); // pick up any new wavetables in the folder
            twisted_tables::requestScan();
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

uint32_t craftyNowMs()
{
    return Tracker::getInstance()->getPlatform()->getTimer()->millis();
}

bool readTableWaves(FileSystem* fs, const char* path, int16_t out[5][CR_WT_N]);

// main loop: read the wavetable files the voices want
void serviceFileTables(FileSystem* fs)
{
    for (int i = 0; i < kPoolSize; i++)
    {
        if (!craftyPoolUsed[i])
            continue;
        auto* in = reinterpret_cast<cr_inst_t*>(craftyPool[i]);
        for (int k = 0; k < 2; k++)
        {
            const int want = fileWant[i][k];
            if (want == fileLoaded[i][k])
                continue;
            if (want != fileSeen[i][k])
            {
                fileSeen[i][k] = want;
                fileSeenMs[i][k] = craftyNowMs();
                return;
            }
            if (craftyNowMs() - fileSeenMs[i][k] < kFileRestMs)
                continue;
            if (want != 0 && !twisted_tables::known())
                continue; // the folders haven't been walked yet: the number means nothing so far
            bool ok = false;
            twCrashCrumb(kCrumbCraftyFileLoad);
            if (want != 0 && twisted_tables::isPresent(want))
            {
                char path[340];
                twisted_tables::fullPath(want, path, sizeof(path));
                ok = readTableWaves(fs, path, fileWaves[i][k]);
            }
            fileLoaded[i][k] = want;
            cr_inst_file_table(in, static_cast<uint32_t>(k), ok ? fileWaves[i][k] : nullptr);
            twCrashCrumb(kCrumbCraftyFileDone);
            return; // one file a call
        }
    }
}

// ------------------------------------------------------------------ stepped values
// how many choices a stepped parameter has (0: continuous) and the patch value of choice i
int choiceCount(int id)
{
    switch (CR_P[id].kind)
    {
        case CR_K_BANK: return id == CR_W2BANK ? CR_BANKS : CR_BANKS - 1; // factory + the 32 user banks
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
        case CR_K_SUBOCT: return 2;
        case CR_K_SUBWAVE: return 3;
        default: return 0;
    }
}
int choiceValue(int id, int i)
{
    switch (CR_P[id].kind)
    {
        case CR_K_BANK:
        {
            // Wave 1 has no sine / noise bank: its choices skip it
            uint32_t bank = static_cast<uint32_t>(i);
            if (id != CR_W2BANK && bank >= CR_BANK_NOISE)
                bank++;
            return static_cast<int>(cr_bank_val(bank, id == CR_W2BANK));
        }
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
    return v ? std::clamp<int>(static_cast<int>(std::lround(*v)), 0, CR_NPRESET - 1) : 0;
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
    (void)cr_preset_get(userArea(), static_cast<uint32_t>(presetOf(patch)), &t);
    const int pv = t.p[id];
    const int fv = finalValue(id, pv, knob);
    t.p[id] = static_cast<uint8_t>(fv);
    char b[32] = {};
    cr_param_text(&t, static_cast<uint32_t>(id), b, 1, userBankName, nullptr);
    std::string s = titleCase(b);
    if (choiceCount(id) > 0 && std::lround(knob) <= 0)
        s = "PRESET (" + s + ")";
    return s;
}

// ------------------------------------------------------------------ reading the wavetable folder
constexpr const char* kCraftyFolder = "/Crafty";
constexpr const char* kCraftyTables = "/Crafty/Wavetables";
constexpr const char* kCraftyUserFile = "/Crafty/Crafty Synth.user";
constexpr int kMaxFrame = 4096;      // the longest frame (one cycle) read
constexpr int kMaxFiles = 64;
constexpr int kWaveN = CR_WT_N;      // 256 samples a wave
constexpr float kPeak = 26000.0f;    // the firmware's wavetable peak (CR_WT_PEAK)

__NOINIT(EXTERNAL_RAM) float scanFrame[kMaxFrame];
__NOINIT(EXTERNAL_RAM) float scanCos[kMaxFrame];
__NOINIT(EXTERNAL_RAM) float scanSin[kMaxFrame];
__NOINIT(EXTERNAL_RAM) float scanWaves[5][kWaveN];
__NOINIT(EXTERNAL_RAM) float scanC256[kWaveN];
__NOINIT(EXTERNAL_RAM) float scanS256[kWaveN];
__NOINIT(EXTERNAL_RAM) int16_t importWaves[5][kWaveN];

struct Scan
{
    bool requested = true; // read the folder once after start-up
    int stage = 0;         // 0 idle, 1 start, 2 one file at a time
    char* names[kMaxFiles] = {};
    int count = 0, next = 0, bank = 0;
    uint32_t signature = 0; // the folder and user file as last read
} scan;

struct WavInfo
{
    uint32_t dataOff = 0, dataLen = 0;
    int format = 0, channels = 0, bits = 0, clm = 0;
};

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool readWavInfo(FileSystem* fs, FileSystem::FileHandle fh, WavInfo& w)
{
    uint8_t h[16];
    if (fs->readFile(h, 12, fh) != 12 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0)
        return false;
    const uint32_t size = static_cast<uint32_t>(fs->getFileSize(fh));
    uint32_t pos = 12;
    while (pos + 8 <= size)
    {
        if (!fs->seekFile(pos, fh) || fs->readFile(h, 8, fh) != 8)
            break;
        const uint32_t len = rd32(h + 4), body = pos + 8;
        if (std::memcmp(h, "fmt ", 4) == 0 && len >= 16)
        {
            uint8_t f[16];
            if (fs->readFile(f, 16, fh) != 16)
                return false;
            w.format = rd16(f);
            w.channels = rd16(f + 2);
            w.bits = rd16(f + 14);
        }
        else if (std::memcmp(h, "clm ", 4) == 0)
        {
            char c[17] = {};
            const int n = static_cast<int>(std::min<uint32_t>(len, 16));
            if (fs->readFile(c, n, fh) == n)
            {
                const char* m = std::strstr(c, "<!>");
                if (m)
                    w.clm = std::atoi(m + 3);
            }
        }
        else if (std::memcmp(h, "data", 4) == 0)
        {
            w.dataOff = body;
            w.dataLen = std::min(len, size - body);
        }
        pos = body + len + (len & 1u);
    }
    return w.dataLen > 0 && w.channels > 0 &&
           (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32);
}

// one frame of the table, first channel, as floats
bool readFrame(FileSystem* fs, FileSystem::FileHandle fh, const WavInfo& w, uint32_t frame, int size)
{
    const int bps = w.bits / 8, step = bps * w.channels;
    if (!fs->seekFile(w.dataOff + frame * static_cast<uint32_t>(size) * step, fh))
        return false;
    uint8_t buf[1024];
    int done = 0;
    while (done < size)
    {
        const int want = std::min(size - done, static_cast<int>(sizeof(buf)) / step);
        if (fs->readFile(buf, want * step, fh) != want * step)
            return false;
        for (int i = 0; i < want; i++)
        {
            const uint8_t* p = buf + i * step;
            float x;
            if (w.format == 3 && w.bits == 32)
            {
                uint32_t u = rd32(p);
                std::memcpy(&x, &u, 4);
            }
            else if (w.bits == 16)
                x = static_cast<int16_t>(rd16(p)) / 32768.0f;
            else if (w.bits == 24)
                x = static_cast<float>(static_cast<int32_t>(rd32(p) << 8) >> 8) / 8388608.0f;
            else if (w.bits == 32)
                x = static_cast<float>(static_cast<int32_t>(rd32(p))) / 2147483648.0f;
            else
                x = (p[0] - 128) / 128.0f;
            scanFrame[done + i] = x;
        }
        done += want;
    }
    return true;
}

// a frame of any length to 256 samples with at most 128 harmonics, DC removed (Crafty's editor's way)
void resampleFrame(int L, float* out)
{
    float* c256 = scanC256;
    float* s256 = scanS256;
    static bool ready = false;
    if (!ready)
    {
        for (int i = 0; i < kWaveN; i++)
        {
            c256[i] = std::cos(6.28318531f * i / kWaveN);
            s256[i] = std::sin(6.28318531f * i / kWaveN);
        }
        ready = true;
    }
    for (int i = 0; i < kWaveN; i++)
        out[i] = 0.0f;
    for (int k = 1; k <= 128 && k < L / 2 + (L % 2); k++)
    {
        float re = 0.0f, im = 0.0f;
        int idx = 0;
        for (int i = 0; i < L; i++)
        {
            re += scanFrame[i] * scanCos[idx];
            im += scanFrame[i] * scanSin[idx];
            idx += k;
            if (idx >= L)
                idx -= L;
        }
        re *= 2.0f / L;
        im *= 2.0f / L;
        if (k == 128)
        {
            re *= 0.5f;
            im = 0.0f;
        }
        if (re * re + im * im < 1e-12f)
            continue;
        int j = 0;
        for (int i = 0; i < kWaveN; i++)
        {
            out[i] += re * c256[j] + im * s256[j];
            j = (j + k) & (kWaveN - 1);
        }
    }
}

// a WAV file's five waves (frames picked evenly through the table), scaled to CRAFTY's wavetable peak
bool readTableWaves(FileSystem* fs, const char* path, int16_t out[5][kWaveN])
{
    auto [isOpen, fh] = fs->openFile(path, FileSystem::FileOpenMode::kRead);
    if (!isOpen)
        return false;
    WavInfo w;
    bool ok = readWavInfo(fs, fh, w);
    int size = 0;
    uint32_t frames = 0;
    if (ok)
    {
        const uint32_t n = w.dataLen / static_cast<uint32_t>((w.bits / 8) * w.channels);
        size = w.clm > 0 ? w.clm : 2048;
        if (n < static_cast<uint32_t>(size))
            size = static_cast<int>(n); // one cycle, whatever its length
        ok = size >= 8 && size <= kMaxFrame;
        frames = ok ? std::max<uint32_t>(1, n / static_cast<uint32_t>(size)) : 0;
    }
    auto& waves = scanWaves;
    if (ok)
    {
        for (int i = 0; i < size; i++)
        {
            scanCos[i] = std::cos(6.28318531f * i / size);
            scanSin[i] = std::sin(6.28318531f * i / size);
        }
        for (int k = 0; k < 5 && ok; k++)
        {
            const uint32_t pick = static_cast<uint32_t>(std::lround(k * (frames - 1) / 4.0f));
            ok = readFrame(fs, fh, w, pick, size);
            if (ok)
                resampleFrame(size, waves[k]);
        }
    }
    fs->closeFile(fh);
    if (!ok)
        return false;

    float peak = 0.0f;
    for (auto& wave : waves)
        for (float v : wave)
            peak = std::max(peak, std::fabs(v));
    if (peak <= 0.0f)
        return false;

    const float g = kPeak / peak;
    for (int k = 0; k < 5; k++)
        for (int i = 0; i < kWaveN; i++)
            out[k][i] = static_cast<int16_t>(std::lround(waves[k][i] * g));
    return true;
}

// a WAV file in the folder into user bank u
bool importWav(FileSystem* fs, const char* name, uint32_t u)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", kCraftyTables, name);
    auto& waves = importWaves;
    if (!readTableWaves(fs, path, waves))
        return false;

    auto* b = reinterpret_cast<cr_ubank_t*>(craftyUser + CR_FL_BANKS + u * CR_SECTOR);
    std::memset(b, 0, sizeof(*b));
    b->magic = CR_UBANK_MAGIC;
    b->ver = 1;
    int c = 0;
    for (const char* s = name; *s && *s != '.' && c < 13; s++)
    {
        char ch = *s;
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 'a' + 'A');
        if (ch >= ' ' && ch <= '_')
            b->name[c++] = ch;
    }
    std::memcpy(b->w, waves, sizeof(b->w));
    return true;
}

bool bankInUse(uint32_t u)
{
    const auto* b = reinterpret_cast<const cr_ubank_t*>(craftyUser + CR_FL_BANKS + u * CR_SECTOR);
    return b->magic == CR_UBANK_MAGIC && b->ver == 1u;
}

bool nameLess(const char* a, const char* b)
{
    for (;; a++, b++)
    {
        int x = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a, y = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (x != y || !x)
            return x < y;
    }
}

void craftyRequestScanImpl()
{
    scan.requested = true;
}

// a bank the mod matrix, a knob or a CC moved to: built here, never in the audio interrupt (see cr_inst.c)
void craftyBuildTablesImpl()
{
    for (int i = 0; i < kPoolSize; i++)
    {
        auto* in = reinterpret_cast<cr_inst_t*>(craftyPool[i]);
        if (craftyPoolUsed[i] && cr_inst_tables_pending(in))
        {
            twCrashCrumb(kCrumbCraftyTables);
            cr_inst_tables(in);
        }
    }
}

void craftyServiceImpl()
{
    FileSystem* fs = Tracker::getInstance()->getPlatform()->getFileSystem();
    if (fs == nullptr || fs->isAsyncReadBusy())
        return;

    twisted_tables::service(fs); // the wavetable files' numbers (one folder a call)
    serviceFileTables(fs);       // the files the voices want

    if (scan.stage == 0)
    {
        if (!scan.requested)
            return;
        scan.requested = false;
        scan.stage = 1;
        twCrashCrumb(kCrumbCraftyScan);
    }

    if (scan.stage == 1)
    {
        // First the folder's list and the user file's size: when neither changed since the last read, there's
        // nothing to do. (A scan is asked for whenever a CRAFTY voice is made, which happens a lot while
        // you move around the menus: re-reading the user file and re-importing every table each time stalled
        // the Tracker for seconds and rewrote the user area under the voices that were playing.)
        if (!fs->exists(kCraftyFolder))
            fs->mkdir(0, kCraftyFolder);
        if (!fs->exists(kCraftyTables))
            fs->mkdir(0, kCraftyTables);
        for (auto& n : scan.names)
            n = nullptr;
        uint32_t signature = 0;
        const int found = fs->createFilesList(kCraftyTables, 0, scan.names, kMaxFiles, 6000,
                                              FileSystem::enListFileFilter_wave, 0, 0,
                                              FileSystem::enListFileFilter_wave, &signature);
        scan.count = found > 0 ? found : 0;
        if (fs->exists(kCraftyUserFile))
        {
            auto [isOpen, fh] = fs->openFile(kCraftyUserFile, FileSystem::FileOpenMode::kRead);
            if (isOpen)
            {
                signature = (signature ^ static_cast<uint32_t>(fs->getFileSize(fh))) * 16777619u;
                fs->closeFile(fh);
            }
        }
        else
        {
            signature = (signature ^ 0xA5A5A5A5u) * 16777619u;
        }
        if (craftyUserReady && signature == scan.signature)
        {
            for (int i = 0; i < scan.count; i++)
            {
                delete[] scan.names[i];
                scan.names[i] = nullptr;
            }
            scan.stage = 0;
            return;
        }
        scan.signature = signature;

        // changed: an empty user area, the plugin's user file if there is one, then the folder's tables
        std::memset(craftyUser, 0xFF, sizeof(craftyUser));
        if (fs->exists(kCraftyUserFile))
        {
            auto [isOpen, fh] = fs->openFile(kCraftyUserFile, FileSystem::FileOpenMode::kRead);
            if (isOpen)
            {
                const size_t n = std::min<size_t>(fs->getFileSize(fh), sizeof(craftyUser));
                if (fs->readFile(craftyUser, n, fh) < 0)
                    std::memset(craftyUser, 0xFF, sizeof(craftyUser));
                fs->closeFile(fh);
            }
        }
        std::sort(scan.names, scan.names + scan.count, nameLess);
        scan.next = 0;
        scan.bank = 0;
        scan.stage = 2;
        return;
    }

    // one file per call: into the next free user bank
    while (scan.bank < static_cast<int>(CR_NUBANK) && bankInUse(static_cast<uint32_t>(scan.bank)))
        scan.bank++;
    if (scan.next < scan.count && scan.bank < static_cast<int>(CR_NUBANK))
    {
        twCrashCrumb(kCrumbCraftyImport);
        if (importWav(fs, scan.names[scan.next], static_cast<uint32_t>(scan.bank)))
            scan.bank++;
        scan.next++;
        return;
    }

    // done: every CRAFTY instrument plays the new banks
    for (int i = 0; i < scan.count; i++)
    {
        delete[] scan.names[i];
        scan.names[i] = nullptr;
    }
    craftyUserReady = true;
    for (int i = 0; i < kPoolSize; i++)
    {
        if (craftyPoolUsed[i])
            cr_inst_set_user(reinterpret_cast<cr_inst_t*>(craftyPool[i]), craftyUser);
    }
    scan.stage = 0;
}

} // namespace

namespace
{

class CraftySynthVoice : public sdk::CustomSynthVoice<CraftySynthVoice, CraftyParamId>
{
  public:
    CraftySynthVoice()
      : in(takeInstance())
      , poolIdx(poolIndexOf(in))
    {
        playMode = PlayMode::kLegato;
        std::fill(std::begin(knob), std::end(knob), 0.0f);
    }
    ~CraftySynthVoice() override
    {
        if (in)
            giveInstance(in);
    }

    // Twisted: sounding while a note's amp envelope runs, then until the output (tails, drones) has been
    // silent for 2 s. CRAFTY's own test also counted any patch with AMP EG off full (two factory presets,
    // older random patches) as sounding for ever, so it was rendered -- the whole engine and its effects --
    // all the time, even with nothing playing.
    bool isActive() final { return in != nullptr && (in->syn.env[1].st != 0u || quietBlocks < kQuietBlocks); }

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
        quietBlocks = 0;
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
        if (!isActive()) // silent: nothing to work out
        {
            std::fill(outL.begin(), outL.end(), 0.0f);
            std::fill(outR.begin(), outR.end(), 0.0f);
            return;
        }
        cr_inst_transport(in, -1.0, ctx.tempo > 1.0f ? ctx.tempo : 120.0f, 0);
        float lr[2 * CR_INST_FRAMES];
        const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(outL.size()), CR_INST_FRAMES);
        cr_inst_render(in, lr, n);
        float peak = 0.0f;
        for (uint32_t i = 0; i < n; i++)
        {
            outL[i] = lr[2 * i];
            outR[i] = lr[2 * i + 1];
            peak = std::max(peak, std::max(std::fabs(lr[2 * i]), std::fabs(lr[2 * i + 1])));
        }
        if (in->syn.env[1].st == 0u && peak < 3.0e-5f) // about -90 dB
        {
            if (quietBlocks < kQuietBlocks)
                quietBlocks++;
        }
        else
        {
            quietBlocks = 0;
        }
    }

    static void createMetadata(const Patch& patch, Patch::Metadata& metadata);

    static void setPreset(CraftySynthVoice& v, float x)
    {
        v.preset = std::clamp<int>(static_cast<int>(std::lround(x)), 0, CR_NPRESET - 1);
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
    static void setFile(CraftySynthVoice& v, int k, float x)
    {
        if (v.poolIdx >= 0)
            fileWant[v.poolIdx][k] = std::clamp<int>(static_cast<int>(std::lround(x)), 0, twisted_tables::kMaxTables);
    }
    static void setValue(CraftySynthVoice& v, int id, float x)
    {
        v.knob[id] = x;
        v.apply(id);
    }

  private:
    cr_inst_t* in = nullptr;
    int poolIdx = -1;
    int preset = 0;
    static constexpr int kQuietBlocks = 690; // 2 s of 128 samples
    int quietBlocks = 0;
    float knob[CR_NP];
    bool suppress = false;

    void apply(int id)
    {
        if (in == nullptr)
            return;
        cr_patch_t t;
        (void)cr_preset_get(userArea(), static_cast<uint32_t>(preset), &t);
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
        .maxValue = static_cast<Patch::paramvalue_short_t>(CR_NPRESET - 1),
        .isEnum = true,
        .valueFormatter = [](const Patch&, Patch::paramvalue_t value)
        {
            const uint32_t slot = static_cast<uint32_t>(std::clamp<int>(std::lround(value), 0, CR_NPRESET - 1));
            char label[8];
            cr_preset_label(slot, label);
            if (slot >= CR_NFACTORY && !cr_user_saved(userArea(), slot))
                return std::string(label) + " (EMPTY)";
            cr_patch_t t;
            (void)cr_preset_get(userArea(), slot, &t);
            char name[13] = {};
            std::memcpy(name, t.name, 12);
            return std::string(label) + " " + titleCase(name);
        },
      },
      [](auto& v, Patch::optional_paramvalue_t val) { CraftySynthVoice::setPreset(v, *val); });

    addAllCraftyParams(helper, std::make_integer_sequence<int, CR_NP>{});

    // Twisted: a wavetable file from /Crafty/Wavetables in place of the bank (0: the bank)
    auto fileText = [](Patch::paramvalue_t value) -> std::string
    {
        const int id = static_cast<int>(std::lround(value));
        if (id <= 0)
            return "Off (Bank)";
        const char* p = twisted_tables::path(id);
        if (p == nullptr)
            return twisted_tables::known() ? "Missing file" : "File " + std::to_string(id);
        std::string s(p);
        if (s.size() > 4 && (s.compare(s.size() - 4, 4, ".wav") == 0 || s.compare(s.size() - 4, 4, ".WAV") == 0))
            s.resize(s.size() - 4);
        return twisted_tables::isPresent(id) ? s : "Missing: " + s;
    };
    helper.addParam(*P::kTable1File,
                    { .displayName = "Table 1 File",
                      .defaultValue = 0.0f,
                      .minValue = 0,
                      .maxValue = twisted_tables::kMaxTables,
                      .valueFormatter = [fileText](const Patch&, Patch::paramvalue_t v) { return fileText(v); } },
                    [](auto& v, Patch::optional_paramvalue_t val) { CraftySynthVoice::setFile(v, 0, val.value_or(0)); });
    helper.addParam(*P::kTable2File,
                    { .displayName = "Table 2 File",
                      .defaultValue = 0.0f,
                      .minValue = 0,
                      .maxValue = twisted_tables::kMaxTables,
                      .valueFormatter = [fileText](const Patch&, Patch::paramvalue_t v) { return fileText(v); } },
                    [](auto& v, Patch::optional_paramvalue_t val) { CraftySynthVoice::setFile(v, 1, val.value_or(0)); });

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
    layout.sections.back().elements.insert(layout.sections.back().elements.begin() + 1,
                                           EditorElement{ .type = EditorElement::Type::kParam, .id = *P::kTable1File });
    layout.sections.back().elements.insert(layout.sections.back().elements.begin() + 4,
                                           EditorElement{ .type = EditorElement::Type::kParam, .id = *P::kTable2File });
    layout.sections.push_back(section("Oscillator Mod", "Osc Mod", { CR_OMMODE, CR_OMAMT, CR_BMODE, CR_BAMT }));
    layout.sections.push_back(section("Sub Oscillator", "Sub", { CR_SUBLVL, CR_SUBOCT, CR_SUBWAVE }));
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

void craftyService()
{
    craftyServiceImpl();
}
void craftyRequestScan()
{
    craftyRequestScanImpl();
}

} // namespace polysynth

void twistedCraftyService()
{
    polysynth::craftyService();
}
int twistedCraftyTableStep(int id, int delta, bool byFolder)
{
    return twisted_tables::step(id, delta, byFolder);
}
int twistedCraftyRandomTable()
{
    return twisted_tables::random();
}
void twistedCraftyBuildTables()
{
    polysynth::craftyBuildTablesImpl();
}
void twistedCraftyRequestScan()
{
    polysynth::craftyRequestScan();
}

#endif
