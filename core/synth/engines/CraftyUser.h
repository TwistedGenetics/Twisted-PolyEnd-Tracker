#pragma once
// Twisted: CRAFTY's user wavetables. Serum / Vital wavetables (or single-cycle WAVs) in the SD card's
// /Crafty/Wavetables folder become CRAFTY's USER banks, and a Crafty Synth user file
// (/Crafty/Crafty Synth.user, from the plugin) brings its user presets and banks too.
// The folder is read in the file manager's idle time, one file per call.

#ifdef FEATURE_SYNTH
void twistedCraftyService();     // call from the main loop when the file manager is idle
void twistedCraftyRequestScan(); // read the folder again (a CRAFTY instrument was made)
void twistedCraftyBuildTables(); // every main-loop pass: build the wavetable banks the voices are waiting for
int twistedCraftyTableStep(int id, int delta, bool byFolder); // the wavetable file list, in name order (0: Off)
int twistedCraftyRandomTable();                               // any wavetable file on the card (0: none)
#else
inline int twistedCraftyTableStep(int id, int, bool) { return id; }
inline int twistedCraftyRandomTable() { return 0; }
inline void twistedCraftyBuildTables() {}
inline void twistedCraftyService() {}
inline void twistedCraftyRequestScan() {}
#endif
