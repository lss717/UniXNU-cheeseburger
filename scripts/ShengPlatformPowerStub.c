// Bring-up stub for the native UFS root boot flag.
//
// The firmware leaves the MSM8998 UFS controller powered and clocked for the
// warm-link path. The full NativeUfs driver performs its own controller
// bring-up, so this stub only satisfies the bsd_init integration point; it is
// not the final PMGR implementation.
void sheng_pmgr_run(void)
{
}
