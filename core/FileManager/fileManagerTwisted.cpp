// Twisted song mode - linked pattern variations.
//
// A variation is a pattern linked to a parent pattern. Tracks the variation hasn't
// touched follow the parent: whenever the parent is written, those tracks are copied
// into the variation. A track becomes the variation's own the first time the
// variation is saved with that track different from the parent.

#include "config.h"
#include "fileManager.h"
#include "fileManagerDefs.h"
#include "mtSequencer.h"
#include "mtStructs.h"

#include <cr_section_macros.h>
#include <string.h>

namespace
{
constexpr uint8_t kDirtyEdited = 0x01; // written by the user / an edit: check ownership
constexpr uint8_t kDirtySynced = 0x02; // written by a parent sync: only pass it on

uint8_t variationDirty[PATTERN_INDEX_MAX + 1];

}

// shares the song screen's track copy buffers (fileManagerSong.cpp) - both only run from the
// main loop, never at the same time, and external RAM has no room for two more patterns
extern Sequencer::strPattern songTrackCopy[2];

namespace
{
Sequencer::strPattern& variationParentBuffer = songTrackCopy[0];
Sequencer::strPattern& variationChildBuffer = songTrackCopy[1];

bool tracksEqual(Sequencer::strPattern::strTrack* a, Sequencer::strPattern::strTrack* b)
{
    return memcmp(a->step, b->step, sizeof(a->step)) == 0;
}
} // namespace

int cFileManager::findFreePatternIdx()
{
    for (int idx = PATTERN_INDEX_MIN; idx <= PATTERN_INDEX_MAX; idx++)
    {
        uint8_t bitmaskIdx = idx - 1;
        if (mtProject.values.allPatternsBitmask[bitmaskIdx] != 0 ||
            mtProject.values.allPatternsBitmaskMidi[bitmaskIdx] != 0)
        {
            continue;
        }

        if (idx == mtProject.values.actualPattern)
        {
            continue;
        }

        bool usedInSong = false;
        for (int pos = 0; pos < SONG_MAX && mtProject.song.playlist[pos] != PLAYLIST_EMPTY_SLOT;
             pos++)
        {
            if (mtProject.song.playlist[pos] == idx)
            {
                usedInSong = true;
                break;
            }
        }

        if (usedInSong || twistedSong.getParent(idx) != TwistedSong::kNoParent ||
            twistedSong.hasChildren(idx))
        {
            continue;
        }

        return idx;
    }

    return -1;
}

bool cFileManager::clonePatternFile(uint8_t srcPatternIdx, uint8_t destPatternIdx)
{
    if (srcPatternIdx == 0 || destPatternIdx == 0 || srcPatternIdx == destPatternIdx)
    {
        return false;
    }

    // make sure the copy has the latest edits of the pattern on screen
    if (srcPatternIdx == mtProject.values.actualPattern)
    {
        saveWorkspacePatternNow(srcPatternIdx);
    }

    char path[PATCH_SIZE];
    snprintf(path, PATCH_SIZE, cWorkspacePatternFileFormat, srcPatternIdx);
    if (!readPatternFile(path, &variationParentBuffer))
    {
        sequencer.clearPattern(&variationParentBuffer);
    }

    twistedSong.forgetPattern(destPatternIdx);

    snprintf(path, PATCH_SIZE, cWorkspacePatternFileFormat, destPatternIdx);
    variationSyncWriting = true;
    bool status = writePatternFile(path, &variationParentBuffer);
    variationSyncWriting = false;

    if (updatePatternBitmask(destPatternIdx, &variationParentBuffer))
    {
        setProjectStructChanged();
    }

    // copy the name too, so the song list stays readable
    char* srcName = patternsMetadata.getPatternName(srcPatternIdx);
    char name[PATTERN_NAME_LENGTH + 1];
    if (srcName != nullptr && srcName[0] != 0)
    {
        strncpy(name, srcName, PATTERN_NAME_LENGTH);
        name[PATTERN_NAME_LENGTH] = 0;
    }
    else
    {
        snprintf(name, sizeof(name), "Pattern %d", srcPatternIdx);
    }
    patternsMetadata.setPatternName(destPatternIdx, name);

    mtProject.values.projectNotSavedFlag |= 1;
    changesFlags.project = 1;

    return status;
}

void cFileManager::onPatternFileWritten(uint8_t patternIdx)
{
    if (patternIdx == 0 || patternIdx > PATTERN_INDEX_MAX)
    {
        return;
    }

    variationDirty[patternIdx] |= variationSyncWriting ? kDirtySynced : kDirtyEdited;
}

// a variation was written by the user: tracks that now differ from the parent become its own
void cFileManager::syncVariationEdits(uint8_t child)
{
    uint8_t parent = twistedSong.getParent(child);
    if (parent == TwistedSong::kNoParent)
    {
        return;
    }

    char path[PATCH_SIZE];
    snprintf(path, PATCH_SIZE, cWorkspacePatternFileFormat, parent);
    if (!readPatternFile(path, &variationParentBuffer))
    {
        sequencer.clearPattern(&variationParentBuffer);
    }

    snprintf(path, PATCH_SIZE, cWorkspacePatternFileFormat, child);
    if (!readPatternFile(path, &variationChildBuffer))
    {
        return;
    }

    uint32_t own = 0;
    for (int track = 0; track < kAllTracks; track++)
    {
        if (!tracksEqual(&variationParentBuffer.track[track], &variationChildBuffer.track[track]))
        {
            own |= (1u << track);
        }
    }

    if (variationParentBuffer.track[0].length != variationChildBuffer.track[0].length)
    {
        own |= TwistedSong::kOwnLengthBit;
    }

    twistedSong.addOwnMask(child, own);
}

