// Early OP5 CPU/OSM bring-up. This file is linked into bsd_init through
// sheng_pmgr_run(), which runs before vfs_mountroot. The late display probe
// is too late for the nx_mount stall, so the CPRh/OSM programming must happen
// here. The tables are captured from the same phone running Android/TWRP;
// the code is an independent register-level implementation.
#include <stdint.h>
#include <mach/vm_types.h>
#include <kern/debug.h>
#include "cpu-tables.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern "C" int PE_parse_boot_argn(const char *, void *, int);
extern "C" int kprintf(const char *, ...);
#ifdef panic
#undef panic
#endif
extern "C" void panic(const char *, ...) __attribute__((noreturn));

extern "C" void cheeseburger_ufs_register(void) __attribute__((weak));
extern "C" void cheeseburger_ufs_register(void) {}
extern "C" void cheeseburger_pmgr_startup(void);

namespace {

static void write32(volatile uint32_t *base, uint32_t offset, uint32_t value) {
    base[offset / 4] = value;
    __asm__ volatile ("dmb oshst" ::: "memory");
}

static uint32_t read32(volatile uint32_t *base, uint32_t offset) {
    uint32_t value = base[offset / 4];
    __asm__ volatile ("dmb oshld" ::: "memory");
    return value;
}

static void set32(volatile uint32_t *base, uint32_t offset,
                  uint32_t mask, uint32_t value) {
    uint32_t old = read32(base, offset);
    write32(base, offset, (old & ~mask) | (value & mask));
}

static bool programCprh(volatile uint32_t *cprh, unsigned cluster, bool enableLoop) {
    const uint32_t *words = cluster ? kPerfCornerWords : kPwrCornerWords;
    const unsigned count = cluster ? kPerfCornerCount : kPwrCornerCount;
    const uint32_t baseQuot = cluster ? kPerfBaseQuot : kPwrBaseQuot;

    write32(cprh, 0x0a0 + 4 * 11, 19);
    write32(cprh, 0x00c, 96000);
    write32(cprh, 0x004, 0x21e);
    write32(cprh, 0x014, cluster ? (9U | (14U << 6)) : (11U | (12U << 6)));
    for (unsigned sensor = 0; sensor < (cluster ? 9U : 6U); ++sensor)
        write32(cprh, 0x200 + 4 * sensor, 0);
    write32(cprh, 0x808, 0x4202);
    write32(cprh, 0x80c, 1U << 11);
    for (unsigned ro = 0; ro < 16; ++ro)
        write32(cprh, 0x840 + 4 * ro, ro == 11 ? baseQuot : 0);
    for (unsigned i = 0; i < count; ++i)
        write32(cprh, 0x3a00 + 4 * i, words[i]);
    set32(cprh, 0x7f8, 0xfc01f197U, 0x4090U);
    set32(cprh, 0x7a4, 0x3ffU, 0x21U);
    set32(cprh, 0x7a8, 0x1ffc0000U, 33U << 18);
    if (!enableLoop)
        return true;
    set32(cprh, 0x3aa0, 0x1ffe07ffU,
          (4U << 25) | (5U << 17) | (88U << 1) | 1U);
    set32(cprh, 0x004, 1U, 1U);
    return (read32(cprh, 0x004) & 1U) != 0;
}

static bool programOsm(volatile uint32_t *osm, unsigned cluster) {
    const OsmRow *rows = cluster ? kPerfOsmRows : kPwrOsmRows;
    const unsigned active = cluster ? kPerfActive : kPwrActive;
    const uint32_t off = cluster * 0x2000U;
    const uint32_t desired = cluster ? 29U : 21U;  // 2.3616 / 1.9008 GHz

    for (unsigned i = 0; i < 40; ++i) {
        const OsmRow &row = rows[i < active ? i : active - 1];
        const uint32_t base = off + 0x1150 + i * 32;
        write32(osm, base, i);
        write32(osm, base + 4, row.frequency);
        write32(osm, base + 8, row.voltage);
        write32(osm, base + 12, row.override);
        write32(osm, base + 20, row.spare);
    }
    write32(osm, off + 0x1f00, 1U | (9U << 1) | (1U << 8));
    write32(osm, off + 0x101c, (200U << 16) | 200U);
    write32(osm, off + 0x100c, 1);
    write32(osm, off + 0x1024, 0);
    write32(osm, off + 0x1020, 0);
    write32(osm, off + 0x102c, 0xffffffffU);
    write32(osm, off + 0x1030, 0xffffffffU);
    write32(osm, off + 0x1034, read32(osm, off + 0x1034));
    const uint32_t boostTimers[3] = {0x1074, 0x1084, 0x1094};
    for (unsigned i = 0; i < 3; ++i) {
        write32(osm, off + boostTimers[i], (1000U << 16) | 2000U);
        write32(osm, off + boostTimers[i] + 4, (2000U << 16) | 2000U);
        set32(osm, off + boostTimers[i] + 8, 0xffffU, 200U);
    }
    write32(osm, off + 0x10a0, 5);
    write32(osm, off + 0x10ac, (100U << 16) | 100U);
    write32(osm, off + 0x10b0, (50U << 16) | 50U);
    write32(osm, off + 0x10bc, 1);
    write32(osm, off + 0x10a8, 1);
    write32(osm, off + 0x10b8, 100);
    set32(osm, off + 0x10a4, 0x807f007fU, 0x80020008U);
    set32(osm, off + 0x10c0, 1U, 1U);
    set32(osm, off + 0x1070, 0x3fU, 0x3fU);
    write32(osm, off + 0x1f10, desired);
    write32(osm, off + 0x1004, 1);
    return (read32(osm, off + 0x1004) & 1U) != 0;
}

static volatile uint32_t *mapDevice(uint64_t pa, uint32_t size) {
    vm_offset_t va = ml_io_map((vm_offset_t)pa, (vm_size_t)size);
    return va ? reinterpret_cast<volatile uint32_t *>(va) : nullptr;
}

static void waitMicroseconds(unsigned us) {
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (frequency));
    if (!frequency) frequency = 19200000;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (start));
    const uint64_t delta = (frequency / 1000000ULL) * us;
    do {
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (now));
    } while (now - start < delta);
}

