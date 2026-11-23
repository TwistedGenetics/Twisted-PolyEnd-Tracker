#pragma once

#include "PolySynth/sdk/InstrumentType.h"
#include <memory>

namespace polysynth
{
// Twisted: REESE, the jungle / drum & bass Reese bass from DREADNOUGHT (TwistedGenetics),
// written again for the Tracker's synth SDK in float.
std::unique_ptr<InstrumentType> createReeseSynth();
} // namespace polysynth
