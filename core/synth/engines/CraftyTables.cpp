// Twisted: CRAFTY's wavetable files. Every WAV under /Crafty/Wavetables (sub-folders too) gets a number that
// never changes, kept in /Crafty/TableIndex.txt (one path a line: line n is number n). A CRAFTY patch's
// "Table 1 File" / "Table 2 File" hold those numbers, so saved patches and projects find their tables again.
// The folders are walked in the file manager's idle time, one folder per call; the names live in external RAM.
#ifdef FEATURE_SYNTH

#include "CraftyTables.h"

#include "Platform.h"
#include "Tracker.h"
#include "cr_section_macros.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace
{
constexpr const char* kRoot = "/Crafty/Wavetables";
constexpr const char* kIndexFile = "/Crafty/TableIndex.txt";
constexpr int kMax = twisted_tables::kMaxTables;
constexpr uint32_t kPoolBytes = 200 * 1024;
constexpr int kHash = 8192;
constexpr int kMaxFolders = 512;
constexpr uint32_t kFolderPoolBytes = 32 * 1024;
constexpr uint32_t kRewalkMs = 20000;

__NOINIT(EXTERNAL_RAM) char namePool[kPoolBytes];
__NOINIT(EXTERNAL_RAM) uint32_t nameOff[kMax + 1];
__NOINIT(EXTERNAL_RAM) uint16_t hashTab[kHash];
__NOINIT(EXTERNAL_RAM) uint16_t sorted[kMax];
__NOINIT(EXTERNAL_RAM) uint16_t sortPos[kMax + 1];
__NOINIT(EXTERNAL_RAM) uint8_t present[kMax + 1];
__NOINIT(EXTERNAL_RAM) char folderPool[kFolderPoolBytes];
__NOINIT(EXTERNAL_RAM) uint32_t folderOff[kMaxFolders];
__NOINIT(EXTERNAL_RAM) char lineBuf[1024];

int count = 0;      // numbers 1..count are in use
int saved = 0;      // numbers 1..saved are in the index file
int sortedCount = 0;
uint32_t poolUsed = 0;
int folders = 0, folderNext = 0;
uint32_t folderUsed = 0;
int stage = 0; // 0 idle, 1 start, 2 walking, 3 finish
bool requested = true;
bool indexLoaded = false;
bool walkedOnce = false;
uint32_t lastWalkMs = 0;

uint32_t nowMs()
{
    return Tracker::getInstance()->getPlatform()->getTimer()->millis();
}

uint32_t hashOf(const char* s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
    {
        char c = *s;
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    return h;
}

const char* nameOf(int id)
{
    return (id >= 1 && id <= count) ? namePool + nameOff[id] : nullptr;
}

int find(const char* path)
{
    for (uint32_t h = hashOf(path) & (kHash - 1);; h = (h + 1) & (kHash - 1))
    {
        const int id = hashTab[h];
        if (id == 0)
            return 0;
        if (strcasecmp(nameOf(id), path) == 0)
            return id;
    }
}

int add(const char* path)
{
    const uint32_t len = static_cast<uint32_t>(std::strlen(path)) + 1;
    if (count >= kMax || poolUsed + len > kPoolBytes || len < 2)
        return 0;
    const int id = ++count;
    nameOff[id] = poolUsed;
    std::memcpy(namePool + poolUsed, path, len);
    poolUsed += len;
    present[id] = 0;
    uint32_t h = hashOf(path) & (kHash - 1);
    while (hashTab[h] != 0)
        h = (h + 1) & (kHash - 1);
    hashTab[h] = static_cast<uint16_t>(id);
    return id;
}

void reset()
{
    count = saved = sortedCount = 0;
    poolUsed = 0;
    std::memset(hashTab, 0, sizeof(hashTab));
    std::memset(present, 0, sizeof(present));
    std::memset(sortPos, 0xFF, sizeof(sortPos));
}

bool isWav(const char* name)
{
    const size_t n = std::strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".wav") == 0 && name[0] != '.';
}

void pushFolder(const char* rel)
{
    const uint32_t len = static_cast<uint32_t>(std::strlen(rel)) + 1;
    if (folders >= kMaxFolders || folderUsed + len > kFolderPoolBytes)
        return;
    folderOff[folders++] = folderUsed;
    std::memcpy(folderPool + folderUsed, rel, len);
    folderUsed += len;
}

// the index file: one path a line
void loadIndex(FileSystem* fs)
{
    reset();
    if (!fs->exists(kIndexFile))
        return;
    auto [isOpen, fh] = fs->openFile(kIndexFile, FileSystem::FileOpenMode::kRead);
    if (!isOpen)
        return;
    char line[300];
    int len = 0;
    int32_t got;
    while ((got = fs->readFile(lineBuf, sizeof(lineBuf), fh)) > 0)
    {
        for (int i = 0; i < got; i++)
        {
            const char c = lineBuf[i];
            if (c == '\n' || c == '\r')
            {
                if (len > 0)
                {
                    line[len] = 0;
                    if (find(line) == 0) // a repeated line keeps its first number
                        add(line);
                    len = 0;
                }
                continue;
            }
            if (len < static_cast<int>(sizeof(line)) - 1)
                line[len++] = c;
        }
    }
    if (len > 0)
    {
        line[len] = 0;
        if (find(line) == 0)
            add(line);
    }
    fs->closeFile(fh);
    saved = count;
}

void saveNew(FileSystem* fs)
{
    if (saved >= count)
        return;
    auto [isOpen, fh] = fs->openFile(kIndexFile, fs->exists(kIndexFile) ? FileSystem::FileOpenMode::kWriteAppend
                                                                        : FileSystem::FileOpenMode::kWriteCreateNew);
    if (!isOpen)
        return;
    for (int id = saved + 1; id <= count; id++)
    {
        const char* n = nameOf(id);
        fs->writeFile(n, std::strlen(n), fh);
        fs->writeFile("\r\n", 2, fh);
    }
    fs->closeFile(fh);
    saved = count;
}

// one folder: its WAVs get numbers, its sub-folders wait their turn
void walkFolder(FileSystem* fs, const char* rel)
{
    char path[300];
    snprintf(path, sizeof(path), rel[0] ? "%s/%s" : "%s", kRoot, rel);
    if (!fs->nesDir_open(path))
        return;
    char name[300];
    char full[300];
    uint8_t isDir = 0, isHid = 0;
    uint32_t size = 0;
    while (fs->nesDir_readItem(name, &isDir, &isHid, &size))
    {
        if (isHid || name[0] == '.' || name[0] == 0)
            continue;
        snprintf(full, sizeof(full), rel[0] ? "%s/%s" : "%s%s", rel, name);
        if (isDir)
        {
            pushFolder(full);
            continue;
        }
        if (!isWav(name))
            continue;
        int id = find(full);
        if (id == 0)
            id = add(full);
        if (id != 0)
            present[id] = 1;
    }
    fs->nesDir_close();
}

void finish(FileSystem* fs)
{
    saveNew(fs);
    sortedCount = 0;
    for (int id = 1; id <= count; id++)
        if (present[id])
            sorted[sortedCount++] = static_cast<uint16_t>(id);
    std::sort(sorted, sorted + sortedCount,
              [](uint16_t a, uint16_t b) { return strcasecmp(nameOf(a), nameOf(b)) < 0; });
    std::memset(sortPos, 0xFF, sizeof(sortPos));
    for (int i = 0; i < sortedCount; i++)
        sortPos[sorted[i]] = static_cast<uint16_t>(i);
}

// the folder part of a path ("" at the top)
size_t folderLen(const char* p)
{
    const char* s = std::strrchr(p, '/');
    return s ? static_cast<size_t>(s - p) : 0;
}

bool sameFolder(int a, int b)
{
    const char* pa = nameOf(a);
    const char* pb = nameOf(b);
    const size_t la = folderLen(pa), lb = folderLen(pb);
    return la == lb && strncasecmp(pa, pb, la) == 0;
}

uint32_t rndState = 0;
uint32_t rnd()
{
    if (rndState == 0)
        rndState = Tracker::getInstance()->getPlatform()->getTimer()->micros() | 1u;
    rndState ^= rndState << 13;
    rndState ^= rndState >> 17;
    rndState ^= rndState << 5;
    return rndState;
}
} // namespace

