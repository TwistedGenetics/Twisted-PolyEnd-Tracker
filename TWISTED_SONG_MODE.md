# Twisted Song Mode

Song mode additions for the Polyend Tracker+, brought over from Paula's Revenge:
named song sections, section and selection looping (with live jumping between
sections), clone in place, and linked pattern variations.

Built on Polyend Tracker Community Edition. Projects stay compatible with stock
firmware: the extra song data is kept in a side file (`patterns/twistedSong`)
that stock firmware ignores.

**Manual:** the full illustrated manual is [`docs/Twisted_Song_Mode_Manual.pdf`](docs/Twisted_Song_Mode_Manual.pdf), and it's attached to every release. Its source is `docs/manual/manual.html`.

## Opening the tools

In the **Song** screen, press **INSERT** to swap the eight buttons under the
screen to the song tools. Press **INSERT** again to return to the normal song
buttons. **SHIFT + INSERT** still jumps to the top of the song.

The arrows, encoder, PLAY and REC keep working while the tools are showing.

| Button | Tool | Needs REC (edit mode) |
|---|---|---|
| F1 | **Section Name** — start a section at the selected slot, or rename it | Yes |
| F2 | **Pattern Name** — name the selected slot's pattern (same name as in the pattern editor) | No |
| SHIFT+F1 | **Section Remove** — remove the divider of the section the selection is in | Yes |
| F3 | **Loop Section** — loop the section of the selected slot (press again to stop) | No |
| F4 | **Loop Selection** — loop the highlighted slots (press again to stop) | No |
| F5 | **Clone In Place** — give this slot its own copy of the pattern | Yes |
| F6 | **Make Variation** — give this slot a linked variation of the pattern | Yes |
| F7 | **Block Colour** — colour the selected blocks | No |
| SHIFT+F6 | **Unlink Variation** — make a variation an ordinary pattern | Yes |
| F8 | **Solo Track** — solo the track under the grid cursor (again to end, restoring the previous mutes) | No |

Buttons that need edit mode are greyed out until you press **REC**.

## Song sections

Mark the parts of a song (INTRO, DROP, BREAK…) with dividers.

* Select a slot in the song list and press **F1 (Section Name)**. Type the name
  with the pads (as when naming a pattern) and press **F8 (Save)**. **F7** cancels.
  Saving an empty name removes the section.
* A section runs from its divider to the next one. Slots before the first divider
  count as one section too.
* The divider shows as a red line across the song blocks, and the slot's list entry
  starts with the section name: `DROP: AMEN CHOP`.
* Adding and deleting slots moves the dividers with the music.
* Up to 32 sections per song, names up to 16 characters.

## Looping and jumping between sections

**Loop Section (F3)** loops the section of the selected slot. The looped slots are
marked with a red bar down the left edge, and the title bar shows `LOOP: DROP`.

While the song plays with Loop Section on, move the selection with the arrows or
the encoder (no need for REC). Picking a slot in **another** section queues a jump:
playback goes to that slot when the current pattern ends, and from then on loops
that section. The queued slot gets a highlighted bar on the left edge until playback
arrives. Moving back into the playing section cancels the jump. This is the
Paula's Revenge "jam between parts" workflow.

**Loop Selection (F4)** loops the slots you highlighted on the left (SHIFT + arrows
in the block grid), or just the selected slot when you're in the list. If the song
is playing somewhere else, it goes to the start of the selection when the current
pattern ends.

Loops last until you switch them off (F3 / F4 again) or add or delete a slot.
Exporting the song always renders it straight through once, ignoring loops.

## Clone in place

**F5** copies the selected slot's pattern into the next free pattern and points the
slot at it. The free pattern is the lowest-numbered one from 11 up (1-10 are kept for your own ideas) that's empty and isn't used
anywhere in the song. Now you can change this slot without touching the other places
the original pattern is used. The pattern name is copied too.

## Linked variations

**F6** works like Clone In Place, but the copy stays linked to the original:

* Tracks you haven't changed in the variation **follow the original** — edit the
  original and those tracks update in the variation as well.
* As soon as you change a track in the variation (in the pattern editor, or by
  pasting or deleting tracks in the song screen), that track becomes the
  variation's own and stops following.
* The pattern length follows the original too, until you change the variation's
  length.
* Variations are marked with `~` in the song list: `~AMEN CHOP`.
* A variation can be the original of another variation.
* **SHIFT + F6 (Unlink Variation)** makes it an ordinary, independent pattern.

