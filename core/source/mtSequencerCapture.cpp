// Twisted: CAPTURE, like Ableton's Capture MIDI (and Paula's Revenge's CAPTURE). The Tracker always listens to the
// notes you play on the pads or a MIDI keyboard when it isn't recording them itself, and keeps the last 1,024.
// Capture writes them into the pattern, quantised to the steps, as one undo step:
//  - played while the pattern was playing: each note on the step where you played it (nearest step), taking the
//    last time round the pattern that had notes in it, so a loop never piles up on itself;
//  - played while stopped: from the cursor at the tempo, the first note on the cursor's step (the notes since your
//    last pause of more than 4 seconds).
// Notes go on the cursor's track with the current instrument; notes played together (a chord) spread over the next
// free tracks of the same kind. It never writes over a note. A note-off goes where you let go, unless the next note
// on that track ends it first. After a capture it starts listening afresh.

#include "mtSequencer.h"

#include "Platform.h"
#include "Tracker.h"
#include "fileManager.h"
#include "mtStructs.h"
#include "patternEditor/patternEditor.h"
#include "util_math.h"

#include <algorithm>
#include <cr_section_macros.h>
#include <cstring>

namespace
{
struct CapEvent
{
    uint32_t ms;
    uint16_t pass;
    uint8_t step;
    uint8_t note;
    uint8_t velocity;
    uint8_t pattern;
    uint8_t on : 1;
    uint8_t playing : 1;
};
static_assert(sizeof(CapEvent) <= 12, "capture events stay small");

struct Placed
{
    int16_t step;
    int16_t offStep; // -1: none
    uint8_t note, velocity;
};
struct Written
{
    int16_t step, offStep;
    uint8_t track;
};

constexpr int kCapEvents = 1024;
constexpr uint32_t kPauseMs = 4000;

__NOINIT(EXTERNAL_RAM) CapEvent capLog[kCapEvents];
__NOINIT(EXTERNAL_RAM) CapEvent ev[kCapEvents];
__NOINIT(EXTERNAL_RAM) Placed notes[kCapEvents];
__NOINIT(EXTERNAL_RAM) Written written[kCapEvents];
volatile uint32_t capHead = 0; // events written (the ring holds the last kCapEvents)
volatile uint16_t capPass = 0;
uint32_t capLastPos = 0; // the poll's last position (steps x 48 + micro step)
bool capWasPlaying = false;

uint32_t capNowMs()
{
    return Tracker::getInstance()->getPlatform()->getTimer()->millis();
}
} // namespace

void Sequencer::twCaptureLog(bool on, uint8_t note, int8_t velocity, uint8_t channel)
{
    // notes the Tracker writes in itself (live recording, step entry) aren't captured again
    if (isRec())
        return;
    if (isEditMode() && channel != GRID_OUTSIDE_PATTERN)
        return;

    CapEvent e{};
    e.ms = capNowMs();
    e.note = note;
    e.velocity = static_cast<uint8_t>(velocity < 0 ? 127 : velocity);
    e.on = on ? 1 : 0;
    e.playing = isPlay() ? 1 : 0;
    e.pattern = mtProject.values.actualPattern;
    e.pass = capPass;
    if (e.playing)
    {
        uint8_t track = selection.firstTrack < 16 ? selection.firstTrack : 0;
        e.step = getDestRecordingStep(track, true); // the nearest step
    }
    const uint32_t h = capHead;
    capLog[h % kCapEvents] = e;
    capHead = h + 1;
}

// main loop: a new pass each time playback goes back round the pattern (or starts)
void Sequencer::twCapturePoll()
{
    const bool playing = isPlay();
    if (!playing)
    {
        capWasPlaying = false;
        return;
    }
    uint8_t track = selection.firstTrack < 16 ? selection.firstTrack : 0;
    const uint32_t pos = static_cast<uint32_t>(ptrPlayer->track[track].actual_pos) * 48u + ptrPlayer->track[track].uStep;
    if (!capWasPlaying || pos + 48u < capLastPos) // started, or wrapped back to the start
        capPass = static_cast<uint16_t>(capPass + 1);
    capWasPlaying = true;
    capLastPos = pos;
}

