// Twisted: a crash and freeze recorder. The stock firmware stops dead on a fault ("B ." / while (1)), which looks
// like a freeze and leaves nothing to go on. Here a fault, running out of heap memory, or the main loop not coming
// round for 10 seconds (while the file manager is idle, so long loads and saves never trip it) records where the
// processor was into memory that survives a reset, then restarts the Tracker. The next start writes the report to
// /TwistedCrash.txt on the SD card and says so on screen.

#include "fsl_device_registers.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <malloc.h>

extern volatile uint32_t msTicks;
extern volatile uint32_t cyclesTicks;

namespace
{
constexpr uint32_t kMagic = 0x54574352u; // "TWCR"
constexpr uint32_t kFreezeMs = 10000;
constexpr int kCrumbs = 8;

enum Type : uint32_t
{
    kHardFault = 1,
    kMemFault,
    kBusFault,
    kUsageFault,
    kFreeze,
    kOutOfMemory,
};

struct Record
{
    uint32_t magic;
    uint32_t type;
    uint32_t pc, lr, xpsr, sp;
    uint32_t cfsr, hfsr, mmfar, bfar;
    uint32_t size, heapFree;
    uint32_t uptimeMs;
    uint32_t crumbs[kCrumbs];
    uint32_t crumbHead;
    uint32_t sum;
};

__attribute__((section(".noinit.twcrash"))) Record rec;
Record pending;
bool havePending = false;
uint32_t resetReason = 0; // SRC->SRSR at start-up

// A copy of the essentials in the SNVS general-purpose registers, which keep their contents through a reset even
// when the boot loader reuses the RAM the full record is in.
constexpr uint32_t kSnvsMagic = 0x5457u; // "TW"
void snvsSave(const Record& r)
{
    const uint32_t count = r.crumbHead < kCrumbs ? r.crumbHead : kCrumbs;
    uint32_t packed = 0;
    for (uint32_t i = 0; i < 4 && i < count; i++)
        packed |= (r.crumbs[(r.crumbHead - 1 - i) % kCrumbs] & 0xFFu) << (8 * i); // newest first
    SNVS->LPGPR[2] = (kSnvsMagic << 16) | ((r.type & 0xFFu) << 8) | (r.xpsr & 0xFFu);
    SNVS->LPGPR[3] = r.pc;
    SNVS->LPGPR[4] = r.lr;
    SNVS->LPGPR[5] = r.cfsr;
    SNVS->LPGPR[6] = packed;
    SNVS->LPGPR[7] = (r.uptimeMs / 1000u) | (r.size << 20);
}
bool snvsLoad(Record& r)
{
    const uint32_t w = SNVS->LPGPR[2];
    if ((w >> 16) != kSnvsMagic)
        return false;
    std::memset(&r, 0, sizeof(r));
    r.type = (w >> 8) & 0xFFu;
    r.xpsr = w & 0xFFu;
    r.pc = SNVS->LPGPR[3];
    r.lr = SNVS->LPGPR[4];
    r.cfsr = SNVS->LPGPR[5];
    const uint32_t packed = SNVS->LPGPR[6];
    for (uint32_t i = 0; i < 4; i++)
        r.crumbs[3 - i] = (packed >> (8 * i)) & 0xFFu;
    r.crumbHead = 4;
    r.uptimeMs = (SNVS->LPGPR[7] & 0xFFFFFu) * 1000u;
    r.size = SNVS->LPGPR[7] >> 20;
    return true;
}
void snvsClear()
{
    SNVS->LPGPR[2] = 0;
}

uint32_t checksum(const Record& r)
{
    const uint32_t* w = reinterpret_cast<const uint32_t*>(&r);
    uint32_t s = 0x9E3779B9u;
    for (size_t i = 0; i < offsetof(Record, sum) / 4; i++)
        s = (s ^ w[i]) * 16777619u;
    return s;
}

volatile uint32_t crumbs[kCrumbs];
volatile uint32_t crumbHead = 0;
volatile uint32_t heartbeat = 0;
volatile uint32_t armed = 0;
uint32_t lastBeat = 0, lastBeatMs = 0;
bool capturing = false;
uint32_t oomFree = 0;
} // namespace

