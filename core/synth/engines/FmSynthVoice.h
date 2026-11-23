#pragma once

#include "PolySynth/sdk/InstrumentType.h"
#include <memory>

namespace polysynth
{
// Twisted: a DREADNOUGHT engine (see FmSynthVoice.cpp)
std::unique_ptr<InstrumentType> createFmSynth();
} // namespace polysynth
