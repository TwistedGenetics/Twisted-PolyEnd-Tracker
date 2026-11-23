#pragma once

#include "PolySynth/sdk/InstrumentType.h"
#include <memory>

namespace polysynth
{
// Twisted: a DREADNOUGHT engine (see JunoSynthVoice.cpp)
std::unique_ptr<InstrumentType> createJunoSynth();
} // namespace polysynth
