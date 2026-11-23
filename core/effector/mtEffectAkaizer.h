#ifndef EFFECTOR_MTEFFECTAKAIZER_H_
#define EFFECTOR_MTEFFECTAKAIZER_H_

// Akaizer - the classic Akaizer v1.2 timestretch (as in Twisted Timestretch) as a
// Sample Editor effect. The algorithm lives in akaizerCore.h.

#include "akaizerCore.h"
#include "mtEffect.h"

class mtEffectAkaizer : public mtEffect
{
  public:
    enum Params
    {
        STRETCH,     // % of the original length, 25..2000
        CYCLE_LEN,   // samples, 20..2000
        CYCLE_FREQ,  // cycles per second, 0 = linked to the cycle length (41000 / length)
        PITCH,       // semitones, -24..24, repitch before the stretch (length is kept)
        LOOP_BPM,    // 0 = off; otherwise stretch the loop from this tempo to the project tempo
        PARAMS_COUNT
    };

    virtual void setParameter(float& value, int idx) override;
    virtual bool startProcess() override;
    virtual int32_t processBlock() override;
    virtual bool getProcessState() override;
    virtual uint32_t getExpectedProcessLength() override;

  private:
    akaizer::Settings makeSettings();
    int channelsToProcess();
    uint32_t setupCore(akaizer::Core& target);

    float stretch = 100.0f;
    float cycleLength = 800.0f;
    float cycleFrequency = 0.0f;
    float pitch = 0.0f;
    float loopBpm = 0.0f;

    akaizer::Core core;
    int channels = 1;
    uint32_t outFrames = 0;
    bool finished = true;
};

extern mtEffectAkaizer sampleEditorAkaizer;

#endif /* EFFECTOR_MTEFFECTAKAIZER_H_ */
