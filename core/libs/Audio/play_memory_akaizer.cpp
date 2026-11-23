// Akaizer play mode - a live version of the Akaizer v1.2 timestretch.
//
// The original (and the Sample Editor's Akaizer effect) lays half-sine grains of 2 x cycle
// length every C = sample rate / cycle frequency samples of output, and moves the grain's
// source start by C / stretch each time, so the result is stretch x the original length.
// Pitch works like the offline "repitch, then stretch back to length": each grain reads
// the sample at the note's pitch, the grain spacing in the source stays C / stretch.
//
// Sizes (grain spacing, hop rounding, output length) follow the original exactly; checked
// on the desktop against the offline core, the live result is within ~50-60 dB of it.
// Live differences from the offline effect: the output isn't clipped grain by grain
// (the sum is saturated once), and at most kAkaizerMaxGrains grains overlap - very dense
// settings (long cycles with a high cycle frequency) keep only the newest grains.

#include "Audio.h"
#include "play_memory.h"
#include "utility/dspinst.h"
#include "akaizerCore.h"

#include <math.h>

void AudioPlayMemory::setAkaizerStretchOverride(uint16_t percent)
{
    akaizerStretchOverride = percent;
}

void AudioPlayMemory::setAkaizerCycleOverride(uint16_t samples)
{
    akaizerCycleOverride = samples;
}

void AudioPlayMemory::clearAkaizerOverrides()
{
    akaizerStretchOverride = 0;
    akaizerCycleOverride = 0;
}

// stretch, cycle length and frequency can change while a note plays (screen, FX commands)
AUDIO_CORE_RAM void AudioPlayMemory::refreshAkaizerParams()
{
    const strInstrument& instr = mtProject.instrument[currentInstrIdx];

    float factor;
    if (akaizerStretchOverride != 0)
    {
        factor = (float)akaizerStretchOverride / 100.0f;
    }
    else if (akaizerSettings::loopBpm(instr) != 0 && mtProject.values.globalTempo > 0.0f)
    {
        // a loop at loop BPM, fitted to the project tempo
        factor = (float)akaizerSettings::loopBpm(instr) / mtProject.values.globalTempo;
    }
    else
    {
        factor = (float)akaizerSettings::stretchPercent(instr) / 100.0f;
    }

    uint16_t cycle = akaizerCycleOverride != 0
                       ? akaizerSettings::clampU16(akaizerCycleOverride,
                                                   akaizerSettings::kMinCycle,
                                                   akaizerSettings::kMaxCycle)
                       : akaizerSettings::cycleLength(instr);
    uint16_t freq = akaizerSettings::cycleFrequency(instr);
    if (freq == 0)
    {
        freq = (uint16_t)akaizer::linkedFrequency(cycle); // 41000 / cycle length
    }

    // the original's sizes, worked out the same way (see akaizerCore.h), with the
    // note's pitch as the repitch ratio
    const double r = pitchControl > 0.0f ? (double)pitchControl : 1.0;
    int64_t C = akaizer::roundHalfEven((double)AUDIO_SAMPLE_RATE_EXACT / (double)freq);
    if (C < 1)
    {
        C = 1;
    }
    int64_t pitchedFrames = (int64_t)llround((double)akaizerRegion / r);
    if (pitchedFrames < 1)
    {
        pitchedFrames = 1;
    }
    const double pct = (double)factor * 100.0 * ((double)akaizerRegion / (double)pitchedFrames);
    int64_t nOut = akaizer::roundHalfEven((double)pitchedFrames * (pct / 100.0));
    if (nOut < 1)
    {
        nOut = 1;
    }
    const int64_t S = akaizer::roundHalfEven((double)pitchedFrames / ((double)nOut / (double)C));

    akaizerOutHop = (uint32_t)C;
    akaizerGrainLen = 2u * cycle;
    akaizerOutTotal = (uint32_t)nOut;
    akaizerGrainMax = (int32_t)akaizer::roundHalfEven((double)nOut / (double)C - 1.0);
    // grain spacing in the source: S pitched frames = S x pitch original frames, Q32.32
    akaizerSrcHop = (uint64_t)((double)S * r * 4294967296.0);
}

AUDIO_CORE_FLASH void AudioPlayMemory::playAkaizer(uint8_t instrIdx, int8_t note)
{
    AudioNoInterrupts();

    if (instrIdx > kSampleInstrumentsCount - 1)
        instrIdx = kSampleInstrumentsCount - 1;
    if (note > MAX_NOTE)
        note = MAX_NOTE;

    lastInstrIdxInPlay = currentInstrIdxInPlay;
    currentInstrIdxInPlay = instrIdx;
    currentInstrIdx = instrIdx;

    refreshStartParameters();

    constrainCurrentTune(note);

    setStartPitch(note);

    applyFinetuneOnPitch(note);

    calculateGlidePitch(note);

    lastNote = note;

    uint16_t startPoint =
      pointsForceFlag ? forcedStartPoint : mtProject.instrument[currentInstrIdx].startPoint;
    uint16_t endPoint =
      pointsForceFlag ? forcedEndPoint : mtProject.instrument[currentInstrIdx].endPoint;

    if (startPoint >= endPoint || currentStartAddress == nullptr)
    {
        AudioInterrupts();
        return;
    }

    pointsInSamples.start = calculatePointPositionAndSnapToZero(
      startPoint, (uint32_t)currentSampleLength, (int16_t*)currentStartAddress);
    pointsInSamples.end = calculatePointPositionAndSnapToZero(
      endPoint, (uint32_t)currentSampleLength, (int16_t*)currentStartAddress);
    pointsInSamples.loop1 = 0;
    pointsInSamples.loop2 = 0;

    if ((pointsInSamples.start >= currentSampleLength) ||
        (pointsInSamples.end > currentSampleLength) ||
        (pointsInSamples.end <= pointsInSamples.start))
    {
        AudioInterrupts();
        return;
    }

    akaizerRegion = pointsInSamples.end - pointsInSamples.start;

    // a sample still streamed from the card only has its first part in memory
    if (mtProject.sampleMemory[currentInstrIdx].isStreamed)
    {
        uint32_t inRam = mtProject.sampleMemory[currentInstrIdx].preCacheFrames;
        uint32_t avail = inRam > pointsInSamples.start ? inRam - pointsInSamples.start : 0;
        if (akaizerRegion > avail)
        {
            akaizerRegion = avail;
        }
    }

    if (akaizerRegion < 2)
    {
        AudioInterrupts();
        return;
    }

    next = currentStartAddress + pointsInSamples.start;

    for (int g = 0; g < kAkaizerMaxGrains; g++)
    {
        akaizerGrain[g].active = false;
    }
    akaizerGrainCount = 0;
    refreshAkaizerParams();
    akaizerNextSrc = 0;
    akaizerOutPos = 0;
    akaizerNextOut = 0;
    akaizerScheduling = true;

    playing = 1;

    AudioInterrupts();
}

