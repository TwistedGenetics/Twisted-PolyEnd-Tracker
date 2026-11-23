#pragma once
// Twisted: CRAFTY's user wavetables. Serum / Vital wavetables (or single-cycle WAVs) in the SD card's
// /Crafty/Wavetables folder become CRAFTY's USER banks, and a Crafty Synth user file
// (/Crafty/Crafty Synth.user, from the plugin) brings its user presets and banks too.
// The folder is read in the file manager's idle time, one file per call.

#ifdef FEATURE_SYNTH
void twistedCraftyService();     // call from the main loop when the file manager is idle
void twistedCraftyRequestScan(); // read the folder again (a CRAFTY instrument was made)
void twistedCraftyBuildTables(); // every main-loop pass: build the wavetable banks the voices are waiting for
#else
inline void twistedCraftyBuildTables() {}
inline void twistedCraftyService() {}
inline void twistedCraftyRequestScan() {}
#endif
