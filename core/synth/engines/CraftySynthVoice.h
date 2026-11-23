#pragma once

#include "PolySynth/sdk/InstrumentType.h"
#include <memory>

namespace polysynth
{
// Twisted: CRAFTY, Crafty Synth (the FM-1's CRAFTsynth 2.0-style wavetable synth) as a synth engine.
std::unique_ptr<InstrumentType> createCraftySynth();
} // namespace polysynth
