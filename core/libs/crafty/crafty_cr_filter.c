/* Twisted: builds Crafty Synth's cr_filter.c (core/synth/crafty) for the Tracker+ and Mini only */
#ifdef FEATURE_SYNTH
#ifdef PLATFORM_IMX /* the per-sample code runs from the fast instruction RAM (ITC), like the stock synths' DSP */
#define CR_HOT __attribute__((section(".audio_ram_functions"), noinline))
#endif
#include "../../synth/crafty/cr_filter.c"
#endif