namespace twisted_tables
{
void requestScan()
{
    requested = true;
}

void service(FileSystem* fs)
{
    if (stage == 0)
    {
        if (!requested)
            return;
        if (walkedOnce && nowMs() - lastWalkMs < kRewalkMs)
            return; // looked very recently: the next request walks again
        requested = false;
        stage = 1;
    }
    if (stage == 1)
    {
        if (!fs->exists("/Crafty"))
            fs->mkdir(0, "/Crafty");
        if (!fs->exists(kRoot))
            fs->mkdir(0, kRoot);
        if (!indexLoaded)
        {
            loadIndex(fs);
            indexLoaded = true;
        }
        std::memset(present, 0, sizeof(present));
        folders = folderNext = 0;
        folderUsed = 0;
        pushFolder("");
        stage = 2;
        return;
    }
    if (stage == 2)
    {
        if (folderNext < folders)
        {
            walkFolder(fs, folderPool + folderOff[folderNext++]);
            return;
        }
        stage = 3;
    }
    finish(fs);
    walkedOnce = true;
    lastWalkMs = nowMs();
    stage = 0;
}

const char* path(int id)
{
    return nameOf(id);
}

bool isPresent(int id)
{
    return id >= 1 && id <= count && present[id] && sortPos[id] != 0xFFFF;
}

bool known()
{
    return walkedOnce;
}

int step(int id, int delta, bool byFolder)
{
    if (sortedCount == 0 || delta == 0)
        return id;
    int pos = isPresent(id) ? sortPos[id] : -1; // -1: Off
    if (!byFolder)
    {
        pos = std::clamp(pos + delta, -1, sortedCount - 1);
        return pos < 0 ? 0 : sorted[pos];
    }
    // a folder at a time: the first table of the next (or this / the previous) folder
    for (int n = 0; n < std::abs(delta); n++)
    {
        if (delta > 0)
        {
            if (pos < 0)
            {
                pos = 0;
                continue;
            }
            int p = pos;
            while (p < sortedCount - 1 && sameFolder(sorted[p], sorted[pos]))
                p++;
            if (sameFolder(sorted[p], sorted[pos]))
                break; // the last folder
            pos = p;
        }
        else
        {
            if (pos <= 0)
            {
                pos = -1;
                break;
            }
            int first = pos;
            while (first > 0 && sameFolder(sorted[first - 1], sorted[pos]))
                first--;
            if (first < pos)
            {
                pos = first; // back to this folder's first table
                continue;
            }
            int p = pos - 1; // the previous folder's first table
            while (p > 0 && sameFolder(sorted[p - 1], sorted[pos - 1]))
                p--;
            pos = p;
        }
    }
    return pos < 0 ? 0 : sorted[pos];
}

int random()
{
    return sortedCount > 0 ? sorted[rnd() % static_cast<uint32_t>(sortedCount)] : 0;
}

void fullPath(int id, char* out, size_t outLen)
{
    const char* p = nameOf(id);
    snprintf(out, outLen, "%s/%s", kRoot, p ? p : "");
}
} // namespace twisted_tables

#endif