extern "C"
{
    // frame: the stacked registers of the code that was running (r0 r1 r2 r3 r12 lr pc xpsr), or 0
    __attribute__((used, noreturn)) void twCrashCapture(uint32_t* frame, uint32_t type, uint32_t size)
    {
        __disable_irq();
        if (!capturing)
        {
            capturing = true;
            std::memset(&rec, 0, sizeof(rec));
            rec.type = type;
            if (frame != nullptr)
            {
                rec.lr = frame[5];
                rec.pc = frame[6];
                rec.xpsr = frame[7];
                rec.sp = reinterpret_cast<uint32_t>(frame);
            }
            rec.cfsr = SCB->CFSR;
            rec.hfsr = SCB->HFSR;
            rec.mmfar = SCB->MMFAR;
            rec.bfar = SCB->BFAR;
            rec.size = size;
            rec.heapFree = type == kOutOfMemory ? oomFree : 0;
            rec.uptimeMs = msTicks;
            for (int i = 0; i < kCrumbs; i++)
                rec.crumbs[i] = crumbs[i];
            rec.crumbHead = crumbHead;
            rec.sum = checksum(rec);
            rec.magic = kMagic;
            snvsSave(rec);
            __DSB();
        }
        NVIC_SystemReset();
        while (1)
        {
        }
    }

    void twCrashOutOfMemory(size_t size, void* caller)
    {
        uint32_t frame[8] = {};
        frame[5] = reinterpret_cast<uint32_t>(caller);
        frame[6] = reinterpret_cast<uint32_t>(caller);
        struct mallinfo mi = mallinfo();
        oomFree = static_cast<uint32_t>(mi.fordblks);
        twCrashCapture(frame, kOutOfMemory, static_cast<uint32_t>(size));
    }

    // from the SysTick interrupt (the highest priority), every millisecond
    void twCrashTick(uint32_t* frame)
    {
        const uint32_t beat = heartbeat;
        if (beat != lastBeat || !armed)
        {
            lastBeat = beat;
            lastBeatMs = msTicks;
            return;
        }
        if (msTicks - lastBeatMs > kFreezeMs)
            twCrashCapture(frame, kFreeze, 0);
    }

    void twCrashHeartbeat()
    {
        heartbeat = heartbeat + 1;
    }

    void twCrashArm(int on)
    {
        if (on && !armed)
            lastBeatMs = msTicks; // count from now
        armed = on ? 1u : 0u;
    }

    void twCrashCrumb(uint32_t id)
    {
        const uint32_t h = crumbHead;
        crumbs[h % kCrumbs] = id;
        crumbHead = h + 1;
    }

    // at start-up: keep a report from before the reset, if there is one
    void twCrashInit()
    {
        resetReason = SRC->SRSR;
        SRC->SRSR = resetReason; // (write 1 to clear)
        if (rec.magic == kMagic && rec.sum == checksum(rec))
        {
            pending = rec;
            havePending = true;
        }
        else if (snvsLoad(pending))
        {
            havePending = true; // the RAM copy was lost: the short one from the SNVS registers
        }
        rec.magic = 0;
        snvsClear();
    }

    // the report as text (once), or 0
    int twCrashTakeReport(char* out, int len, const char* version)
    {
        if (out == nullptr || len < 64)
            return 0;
        if (!havePending)
        {
            // no record, but not a plain power-on: say what restarted it (bit 0 = power-on reset)
            const uint32_t why = resetReason;
            resetReason = 0;
            if (why == 0 || (why & ~1u) == 0)
                return 0;
            return snprintf(out, len, "---- Twisted Tracker %s: restarted without a record, reset reason %08lx\r\n",
                            version, (unsigned long)why);
        }
        havePending = false;
        const Record& r = pending;
        static const char* const kTypes[] = { "?", "hard fault", "memory fault", "bus fault", "usage fault",
                                              "freeze (main loop stuck 10 s)", "out of heap memory" };
        const char* type = r.type < sizeof(kTypes) / sizeof(kTypes[0]) ? kTypes[r.type] : "?";
        const uint32_t exc = r.xpsr & 0x1FFu;
        int n = snprintf(out, len,
                         "---- Twisted Tracker %s: %s after %lu s\r\n"
                         "pc %08lx lr %08lx xpsr %08lx (%s %lu) sp %08lx\r\n"
                         "cfsr %08lx hfsr %08lx mmfar %08lx bfar %08lx size %lu heap free %lu\r\n"
                         "steps:",
                         version, type, static_cast<unsigned long>(r.uptimeMs / 1000), (unsigned long)r.pc,
                         (unsigned long)r.lr, (unsigned long)r.xpsr, exc ? "in interrupt" : "in main loop",
                         (unsigned long)exc, (unsigned long)r.sp, (unsigned long)r.cfsr, (unsigned long)r.hfsr,
                         (unsigned long)r.mmfar, (unsigned long)r.bfar, (unsigned long)r.size,
                         (unsigned long)r.heapFree);
        const uint32_t count = r.crumbHead < kCrumbs ? r.crumbHead : kCrumbs;
        for (uint32_t i = 0; i < count && n > 0 && n < len - 12; i++)
        {
            const uint32_t idx = (r.crumbHead - count + i) % kCrumbs;
            n += snprintf(out + n, len - n, " %lx", (unsigned long)r.crumbs[idx]);
        }
        if (n > 0 && n < len - 3)
            n += snprintf(out + n, len - n, "\r\n");
        return n;
    }

    __attribute__((naked)) void HardFault_Handler(void)
    {
        __asm volatile("tst lr, #4\n"
                       "ite eq\n"
                       "mrseq r0, msp\n"
                       "mrsne r0, psp\n"
                       "movs r1, #1\n"
                       "movs r2, #0\n"
                       "b twCrashCapture\n");
    }
    __attribute__((naked)) void MemManage_Handler(void)
    {
        __asm volatile("tst lr, #4\n"
                       "ite eq\n"
                       "mrseq r0, msp\n"
                       "mrsne r0, psp\n"
                       "movs r1, #2\n"
                       "movs r2, #0\n"
                       "b twCrashCapture\n");
    }
    __attribute__((naked)) void BusFault_Handler(void)
    {
        __asm volatile("tst lr, #4\n"
                       "ite eq\n"
                       "mrseq r0, msp\n"
                       "mrsne r0, psp\n"
                       "movs r1, #3\n"
                       "movs r2, #0\n"
                       "b twCrashCapture\n");
    }
    __attribute__((naked)) void UsageFault_Handler(void)
    {
        __asm volatile("tst lr, #4\n"
                       "ite eq\n"
                       "mrseq r0, msp\n"
                       "mrsne r0, psp\n"
                       "movs r1, #4\n"
                       "movs r2, #0\n"
                       "b twCrashCapture\n");
    }
}