static uint32_t measureCpuHz(void) {
    uint64_t pmcr = 0, enable = 1ULL << 31;
    __asm__ volatile ("mrs %0, pmcr_el0" : "=r" (pmcr));
    pmcr |= 1;
    __asm__ volatile ("msr pmcr_el0, %0" :: "r" (pmcr));
    __asm__ volatile ("msr pmcntenset_el0, %0" :: "r" (enable));
    __asm__ volatile ("isb" ::: "memory");
    uint64_t c0 = 0, c1 = 0, t0 = 0, t1 = 0, frequency = 0;
    __asm__ volatile ("mrs %0, pmccntr_el0" : "=r" (c0));
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (t0));
    waitMicroseconds(10000);
    __asm__ volatile ("mrs %0, pmccntr_el0" : "=r" (c1));
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (t1));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (frequency));
    if (!frequency) frequency = 19200000;
    uint64_t cycles = c1 - c0, ticks = t1 - t0;
    if (!cycles || !ticks) return 0;
    uint64_t hz = (cycles * frequency) / ticks;
    return hz > 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t>(hz);
}

static void earlyCpuFix(unsigned stage) {
    volatile uint32_t *osm = mapDevice(0x179c0000ULL, 0x4000);
    volatile uint32_t *cprh0 = mapDevice(0x179c8000ULL, 0x4000);
    volatile uint32_t *cprh1 = mapDevice(0x179c4000ULL, 0x4000);
    if (!osm || !cprh0 || !cprh1) {
        kprintf("NATIVE_CPUFREQ_EARLY: map failed osm=%p c0=%p c1=%p\n",
                osm, cprh0, cprh1);
        return;
    }
    kprintf("NATIVE_CPUFREQ_EARLY: stage=%u maps ok\n", stage);
    bool c0 = programCprh(cprh0, 0, stage >= 2);
    kprintf("NATIVE_CPUFREQ_EARLY: cprh0=%u\n", c0);
    bool c1 = programCprh(cprh1, 1, stage >= 2);
    kprintf("NATIVE_CPUFREQ_EARLY: cprh1=%u\n", c1);
    bool o0 = false, o1 = false;
    if (stage >= 2 && c0 && c1) {
        kprintf("NATIVE_CPUFREQ_EARLY: programming OSM\n");
        o0 = programOsm(osm, 0);
        o1 = programOsm(osm, 1);
    }
    uint32_t hz = measureCpuHz();
    kprintf("NATIVE_CPUFREQ_EARLY: done osm0=%u osm1=%u hz=%u\n", o0, o1, hz);
    unsigned panicMode = 0;
    if (PE_parse_boot_argn("native-cpufreq-early-panic", &panicMode, sizeof(panicMode))
        && panicMode)
        panic("NATIVE_CPUFREQ_EARLY: stage=%u cprh0=%u cprh1=%u osm0=%u osm1=%u hz=%u",
              stage, c0, c1, o0, o1, hz);
}

} // namespace

extern "C" void sheng_pmgr_run(void)
{
    cheeseburger_ufs_register();

    // The early fix is opt-in so the stable baseline remains unchanged.
    unsigned stage = 0;
    if (PE_parse_boot_argn("native-cpufreq-early", &stage, sizeof(stage)) && stage)
        earlyCpuFix(stage);

    cheeseburger_pmgr_startup();
}
