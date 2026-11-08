#include "fileManagerDefs.h"
#include "config.h"
#include "mtStructs.h"

#include "core/interfacePopups.h"

#include "fileManager.h"

// uint8_t patternToLoad = 0;

//__NOINIT(EXTERNAL_RAM) uint8_t undo_Bank[1024*1024];
__NOINIT(EXTERNAL_RAM) Sequencer::strPattern songTrackCopy[2];

void setPatternBitmask(int patternIdx, int trackIdx, bool value);

//------------------------------------------------------------------------------------------
// Twisted song mode: loop ranges (section / selection) and queued jumps.
// These are read from the sequencer interrupt, so they're kept as plain volatile values.
//------------------------------------------------------------------------------------------
static volatile int16_t songLoopStart = -1;
static volatile int16_t songLoopEnd = -1;
static volatile int16_t songQueuedJump = -1;
static volatile int16_t songLatchedNext = -1; // next position chosen when the next pattern was preloaded

static inline bool isSongPosValid(strSong* song, int16_t pos)
{
    return pos >= 0 && pos < SONG_MAX && song->playlist[pos] != PLAYLIST_EMPTY_SLOT;
}

AUDIO_CORE_RAM static int16_t computeNextSongPos(bool* songEnded)
{
    strSong* song = &mtProject.song;
    int16_t pos = song->playlistPos;
    *songEnded = false;

    // exporting the song: always play it straight through once
    bool rendering = sequencer.isRenderMode();

    int16_t queued = rendering ? -1 : songQueuedJump;
    if (isSongPosValid(song, queued))
    {
        return queued;
    }

    int16_t loopStart = rendering ? -1 : songLoopStart;
    int16_t loopEnd = rendering ? -1 : songLoopEnd;
    bool loopActive = isSongPosValid(song, loopStart) && isSongPosValid(song, loopEnd) &&
                      loopStart <= loopEnd;

    if (loopActive && pos == loopEnd)
    {
        return loopStart;
    }

    if (pos + 1 < SONG_MAX && song->playlist[pos + 1] != PLAYLIST_EMPTY_SLOT)
    {
        return pos + 1;
    }

    if (loopActive)
    {
        return loopStart;
    }

    *songEnded = true;
    return 0;
}

AUDIO_CORE_RAM uint8_t cFileManager::getNextSongPattern()
{
    strSong* song = &mtProject.song;
    bool songEnded;
    int16_t next = computeNextSongPos(&songEnded);
    songLatchedNext = next;

    return song->playlist[next] > 0 ? song->playlist[next] : song->playlist[0];
}
uint8_t cFileManager::getSongPattern(uint8_t pos)
{
    strSong* song = &mtProject.song;
    return song->playlist[pos];
}

std::optional<int> cFileManager::getPatternPosInSong(uint8_t patternToSearch)
{
    strSong* song = &mtProject.song;
    int startFromPos = song->playlistPos;

    for (int tryIdx = 0; tryIdx < SONG_MAX; tryIdx++)
    {
        int songPosition = (startFromPos + tryIdx) % SONG_MAX;
        if (song->playlist[songPosition] == patternToSearch)
        {
            return songPosition;
        }
    }

    return std::nullopt;
}

void cFileManager::setSongPos(uint8_t pos)
{
    strSong* song = &mtProject.song;
    song->playlistPos = pos;
    songLatchedNext = -1;
}

bool cFileManager::switchNextPatternInSong()
{
    strSong* song = &mtProject.song;
    bool songEnded = false;
    int16_t next = songLatchedNext;

    if (isSongPosValid(song, next))
    {
        // the pattern for this position is already loaded - stay in step with it
        int16_t pos = song->playlistPos;
        bool rendering = sequencer.isRenderMode();
        songEnded = (rendering || (songQueuedJump < 0 && songLoopStart < 0)) && next == 0 &&
                    !(pos + 1 < SONG_MAX && song->playlist[pos + 1] != PLAYLIST_EMPTY_SLOT);
    }
    else
    {
        next = computeNextSongPos(&songEnded);
    }

    songLatchedNext = -1;

    if (songQueuedJump == next)
    {
        songQueuedJump = -1;
    }

    song->playlistPos = (uint8_t)next;
    return !songEnded;
}

