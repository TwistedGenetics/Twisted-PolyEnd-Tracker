#include "TwistedSong.h"

#include "fileManager.h"
#include "fileManagerDefs.h"
#include "mtStructs.h"

#include <cr_section_macros.h>
#include <string.h>

namespace
{
constexpr char kFileId[4] = { 'T', 'W', 'S', 'G' };
constexpr uint16_t kFileVersion = 1;

struct TwistedSongFile
{
    struct Header
    {
        char id[4];
        uint16_t version;
        uint16_t sectionCount;
        uint32_t dataSize;
        uint32_t reserved;
    } header;

    struct Section
    {
        uint8_t startPos;
        char name[TwistedSong::kSectionNameLength + 1];
        uint8_t reserved[2];
    } section[TwistedSong::kMaxSections];

    struct Variation
    {
        uint8_t parent;
        uint8_t reserved[3];
        uint32_t ownMask;
    } variation[PATTERN_INDEX_MAX + 1];
};

__NOINIT(EXTERNAL_RAM) TwistedSongFile songData;

const char kEmptyText[] = "";
} // namespace

void TwistedSong::init(FileSystem* fileSystem)
{
    this->fileSystem = fileSystem;
    reset();
}

void TwistedSong::reset()
{
    memset(&songData, 0, sizeof(songData));
    memcpy(songData.header.id, kFileId, 4);
    songData.header.version = kFileVersion;
    songData.header.dataSize = sizeof(songData);
}

void TwistedSong::load()
{
    if (!readFile(cWorkspaceTwistedSongFilePath))
    {
        reset();
        writeFile(cWorkspaceTwistedSongFilePath);
    }
}

bool TwistedSong::copyFile(const char* src, const char* dest)
{
    bool correct = readFile(src);
    if (!correct)
    {
        reset();
    }
    writeFile(dest);
    return correct;
}

bool TwistedSong::readFile(const char* path)
{
    if (fileSystem == nullptr || !fileSystem->exists(path))
    {
        return false;
    }

    auto openFileInfo = fileSystem->openFile(path);
    auto isOpen = std::get<0>(openFileInfo);
    auto file = std::get<1>(openFileInfo);

    if (!isOpen)
    {
        return false;
    }

    int32_t readSize = fileSystem->readFile(&songData, sizeof(songData), file);
    fileSystem->closeFile(file);

    if (readSize != (int32_t)sizeof(songData) || memcmp(songData.header.id, kFileId, 4) != 0 ||
        songData.header.version != kFileVersion || songData.header.sectionCount > kMaxSections)
    {
        reset();
        return false;
    }

    for (uint8_t i = 0; i < kMaxSections; i++)
    {
        songData.section[i].name[kSectionNameLength] = 0;
    }

    return true;
}

void TwistedSong::writeFile(const char* path)
{
    if (fileSystem == nullptr)
    {
        return;
    }

    memcpy(songData.header.id, kFileId, 4);
    songData.header.version = kFileVersion;
    songData.header.dataSize = sizeof(songData);

    newFileManager.createWorkspaceDirs();

    auto openFileInfo = fileSystem->openFile(path, FileSystem::FileOpenMode::kWriteOpen);
    auto isOpen = std::get<0>(openFileInfo);
    auto file = std::get<1>(openFileInfo);

    if (isOpen)
    {
        fileSystem->writeFile(&songData, sizeof(songData), file);
        fileSystem->closeFile(file);
    }
}

void TwistedSong::save()
{
    writeFile(cWorkspaceTwistedSongFilePath);

    mtProject.values.projectNotSavedFlag |= 1;
    newFileManager.setProjectStructChanged();
}

//==================================================================================== sections

uint8_t TwistedSong::getSectionCount()
{
    return songData.header.sectionCount;
}

int TwistedSong::findSectionStartingAt(uint8_t pos)
{
    for (uint8_t i = 0; i < songData.header.sectionCount; i++)
    {
        if (songData.section[i].startPos == pos)
        {
            return i;
        }
    }
    return -1;
}

const char* TwistedSong::getSectionName(uint8_t sectionIdx)
{
    if (sectionIdx >= songData.header.sectionCount)
    {
        return kEmptyText;
    }
    return songData.section[sectionIdx].name;
}

uint8_t TwistedSong::getSectionStart(uint8_t sectionIdx)
{
    if (sectionIdx >= songData.header.sectionCount)
    {
        return 0;
    }
    return songData.section[sectionIdx].startPos;
}

const char* TwistedSong::getSectionNameForPos(uint8_t pos)
{
    const char* name = kEmptyText;
    for (uint8_t i = 0; i < songData.header.sectionCount; i++)
    {
        if (songData.section[i].startPos <= pos)
        {
            name = songData.section[i].name;
        }
    }
    return name;
}

void TwistedSong::getSectionRange(uint8_t pos, uint8_t songLength, uint8_t* start, uint8_t* end)
{
    uint8_t s = 0;
    uint8_t e = songLength > 0 ? songLength - 1 : 0;

    // sections are kept sorted by start position
    for (uint8_t i = 0; i < songData.header.sectionCount; i++)
    {
        uint8_t sectionStart = songData.section[i].startPos;
        if (sectionStart <= pos)
        {
            s = sectionStart;
        }
        else
        {
            if (sectionStart > 0 && sectionStart - 1 < e)
            {
                e = sectionStart - 1;
            }
            break;
        }
    }

    if (pos > e)
    {
        e = pos;
    }

    *start = s;
    *end = e;
}

