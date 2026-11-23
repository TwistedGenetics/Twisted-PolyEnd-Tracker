#pragma once
// Twisted: the DX7 engine's banks. 32-voice DX7 sysex banks (.syx) in the SD card's /DX7 folder show up as the DX7's
// voices after DREADNOUGHT's own; the folder is read in the file manager's idle time, one bank per call.
#ifdef FEATURE_SYNTH
void twistedDxService();
#else
inline void twistedDxService() {}
#endif