Sequencer::CaptureResult Sequencer::twCapture(uint8_t cursorTrack, uint8_t cursorStep)
{
    CaptureResult r;
    if (cursorTrack >= 16)
        cursorTrack = 0;

    // the events, oldest first
    const uint32_t head = capHead;
    const uint32_t count = head < static_cast<uint32_t>(kCapEvents) ? head : static_cast<uint32_t>(kCapEvents);
    for (uint32_t i = 0; i < count; i++)
        ev[i] = capLog[(head - count + i) % kCapEvents];

    // the last note played decides: over the playing pattern, or while stopped
    int last = -1;
    for (int i = static_cast<int>(count) - 1; i >= 0; i--)
        if (ev[i].on)
        {
            last = i;
            break;
        }
    if (last < 0)
    {
        r.nothing = true;
        return r;
    }

    int n = 0;

    strPattern* pattern = currentPattern;
    const int length = pattern->track[cursorTrack].length + 1;

    // the note-off for event i: the next release of that note after it
    auto releaseOf = [&](int i) -> int
    {
        for (uint32_t j = static_cast<uint32_t>(i) + 1; j < count; j++)
            if (!ev[j].on && ev[j].note == ev[i].note)
                return static_cast<int>(j);
        return -1;
    };

    if (ev[last].playing)
    {
        const uint16_t pass = ev[last].pass;
        const uint8_t pat = ev[last].pattern;
        if (pat != mtProject.values.actualPattern)
        {
            r.otherPattern = true;
            r.pattern = pat;
            return r;
        }
        for (uint32_t i = 0; i < count; i++)
        {
            if (!ev[i].on || !ev[i].playing || ev[i].pass != pass || ev[i].pattern != pat)
                continue;
            Placed p{ static_cast<int16_t>(ev[i].step), -1, ev[i].note, ev[i].velocity };
            const int j = releaseOf(static_cast<int>(i));
            if (j >= 0 && ev[j].playing)
            {
                if (ev[j].pass == pass && ev[j].step > ev[i].step)
                    p.offStep = ev[j].step; // let go in the same time round
                else if (ev[j].pass != pass && ev[j].step < ev[i].step)
                    p.offStep = ev[j].step; // held over the loop: the OFF early in the pattern, as played
            }
            notes[n++] = p;
        }
    }
    else
    {
        // stopped: the notes since the last pause, timed from the first at the tempo
        int first = last;
        while (first > 0)
        {
            const int prev = first - 1;
            if (ev[prev].playing || ev[first].ms - ev[prev].ms > kPauseMs)
                break;
            first = prev;
        }
        while (first < last && !ev[first].on)
            first++;
        const float stepMs = getStepPeriodMs();
        const uint32_t t0 = ev[first].ms;
        for (int i = first; i <= static_cast<int>(count) - 1; i++)
        {
            if (!ev[i].on || ev[i].playing)
                continue;
            const int step = cursorStep + static_cast<int>((ev[i].ms - t0) / stepMs + 0.5f);
            Placed p{ static_cast<int16_t>(std::min(step, 32767)), -1, ev[i].note, ev[i].velocity };
            const int j = releaseOf(i);
            if (j >= 0 && !ev[j].playing)
            {
                const int off = cursorStep + static_cast<int>((ev[j].ms - t0) / stepMs + 0.5f);
                if (off > step)
                    p.offStep = static_cast<int16_t>(std::min(off, 32767));
            }
            notes[n++] = p;
        }
    }

    if (n == 0)
    {
        r.nothing = true;
        return r;
    }

    std::stable_sort(notes, notes + n, [](const Placed& a, const Placed& b) { return a.step < b.step; });

    newFileManager.storePatternUndoRevision();

    // tracks of the cursor's kind (sample tracks, or MIDI / synth tracks)
    const bool sampleKind = isSampleTrack(cursorTrack);
    uint8_t tracks[16];
    int trackCount = 0;
    for (int t = cursorTrack; t < 16; t++)
        if (isSampleTrack(t) == sampleKind)
            tracks[trackCount++] = static_cast<uint8_t>(t);

    int w = 0;

    for (int k = 0; k < n; k++)
    {
        const Placed& p = notes[k];
        if (p.step < 0 || p.step >= length)
        {
            r.dropped++;
            continue;
        }
        bool placed = false;
        for (int ti = 0; ti < trackCount && !placed; ti++)
        {
            const uint8_t t = tracks[ti];
            if (p.step >= pattern->track[t].length + 1)
                continue;
            auto* step = &pattern->track[t].step[p.step];
            if (!step->isNoteEmpty() && !step->isNoteOff())
                continue; // never over a note
            if (step->isNoteOff())
                continue;
            // a note already captured on this step (a chord): the next track. A note still sounding from an
            // earlier step is fine: the new one ends it (legato)
            bool sameStep = false;
            for (int q = 0; q < w; q++)
                if (written[q].track == t && written[q].step == p.step)
                    sameStep = true;
            if (sameStep)
                continue;
            step->note = static_cast<int8_t>(p.note);
            step->instrument =
              changeInstrumentInSpecificOrder(mtProject.values.lastUsedInstrument, 0, !isSampleTrack(t));
            if (recVelocity())
            {
                const int v =
                  mapClamped<int>(p.velocity, 0, 127, 0, getInstrumentVelo(step->instrument));
                step->volume = velocityToVolume(static_cast<int8_t>(v));
            }
            written[w++] = { p.step, p.offStep, t };
            placed = true;
            r.written++;
        }
        if (!placed)
            r.skipped++;
    }

    // the note-offs: where you let go, unless the track's next note comes first (legato) or the step isn't free
    for (int q = 0; q < w; q++)
    {
        const Written& x = written[q];
        if (x.offStep < 0 || x.offStep >= pattern->track[x.track].length + 1)
            continue;
        bool nextFirst = false;
        for (int q2 = 0; q2 < w; q2++)
        {
            if (q2 == q || written[q2].track != x.track)
                continue;
            const int s = written[q2].step;
            const bool between =
              x.offStep > x.step ? (s > x.step && s <= x.offStep) : (s > x.step || s <= x.offStep);
            if (between)
                nextFirst = true;
        }
        // a note already in the pattern between them ends this one too: no OFF (it would cut that note)
        const int len = pattern->track[x.track].length + 1;
        for (int s = (x.step + 1) % len; s != x.offStep && !nextFirst; s = (s + 1) % len)
        {
            auto& st = pattern->track[x.track].step[s];
            if (!st.isNoteEmpty() || st.isNoteOff())
                nextFirst = true;
        }
        if (nextFirst)
            continue;
        auto* step = &pattern->track[x.track].step[x.offStep];
        if (step->isNoteEmpty() && step->note == STEP_NOTE_EMPTY)
            step->note = STEP_NOTE_OFF_DEFAULT;
    }

    newFileManager.setActualPatternStructChanged();

    // start listening afresh
    capHead = 0;
    return r;
}
