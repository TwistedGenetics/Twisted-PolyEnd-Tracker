#include "mtEffectAkaizer.h"

#include "AudioStream.h"
#include "core/interfacePopups.h"
#include "fileManager.h"
#include "mtStructs.h"

#include <string.h>

mtEffectAkaizer sampleEditorAkaizer;

void mtEffectAkaizer::setParameter(float& value, int idx)
{
    switch (idx)
    {
        case STRETCH: stretch = value; break;
        case CYCLE_LEN: cycleLength = value; break;
        case CYCLE_FREQ:
            // 0 is "linked"; real frequencies start at 20, so step over 1..19
            if (value > 0.0f && value < 20.0f)
            {
                value = (cycleFrequency == 0.0f) ? 20.0f : 0.0f;
            }
            cycleFrequency = value;
            break;
        case PITCH: pitch = value; break;
        case LOOP_BPM:
            // 0 is "off"; tempos start at 40
            if (value > 0.0f && value < 40.0f)
            {
                value = (loopBpm == 0.0f) ? 40.0f : 0.0f;
            }
            loopBpm = value;
            break;
        default: break;
    }
}

akaizer::Settings mtEffectAkaizer::makeSettings()
{
    akaizer::Settings s;

    s.stretchPercent = stretch;
    if (loopBpm > 0.0f && mtProject.values.globalTempo > 0.0f)
    {
        // a loop at loopBpm, made to fit the project tempo
        s.stretchPercent = 100.0 * (double)loopBpm / (double)mtProject.values.globalTempo;
    }

    s.cycleLength = (int)cycleLength;
    s.cycleFrequency = (int)cycleFrequency; // 0 = linked
    s.semitones = pitch;
    return s;
}

int mtEffectAkaizer::channelsToProcess()
{
#ifdef PLATFORM_IMX
    return newFileManager.isInstrumentStereo(mtProject.values.lastUsedInstrument) ? 2 : 1;
#else
    return 1;
#endif
}

uint32_t mtEffectAkaizer::setupCore(akaizer::Core& target)
{
    uint32_t inFrames = confirmed[0].selection.length;
    if (inFrames == 0)
    {
        return 0;
    }

    channels = channelsToProcess();

    const int16_t* src[akaizer::Core::kMaxChannels] = { confirmed[0].selection.addr,
                                                        confirmed[0].selection.addr };
#ifdef PLATFORM_IMX
    src[1] = confirmed[kRight].selection.addr;
#endif

    return target.setup(src, channels, inFrames, AUDIO_SAMPLE_RATE_EXACT, makeSettings());
}

uint32_t mtEffectAkaizer::getExpectedProcessLength()
{
    // a scratch core, so asking for the length never disturbs a render in progress
    akaizer::Core sizing;
    int keepChannels = channels;
    uint32_t len = setupCore(sizing);
    if (!finished)
    {
        channels = keepChannels;
    }
    return len;
}

bool mtEffectAkaizer::startProcess()
{
    outFrames = setupCore(core);
    finished = true;

    if (outFrames == 0)
    {
        return false;
    }

    if (outFrames > EDIT_BUFF_SIZE / 2)
    {
        mtPopups.show(0, "Stretched sample is too long!");
        return false;
    }

    // the original adds each grain on top of what's already there: start from silence
    for (int c = 0; c < kChannels; c++)
    {
        memset(processed[c].selection.addr, 0, outFrames * sizeof(int16_t));
    }

    int16_t* dst[akaizer::Core::kMaxChannels] = { processed[0].selection.addr,
                                                  processed[0].selection.addr };
#ifdef PLATFORM_IMX
    dst[1] = processed[kRight].selection.addr;
#endif
    core.setOutput(dst);

    finished = false;
    return true;
}

int32_t mtEffectAkaizer::processBlock()
{
    if (finished)
    {
        return -1;
    }

    // a few grains per call keeps the Sample Editor responsive
    uint32_t frames = core.step(8192);

    if (core.isDone())
    {
#ifdef PLATFORM_IMX
        // mono instrument: the right channel is a copy of the left
        if (channels == 1)
        {
            memcpy(processed[kRight].selection.addr,
                   processed[kLeft].selection.addr,
                   outFrames * sizeof(int16_t));
        }
#endif
        finished = true;
    }

    return (int32_t)frames;
}

bool mtEffectAkaizer::getProcessState()
{
    return !finished;
}