Updates happen when the original pattern is saved to the workspace — when you
switch patterns, go to another screen, or the Tracker autosaves — so a variation
picks up changes a moment after you leave the original.

## Block colours

Colour-code blocks in the song view with 8 colours (Red, Orange, Yellow, Green,
Cyan, Blue, Purple, Pink), e.g. drums blue and bass red.

* Select blocks in the grid (SHIFT + arrows), press **F7 (Block Colour)** on the
  tools page, then **F1–F8** to pick a colour. **DELETE** clears the colour,
  **INSERT** cancels. In the song list it colours every track of the slot.
* A colour belongs to a track of a pattern, so every slot playing that pattern
  shows it.
* Copying and pasting blocks in the song screen moves the colour with the notes.
  Clone In Place and Make Variation copy the colours; deleting blocks clears them.
* Only blocks with notes are coloured; muted tracks show a darker shade.
* Stored in `patterns/twistedSong` (file version 2; version 1 files still load).

## Pattern names

**F2 (Pattern Name)** names the selected slot's pattern with the pads, like a
section. It's the pattern's own name, so the pattern editor (F8 More → F1
Pattern Name) and the song view always show the same name.

## Akaizer (Sample Editor)

The Akaizer v1.2 timestretch (the engine from Twisted Timestretch, matched sample
for sample against its reference renders) is a built-in Sample Editor effect:
**Stretch** 25–2000 %, **Cycle Len** 20–2000 samples, **Cycle Freq** Auto
(41000 ÷ length) or 20–2050 cps, **Pitch** ±24 semitones (length kept),
**Loop BPM** Off or 40–300 (stretch from that tempo to the project tempo).
Code: `core/effector/akaizerCore.h`, `core/effector/mtEffectAkaizer.*`.

## Akaizer play mode and FX

**Akaizer** is also an instrument play mode (Sample Playback, after Granular): the
Start–End part is stretched live on every note. F3 = Stretch (press again: Loop
BPM, which follows the project tempo), F4 = Cycle Len (press again: Cycle Freq).
Note pitch works like the original's "repitch, then stretch back to length".
Settings are stored in the instrument's granular fields (marked with bit 7 of
`granular.type`), so the instrument file format is unchanged.

FX: **K – Akaizer Stretch** (25–255 %), **Y – Akaizer Cycle** (cycle ÷ 10, 2–200),
using the reserved FX slots R2/R3 so stock firmware ignores them.
Code: `core/libs/Audio/play_memory_akaizer.cpp`, `core/source/mtStructs.h`
(`akaizerSettings`), `core/interface/samplePlayback/*`, `core/AudioEngine/mtAudioSequencer.cpp`.

## Faster sample browser (v1.3.0)

- Folders of up to 499 samples show as one list (stock splits anything over 99
  into `(1-99)`, `(100-198)`… groups). Larger folders split into groups of 499.
- Each file is opened once to both validate it and size it (stock opened every
  wav twice plus an extra directory search), and files are checked in ~12 ms
  slices instead of one per update.
- The bigger lists (`explorerList`, sizes) live in on-chip OCRAM on the IMX
  targets; name allocation is `nothrow`, so a full heap ends the list early
  instead of crashing.
Code: `core/FileManager/fileManagerBrowse.cpp`, `platforms/*/SD.cpp`
(`createFilesList`), `core/interface/sampleImporter/*`.

## Crop to New and pattern track colours (v1.4.0)

- **Crop to New** (Sample Editor, F5): the Start..End part of the current sample
  is saved into the first empty instrument slot (named `<original> crop`), then the
  edited original is saved too if it has applied changes, and the editor reloads on
  the new sample. Uses `importSampleFromSampleEditor` (`fSampleEdit.cpp`).
- **Pattern editor track colours**: the block colour of each track in the current
  pattern is drawn behind its name (`tracker.cpp`, `strTrackerPattern::headerColour`),
  and F3 **Track Colour** on the Name page sets it (cursor track or selected tracks).

## REESE synth and graffiti splash (v1.5.0)

- **REESE** (`core/synth/engines/ReeseSynthVoice.cpp`): DREADNOUGHT's Reese bass rewritten in
  float for the PolySynth SDK (`InstrumentTypeId::kReeseSynth`), mono with glide. Its 10 presets are a
  `Preset` parameter; every other knob is an offset from the preset, shown as the resulting value.
  None of DREADNOUGHT's third-party GPL code (the Clipper ladder, the Felucca helpers) is used.