// a parent was written: copy its tracks into every variation that hasn't made them its own
void cFileManager::syncVariationChildren(uint8_t parent)
{
    if (!twistedSong.hasChildren(parent))
    {
        return;
    }

    char path[PATCH_SIZE];
    snprintf(path, PATCH_SIZE, cWorkspacePatternFileFormat, parent);
    if (!readPatternFile(path, &variationParentBuffer))
    {
        return;
    }

    for (int child = PATTERN_INDEX_MIN; child <= PATTERN_INDEX_MAX; child++)
    {
        if (twistedSong.getParent(child) != parent)
        {
            continue;
        }

        uint32_t own = twistedSong.getOwnMask(child);
        bool isOnScreen = (child == mtProject.values.actualPattern);

        snprintf(path, PATCH_SIZE, cWorkspacePatternFileFormat, child);
        if (!readPatternFile(path, &variationChildBuffer))
        {
            sequencer.clearPattern(&variationChildBuffer);
        }

        bool changed = false;
        for (int track = 0; track < kAllTracks; track++)
        {
            if (own & (1u << track))
            {
                continue;
            }
            if (!tracksEqual(&variationParentBuffer.track[track],
                             &variationChildBuffer.track[track]))
            {
                memcpy(variationChildBuffer.track[track].step,
                       variationParentBuffer.track[track].step,
                       sizeof(variationChildBuffer.track[track].step));
                changed = true;
            }
        }

        if (!(own & TwistedSong::kOwnLengthBit) &&
            variationChildBuffer.track[0].length != variationParentBuffer.track[0].length)
        {
            variationChildBuffer.track[0].length = variationParentBuffer.track[0].length;
            sequencer.updateTrackLengths(&variationChildBuffer);
            changed = true;
        }

        if (!changed)
        {
            continue;
        }

        writePatternFile(path, &variationChildBuffer);
        // a variation can itself be the parent of another variation
        variationDirty[child] |= kDirtySynced;

        if (updatePatternBitmask(child, &variationChildBuffer))
        {
            setProjectStructChanged();
        }

        // the variation is the pattern in memory right now: update it there as well
        if (isOnScreen)
        {
            Sequencer::strPattern* ramPattern = sequencer.getActualPattern();
            for (int track = 0; track < kAllTracks; track++)
            {
                if (own & (1u << track))
                {
                    continue;
                }
                DISABLE_GLOBAL_INTERRUPTS();
                memcpy(ramPattern->track[track].step,
                       variationChildBuffer.track[track].step,
                       sizeof(ramPattern->track[track].step));
                ENABLE_GLOBAL_INTERRUPTS();
            }
            if (!(own & TwistedSong::kOwnLengthBit))
            {
                DISABLE_GLOBAL_INTERRUPTS();
                ramPattern->track[0].length = variationChildBuffer.track[0].length;
                sequencer.updateTrackLengths(ramPattern);
                ENABLE_GLOBAL_INTERRUPTS();
            }
        }

        mtProject.values.projectNotSavedFlag |= 1;
        changesFlags.project = 1;
    }
}

// called from the file manager when it has nothing else to do
void cFileManager::serviceLinkedVariations()
{
    if (isPatternLoading())
    {
        return;
    }

    bool anyDirty = false;
    for (int idx = PATTERN_INDEX_MIN; idx <= PATTERN_INDEX_MAX; idx++)
    {
        if (variationDirty[idx])
        {
            anyDirty = true;
            break;
        }
    }

    if (!anyDirty)
    {
        return;
    }

    // unsaved edits in memory on a variation whose parent is about to sync: save them first,
    // so they count as the variation's own and aren't overwritten
    uint8_t actual = mtProject.values.actualPattern;
    uint8_t actualParent = twistedSong.getParent(actual);
    if (actualParent != TwistedSong::kNoParent && variationDirty[actualParent] &&
        changesFlags.pattern[actual])
    {
        saveWorkspacePatternNow(actual);
    }

    // 1. ownership first (edits on variations)
    for (int idx = PATTERN_INDEX_MIN; idx <= PATTERN_INDEX_MAX; idx++)
    {
        if (variationDirty[idx] & kDirtyEdited)
        {
            syncVariationEdits(idx);
        }
    }

    // 2. then pass parents on to their variations (a synced variation can be a parent too)
    for (int pass = 0; pass < 8; pass++)
    {
        bool any = false;
        for (int idx = PATTERN_INDEX_MIN; idx <= PATTERN_INDEX_MAX; idx++)
        {
            if (variationDirty[idx] == 0)
            {
                continue;
            }
            variationDirty[idx] = 0;
            any = true;
            syncVariationChildren(idx);
        }
        if (!any)
        {
            break;
        }
    }
}
