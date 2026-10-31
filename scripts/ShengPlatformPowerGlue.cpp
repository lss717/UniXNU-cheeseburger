// bsd_init integration point. The warm-link UFS archive is only linked in the
// full NativeUfs configuration; a weak default keeps the probe configuration
// linkable, and the strong warm-link definition overrides it when present.
extern "C" void cheeseburger_ufs_register(void) __attribute__((weak));
extern "C" void cheeseburger_ufs_register(void) {}
extern "C" void cheeseburger_pmgr_startup(void);
extern "C" void sheng_pmgr_run(void)
{
    cheeseburger_ufs_register();
    cheeseburger_pmgr_startup();
}