- **Splash** (Tracker+ only): `core/libs/images/twisted_splash.*`, made from
  `docs/images/twisted_splash_source.png` by `tools/make_splash.py` (720x396, PALETTED565), uploaded to
  RAM_G 690000 at boot (in 32 KB pieces) and drawn by `startScreen.cpp` on white.

## CRAFTY synth (v1.6.0)

- Crafty Synth's own C engine (`core/synth/crafty`, from TwistedGenetics/Crafty-Synth 0.12 @ 2c51b2b (v1.11.0: the sub oscillator; v1.17.0 patch in cr_inst.c/.h: bank builds deferred from the audio to the main loop, `cr_inst_tables_pending` / `cr_inst_tables`, called by `twistedCraftyBuildTables()` from `cFileManager::update()`), with its
  generated `cr_presets.h` / `cr_wavetables.h`), built by the wrappers in `core/libs/crafty` (FEATURE_SYNTH only).
- `core/synth/engines/CraftySynthVoice.cpp`: one SDK parameter per Crafty patch value plus `Preset`; continuous ones
  are offsets from the preset, stepped ones pick "Preset" or a value. Notes go straight to Crafty's own held-note
  stack (legato, glide); pitch bend as MIDI.
- Each `cr_inst_t` is ~190 KB: a pool of four in external RAM, taken from `SAMPLE_MEMORY_SIZE` (768 KB less).

## REESE v2 and CRAFTY wavetable folder (v1.7.0)

- REESE: its own filter envelope (ADSR, starting from the preset's amp envelope), LFO shapes (sine, triangle,
  saw up/down, square, S&H) and key reset, a post-filter distortion (soft, hard, fold, crush) that leaves the
  sub clean, and a stereo chorus (output now stereo). New parameters are appended so saved patches keep working.
- CRAFTY: `/Crafty/Wavetables/*.wav` become user banks (5 frames each, band-limited to 128 harmonics, resampled to
  256 samples: the editor's algorithm), read one file per idle call of the file manager. `/Crafty/Crafty Synth.user`
  (the plugin's user area) is loaded first; user presets U1-1..U16-8 are selectable.

## Folder index (v1.8.0)

- `createFilesList` can return a signature (FNV-1a of the listed files' names, sizes, dates and times).
- After a folder (or 499-file group) is scanned, `saveBrowseIndex` writes `/.TwistedIndex/<path hash>_<offset>.idx`
  (hidden): the signature, the sample settings, the path, and each usable file's name and memory size. Next time
  `loadBrowseIndex` takes it when the signature matches, so no sample file is opened.
- Folder lists too (v1.9.0): a quick listing without sub-folder counting gives the signature (sub-folder names and
  dates); `/.TwistedIndex/<path hash>_d<filter>.idx` holds the counted, split list. A sub-folder that fills the list
  without having been split (or a split group that comes up empty) drops its parent's index so it recounts.

## SUB, JUNO and DX7 (v1.10.0)

- `TwistedPresetVoice.h`: a shared base (Preset + one parameter per value: offsets, or Preset/choice for values with
  few settings) and float DSP pieces (polyBLEP, ladder, times).
- SUB (`SubSynthVoice.cpp`): DREADNOUGHT's SH-101 bass rewritten in float (no Clipper/Felucca code); mono; accent =
  velocity > 100; own filter envelope and cutoff LFO.
- JUNO (`JunoSynthVoice.cpp`): DREADNOUGHT's JUNO code per Tracker voice (6); HPF and chorus per voice (linear; the
  chorus sweep and a free LFO from one shared clock).
- DX7 (`FmSynthVoice.cpp`): DREADNOUGHT's dn_fm.c (Dexed MSFA port, Apache-2.0 origin) vendored in core/synth/dreadnought
  with DN_FM_NV 1 (one engine per Tracker voice, a pool of 24 in external RAM), built by core/libs/dreadnought;
  `/DX7/*.syx` 32-voice banks (up to 8) read in the file manager's idle time.
- SAMPLE_MEMORY_SIZE now 1 MB below stock.

## Files

| File | What |
|---|---|
| `core/FileManager/TwistedSong.*` | Sections and variation links, and the `twistedSong` side file |
| `core/FileManager/fileManagerTwisted.cpp` | Clone in place and the linked-variation sync |
| `core/FileManager/fileManagerSong.cpp` | Loop ranges and queued jumps in song playback |
| `core/interface/songEditor/*` | The tools page, naming, loops and list text |
| `core/libs/NewDisplay/songPlayer.cpp` | Section dividers and loop marks in the song view |
