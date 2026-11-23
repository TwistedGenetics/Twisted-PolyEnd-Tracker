#include "config.h"
#ifdef FEATURE_SYNTH

#include "PolySynth/sdk/InstrumentTypeRegistry.h"
#include "engines/ChiptuneSynthVoice.h"
#include "engines/ReeseSynthVoice.h"
#include "engines/CraftySynthVoice.h"
#include "engines/SubSynthVoice.h"
#include "engines/JunoSynthVoice.h"
#include "engines/FmSynthVoice.h"

namespace polysynth
{
// Factory synth engines (provided by precompiled libPolySynth.a + libPolyDSP.a)
extern std::unique_ptr<InstrumentType> createMidiOut();
extern std::unique_ptr<InstrumentType> createSimpleAnalogSynth();
extern std::unique_ptr<InstrumentType> createFatAnalogSynth();
extern std::unique_ptr<InstrumentType> createVASynth();
extern std::unique_ptr<InstrumentType> createFMSynth();
extern std::unique_ptr<InstrumentType> createDrumSynth();
extern std::unique_ptr<InstrumentType> createShapingSynth();

void InstrumentTypeRegistry::registerInstrumentTypes(
  InstrumentTypeRegistry& registry,
  [[maybe_unused]] const Synth::Allocator& largeBuffersAlloc)
{
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kMidiOut), createMidiOut());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kSimpleAnalogSynth),
                               createSimpleAnalogSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kFatAnalogSynth),
                               createFatAnalogSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kVASynth), createVASynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kFMSynth), createFMSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kDrumSynth), createDrumSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kShapingSynth),
                               createShapingSynth());

    // Open-Source Custom Synth Engines
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kChiptuneSynth),
                               createChiptuneSynth());
    // Twisted: REESE (DREADNOUGHT's Reese bass)
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kReeseSynth), createReeseSynth());
    // Twisted: CRAFTY (Crafty Synth, the FM-1's CRAFTsynth 2.0-style wavetable synth)
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kCraftySynth), createCraftySynth());
    // Twisted: DREADNOUGHT's SUB, JUNO and DX7 engines
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kSubSynth), createSubSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kJunoSynth), createJunoSynth());
    registry.addInstrumentType(static_cast<int>(InstrumentTypeId::kFmSynth), createFmSynth());
}

} // namespace polysynth

#endif
