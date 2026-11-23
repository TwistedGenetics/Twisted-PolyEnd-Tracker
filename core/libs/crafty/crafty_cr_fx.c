/* Twisted: builds Crafty Synth's cr_fx.c (core/synth/crafty) for the Tracker+ and Mini only */
#ifdef FEATURE_SYNTH
#ifdef PLATFORM_IMX /* the reverb runs from the fast instruction RAM (ITC) */
#define CR_HOT __attribute__((section(".audio_ram_functions"), noinline))
#endif
#include "../../synth/crafty/cr_fx.c"
#endif