AUDIO_CORE_RAM audio_block_t* AudioPlayMemory::updateAkaizer()
{
    audio_block_t* block = allocate();
    if (!block)
        return nullptr;

    if (!playing)
    {
        release(block);
        return nullptr;
    }

    refreshAkaizerParams();

    const int16_t* in = (const int16_t*)next;
    int16_t* out = block->data;
    const uint32_t region = akaizerRegion;

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
    {
        //*********************************** GLIDE HANDLE (as in the other modes)
        if (constrainsInSamples.pitchSmoothing)
        {
            if (pitchSmoothingControlCounter <= constrainsInSamples.pitchSmoothing)
            {
                pitchControl += pitchSmoothingControl;
                if (pitchControl > notes[MAX_NOTE - 1])
                {
                    pitchControl = notes[MAX_NOTE - 1];
                    pitchSmoothingControlCounter = constrainsInSamples.pitchSmoothing;
                }
                else if (pitchControl < notes[MIN_NOTE])
                {
                    pitchControl = notes[MIN_NOTE];
                    pitchSmoothingControlCounter = constrainsInSamples.pitchSmoothing;
                }
                pitchSmoothingControlCounter++;
            }
        }

        //*********************************** new grain
        if (akaizerScheduling && akaizerOutPos >= akaizerNextOut)
        {
            if (akaizerGrainCount > akaizerGrainMax)
            {
                akaizerScheduling = false; // the stretched sample has been laid down
            }
            else
            {
                // a free slot, or the grain closest to its end
                int slot = 0;
                uint32_t bestLeft = UINT32_MAX;
                for (int g = 0; g < kAkaizerMaxGrains; g++)
                {
                    if (!akaizerGrain[g].active)
                    {
                        slot = g;
                        break;
                    }
                    uint32_t left = akaizerGrain[g].len - akaizerGrain[g].j;
                    if (left < bestLeft)
                    {
                        bestLeft = left;
                        slot = g;
                    }
                }

                akaizerGrain[slot].pos = akaizerNextSrc;
                akaizerGrain[slot].j = 0;
                akaizerGrain[slot].len = akaizerGrainLen;
                akaizerGrain[slot].active = true;

                akaizerNextSrc += akaizerSrcHop;
                akaizerNextOut += akaizerOutHop;
                akaizerGrainCount++;
            }
        }

        //*********************************** sum the grains
        // read rate, Q32.32 (single precision: the original Tracker has no double FPU)
        const uint64_t rate = (uint64_t)(pitchControl * 65536.0f) << 16;
        int32_t sum = 0;
        bool any = false;

        for (int g = 0; g < kAkaizerMaxGrains; g++)
        {
            AkaizerGrain& gr = akaizerGrain[g];
            if (!gr.active)
            {
                continue;
            }

            const uint32_t idx = (uint32_t)(gr.pos >> 32);
            if (idx >= region)
            {
                gr.active = false; // ran off the end of the sample, as in the original
                continue;
            }

            // half-sine window: sin((j + 1) * pi / len)
            const uint32_t w = ((gr.j + 1) * (uint32_t)GRANULAR_LOOKUP_SIZE) / gr.len;
            const int32_t win = (w < GRANULAR_LOOKUP_SIZE) ? (int32_t)gaussTab[w] : 0;

            int32_t s0 = in[idx];
            if (idx + 1 < region)
            {
                const int32_t frac = (int32_t)((gr.pos >> 16) & 0xFFFF);
                s0 += (int32_t)(((int64_t)(in[idx + 1] - s0) * frac) >> 16);
            }

            sum += (int32_t)(((int64_t)s0 * win) >> 16);
            any = true;

            gr.pos += rate;
            if (++gr.j >= gr.len)
            {
                gr.active = false;
            }
        }

        *out++ = (int16_t)signed_saturate_rshift(sum, 16, 0);
        akaizerOutPos++;

        if ((!akaizerScheduling && !any) || akaizerOutPos >= akaizerOutTotal)
        {
            // finished: the rest of the block is silence
            for (int r = i + 1; r < AUDIO_BLOCK_SAMPLES; r++)
            {
                *out++ = 0;
            }
            playing = 0;
            break;
        }
    }

    return block;
}
