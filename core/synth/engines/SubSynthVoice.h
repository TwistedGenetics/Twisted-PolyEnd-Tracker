#pragma once

#include "PolySynth/sdk/InstrumentType.h"
#include <memory>

namespace polysynth
{
// Twisted: a DREADNOUGHT engine (see SubSynthVoice.cpp)
std::unique_ptr<InstrumentType> createSubSynth();
} // namespace polysynth
