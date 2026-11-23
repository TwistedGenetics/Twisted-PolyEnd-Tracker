#pragma once
// Twisted: CRAFTY's wavetable files (see CraftyTables.cpp). Main loop only.
#include <cstddef>

class FileSystem;

namespace twisted_tables
{
constexpr int kMaxTables = 4000;

void requestScan();               // walk /Crafty/Wavetables again (at most every 20 s)
void service(FileSystem* fs);     // file manager idle time: one step
const char* path(int id);         // "Basses/Growl.wav", relative to /Crafty/Wavetables (nullptr: no such number)
bool isPresent(int id);           // on the card at the last walk
bool known();                     // walked at least once since start-up
int step(int id, int delta, bool byFolder); // the next / previous table in name order (0: Off)
int random();                     // any table on the card (0: none)
void fullPath(int id, char* out, size_t outLen);
} // namespace twisted_tables