void TwistedSong::sortSections()
{
    uint8_t count = songData.header.sectionCount;
    for (uint8_t i = 1; i < count; i++)
    {
        TwistedSongFile::Section tmp = songData.section[i];
        int j = i - 1;
        while (j >= 0 && songData.section[j].startPos > tmp.startPos)
        {
            songData.section[j + 1] = songData.section[j];
            j--;
        }
        songData.section[j + 1] = tmp;
    }
}

bool TwistedSong::setSection(uint8_t pos, const char* name)
{
    int idx = findSectionStartingAt(pos);

    if (idx < 0)
    {
        if (songData.header.sectionCount >= kMaxSections)
        {
            return false;
        }
        idx = songData.header.sectionCount++;
        memset(&songData.section[idx], 0, sizeof(songData.section[idx]));
        songData.section[idx].startPos = pos;
    }

    strncpy(songData.section[idx].name, name, kSectionNameLength);
    songData.section[idx].name[kSectionNameLength] = 0;

    sortSections();
    save();
    return true;
}

void TwistedSong::removeSection(uint8_t pos)
{
    int idx = findSectionStartingAt(pos);
    if (idx < 0)
    {
        return;
    }

    for (uint8_t i = idx; i + 1 < songData.header.sectionCount; i++)
    {
        songData.section[i] = songData.section[i + 1];
    }
    songData.header.sectionCount--;
    save();
}

void TwistedSong::onSlotInserted(uint8_t newPos)
{
    bool changed = false;
    for (uint8_t i = 0; i < songData.header.sectionCount; i++)
    {
        if (songData.section[i].startPos >= newPos && songData.section[i].startPos < SONG_MAX - 1)
        {
            songData.section[i].startPos++;
            changed = true;
        }
    }

    if (changed)
    {
        save();
    }
}

void TwistedSong::onSlotDeleted(uint8_t deletedPos, uint8_t newSongLength)
{
    bool changed = false;

    for (uint8_t i = 0; i < songData.header.sectionCount; i++)
    {
        if (songData.section[i].startPos > deletedPos)
        {
            songData.section[i].startPos--;
            changed = true;
        }
    }

    // drop dividers that now point past the end or onto the same slot as another
    uint8_t write = 0;
    for (uint8_t read = 0; read < songData.header.sectionCount; read++)
    {
        bool keep = songData.section[read].startPos < newSongLength;
        if (keep && write > 0 && songData.section[write - 1].startPos == songData.section[read].startPos)
        {
            keep = false;
        }

        if (keep)
        {
            if (write != read)
            {
                songData.section[write] = songData.section[read];
            }
            write++;
        }
        else
        {
            changed = true;
        }
    }
    songData.header.sectionCount = write;

    if (changed)
    {
        save();
    }
}

//==================================================================================== variations

uint8_t TwistedSong::getParent(uint8_t pattern)
{
    return songData.variation[pattern].parent;
}

uint32_t TwistedSong::getOwnMask(uint8_t pattern)
{
    return songData.variation[pattern].ownMask;
}

bool TwistedSong::hasChildren(uint8_t pattern)
{
    if (pattern == 0)
    {
        return false;
    }

    for (int i = PATTERN_INDEX_MIN; i <= PATTERN_INDEX_MAX; i++)
    {
        if (songData.variation[i].parent == pattern)
        {
            return true;
        }
    }
    return false;
}

void TwistedSong::link(uint8_t child, uint8_t parent)
{
    if (child == parent || child == 0)
    {
        return;
    }

    // never allow a loop (parent chain reaching the child)
    uint8_t p = parent;
    for (int guard = 0; p != 0 && guard <= PATTERN_INDEX_MAX; guard++)
    {
        if (p == child)
        {
            return;
        }
        p = songData.variation[p].parent;
    }

    songData.variation[child].parent = parent;
    songData.variation[child].ownMask = 0;
    save();
}

void TwistedSong::unlink(uint8_t child)
{
    if (songData.variation[child].parent == kNoParent)
    {
        return;
    }
    songData.variation[child].parent = kNoParent;
    songData.variation[child].ownMask = 0;
    save();
}

void TwistedSong::addOwnMask(uint8_t pattern, uint32_t mask)
{
    uint32_t before = songData.variation[pattern].ownMask;
    songData.variation[pattern].ownMask |= mask;
    if (before != songData.variation[pattern].ownMask)
    {
        save();
    }
}

void TwistedSong::forgetPattern(uint8_t pattern)
{
    bool changed = false;

    if (songData.variation[pattern].parent != kNoParent)
    {
        songData.variation[pattern].parent = kNoParent;
        songData.variation[pattern].ownMask = 0;
        changed = true;
    }

    // children of a forgotten pattern keep their current notes as their own
    for (int i = PATTERN_INDEX_MIN; i <= PATTERN_INDEX_MAX; i++)
    {
        if (songData.variation[i].parent == pattern)
        {
            songData.variation[i].parent = kNoParent;
            songData.variation[i].ownMask = 0;
            changed = true;
        }
    }

    if (changed)
    {
        save();
    }
}