void cFileManager::setSongLoop(int16_t start, int16_t end)
{
    if (start < 0 || end < 0 || start >= SONG_MAX || end >= SONG_MAX || start > end)
    {
        start = -1;
        end = -1;
    }

    DISABLE_GLOBAL_INTERRUPTS();
    songLoopStart = start;
    songLoopEnd = end;
    ENABLE_GLOBAL_INTERRUPTS();
}

int16_t cFileManager::getSongLoopStart()
{
    return songLoopStart;
}

int16_t cFileManager::getSongLoopEnd()
{
    return songLoopEnd;
}

void cFileManager::queueSongJump(int16_t pos)
{
    if (pos >= SONG_MAX)
    {
        pos = -1;
    }
    songQueuedJump = pos;
}

int16_t cFileManager::getQueuedSongJump()
{
    return songQueuedJump;
}

void cFileManager::copySongTracks(char* currentProjectPath,
                                  uint8_t srcPatternIdx,
                                  uint8_t destPaternIdx,
                                  uint8_t trackStartSrc,
                                  uint8_t trackStartDest,
                                  uint8_t tracksNum)
{
    bool status = false;
    char currentPath[PATCH_SIZE];
    Sequencer::strPattern* srcPattern = &songTrackCopy[0];
    Sequencer::strPattern* destPattern = &songTrackCopy[1];

    snprintf(currentPath,
             sizeof(currentPath),
             "%s/patterns/pattern_%02d.mtp",
             currentProjectPath,
             srcPatternIdx);
    status = readPatternFile(currentPath, srcPattern);

    if (!status)
    {
        return;
    }

    newFileManager.storeSongUndoRevision(destPaternIdx);

    snprintf(currentPath,
             sizeof(currentPath),
             "%s/patterns/pattern_%02d.mtp",
             currentProjectPath,
             destPaternIdx);
    status = readPatternFile(currentPath, destPattern);
    if (status == false)
    {
        sequencer.clearPattern(destPattern);
    }

    while ((trackStartSrc + tracksNum) > kAllTracks || (trackStartDest + tracksNum) > kAllTracks)
    {
        tracksNum--;

        if (tracksNum == 0)
        {
            return;
        }
    }

    for (int trackIdx = 0; trackIdx < tracksNum; trackIdx++)
    {
        if (&destPattern->track[trackStartDest + trackIdx] ==
            &srcPattern->track[trackStartSrc + trackIdx]) // memcpy restrict protection
        {
            continue;
        }

        int srcTrackIdx = trackStartSrc + trackIdx;
        int destTrackIdx = trackStartDest + trackIdx;
        Sequencer::strPattern::strTrack* srcTrack = &srcPattern->track[srcTrackIdx];
        Sequencer::strPattern::strTrack* destTrack = &destPattern->track[destTrackIdx];

        for (int stepIdx = 0; stepIdx <= Sequencer::MAXSTEP; stepIdx++)
        {
            sequencer.copyStep(
              &srcTrack->step[stepIdx], &destTrack->step[stepIdx], !isSampleTrack(destTrackIdx));
        }
    }

    uint8_t destBitmaskIdx = destPaternIdx - 1;
    bool isDestPatternEmpty =
      !status || (mtProject.values.allPatternsBitmask[destBitmaskIdx] == 0 &&
                  mtProject.values.allPatternsBitmaskMidi[destBitmaskIdx] == 0);

    if (tracksNum == kAllTracks || isDestPatternEmpty)
    {
        destPattern->track[0].length = srcPattern->track[0].length;
    }
    else
    {
        destPattern->track[0].length =
          std::max(srcPattern->track[0].length, destPattern->track[0].length);
    }

    sequencer.updateTrackLengths(destPattern);

    if (updatePatternBitmask(destPaternIdx, destPattern))
    {
        setProjectStructChanged();
    }

    writePatternFile(currentPath, destPattern);
    onPatternFileWritten(destPaternIdx);
}

