#ifndef FILEMANAGER_TWISTEDSONG_H_
#define FILEMANAGER_TWISTEDSONG_H_

// Twisted song extras (Paula's Revenge style song mode):
//  - named song sections (dividers in the song list)
//  - linked pattern variations (untouched tracks follow the parent pattern)
//
// Everything lives in a side file next to the patterns
// (Workspace/patterns/twistedSong, <project>/patterns/twistedSong), so stock
// firmware ignores it and projects stay fully compatible.

#include "mtStructs.h"

#include <stdint.h>

class FileSystem;

class TwistedSong
{
  public:
    static constexpr uint8_t kMaxSections = 32;
    static constexpr uint8_t kSectionNameLength = 16;
    static constexpr uint8_t kNoParent = 0;
    // bit 31 of the own mask: the variation has its own pattern length
    static constexpr uint32_t kOwnLengthBit = 1u << 31;

    void init(FileSystem* fileSystem);
    void load();                                      // Workspace -> RAM (or reset)
    bool copyFile(const char* src, const char* dest); // project open / save

    //------------------------------------------------ sections
    uint8_t getSectionCount();
    // section that starts exactly at song position pos, -1 if none
    int findSectionStartingAt(uint8_t pos);
    const char* getSectionName(uint8_t sectionIdx);
    uint8_t getSectionStart(uint8_t sectionIdx);
    // name shown for the section containing pos ("" before the first divider)
    const char* getSectionNameForPos(uint8_t pos);
    // first and last song position of the section containing pos
    void getSectionRange(uint8_t pos, uint8_t songLength, uint8_t* start, uint8_t* end);
    bool setSection(uint8_t pos, const char* name); // add or rename
    void removeSection(uint8_t pos);
    void onSlotInserted(uint8_t newPos);
    void onSlotDeleted(uint8_t deletedPos, uint8_t newSongLength);

    //------------------------------------------------ linked variations
    uint8_t getParent(uint8_t pattern);
    uint32_t getOwnMask(uint8_t pattern);
    bool hasChildren(uint8_t pattern);
    void link(uint8_t child, uint8_t parent);
    void unlink(uint8_t child);
    void addOwnMask(uint8_t pattern, uint32_t mask);
    // a pattern stopped existing / was overwritten as a plain pattern
    void forgetPattern(uint8_t pattern);

  private:
    bool readFile(const char* path);
    void writeFile(const char* path);
    void save();
    void reset();
    void sortSections();

    FileSystem* fileSystem = nullptr;
};

#endif /* FILEMANAGER_TWISTEDSONG_H_ */
