# Twisted Song Mode

Song mode additions for the Polyend Tracker+, brought over from Paula's Revenge:
named song sections, section and selection looping (with live jumping between
sections), clone in place, and linked pattern variations.

Built on Polyend Tracker Community Edition. Projects stay compatible with stock
firmware: the extra song data is kept in a side file (`patterns/twistedSong`)
that stock firmware ignores.

## Opening the tools

In the **Song** screen, press **INSERT** to swap the eight buttons under the
screen to the song tools. Press **INSERT** again, or **F7 (Back)**, to return to
the normal song buttons. **SHIFT + INSERT** still jumps to the top of the song.

The arrows, encoder, PLAY and REC keep working while the tools are showing.

| Button | Tool | Needs REC (edit mode) |
|---|---|---|
| F1 | **Section Name** — start a section at the selected slot, or rename it | Yes |
| F2 | **Section Remove** — remove the divider of the section the selection is in | Yes |
| F3 | **Loop Section** — loop the section of the selected slot (press again to stop) | No |
| F4 | **Loop Selection** — loop the highlighted slots (press again to stop) | No |
| F5 | **Clone In Place** — give this slot its own copy of the pattern | Yes |
| F6 | **Make Variation** — give this slot a linked variation of the pattern | Yes |
| F7 | **Back** — normal song buttons | No |
| F8 | **Unlink Variation** — make a variation an ordinary pattern | Yes |

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
slot at it. The free pattern is the lowest-numbered one that's empty and isn't used
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
* **F8 (Unlink Variation)** makes it an ordinary, independent pattern.

Updates happen when the original pattern is saved to the workspace — when you
switch patterns, go to another screen, or the Tracker autosaves — so a variation
picks up changes a moment after you leave the original.

## Files

| File | What |
|---|---|
| `core/FileManager/TwistedSong.*` | Sections and variation links, and the `twistedSong` side file |
| `core/FileManager/fileManagerTwisted.cpp` | Clone in place and the linked-variation sync |
| `core/FileManager/fileManagerSong.cpp` | Loop ranges and queued jumps in song playback |
| `core/interface/songEditor/*` | The tools page, naming, loops and list text |
| `core/libs/NewDisplay/songPlayer.cpp` | Section dividers and loop marks in the song view |
