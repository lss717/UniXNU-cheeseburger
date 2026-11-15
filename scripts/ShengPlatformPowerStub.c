// Bring-up stub for the native UFS root boot flag.
//
// The firmware leaves the MSM8998 UFS controller powered and clocked for the
// warm-link path. The full NativeUfs driver performs its own controller
// bring-up, so this stub only satisfies the bsd_init integration point; it is
// not the final PMGR implementation.
#include <stdint.h>
#include <stdbool.h>

void sheng_pmgr_run(void)
{
}

// UFS negative-control build: NativeRamBlock finds these symbols, but mode 0
// keeps the Data disk entirely volatile. The formal runtime archive replaces
// this stub in the UFS component matrix.
unsigned native_ufs_store_mode(void)
{
    return 0;
}

int native_ufs_store_open(uint64_t first_lba, uint64_t *window,
    const char **reason)
{
    (void)first_lba;
    (void)window;
    if (reason != 0) {
        *reason = "UFS store disabled in negative-control build";
    }
    return 0;
}

int native_ufs_store_read(uint64_t block, uint32_t count, void *data)
{
    (void)block;
    (void)count;
    (void)data;
    return 0;
}

int native_ufs_store_write(uint64_t block, uint32_t count, const void *data)
{
    (void)block;
    (void)count;
    (void)data;
    return 0;
}

int native_ufs_store_write_cached(uint64_t block, uint32_t count,
    const void *data)
{
    (void)block;
    (void)count;
    (void)data;
    return 0;
}

int native_ufs_store_sync(void)
{
    return 0;
}

unsigned native_ufs_store_gpt_check(void)
{
    return 0;
}

// The iPadOS 17.7.11 profile does not carry payload-addon-release.patch.
// NativeRamBlock probes this symbol as a weak import and falls back to keeping
// the seed mapped when it is absent. The stub makes that fallback explicit.
bool ml_static_mfree_prepare(uint64_t address, uint64_t size)
{
    (void)address;
    (void)size;
    return false;
}