void cFileManager::deleteTracks(char* currentProjectPath,
                                uint8_t srcPatternIdx,
                                uint8_t trackStartSrc,
                                uint8_t tracksNum)
{
    bool status = false;
    char currentPath[PATCH_SIZE];
    Sequencer::strPattern* srcPattern = &songTrackCopy[0];

    snprintf(currentPath,
             sizeof(currentPath),
             "%s/patterns/pattern_%02d.mtp",
             currentProjectPath,
             srcPatternIdx);
    status = readPatternFile(currentPath, srcPattern);

    if (status)
    {
        storeSongUndoRevision(srcPatternIdx);

        for (uint8_t track = 0; track < tracksNum; track++)
        {
            sequencer.clearSingleTrack(&srcPattern->track[track + trackStartSrc]);
        }

        if (updatePatternBitmask(srcPatternIdx, srcPattern))
        {
            setProjectStructChanged();
        }

        writePatternFile(currentPath, srcPattern);
        onPatternFileWritten(srcPatternIdx);
    }
}

void cFileManager::updateActualPatternBitmask()
{
    assert(mtProject.values.actualPattern > 0);

    if (updatePatternBitmask(mtProject.values.actualPattern, sequencer.getPatternToSaveToFile()))
    {
        setProjectStructChanged();
    }
}

bool cFileManager::updatePatternBitmask(uint8_t patternIdx, Sequencer::strPattern* sourcePattern)
{
    assert(patternIdx > 0);

    uint8_t bitmaskIdx = patternIdx - 1;

    uint16_t dataBefore = mtProject.values.allPatternsBitmask[bitmaskIdx] |
                          (mtProject.values.allPatternsBitmaskMidi[bitmaskIdx] << 8);

    uint8_t length = sourcePattern->track[0].length;

    for (uint8_t trackNum = 0; trackNum < kAllTracks; trackNum++)
    {
        bool isAnyActive = false;

        for (uint8_t stepIdx = 0; stepIdx <= length; stepIdx++)
        {
            Sequencer::strPattern::strTrack::strStep* step =
              &((Sequencer::strPattern*)sourcePattern)->track[trackNum].step[stepIdx];

            if (step->note != Sequencer::STEP_NOTE_EMPTY || step->hasAnyFx() ||
                step->instrument >= MIDI_INSTRUMENTS_START_IDX)
            {
                isAnyActive = true;
                break;
            }
        }

        setPatternBitmask(bitmaskIdx, trackNum, isAnyActive);
    }

    uint16_t dataAfter = mtProject.values.allPatternsBitmask[bitmaskIdx] |
                         (mtProject.values.allPatternsBitmaskMidi[bitmaskIdx] << 8);

    if (dataBefore == dataAfter)
    {
        return false;
    }

    return true;
}

inline void setPatternBitmask(int bitmaskIdx, int trackIdx, bool value)
{
    assert(bitmaskIdx <= PATTERN_INDEX_MAX);
    assert(trackIdx >= 0);

    if (trackIdx < kSampleTracks)
    {
        if (value)
        {
            mtProject.values.allPatternsBitmask[bitmaskIdx] |= (1 << trackIdx);
        }
        else
        {
            mtProject.values.allPatternsBitmask[bitmaskIdx] &= ~(1 << trackIdx);
        }
    }
    else if (trackIdx < kSampleTracks + kMidiTracks)
    {
        trackIdx = trackIdx - kSampleTracks;

        if (value)
        {
            mtProject.values.allPatternsBitmaskMidi[bitmaskIdx] |= (1 << trackIdx);
        }
        else
        {
            mtProject.values.allPatternsBitmaskMidi[bitmaskIdx] &= ~(1 << trackIdx);
        }
    }
}
