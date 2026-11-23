/* SPDX-License-Identifier: GPL-3.0-only
 * Crafty Synth: the engine as one instance, for every host but the FM-1 (whose src/app does this itself, with its
 * panel): the editor's preview (WebAssembly), the AU / VST3 plugin and the MPC plugin. The FM-1's own synth, arp,
 * wavetables and patch code, so all of them play what the FM-1 plays.
 *
 * Plain C, no allocation and no libc (the WebAssembly build has none): the host owns the struct (about 600 KB,
 * most of it the effects' memory), calls cr_inst_init(), then render() for at most 128 frames at a time at
 * 44.1 kHz. Not thread-safe: the host keeps MIDI, parameter changes and render() from running at the same time.
 * Copyright (C) 2026 Jamie (TwistedGenetics). */
#pragma once
#include <stdint.h>
#include "cr_synth.h"
#include "cr_arp.h"
#include "cr_user.h"

#define CR_INST_FRAMES 128u
#define CR_INST_RATE 44100u
#define CR_WORKING 0x3FFFu              /* "the working patch", where a preset slot is asked for */

typedef struct cr_inst {
    cr_synth_t syn;
    cr_arp_t arp;
    cr_patch_t p;                       /* the working patch: what sounds */
    cr_globals_t g;                     /* bend range, velocity, breath (the FM-1's GLO pages) */
    cr_wtbank_t tab[2];                 /* Wave 1's and Wave 2's bank, ready to play */
    uint8_t tb[2];                      /* which banks those are (0xFF: none yet) */
    volatile uint8_t tb_pending;        /* Twisted: render wants another bank (cr_inst_tables) */
    const int16_t (*file[2])[CR_WT_N];  /* Twisted: a wavetable file in place of the wave's bank (0: the bank) */
    uint8_t file_gen[2], tb_gen[2];     /*   ... which file (built when tb_gen differs) */
    const uint8_t *user;                /* the user area (CR_USER_BYTES, src/core/cr_user.h), or 0: no user presets / banks */
    uint16_t cur;                       /* the preset last loaded (0..191) */
    uint8_t dirty;                      /* the working patch differs from it */
    uint8_t sus, patch_changed;
    uint32_t sus_bits[4];
    uint32_t changed[4];                /* parameters changed from inside (MIDI CC), for the host to read back */
    double ppq;                         /* the host's song position at this block, in beats (-1: none) */
    float bpm;                          /* the host's tempo (0: none: the patch's TEMPO) */
    int playing;
    int64_t tick;                       /* the last MIDI-clock tick (ppq x 24) the arp was given */
    int16_t mem[CR_SYN_MEM];
} cr_inst_t;

void cr_inst_init(cr_inst_t *in);
void cr_inst_set_user(cr_inst_t *in, const uint8_t *user);   /* the user area (or 0); call again after it changes */
void cr_inst_user_changed(cr_inst_t *in);                     /* its banks changed: rebuild what plays them */

int cr_preset_get(const uint8_t *user, uint32_t slot, cr_patch_t *out);  /* 0..191; 1 = a real preset, 0 = INIT */
int cr_user_saved(const uint8_t *user, uint32_t slot);       /* a user slot (64..191) holds a patch */
const cr_ubank_t *cr_user_bank(const uint8_t *user, uint32_t u);   /* user bank u, or 0 (empty) */
void cr_preset_label(uint32_t slot, char *b);                 /* "3-1" (factory), "U12-8" (user): 6 chars */

void cr_inst_use(cr_inst_t *in, const cr_patch_t *p);         /* a whole patch: it plays now */
void cr_inst_load(cr_inst_t *in, uint32_t slot);              /* preset 0..191 */
void cr_inst_param(cr_inst_t *in, uint32_t id, uint32_t v);   /* one value, 0..127, from the host */
void cr_inst_midi(cr_inst_t *in, const uint8_t *m, uint32_t len);   /* one MIDI message (any channel) */
void cr_inst_note(cr_inst_t *in, uint32_t note, uint32_t vel);      /* vel 0: off (the arp and sustain apply) */
void cr_inst_all_off(cr_inst_t *in);
/* once a block, before render(): the host's song position (beats, -1 = none), tempo (0 = none) and transport.
 * With a position and the transport running, the arp-seq locks to the host's beat; otherwise it runs at the
 * host's tempo, or the patch's TEMPO when the host gives none. */
void cr_inst_transport(cr_inst_t *in, double ppq, float bpm, int playing);
void cr_inst_render(cr_inst_t *in, float *lr, uint32_t n);    /* n <= 128 frames, interleaved L R, about -1..1 */
int cr_inst_tables_pending(const cr_inst_t *in);              /* Twisted: render wants a bank built ... */
void cr_inst_tables(cr_inst_t *in);                           /* ... so build it (main loop, not the audio) */
/* Twisted: play the 5 x 256 waves at base (the host keeps them) on wave k instead of its bank, or 0: the bank
 * again. Builds now (main loop only). */
void cr_inst_file_table(cr_inst_t *in, uint32_t k, const int16_t (*base)[CR_WT_N]);
/* a parameter changed from inside (MIDI CC): its id + 1, or 0 when none is left. A whole new patch (program change)
 * is reported once by cr_inst_take_patch_changed(). */
uint32_t cr_inst_take_changed(cr_inst_t *in);
int cr_inst_take_patch_changed(cr_inst_t *in);
