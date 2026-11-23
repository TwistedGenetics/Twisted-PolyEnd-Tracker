#pragma once
// Twisted: the crash and freeze recorder (platforms/imx/source/TwistedCrash.cpp). Tracker+ / Mini only.
#include <cstdint>

#ifdef PLATFORM_IMX
extern "C"
{
    void twCrashArm(int on);       // watch the main loop (only while the file manager is idle)
    void twCrashCrumb(uint32_t id); // the last few steps, kept in the report
    int twCrashTakeReport(char* out, int len, const char* version);
}
#else
inline void twCrashArm(int) {}
inline void twCrashCrumb(uint32_t) {}
inline int twCrashTakeReport(char*, int, const char*) { return 0; }
#endif

// steps (shown in hex in the report)
enum TwistedCrumb : uint32_t
{
    kCrumbRandomStart = 0x10,
    kCrumbRandomSet = 0x11,
    kCrumbRandomEnd = 0x12,
    kCrumbEditorShow = 0x20,
    kCrumbEditorList = 0x21,
    kCrumbEditorListDone = 0x22,
    kCrumbParamChange = 0x23,
    kCrumbCraftyScan = 0x30,
    kCrumbCraftyImport = 0x31,
    kCrumbCraftyTables = 0x32,
    kCrumbCraftyFileLoad = 0x33,
    kCrumbCraftyFileDone = 0x34,
    kCrumbTablesWalk = 0x35,
    kCrumbCraftyInstance = 0x36,
    kCrumbLoadSaved = 0x40,
};
