// SPDX-License-Identifier: BSD-2-Clause-Patent
// Minimal MSM8998 UFS warm-link GPT/LUN probe for OnePlus 5 (cheeseburger).
//
// The controller is left enabled by the firmware.  This probe follows the
// proven DinnerABC/UniXNU-cheeseburger P8/P11 UFS path: attach as an
// IOPlatformDevice after the device tree is published, use a private
// physically-contiguous UTRL/UCD/data allocation, install that queue, send a
// NOP OUT first, then query LUN capacity and read LUN0's GPT with SCSI READ(10).
// It never reuses firmware descriptors, never programs an SMMU, and has no
// SCSI write/format/unmap path.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <sys/types.h>
#include <sys/param.h>
// This file is C++: XNU's smr.h uses the result of an assignment to a volatile
// pointer, which clang 16 reports as deprecated; the warning is not ours and the
// kernel build promotes warnings to errors.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-volatile"
/* proc_internal.h is C: without this the C++ translation unit asks for a mangled
 * "proc_iterate(...)" while the kernel exports _proc_iterate. */
extern "C" {
#include <sys/proc_internal.h>
}
#pragma clang diagnostic pop
#include <kern/debug.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

// H183: addon/owner-data state from bsd/kern/kern_exec.c. The boot's own console lines for the Data
// mount are not reliably captured by the log slots (the writer re-copies large stale regions), so the
// heartbeat samples these globals instead: the ALIVE lines appear in every capture.
extern "C" {
extern volatile int native_owner_data_stage;
extern volatile int native_owner_data_status;
extern volatile int native_gpu_addon_calls;
extern volatile int native_gpu_addon_files;
extern volatile int native_gpu_addon_written;
extern volatile int native_gpu_addon_same;
extern volatile int native_gpu_addon_failed;
extern volatile int native_gpu_addon_error;
extern volatile int native_gpu_addon_stage;
extern volatile int native_gpu_addon_status;
extern volatile int native_gpu_addon2_stage;
extern volatile int native_gpu_addon2_status;
extern volatile int native_gpu_addon3_stage;
extern volatile int native_gpu_addon3_status;
// H203: A540 accelerator ABI state (NativeA540AcceleratorProbe.cpp), printed by the heartbeat.
extern volatile int native_a540_abi_calls;
extern volatile int native_a540_abi_last_method;
extern volatile int native_a540_abi_reports;
extern char native_a540_abi_report[64];
// H210: addon source CRC failures and the addon block's physical address. The
// per-entry crc printf can fall inside the log writer's snapshot window and be
// lost, so the heartbeat carries the verdict as well.
extern volatile uint32_t native_gpu_addon_crc_bad;
extern uint64_t native_ram_block_gpu_addon_physical;
// H219: accelerator open/selector counts, written by NativeA540AcceleratorProbe.cpp.
extern volatile uint32_t native_a540_open_count;
extern volatile uint32_t native_a540_open_type;
extern volatile uint32_t native_a540_sel_count;
extern volatile uint32_t native_a540_sel_last;
}

static volatile uint64_t gUfsCommands;
// H305: every failed window read is what APFS surfaces as err 5 and what a process faults on.
volatile uint32_t native_ufs_read_failures;
static volatile uint64_t gUfsInFlight;
static volatile uint64_t gUfsLastLba;
static volatile uint64_t gUfsLastBytes;
static volatile uint64_t gUfsLastLoops;
static volatile uint64_t gUfsLastTicks;
static bool gNopActive;

#define UFS_TIMEOUT_TICKS 153600000ULL   // H361: 8 s; loaded boots run commands slower than 5.2 s
static volatile uint64_t gWindowRequests;
static volatile uint64_t gWindowBytes;
static volatile uint64_t gWindowInFlight;
static volatile uint64_t gWindowLastBlock;
static volatile uint64_t gWindowLastCount;
static volatile uint64_t gWindowLastReqBlock;
static volatile uint64_t gWindowLastReqCount;
static volatile uint64_t gWindowStartTicks;
static volatile uint32_t gH103LogMode;
static volatile uint64_t gH103LogFirst;
static volatile uint64_t gH103RootFirst;
static volatile uint64_t gH103RootLast;
static uint64_t gH391RootImageBytes;
static uint64_t gH391StoreFirst, gH391StoreBlocks;
static uint64_t gH103HeartbeatStart;
static volatile int gH103SkipPattern;
static uint32_t gH103SlotCursor;
// H163: every slot carries this text stamp so the pull tool can tell which
// boot wrote it.  The header alone is not enough: it is written after the
// slot, and a late header write failure leaves a stale slots_written behind,
// which made earlier logs mix slots from several boots.
#define H103_BUILD "H394"
static uint64_t gH103BootId;
static volatile int gH103Quiet;
static volatile uint64_t gH103LastSlotTicks;
static volatile int gH103SlotBusy;
// Kernel-lifetime scratch, serialized by gH103SlotBusy. Logging must not wait
// for heap allocation while the system is under startup memory pressure.
alignas(4096) static uint8_t gH103SlotBody[32U * 4096U];
alignas(4096) static uint8_t gH103SlotHeader[4096];
static volatile int gH103LastSlotMsgX = -1;
// H103fl diagnostics: 0=no slot writer, 1=default cadence, 2=2 s fast control.
static volatile int gH103SlotMode = 1;
// H106: ios-sweet 0069's batching. The flush runs at most once per interval,
// writes only the message bytes produced since the previous flush, and writes
// nothing while the kernel is quiet. h103-log-secs sets the interval (1..60).
static volatile uint32_t gH103LogSecs = 10;
// A 1 Hz work-loop timer drives the display heartbeat cell independently of
// UFS I/O, so a frozen cell means a real kernel/work-loop hang.
static volatile int gH103HbTimerOn;
static volatile uint64_t gH103StartTicks;
// H311: the heartbeat stamps this every beat; the sampler writes a "stall" marker into the
// reserved block when it has not moved for ten seconds, which distinguishes a hang from a panic.
static volatile uint64_t gH103BeatTicks;
// H312: set once the stall record is on disk, so the watchdog stops overwriting it with "alive".
static volatile int gH103StallRecorded;
// H317: set when a log flush lost the I/O lock, so the next flushes step aside for the main path.
static volatile uint64_t gH103FlushBackoffTicks;
static volatile uint32_t gH103FlushSkips;
static volatile uint32_t gH103SlotWriteErrors;
static volatile uint64_t gH103LastSlotSuccessTicks;
/* H289: the persistent ring stopped at 258 s in H288 while the system kept running for
 * another 300 s, so the flush path needs to say whether it is failing or merely idle. */
static uint32_t gH103FlushRounds, gH103FlushIdleRounds;
// H103fm: even when the message buffer is unchanged, rewrite the header every
// two seconds from the next completed read.  uptime_s then proves whether the
// read path was still alive after the last kernel log line.
static volatile uint64_t gH103LastHeaderTicks;
static volatile uint32_t gH103LastMsgBytes;
// timer_call lives in osfmk and is a C symbol. Declare it with C linkage here
// instead of including kern/timer_call.h, whose XNU declarations are C++ in
// this translation unit and would produce mangled names.
struct timer_call;
typedef struct timer_call *timer_call_t;
typedef void *timer_call_param_t;
typedef void (*timer_call_func_t) (timer_call_param_t, timer_call_param_t);
extern "C" {
timer_call_t timer_call_alloc (timer_call_func_t func, timer_call_param_t param0);
boolean_t timer_call_enter (timer_call_t call, uint64_t deadline, uint32_t flags);
boolean_t timer_call_cancel (timer_call_t call);
void timer_call_free (timer_call_t call);
}
static timer_call_t gH103TimerCall;
static volatile uint32_t gH103TimerFires;
static void h103TimerCallFunc (timer_call_param_t, timer_call_param_t);
// H103fn: post-root IOGPUFamily load probe. The display archive's A540 stub
// cannot be called before user space is up (A1 panicked); this only tests
// whether the kext can be loaded after SpringBoard exec, without touching the
// display archive.
extern "C" kern_return_t OSKextLoadKextWithIdentifier (const char *);
extern "C" volatile uint32_t native_boot_flags;
extern "C" volatile unsigned native_meta_serialize_count;
extern "C" volatile const char *native_meta_serialize_class;
static void h103ReportMetaSerialize (void);
static volatile int gH103LoadIogpu;
static volatile int gH103IogpuTried;
static volatile uint32_t gH103IogpuRc = 0xffffffffU;
static void h103MaybeLoadIogpu (void);
// H104e: publish the real prelinked IOAcceleratorES/IOGPU class after
// backboardd has exec'd. This does not register a local OSMetaClass.
extern "C" int native_iogpu_accel_probe (void);
static volatile int gH103IogpuProbeTried;
static volatile int gH103IogpuProbeRc = -1;
static void h103MaybePublishIogpu (void);
extern "C" {
volatile uint32_t native_h103_slot_state = 0;
}
class NativeUfsProbe;
static NativeUfsProbe *gH103Probe;
static void h103SlotWriteThunk (void);
static void h103HeaderWriteThunk (void);
static void h103FlushNow (void);
static void h103SlotTick (void);
static void h103ProbeIomfb (void);

// H103cg's probe periodically redrew the GOP test pattern from its heartbeat.
// The display archive is force-loaded into the kernel and exports this symbol;
// call it directly like the original code (a weak import can bind to null).
extern "C" int native_legacy_test_pattern(void);

// H104i: after backboardd is up, ask the XNU proc-diag helper to dump the
// display-side thread states. The helper lives in the kernel because the
// driver's public headers keep struct task/thread incomplete.
extern "C" void native_proc_state_dump(void);
// H124: the full per-process wait graph is only printed at the later stages;
// the +2 s snapshot proved the ipc_mqueue_full block there is a startup
// transient, so the extra ~70 lines are not worth the console time early on.
extern "C" void native_proc_wait_graph(void);
// H310: the stage-2 dump flushes around itself so a stall is visible; the hook is defined
// further down, so it needs a forward declaration here.
extern "C" void native_log_flush_hook(void);
// H315: the display archive paints the panel solid on panic; weak because a build without that
// archive must still link.
extern "C" void native_legacy_panic_paint (uint32_t color) __attribute__((weak_import));
// H343: the panel progress bar, drawn from the watchdog thread.
extern "C" void native_legacy_progress (unsigned step, unsigned total) __attribute__((weak_import));

// H108: independent heartbeat. ios-sweet's log thread sleeps on tsleep, not
// IOSleep; this port never woke from IOSleep, so this thread first proves
// whether tsleep wakes here. When it does, it flips only the 16x16 heartbeat
// cell (no full 2x2 redraw), runs any due dump and flushes that dump, so the
// diagnostics no longer depend on UFS read traffic.
// H109: tsleep does not wake either (only "NATIVE_TICK: thread rc=0" appears).
// The heartbeat and the dump wakeup now come from a timer_call, which runs
// from the timer interrupt and needs no thread timer at all.
extern "C" void native_legacy_heartbeat_cell(void);
extern "C" void native_legacy_slot_cell(void);
// H223: the GPUCC survey runs at the late probe (~35 s), before the persistent log ring starts
// capturing (~40 s); repeat it once from the heartbeat so the values reach the pulled log.
extern "C" int sheng_gpucc_next(unsigned *index, uint32_t *value);
// C1b: one GPU power-up step per beat, gated by sheng-gpu-power=1 inside the callee.
extern "C" int sheng_gpu_power_step(void);
// H115: display-side event counters and the first-frame content scan. The
// client frame is scanned (nonblack count and bounding box) while the CPU copy
// runs, so a black screen can be attributed to a black client frame.
extern "C" volatile uint32_t native_legacy_client_calls;
extern "C" volatile uint32_t native_legacy_transfer_calls;
extern "C" volatile uint32_t native_legacy_frame_count;
extern "C" volatile uint32_t native_legacy_swap_validation_calls;
extern "C" unsigned native_legacy_client_stats(uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*) __attribute__((weak_import));
extern "C" volatile uint32_t native_legacy_frame_kind[32];
extern "C" volatile uint64_t native_legacy_frame_stats[32][5];

// H111: ArmCpu counts every INTID 27 (virtual timer) it takes before calling
// rtclock_intr(). A zero count after minutes of uptime means the timer
// interrupt itself never reaches the kernel, which would explain IOSleep,
// tsleep and timer_call all failing to wake anything.
extern "C" volatile uint64_t armcpu_timer_interrupts;
// H127b: which candidate timer PPI (19 or 27) actually reaches EL1, plus the
// storm guard's last masked INTID.
extern "C" volatile uint64_t armcpu_timer19_interrupts;
extern "C" volatile uint64_t armcpu_timer27_interrupts;
extern "C" volatile uint32_t armcpu_irq_storm_id;
extern "C" volatile uint64_t armcpu_irq_storm_count;
extern "C" volatile uint64_t armcpu_timer19_rtclock_count;
extern "C" volatile uint64_t armcpu_timer19_forced_count;
extern "C" volatile uint64_t armcpu_ipi_interrupts;
// H114: ArmCpu export that reads the boot CPU's redistributor PPI 27 state.
extern "C" void armcpu_probe_ppi27 (void);
extern "C" volatile uint64_t armcpu_ppi_probe[13];

static int gH109Chan;
static volatile uint32_t gH109Ticks;
static timer_call_t gH109TimerCall;
// H117: h114-gic=0 keeps the H114 redistributor/ICC reads out of the A/B run.
// H118/H120: the GIC values are cached at init; no runtime GIC/ICC reads.
static volatile uint32_t gH114GicProbe = 1;
// H110: h109-tick=0 keeps the timer_call off so the same kernel can be booted
// with and without the interrupt heartbeat.
static volatile uint32_t gH110TickEnabled = 1;
static volatile uint32_t gH110WorkerWakes;

// H105: this thread never wakes from IOSleep on this kernel. No H103/H104
// capture contains a single NATIVE_UFS_ALIVE line after NATIVE_UFS_HB: enter,
// so the H104i/H104j dump placed inside the heartbeat loop below could never
// run. Drive the dump from the UFS completion and read paths instead: they are
// proven alive (NATIVE_UFS_PROGRESS). Three snapshots follow the gate, so the
// state just after exec and the later stall are both recorded.
// H107: the gate also opens 60 s after SpringBoard exec when backboardd never
// runs, so a green boot leaves evidence too.
static volatile uint64_t gH105GateTicks;
static volatile uint64_t gH107SpringBoardTicks;
static volatile int gH105DumpStage;

static bool
h107GateOpen (void)
{
  if (native_boot_flags & 4) {
    return true;
  }
  if (!(native_boot_flags & 8)) {
    return false;
  }
  uint64_t now = mach_absolute_time ();
  if (!gH107SpringBoardTicks) {
    gH107SpringBoardTicks = now;
    return false;
  }
  // The launchd ordering race (~200-230 s) is decided long before this.
  return now - gH107SpringBoardTicks >= 60 * 19200000ULL;
}

// H360: doorbell/recovery accounting for the stall work (see the UFS plan document).
static volatile uint64_t gUfsDbTimeouts, gUfsRecovered, gUfsWriteFailed;
static void h103Marker (const char *kind, bool urgent);
// H358: the AppleKeyStore driver publishes the keybag service keybagd looks for; weak so a
// kernel built without that archive still links.
extern "C" IOService * native_keyevents_allocate (void) __attribute__((weak_import));
extern "C" void cheeseburger_usb_probe_start (void) __attribute__((weak_import));
static IOService *gNativeProvider;
static bool gNativeKeystoreDone;

static void nativePublishKeystore (void)
{
  // H358: restore the publish.  The sibling port's record ties a backboardd stuck in a Mach
  // receive to a missing persona keybag, and our logs show the sandbox finding no container
  // paths at all, so the keystore is the first thing to put back.  Still opt-in.
  static int publishWanted = -1;
  if (publishWanted < 0) {
    int value = 0;
    publishWanted = PE_parse_boot_argn ("sheng-aks-publish", &value, sizeof value) && value ? 1 : 0;
    IOLog ("NATIVE_AKS: gate=%d\n", publishWanted);
  }
  if (!publishWanted) {
    return;
  }
  if (gNativeKeystoreDone) {
    return;
  }
  gNativeKeystoreDone = true;
  if (native_keyevents_allocate == nullptr) {
    IOLog ("NATIVE_AKS: driver not linked\n");
    return;
  }
  IOService * keystore = native_keyevents_allocate ();
  IOService * parent = gNativeProvider != nullptr ? gNativeProvider : nullptr;
  if (keystore != nullptr && parent != nullptr && keystore->init ()) {
    if (keystore->attach (parent)) {
      keystore->start (parent);
      IOLog ("NATIVE_AKS: AppleKeyStore published (late)\n");
    } else {
      IOLog ("NATIVE_AKS: attach failed\n");
      keystore->release ();
    }
  } else {
    IOLog ("NATIVE_AKS: allocation failed\n");
  }
}

// H311: ten seconds without a heartbeat beat means the heartbeat thread is stuck.  The sampler
// keeps running on its own, so it leaves a "stall" record in the reserved block; finding that
// record instead of a "panic" one says the machine hung rather than panicked.
static void h103Marker (const char *kind, bool urgent);  // defined after the driver class is complete

static void h103MaybeKillExclavekit (unsigned seconds);
// H368: the SpringBoard restart probe was written but never wired up, so the "relaunch against a
// warm launchservicesd" experiment never actually ran.
static void h103MaybeRestartSpringBoard (uint32_t displayClients);

static void
h103StallCheck (void)
{
  static uint64_t reportedAt;
  uint64_t now = mach_absolute_time ();
  if (!gH103BeatTicks || now - gH103BeatTicks < 10 * 19200000ULL) {
    return;
  }
  if (reportedAt && now - reportedAt < 30 * 19200000ULL) {
    return;
  }
  reportedAt = now;
  h103Marker ("stall", true);
  gH103StallRecorded = 1;
}

// H312: the sampler thread ends around 550 s, which is exactly the gap that left H311 silent at
// 933 s with no record at all.  This thread never returns: it overwrites the marker block with
// "alive" once a minute (proving the marker path itself works) and stops stamping once a stall
// has been recorded, so the stall is not erased.
// H313: the ring only keeps its last slots, so the "client enter" lines that say who registered
// on the legacy display are long gone by the time a boot dies.  Dump the whole client table - it
// carries pid, comm and the per-selector call counts - from the watchdog at fixed points.
extern "C" unsigned native_legacy_clients_dump (char *out, unsigned cap);

static void
h103DumpDisplayClients (void)
{
  static char buffer[1200];
  buffer[0] = 0;
  unsigned count = native_legacy_clients_dump (buffer, sizeof buffer);
  IOLog ("NATIVE_LEGACY_CLIENTS: n=%u %s\n", count, buffer);
}

static void
nativeWatchdogThread (void *, wait_result_t)
{
  unsigned ticks = 0;
  unsigned seconds = 0;
  unsigned half = 0;
  for (;;) {
    IOSleep (500);
    half++;
    if ((half & 1U) == 0U) {
      seconds++;
      // H343: how far the bar got is how long the machine lived, whatever the log does.
      if (seconds <= 900 && native_legacy_progress != nullptr) {
        native_legacy_progress (seconds, 900);
      }
    } else {
      // H343: the first ten seconds are stamped every half second; these are the only records
      // that exist before the ring's first write is allowed.
      if (half <= 20) {
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
          h103Marker ("boot", true);
          IOSleep (5);
        }
      }
      continue;
    }
    // Keep persistence independent of process sampling and display callbacks.
    native_log_flush_hook ();
    h103StallCheck ();
    h103MaybeKillExclavekit (seconds);
    h103MaybeRestartSpringBoard (native_legacy_client_calls);
    if (gH103StallRecorded) {
      continue;
    }
    // H342: the first alive stamp waited a full minute, so a boot that died around the display
    // start (40-50 s) left no trace at all and the block still held an older boot's text.  Stamp
    // early and often in the first half minute, on the unlocked path, so the death time is exact.
    if (seconds == 5 || seconds == 15 || seconds == 30) {
      h103Marker ("alive", true);
    }
    if (seconds == 30) {
      nativePublishKeystore ();
    }
    if (seconds == 120 || seconds == 300 || seconds == 600 || seconds == 900) {
      h103DumpDisplayClients ();
    }
    if (++ticks >= 60) {
      ticks = 0;
      h103Marker ("alive", false);
    }
  }
}

static bool
h105DumpTick (void)
{
  if (gH105DumpStage >= 3 || !h107GateOpen ()) {
    return false;
  }
  // H310: this dump runs on the heartbeat thread, which is also the only thing that flushes
  // the persistent ring, so any stall here takes the record with it: H309 ended exactly at
  // "trigger stage=2" with not even the dump's own "begin" line on disk.  It is an H105-era
  // diagnostic, so it is off unless the boot explicitly asks for it.
  {
    static int dumpWanted = -1;
    if (dumpWanted < 0) {
      int value = 0;
      dumpWanted = PE_parse_boot_argn ("h105-dump", &value, sizeof value) && value ? 1 : 0;
    }
    if (!dumpWanted) {
      return false;
    }
  }
  uint64_t now = mach_absolute_time ();
  if (!gH105GateTicks) {
    gH105GateTicks = now;
    return false;
  }
  uint64_t elapsed = now - gH105GateTicks;
  unsigned stage = 0;
  if (gH105DumpStage == 0 && elapsed >= 2 * 19200000ULL) {
    stage = 1;
  } else if (gH105DumpStage == 1 && elapsed >= 12 * 19200000ULL) {
    stage = 2;
  } else if (gH105DumpStage == 2 && elapsed >= 60 * 19200000ULL) {
    stage = 3;
  }
  if (!stage) {
    return false;
  }
  gH105DumpStage = stage;
  IOLog ("NATIVE_PROC_DUMP: trigger stage=%u flags=%x cmds=%llu elapsed_ms=%llu%s\n",
         stage, native_boot_flags, (unsigned long long) gUfsCommands,
         (unsigned long long) (elapsed / 19200),
         (native_boot_flags & 4) ? "" : " fallback");
  // H310: flush before and inside the dump so a stall is visible instead of silent.
  native_log_flush_hook ();
  native_proc_state_dump ();
  native_log_flush_hook ();
  if (stage >= 2) {
    native_proc_wait_graph ();
    native_log_flush_hook ();
  }
  return true;
}

// H103ea keeps H103bd's 10-second heartbeat byte-for-byte: the ALIVE console
// line is written first, then the 2x2 panel. H103bd was built exactly this way
// and is the only confirmed 2x2 image; changing the interval or dropping the
// ALIVE line was not proven safe.
// H175: CoreAnimation reads MetalPluginName from the matched
// IOAcceleratorES service and then loads
// /System/Library/Extensions/<name>.bundle.  Adding this to the display
// archive's accelerator object produced a bad prelink artifact (H173 panicked
// in AppleM2ScalerCSCDriver), so the property is injected from here instead:
// the heartbeat thread retries until the accelerator service appears, which is
// always before backboardd reads the property.
// H180: describe the service we are looking at: its real metaclass name, the
// registry name, whether it satisfies the Apple plugin's class match, and the
// retain count (so a later "no service" can be told apart from "wrong class").
static void
h103NoteAcceleratorService (const char *tag, IOService *service)
{
  if (!service) {
    IOLog ("NATIVE_UFS_ACCEL: %s none\n", tag);
    return;
  }
  const OSMetaClass *mc = service->getMetaClass ();
  const char *name = service->getName ();
  IOLog ("NATIVE_UFS_ACCEL: %s class=%s name=%s agx=%d refs=%d\n",
         tag, mc ? mc->getClassName () : "-", name ? name : "-",
         service->metaCast ("AGXAccelerator") ? 1 : 0, service->getRetainCount ());
}

static void
h103ProbeAcceleratorService (void)
{
  static unsigned beats;
  if ((beats++ % 10) != 0) {
    return;
  }
  OSDictionary *matching = IOService::serviceMatching ("IOAcceleratorES");
  IOService *service = matching ? IOService::copyMatchingService (matching) : nullptr;
  h103NoteAcceleratorService ("probe", service);
  OSSafeReleaseNULL (service);
  OSSafeReleaseNULL (matching);
}

static void
h103MaybeSetAcceleratorPlugin (void)
{
  static int done;
  if (done) {
    return;
  }
  unsigned enabled = 0;
  if (!PE_parse_boot_argn ("offline-accelerator", &enabled, sizeof enabled) ||
      enabled != 1) {
    done = 1;
    return;
  }
  char plugin[17];
  int fromArg = 0;
  if (PE_parse_boot_argn ("_offline-accelerator-plugin", plugin, sizeof plugin) &&
      plugin[0]) {
    fromArg = 1;
  } else {
    strlcpy (plugin, "AGXMetalA10", sizeof plugin);
  }
  OSDictionary *matching = IOService::serviceMatching ("IOAcceleratorES");
  IOService *service = matching ? IOService::copyMatchingService (matching) : nullptr;
  if (!service) {
    OSSafeReleaseNULL (matching);
    return;
  }
  h103NoteAcceleratorService ("inject", service);
  bool set = service->setProperty ("MetalPluginName", plugin);
  // H198: the sandbox grant must be on the instance CoreAnimation actually opens. The display stub
  // sets it in start(), but a service re-published later did not carry it (H197: grant matches at
  // 260/401 s, backboardd MACF refusals at 340/349 s). Set it on the matched service as well.
  service->setProperty ("NativeSandboxGrantProcess", "backboardd");
  IOLog ("NATIVE_UFS_ACCEL: service=%p MetalPluginName=%s source=%s set=%u\n",
         service, plugin, fromArg ? "arg" : "default", set ? 1 : 0);
  OSSafeReleaseNULL (service);
  OSSafeReleaseNULL (matching);
  done = 1;
}

// H236hw: SpringBoard asks QuartzCore for the main CADisplay before backboardd's render server
// exists, and QuartzCore caches that empty answer for the life of the process (ensure_displays'
// static flag, QuartzCore 0x18948d2c0).  On this board launchd starts SpringBoard at ~115 s but
// backboardd only at ~131 s, whose window server builds the IOMFB display at ~150 s.  One
// SIGTERM makes launchd relaunch SpringBoard after the server publishes displays (it publishes
// LCD/primary plus five wireless ones).  Gated on the GOP display being the active backend.
extern "C" void psignal (proc_t, int);

static int
springboard_filter (proc_t p, void *)
{
  return p && strncmp (p->p_comm, "SpringBoard", MAXCOMLEN) == 0;
}

static int
springboard_restart (proc_t p, void *)
{
  IOLog ("NATIVE_RESTART: SpringBoard pid=%d sigterm\n", proc_pid (p));
  psignal (p, SIGTERM);
  return PROC_RETURNED;
}

static void
h103MaybeRestartSpringBoard (uint32_t displayClients)
{
  static int enabled = -1, sawClient, waited, done;
  if (done) {
    return;
  }
  if (enabled < 0) {
    // H369: H368 proved the gate now runs, but launchd did not bring SpringBoard back within the
    // 90 s we could watch, so killing it by default leaves the machine without a SpringBoard at
    // all. Keep it behind its own argument and leave the display baseline untouched.
    int wanted = 0;
    enabled = PE_parse_boot_argn ("sheng-restart-sb", &wanted, sizeof wanted) && wanted ? 1 : 0;
    IOLog ("NATIVE_RESTART: gate=%d\n", enabled);
  }
  if (!enabled) {
    done = 1;
    return;
  }
  if (!displayClients) {
    return;
  }
  if (!sawClient) {
    sawClient = 1;
    IOLog ("NATIVE_RESTART: display client seen\n");
    return;
  }
  // three 10 s beats after backboardd opened the display: its window server has finished the
  // IOMFB handshake by then (the opens and the Metal device both happen inside it).
  if (++waited < 3) {
    return;
  }
  done = 1;
  proc_iterate (PROC_ALLPROCLIST, springboard_restart, NULL, springboard_filter, NULL);
  IOLog ("NATIVE_RESTART: sent\n");
}

// H356: launchd's boot-task sequence stops at init-exclavekit ("Doing boot task" and never
// "Finished boot task"), and every boot task after it is skipped.  The helper is an exclave
// initialiser, which this board has no hardware for, so if it is still alive minutes later it is
// never going to finish.  Kill it once so launchd records a failure and moves on.  Gated so the
// baseline stays reproducible.
static int
a540ExclavekitFilter (proc_t p, void *)
{
  return p && strncmp (p->p_comm, "init_exclavekit", MAXCOMLEN) == 0;
}

static int
a540ExclavekitKill (proc_t p, void *)
{
  IOLog ("NATIVE_EXCLAVE: pid=%d sigkill\n", proc_pid (p));
  psignal (p, SIGKILL);
  return PROC_RETURNED;
}

static void
h103MaybeKillExclavekit (unsigned seconds)
{
  static int enabled = -1, done;
  if (done) {
    return;
  }
  if (enabled < 0) {
    int value = 0;
    enabled = PE_parse_boot_argn ("native-kill-exclavekit", &value, sizeof value) && value ? 1 : 0;
    IOLog ("NATIVE_EXCLAVE: gate=%d\n", enabled);
  }
  if (!enabled) {
    return;
  }
  if (seconds < 200) {
    return;
  }
  done = 1;
  proc_iterate (PROC_ALLPROCLIST, a540ExclavekitKill, NULL, a540ExclavekitFilter, NULL);
  IOLog ("NATIVE_EXCLAVE: pass done\n");
}

// H236im: the sampling itself lives in the kernel (osfmk/kern/task.c, native_sb_pc_sample); the// driver only names the processes.  Kept in a dedicated thread exactly like il (which boots) and
// only the name list grew, so a boot failure here points at the list, not at a new mechanism.
extern "C" void native_sb_pc_sample (const char *name, unsigned sample);
extern "C" void native_sb_pc_sample_instance (const char *name, unsigned sample,
                                              unsigned instance);
extern "C" void native_sb_set_quiet (int on);
extern "C" void native_sb_trace_start (void);
extern "C" void native_sb_trace_dump (unsigned burst);
extern "C" volatile uint32_t native_data_persist_state, native_data_persist_header;
extern "C" uint32_t cs_text_fixup_parsed, cs_text_fixup_applied;
extern "C" void native_kernel_object_sample (unsigned sample);
extern "C" volatile uint64_t native_data_persist_first_lba, native_data_persist_present,
    native_data_persist_loaded, native_data_persist_writes, native_data_persist_errors;

// H241/H242: the first three are sampled every round.  The comm of the container daemon is
// "containermanager" (not "containermanagerd"), which is why earlier rounds never matched it.
static const char *const a540SbNames[] = {
  "SpringBoard", "lsd", "containermanager", "keybagd", "runningboardd", "usermanagerd", "backboardd",
};

extern "C" void native_log_flush_hook (void);
// H308: kern_sig.c parks signal deliveries in a lock-free ring; printing them from here keeps
// printf (and its msgbuf mutex) out of proc_exit, which runs with preemption disabled.
extern "C" void native_sigbus_drain (void);

static void
a540SpringBoardSamplerThread (void *, wait_result_t)
{
  // H326: one boot needs the ring for the *userspace* side.  Our sampler alone writes thousands
  // of lines into the 256 slots, so SpringBoard's and launchservicesd's own messages never
  // survive.  native-probe-quiet=1 parks this thread and the heartbeat drops to once a minute.
  {
    static int quiet = -1;
    if (quiet < 0) {
      int value = 0;
      quiet = PE_parse_boot_argn ("native-probe-quiet", &value, sizeof value) &&
          value >= 1 && value <= 2 ? value : 0;
    }
    if (quiet == 1) {
      IOLog ("NATIVE_SBPC: sampler parked (native-probe-quiet=1)\n");
      for (;;) {
        IOSleep (60000);
      }
    }
    if (quiet == 2) {
      IOLog ("NATIVE_SBPC: late sampler active (native-probe-quiet=2)\n");
      IOSleep (240000);
      for (unsigned sample = 0;; sample++) {
        native_kernel_object_sample (6000 + sample * 30);
        native_sb_pc_sample ("SpringBoard", 6000 + sample * 30);
        // The kernel emits non-SpringBoard frame chains only for sample % 30 == 0.
        native_sb_pc_sample ("lsd", 7020 + sample * 30);
        // Follow both per-user and system services; ordinal is not a fixed UID.
        native_sb_pc_sample_instance ("lsd", 10020 + sample * 30, 1);
        native_sb_pc_sample ("installd", 11010 + sample * 30);
        native_sb_pc_sample_instance ("containermanager", 8010 + sample * 30, 0);
        native_sb_pc_sample_instance ("containermanager", 9000 + sample * 30, 1);
        native_log_flush_hook ();
        IOSleep (60000);
      }
    }
  }
  // SpringBoard is exec'd between ~115 s and ~190 s on this board, after a slow launchd phase.
  // H239: sample for two minutes ten seconds apart, so the window covers SpringBoard's whole
  // life (it is killed by the plugin's restart probe at ~243 s).  Only SpringBoard repeats;
  // the other names are useful once and would otherwise push the ring log out.
  // H283: start early.  The image resolver has to land in the persistent ring before the
  // boot dies, and the last runs panic in the UFS path around 200-240 s, after the old
  // 150 s wait and the first bursts.  SpringBoard is exec'd from ~115 s on this board.
  IOSleep (60000);
  // H300: the addon decision happens around 31 s, before the persistent ring's first retained
  // slot (~33 s), so its lines are always lost.  Report the recorded outcome here instead.
  {
    extern volatile int native_owner_data_stage, native_owner_data_status;
    extern volatile int native_gpu_addon_files, native_gpu_addon_written, native_gpu_addon_same,
        native_gpu_addon_failed, native_gpu_addon_calls;
    IOLog ("NATIVE_GPU_ADDON_SUMMARY: stage=%d status=%d calls=%d files=%d written=%d same=%d failed=%d\n",
           (int) native_owner_data_stage, (int) native_owner_data_status,
           (int) native_gpu_addon_calls, (int) native_gpu_addon_files,
           (int) native_gpu_addon_written, (int) native_gpu_addon_same,
           (int) native_gpu_addon_failed);
  }
  // H271: register the scene-side pids before the bursts, otherwise the latch
  // can never fire (they are only sampled in the slow series, which starts
  // after the bursts are over).
  native_sb_pc_sample ("runningboardd", 1);
  native_sb_pc_sample ("backboardd", 1);
  // Ten four-second bursts separated by thirty seconds. Sample once per second
  // and capture peer stacks once per burst to bound diagnostic overhead.
  for (unsigned burst = 0; burst < 10; ++burst) {
    IOLog ("NATIVE_SBPC: burst=%u start\n", burst);
    native_sb_trace_start ();
    native_sb_set_quiet (1);
    for (unsigned b = 0; b < 4; ++b) {
      native_sb_pc_sample ("SpringBoard", 300 + burst * 30 + b);
      // H293: the persistent ring stops early in most boots (196 s in H292, 187 s in H289)
      // while this sampler keeps running past 500 s.  Flush from here as well, so the
      // record survives even when the heartbeat path stops calling h103FlushNow().
      if (b == 0) {
        native_log_flush_hook ();
        h103StallCheck ();
        // H313: SpringBoard's blocked receive has a port whose peer is lsd (pid 92), and H283
        // put the trap in libsystem_kernel's receive path, so the unanswered half of that RPC is
        // lsd.  Sample it once a second alongside SpringBoard to see where it is stuck.
        native_sb_pc_sample ("lsd", 700 + burst * 100 + b);
        // H320: SpringBoard reaches its first block right after runningboardd starts, and the
        // sampler never looked at runningboardd after that point, so the scene-side peer was
        // invisible.  Sample it in the bursts as well.
        native_sb_pc_sample ("runningboardd", 300 + burst * 100 + b);
        // H349: SpringBoard's blocked request in H348 pointed at launchd, so sample launchd too.
        // H353: the kernel only walks a frame chain for samples divisible by 30 (and only for
        // the first two threads), so the sample id must be a multiple of 30 as well.
        native_sb_pc_sample ("launchd", 600 + burst * 30);
        // H352: the request's receive right is still launchd's, i.e. it went to a MachService
        // that has not checked in.  backboardd is the service that has not opened the display
        // in this boot, so sample it too (sample ids divisible by 30 make the kernel walk its
        // frame chain).
        native_sb_pc_sample ("backboardd", 900 + burst * 30);
        // H354: backboardd is blocked in xpc_user_sessions_info to launchd's bootstrap port, and
        // the user-session database belongs to usermanagerd; cfprefsd is the other peer a
        // SpringBoard thread waits on.
        native_sb_pc_sample ("usermanagerd", 1200 + burst * 30);
        native_sb_pc_sample ("cfprefsd", 1500 + burst * 30);
        // H362: the gate moved to launchservicesd, whose sampled thread sits in Security.framework,
        // so take lsd's and securityd's frame chains on the same cadence as backboardd's.
        native_sb_pc_sample ("lsd", 1800 + burst * 30);
        native_sb_pc_sample ("securityd", 2100 + burst * 30);
        native_sb_pc_sample_instance ("containermanager", 2400 + burst * 30, 0);
        native_sb_pc_sample_instance ("containermanager", 2700 + burst * 30, 1);
      }
      IOSleep (1000);
    }
    native_sb_set_quiet (0);
    // H253: sample first, so the send-ring dump can compare the reply port of
    // each recorded send against the port the blocked thread is receiving on.
    native_sb_pc_sample ("SpringBoard", 900 + burst);
    native_sb_trace_dump (burst);
    IOLog ("NATIVE_SBPC: burst=%u done\n", burst);
    if (burst < 9) {
      // H303: the gaps between bursts were 30 s of no flushing at all, which is where the
      // persistent ring kept dying (187-242 s) while the system still had 300+ s to run.
      // Flush once a second across the whole gap instead.
      for (unsigned gap = 0; gap < 30; ++gap) {
        IOSleep (1000);
        h103StallCheck ();
        if (gap == 29) {
          native_sb_pc_sample ("runningboardd", 3000 + burst * 100 + gap);
          native_sb_pc_sample ("launchd", 4000 + burst * 100 + gap);
        }
        native_log_flush_hook ();
      }
    }
  }
  for (unsigned sample = 0; sample < 10; ++sample) {
    for (unsigned index = 0; index < sizeof a540SbNames / sizeof a540SbNames[0]; ++index) {
      // H261: runningboardd / backboardd are the scene-side peers now, so they
      // are sampled every round (the other daemons keep the old 2-round budget).
      if (index > 2 && sample >= 2 &&
          strcmp (a540SbNames[index], "runningboardd") != 0 &&
          strcmp (a540SbNames[index], "backboardd") != 0) {
        continue;
      }
      IOLog ("NATIVE_SBPC: call name=%s sample=%u\n", a540SbNames[index], sample);
      native_sb_pc_sample (a540SbNames[index], sample);
    }
    // H303: the slow series used to sleep ten seconds with no sync; keep the ring moving.
    for (unsigned gap = 0; gap < 10; ++gap) {
      IOSleep (1000);
      h103StallCheck ();
      native_log_flush_hook ();
    }
  }
  IOLog ("NATIVE_SBPC: done\n");
}

static void
heartbeatThread (void *, wait_result_t)
{
  gH103HeartbeatStart = mach_absolute_time ();
  IOLog ("NATIVE_UFS_HB: enter\n");
  unsigned beat = 0;
  for (;;) {
    // H157: a boot that dies in launchd is over in a few seconds, long before a
    // 10 s beat.  Beat once per second until the h107 gate opens so the early
    // flush in h103SlotTick() can persist the crash window.
    // H303: after the gate opened this slept 10 s, so a crash could lose ten seconds of the
    // record; that gap is where the ring kept ending (187-242 s). Beat every second and sync
    // the persistent ring every fifth beat, which bounds the loss to five seconds.
    IOSleep (1000);
    h103MaybeSetAcceleratorPlugin ();
    h103ProbeAcceleratorService ();
    beat++;
    // H311: stamped before the flush, so a stuck flush shows up as a stale beat.
    gH103BeatTicks = mach_absolute_time ();
    // H305: five seconds was still enough to lose the moment launchd faults; sync every beat.
    native_log_flush_hook ();
    native_sigbus_drain ();
    // H220: the long ALIVE line can be truncated inside a log slot, and its abi=/accel= fields
    // are the ones that matter here. This short line carries them alone, once per beat.
    // H225: two GPUCC registers per beat, inside the line that always survives the log window
    // (order: gpupll0 L/config/config_ctl/user_ctl/user_ctl_u/state/status, gx_clamp_io,
    // cx_bcr/cx_gdscr/cx_gds_hw_ctrl, cxo_branch, rbcpr_rcg/bcr/branch,
    // gfx3d_rcg, gx_bcr/gx_gdscr/gfx3d_branch, rbbmtimer_rcg/branch, isense_rcg/bcr/branch).
    // H236: sheng-gpu-power=2/3/4 runs the C1e-2 PM8005 S1 SPMI sequence; its results use
    // gc indices 80 and up and are emitted before the GPUCC survey.
    sheng_gpu_power_step ();
    unsigned gpucc_index = 0;
    uint32_t gpucc_value = 0;
    int gpucc_got = sheng_gpucc_next (&gpucc_index, &gpucc_value);
    uint32_t dpc=0,dca=0,dfa=0,ds6=0,drg=0,ds3=0,ds4=0,ds5=0;
    if (native_legacy_client_stats)
      native_legacy_client_stats (&dpc,&dca,&dfa,&ds6,&drg,&ds3,&ds4,&ds5);
    // H326: with native-probe-quiet=1 this line only appears once a minute, so a boot can be
    // read for the daemons' own messages instead of our counters.
    static int quietBeat = -1;
    if (quietBeat < 0) {
      int value = 0;
      quietBeat = PE_parse_boot_argn ("native-probe-quiet", &value, sizeof value) && value ? 1 : 0;
    }
    if (!quietBeat || (beat % 60) == 1) {
      uint32_t dataMode = 0, usbMode = 0;
      PE_parse_boot_argn ("sheng-data-persist", &dataMode, sizeof dataMode);
      PE_parse_boot_argn ("cheeseburger-usb", &usbMode, sizeof usbMode);
      IOLog ("NATIVE_DATA_STATUS: requested=%u state=%u header=%u first=%llx "
             "present=%llu loaded=%llu writes=%llu errors=%llu usb_requested=%u text=%u/%u\n",
             dataMode, (unsigned) native_data_persist_state,
             (unsigned) native_data_persist_header,
             (unsigned long long) native_data_persist_first_lba,
             (unsigned long long) native_data_persist_present,
             (unsigned long long) native_data_persist_loaded,
             (unsigned long long) native_data_persist_writes,
             (unsigned long long) native_data_persist_errors, usbMode,
             cs_text_fixup_parsed, cs_text_fixup_applied);
      IOLog ("NATIVE_ACCEL_HB: accel=%u/%u/%u/%u abi=%d/%d/%d rep=%.31s gc=%d:%08x "
             "dpy=%u/%u/%u/%u/%u s345=%u/%u/%u val=%u xf=%u fr=%u\n",
             (unsigned) native_a540_open_count, (unsigned) native_a540_open_type,
             (unsigned) native_a540_sel_count, (unsigned) native_a540_sel_last,
             (int) native_a540_abi_calls, (int) native_a540_abi_last_method,
             (int) native_a540_abi_reports, native_a540_abi_report,
             gpucc_got ? (int) gpucc_index : -1, gpucc_value,
             dpc,dca,dfa,ds6,drg,ds3,ds4,ds5,
             (unsigned) native_legacy_swap_validation_calls,
             (unsigned) native_legacy_transfer_calls,
             (unsigned) native_legacy_frame_count);
    }
    if (beat == 1 || (beat % 10) == 0) {
    IOLog ("NATIVE_UFS_ALIVE: winFlight=%llu ufsFlight=%llu cmds=%llu "
           "winReq=%llu winBytes=%llu lastCmd=%llx/%llu req=%llx/%llu "
           "addon=%d/%d/%d/%d/%d/%d owner=%d/%d ext=%d/%d dm=%d/%d wp=%d/%d "
           "abi=%d/%d/%d rep=%.31s gpup=%llx crcbad=%u accel=%u/%u/%u/%u\n",
           (unsigned long long) gWindowInFlight,
           (unsigned long long) gUfsInFlight,
           (unsigned long long) gUfsCommands,
           (unsigned long long) gWindowRequests,
           (unsigned long long) gWindowBytes,
           (unsigned long long) gUfsLastLba,
           (unsigned long long) gUfsLastBytes,
           (unsigned long long) gWindowLastReqBlock,
           (unsigned long long) gWindowLastReqCount,
           (int) native_gpu_addon_files,
           (int) native_gpu_addon_written,
           (int) native_gpu_addon_same,
           (int) native_gpu_addon_failed,
           (int) native_gpu_addon_error,
           (int) native_gpu_addon_calls,
           (int) native_owner_data_stage,
           (int) native_owner_data_status,
           (int) native_gpu_addon_stage,
           (int) native_gpu_addon_status,
           (int) native_gpu_addon2_stage,
           (int) native_gpu_addon2_status,
           (int) native_gpu_addon3_stage,
           (int) native_gpu_addon3_status,
           (int) native_a540_abi_calls,
           (int) native_a540_abi_last_method,
           (int) native_a540_abi_reports,
           native_a540_abi_report,
           (unsigned long long) native_ram_block_gpu_addon_physical,
           (unsigned) native_gpu_addon_crc_bad,
           (unsigned) native_a540_open_count,
           (unsigned) native_a540_open_type,
           (unsigned) native_a540_sel_count,
           (unsigned) native_a540_sel_last);
    }
    // H128: the process dumps and the persistent-log flush run here, in thread
    // context with no storage lock held.  They used to run from the UFS
    // completion path and from inside NativeUfsWindow's request lock, where
    // they could hold a block-storage request for tens of seconds.
    h103SlotTick ();
    {
      uint64_t now = mach_absolute_time ();
      uint64_t staleSeconds = gH103LogSecs > 1 ? gH103LogSecs * 3ULL : 3ULL;
      if (gH103LastSlotSuccessTicks &&
          now - gH103LastSlotSuccessTicks <= staleSeconds * 19200000ULL) {
        native_h103_slot_state = (beat & 1U) ? 4U : 2U;
      } else if (native_h103_slot_state == 2U || native_h103_slot_state == 4U) {
        native_h103_slot_state = 1U;
      }
      native_legacy_slot_cell ();
    }
    // H124/H128: the 2x2 repaint moved here with the dumps.  Repaint on a gate
    // bit change, and keep blinking until backboardd execs.
    {
      static uint32_t paintedFlags;
      uint32_t gate = native_boot_flags & 0xfff;
      if (!gH103SkipPattern && !gH103HbTimerOn &&
          (!(native_boot_flags & 4) || gate != paintedFlags)) {
        paintedFlags = gate;
        native_legacy_test_pattern ();
      }
    }
    uint64_t start = gWindowStartTicks;
    if (gWindowInFlight && start) {
      uint64_t waited = mach_absolute_time () - start;
      // H359: with the keystore running the disk load is far higher, and a queue
      // that hits an OCS timeout is recovered and retried.  Ten seconds of a
      // held request is therefore a symptom, not a death sentence: record it at
      // ten and only panic if the request is still held a minute later.
      if (waited > 10 * 19200000ULL) {
        static int logged;
        if (!logged) {
          logged = 1;
          h103Marker ("ufsstall", true);
          IOLog ("NATIVE_UFS_STALL_LOG: req=%llx/%llu cmds=%llu winBytes=%llu "
                 "lastCmd=%llx/%llu ufsFlight=%llu waited=%llu\n",
                 (unsigned long long) gWindowLastReqBlock,
                 (unsigned long long) gWindowLastReqCount,
                 (unsigned long long) gUfsCommands,
                 (unsigned long long) gWindowBytes,
                 (unsigned long long) gUfsLastLba,
                 (unsigned long long) gUfsLastBytes,
                 (unsigned long long) gUfsInFlight,
                 (unsigned long long) waited);
        }
      }
      if (waited > 30 * 19200000ULL) {
        IOLog ("NATIVE_UFS_STALL_NOPANIC: req=%llx/%llu cmds=%llu winBytes=%llu "
               "lastCmd=%llx/%llu ufsFlight=%llu",
               (unsigned long long) gWindowLastReqBlock,
               (unsigned long long) gWindowLastReqCount,
               (unsigned long long) gUfsCommands,
               (unsigned long long) gWindowBytes,
               (unsigned long long) gUfsLastLba,
               (unsigned long long) gUfsLastBytes,
               (unsigned long long) gUfsInFlight);
      }
    }
  }
}

// H103fm: timer_call runs from the timer interrupt, unlike IOTimerEventSource
// and IOSleep which depend on a kernel thread wakeup. It is only a diagnostic:
// h103-hb-timer=2 arms it and the 3rd cell then blinks from interrupt context.
static void
h103TimerCallFunc (timer_call_param_t, timer_call_param_t)
{
  gH103TimerFires = gH103TimerFires + 1;
  if (!gH103SkipPattern) {
    native_legacy_test_pattern ();
  }
  if (gH103TimerCall) {
    timer_call_enter (gH103TimerCall, mach_absolute_time () + 19200000ULL, 0);
  }
}

// H103fn: only trigger after SpringBoard has exec'd, i.e. after user space and
// kextd/driverkitd are running. Never call this pre-root.
static void
h103MaybeLoadIogpu (void)
{
  if (!gH103LoadIogpu || gH103IogpuTried) {
    return;
  }
  if (!(native_boot_flags & 8)) {
    return;
  }
  gH103IogpuTried = 1;
  IOLog ("NATIVE_IOGPU_LOAD: begin flags=%x\n", native_boot_flags);
  kern_return_t kr = OSKextLoadKextWithIdentifier ("com.apple.iokit.IOGPUFamily");
  gH103IogpuRc = static_cast<uint32_t> (kr);
  IOLog ("NATIVE_IOGPU_LOAD: rc=%x flags=%x\n", gH103IogpuRc,
         native_boot_flags);
}

static void
h103MaybePublishIogpu (void)
{
  if (gH103IogpuProbeTried || !(native_boot_flags & 4)) {
    return;
  }
  gH103IogpuProbeTried = 1;
  gH103IogpuProbeRc = native_iogpu_accel_probe ();
  IOLog ("NATIVE_IOGPU_PUBLISH: rc=%d flags=%x\n", gH103IogpuProbeRc,
         native_boot_flags);
}

// H103ea: the 2-second slot is a separate thread. H103bd's heartbeat is left
// at its proven 10-second cadence, so the persistent log cannot perturb the
// 2x2 redraw order that H103bd uses.
static void
h103SlotThread (void *, wait_result_t)
{
  if (!gH103SlotMode) {
    return;
  }
  native_h103_slot_state = 3;
  for (;;) {
    IOSleep (2000);
    if (native_boot_flags & 4) {
      h103FlushNow ();
    }
  }
}

#define MSM8998_UFS_HC_BASE 0x1da4000ULL
#define MSM8998_UFS_HC_SIZE 0x2500ULL
#define MSM8998_GCC_BASE 0x100000ULL
#define MSM8998_GCC_SIZE 0xb0000ULL

#define UFS_RSP_OFFSET 512U
#define UFS_PRDT_OFFSET 1024U
#define UFS_UCD_OFFSET 0x100U
#define UFS_FW_UCD_SIZE 0xF00U
#define UFS_FW_DATA_OFFSET 0x800U
#define UFS_FW_DATA_MAX 0x700U
#define UFS_DATA_MAX 0x20000U

// H171: the UFS read/log buffers must be physically contiguous below 4 GB (the
// controller here is addressed with a 32-bit physical mask).  A 128 KB request
// can fail once low memory is fragmented, and the old high-memory fallback
// then handed the controller an address it cannot use - APFS read garbage and
// trapped.  Retry with smaller contiguous runs and never exceed the mask.
static IOBufferMemoryDescriptor *
allocLowContiguous (IOOptionBits options, uint32_t wanted, uint32_t *capacity,
                    uint64_t *phys, IOByteCount *contiguous)
{
  static const uint32_t shares[] = {1, 2, 4, 8, 16, 32};
  for (unsigned i = 0; i < sizeof (shares) / sizeof (shares[0]); ++i) {
    uint32_t size = wanted / shares[i];
    if (size < 4096U) {
      break;
    }
    IOBufferMemoryDescriptor *dma =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask (
            kernel_task, options, size, 0x00000000ffffffffULL);
    if (!dma) {
      IOLog ("NATIVE_UFS_DMA: low size=%x unavailable\n", size);
      continue;
    }
    IOByteCount cont = 0;
    uint64_t where = dma->getPhysicalSegment (0, &cont, kIOMemoryMapperNone);
    if (!where || (where & 0xfffU) || cont < size) {
      IOLog ("NATIVE_UFS_DMA: low size=%x invalid phys=%llx cont=%llx\n", size,
             (unsigned long long) where, (unsigned long long) cont);
      dma->release ();
      continue;
    }
    IOLog ("NATIVE_UFS_DMA: low size=%x phys=%llx cont=%llx\n", size,
           (unsigned long long) where, (unsigned long long) cont);
    *capacity = size;
    *phys = where;
    *contiguous = cont;
    return dma;
  }
  return nullptr;
}

// H103 persistent log snapshot: SHLOG001 v3 header + 1 MiB kernel msgbuf in the
// last 32 MiB of userdata_ios. Written once after root; the boot log ring holds
// the early NATIVE_LEGACY lines. TWRP reads the window back read-only.
#define H103_LOG_BLOCKS 8192U
#define H103_LOG_MSG_BLOCKS 256U

struct H103LogHeader {
  char magic[8];
  uint32_t version, header_bytes;
  uint64_t boot_id;
  uint64_t first_lba;
  uint32_t window_blocks, msg_first, msg_blocks, msg_bytes;
  uint32_t slot_first, slot_blocks, slot_count, slots_written;
  uint32_t uptime_s, rounds, write_errors, gpt_open, gpt_last, state;
  char tag[32];
  uint32_t export_first, export_blocks, export_bytes, export_generation;
  uint32_t export_below;
};

struct H103MsgBuf {
  int msg_magic, msg_size, msg_bufx, msg_bufr;
  char *msg_bufc;
};
extern "C" struct H103MsgBuf *msgbufp;

static uint64_t
h103LogTicks (void)
{
  uint64_t value = 0;
  __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (value));
  return value;
}

static uint32_t
h103CopyMsgbuf (uint8_t *out)
{
  memset (out, 0, H103_LOG_MSG_BLOCKS * 4096U);
  struct H103MsgBuf *m = msgbufp;
  if (!m || !m->msg_bufc || m->msg_size <= 0) {
    return 0;
  }
  int size = m->msg_size, x = m->msg_bufx;
  if (x < 0 || x >= size) {
    x = 0;
  }
  uint32_t n = 0;
  for (int i = 0; i < size && n < H103_LOG_MSG_BLOCKS * 4096U; ++i) {
    char c = m->msg_bufc[(x + i) % size];
    if (c) {
      out[n++] = static_cast<uint8_t> (c);
    }
  }
  return n;
}


#define UFS_REG_IS 0x20U
#define UFS_REG_IE 0x24U
#define UFS_REG_HCS 0x30U
#define UFS_REG_HCE 0x34U
#define UFS_REG_UTRLBA 0x50U
#define UFS_REG_UTRLBAU 0x54U
#define UFS_REG_UTRLDBR 0x58U
#define UFS_REG_UTRLRSR 0x60U
#define UFS_REG_TASK_DOORBELL 0x78U
#define UFS_REG_AHIT 0x18U
#define UFS_REG_UICCMD 0x90U
#define UFS_REG_UICARG1 0x94U
#define UFS_REG_UICARG2 0x98U
#define UFS_REG_UICARG3 0x9cU
#define UFS_IS_UCCS 0x400U
#define UFS_IS_UEE 0x2U
#define UFS_UIC_DME_GET 0x01U
#define UFS_UIC_DME_HIBERNATE_EXIT 0x18U

#define UFS_FATAL_STATUS 0x71880U

struct UfsQueue {
  uintptr_t hc;
  uint8_t *list;
  uint8_t *ucd;
  uint8_t *data;
  uint64_t ucdPhys;
  uint64_t dataPhys;
  uint32_t dataCapacity;
  bool poisoned;
};

class NativeUfsProbe;

class NativeUfsLun : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeUfsLun)
  NativeUfsProbe *host = nullptr;
  uint8_t lun = 0;
  uint64_t last = 0;
  uint32_t sector = 0;

public:
  bool configure (NativeUfsProbe *parent, uint8_t unit, uint64_t lastLba, uint32_t blockSize);
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Qualcomm"); }
  char *getProductString () override { return const_cast<char *> ("MSM8998 UFS read-only"); }
  char *getRevisionString () override { return const_cast<char *> ("B5"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("NativeUfsProbe warm-link LUN"); }
  IOReturn reportBlockSize (UInt64 *value) override { *value = sector; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override { *value = last; return kIOReturnSuccess; }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    *present = true;
    if (changed) {
      *changed = false;
    }
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override { return kIOReturnNotWritable; }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return kIOReturnNotWritable; }
  IOReturn getWriteCacheState (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnNotWritable; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                             IOStorageAttributes *attributes,
                             IOStorageCompletion *completion) override;
};

class NativeUfsWindow : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeUfsWindow)
  NativeUfsProbe *host = nullptr;
  uint64_t absolute = 0;
  uint64_t bytes = 0;
  uint8_t *bounce = nullptr;
  uint8_t *readAhead = nullptr;
  uint64_t readAheadLba = 0;
  uint64_t readAheadBytes = 0;
  IOLock *lock = nullptr;
  char label[32] = {};
  bool online = false;

public:
  bool configure (NativeUfsProbe *parent, const char *name, uint64_t absoluteByte,
                  uint64_t length);
  bool start (IOService *) override { online = true; return true; }
  void free () override {
    if (readAhead) { IOFree (readAhead, UFS_DATA_MAX); readAhead = nullptr; }
    IOBlockStorageDevice::free ();
  }
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Native"); }
  char *getProductString () override { return label; }
  char *getRevisionString () override { return const_cast<char *> ("B5"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("SHNGROOT read-only window"); }
  IOReturn reportBlockSize (UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override { *value = bytes / 512 - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    *present = online;
    if (changed) {
      *changed = false;
    }
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnNotWritable; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                             IOStorageAttributes *attributes,
                             IOStorageCompletion *completion) override;
};

class NativeUfsProbe : public IOService {
  OSDeclareDefaultStructors (NativeUfsProbe)
  UfsQueue fQueue = {};
  OSSharedPtr<IOMemoryMap> fListMap;
  OSSharedPtr<IOMemoryMap> fUcdMap;
  IOLock *fLock = nullptr;
  bool fReady = false;
  uint64_t fLogFirst = 0;
  uint32_t fLogBlocks = 0;
  uint64_t fLogImageBytes = 0;
  IOBufferMemoryDescriptor *fLogDma = nullptr;
  uint8_t *fLogData = nullptr;
  uint64_t fLogDataPhys = 0;
  uint32_t fLogCapacity = 0;
  IOWorkLoop *fHbLoop = nullptr;
  IOTimerEventSource *fHbTimer = nullptr;
  uint64_t fHbNext = 0;
  static void logThread (void *, wait_result_t);
  static void hbTimerFired (OSObject *owner, IOTimerEventSource *sender);
  void hbTimerTick (IOTimerEventSource *sender);
  bool logWrite (uint64_t rel, uint32_t blocks, const void *src,
      uint64_t lockTicks = 38400000ULL);
  bool logSync (void);
  // H311: one-block record outside the ring so a dead machine still says whether it panicked
  // or stalled.  Block 250 sits in the reserved header area (slots begin at 257) and takes no
  // lock, so it can still land when the flush path is stuck.
  bool logMarker (const char *kind, bool urgent);

public:
  // H311: free-function access to the panic/stall marker (the panic hook is extern "C").
  void marker (const char *kind, bool urgent) { (void) logMarker (kind, urgent); }
  bool start (IOService *provider) override;
  void free () override;
  bool h103Snapshot (const char *tag, bool pause, bool verify);
  void h103SlotWrite (void);
  void h103HeaderOnly (void);
  void h103SingleWrite (void);
  void h103WindowRead (void);
  bool readBlocks (uint8_t unit, uint64_t block, uint64_t count, uint32_t blockSize,
                   IOMemoryDescriptor *dst, uint64_t *done);
  bool readWindowBulk (uint64_t lba, uint32_t blocks, void *out);
  bool readWindow4k (uint64_t lba, void *out);
};

// Read-only bridge; uses the active controller, never a second driver.
extern "C" unsigned native_ufs_store_mode (void)
{
  uint32_t mode = 0;
  PE_parse_boot_argn ("sheng-data-persist", &mode, sizeof mode);
  return mode == 1 ? 1 : 0; // Writable mode deliberately unsupported.
}

extern "C" int native_ufs_store_open (uint64_t blocks, uint64_t *first, const char **why)
{
  const char *reason = "off or writable mode unsupported";
  bool ok = false;
  if (native_ufs_store_mode () == 1 && gH103Probe && gH103RootFirst &&
      gH103RootLast >= gH103RootFirst && gH391RootImageBytes) {
    uint32_t mib = 1024;
    PE_parse_boot_argn ("sheng-log-reserve", &mib, sizeof mib);
    uint64_t partition = gH103RootLast - gH103RootFirst + 1;
    uint64_t reserve = uint64_t (mib) * 256;
    uint64_t image = (gH391RootImageBytes - 1) / 4096 + 1;
    reason = "invalid size, reserve, or root overlap";
    if (mib >= 288 && mib <= 16384 && reserve < partition && blocks &&
        blocks <= partition - reserve && blocks <= partition / 4 &&
        image <= partition - reserve - blocks &&
        partition - reserve - blocks - image >= 1024) {
      uint64_t start = gH103RootLast + 1 - reserve - blocks;
      reason = "store already open with different geometry";
      if (!gH391StoreBlocks || (gH391StoreFirst == start && gH391StoreBlocks == blocks)) {
        gH391StoreFirst = start;
        gH391StoreBlocks = blocks;
        if (first) *first = start;
        reason = "read-only";
        ok = true;
      }
    }
  }
  if (why) *why = reason;
  IOLog ("NATIVE_DATA_STORE: ro=%u first=%llx blocks=%llu reason=%s\n",
         ok ? 1 : 0, (unsigned long long) gH391StoreFirst,
         (unsigned long long) blocks, reason);
  return ok ? 1 : 0;
}

extern "C" int native_ufs_store_read (uint64_t block, uint32_t count, void *out)
{
  if (!gH103Probe || !out || !count || !gH391StoreBlocks ||
      block >= gH391StoreBlocks || count > gH391StoreBlocks - block) return 0;
  return gH103Probe->readWindowBulk (gH391StoreFirst + block, count, out) ? 1 : 0;
}

extern "C" int native_ufs_store_write (uint64_t, uint32_t, const void *) { return 0; }
extern "C" int native_ufs_store_sync (void) { return 0; }
// Unknown: GPT recheck is not implemented by this read-only bridge.
extern "C" unsigned native_ufs_store_gpt_check (void) { return 0xff; }

static void
h103SlotWriteThunk (void)
{
  if (gH103Probe) {
    gH103Probe->h103SlotWrite ();
  }
}

static void
h103HeaderWriteThunk (void)
{
  if (gH103Probe) {
    gH103Probe->h103HeaderOnly ();
  }
}

// H108: one flush, shared by the read path and the 1 Hz diagnostic thread.
// h103SlotWrite() computes its delta from gH103LastSlotMsgX, so this helper
// owns the busy flag, the interval stamp and the cursor update.
static void
h103FlushNow (void)
{
  if (__atomic_load_n (&gH103SlotBusy, __ATOMIC_ACQUIRE)) {
    return;
  }
  if (gH103FlushBackoffTicks != 0 && mach_absolute_time () < gH103FlushBackoffTicks) {
    gH103FlushSkips = gH103FlushSkips + 1;
    return;
  }
  struct H103MsgBuf *m = msgbufp;
  if (!m || !m->msg_bufc || m->msg_size <= 0) {
    return;
  }
  int expected = 0;
  if (!__atomic_compare_exchange_n (&gH103SlotBusy, &expected, 1, false,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return;
  }
  gH103LastSlotTicks = mach_absolute_time ();
  h103SlotWriteThunk ();
  __atomic_store_n (&gH103SlotBusy, 0, __ATOMIC_RELEASE);
}

// H116: called by the display archive from the client-open and frame-copy paths.
// Those paths still run after the UFS read path has gone quiet, so a forced
// flush here is the only way to persist the black-screen moment.
extern "C" void
native_log_flush_hook (void)
{
  if (gH103Probe == nullptr) {
    return;
  }
  h103FlushNow ();
}

// H315: strstr is not declared for this driver, and the panic hook only needs a substring test.
static bool
nativeContains (const char *text, const char *needle)
{
  size_t length = strlen (needle);
  if (!length) {
    return true;
  }
  for (const char *at = text; *at; ++at) {
    if (strncmp (at, needle, length) == 0) {
      return true;
    }
  }
  return false;
}

// H117: called from XNU's panic path before the machine-layer handler prints
// and stops the machine. The marker goes into the message buffer first, so a
// written log proves a panic even though the panic text is printed afterwards.
extern "C" void
native_panic_flush (void)
{
  if (gH103Probe == nullptr) {
    return;
  }
  // H311: the ring flush below can lose the race for fLock, which is why several boots ended
  // with no panic in the log at all.  This block needs no lock, so its presence (or absence)
  // answers "panicked or stalled" even when nothing else survives.
  // H315: and the panel turns solid, so the death type is readable without the log at all.
  if (native_legacy_panic_paint != nullptr) {
    uint32_t color = 0xffff0000U;   // red: panic, family unknown
    struct H103MsgBuf *m = msgbufp;
    if (m != nullptr && m->msg_size > 0 && m->msg_bufc != nullptr) {
      const int size = m->msg_size;
      const int cursor = (m->msg_bufx >= 0 && m->msg_bufx < size) ? m->msg_bufx : 0;
      const int want = 1200;
      char tail[want + 1];
      for (int i = 0; i < want; ++i) {
        char ch = m->msg_bufc[(cursor + size - want + i) % size];
        tail[i] = (ch >= 32 && ch < 127) ? ch : ' ';
      }
      tail[want] = 0;
      if (nativeContains (tail, "initproc exited")) {
        color = 0xffff8000U;        // orange: launchd exited
      } else if (nativeContains (tail, "Kernel data abort")) {
        color = 0xff8000ffU;        // purple: kernel data abort
      }
    }
    native_legacy_panic_paint (color);
  }
  gH103Probe->marker ("panic", true);
  IOLog ("NATIVE_PANIC: flags=%x client=%u xfer=%u frames=%u\n",
         native_boot_flags, native_legacy_client_calls,
         native_legacy_transfer_calls, native_legacy_frame_count);
  // H302: the ring contained nothing after 242 s in H301 even though this hook ran, so say
  // whether the flush that follows actually advanced the slot cursor or failed to write.
  {
    uint32_t beforeSeq = gH103SlotCursor;
    uint32_t beforeErrors = gH103SlotWriteErrors;
    h103FlushNow ();
    IOLog ("NATIVE_PANIC_FLUSH: seq=%u->%u errors=%u->%u\n",
           (unsigned) beforeSeq, (unsigned) gH103SlotCursor,
           (unsigned) beforeErrors, (unsigned) gH103SlotWriteErrors);
  }
}

// H311: the sampler's stall detector only needs the marker call, and the driver class is still
// incomplete where the detector sits, so this wrapper holds the typed access.
static void
h103Marker (const char *kind, bool urgent)
{
  if (gH103Probe == nullptr) {
    return;
  }
  gH103Probe->marker (kind, urgent);
}

// H128: driven from heartbeatThread() only.  The spawned slot thread never
// woke from IOSleep on the pre-H127 kernels, which is why this used to be
// called from the normal read paths after fLock had been released.  It must
// never run from a block-storage completion: it allocates, copies the message
// buffer, writes to the persistent log and can print process dumps, all of
// which took tens of seconds while NativeUfsWindow's request lock was held.
// H149: OSMetaClass::serialize runs with preemption disabled, so the count is
// recorded there and printed here, in thread context.
static void
h103ReportMetaSerialize (void)
{
  static volatile unsigned seen;
  unsigned n = native_meta_serialize_count;
  if (n == seen) {
    return;
  }
  seen = n;
  const char *cls = const_cast<const char *> (native_meta_serialize_class);
  IOLog ("NATIVE_META: serialize skipped n=%u first=%s\n", n, cls ? cls : "-");
}

static void
h103SlotTick (void)
{
  // H109: prove the timer_call heartbeat from the log, independently of the
  // display: one line every 30 s when the tick counter advanced.
  static volatile uint32_t printedTicks;
  uint32_t ticks = gH109Ticks;
  if (ticks && ticks != printedTicks && (ticks % 30 == 0 || printedTicks == 0)) {
    printedTicks = ticks;
    IOLog ("NATIVE_TICK: sec=%u flags=%x gate=%d\n", ticks, native_boot_flags,
           h107GateOpen () ? 1 : 0);
  }
  // H111/H112: sample the arch timer state every 5 s from the read path, so a
  // freeze leaves one in the last screen. The ArmCpu counter only moves when
  // INTID 27 actually arrives; CNTV_CTL shows whether the kernel enabled and
  // unmasked the virtual timer at all.
  {
    static volatile uint64_t lastIrqPrint;
    uint64_t nowIrq = mach_absolute_time ();
    if (!lastIrqPrint) {
      lastIrqPrint = nowIrq;
    } else if (nowIrq - lastIrqPrint >= 5 * 19200000ULL) {
      lastIrqPrint = nowIrq;
      // H113: the inline "mrs %0, cntv_ctl_el0" read returned a kernel
      // address, so use the same builtins XNU's machine_routines.c uses.
      // DAIF and the GIC group/Priority Mask registers tell whether ANY
      // interrupt can reach this CPU.
      uint64_t ctl = __builtin_arm_rsr64 ("CNTV_CTL_EL0");
      uint64_t tval = __builtin_arm_rsr64 ("CNTV_TVAL_EL0");
      uint64_t daif = 0, pmr = 0, grp1 = 0;
      __asm__ volatile ("mrs %0, daif" : "=r" (daif));
      __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (pmr));
      __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (grp1));
      IOLog ("NATIVE_IRQ: timer=%llu ipi=%llu daif=%llx pmr=%llx grp1=%llx ctl=%llx tval=%llx t19=%llu t27=%llu clock19=%llu forced=%llu storm=%llx sn=%llu flags=%x tick=%u\n",
             (unsigned long long) armcpu_timer_interrupts,
             (unsigned long long) armcpu_ipi_interrupts,
             (unsigned long long) (daif & 0x3ffULL),
             (unsigned long long) (pmr & 0xffULL),
             (unsigned long long) (grp1 & 1ULL),
             (unsigned long long) ctl, (unsigned long long) tval,
             (unsigned long long) armcpu_timer19_interrupts,
             (unsigned long long) armcpu_timer27_interrupts,
             (unsigned long long) armcpu_timer19_rtclock_count,
             (unsigned long long) armcpu_timer19_forced_count,
             (unsigned long long) armcpu_irq_storm_id,
             (unsigned long long) armcpu_irq_storm_count,
             native_boot_flags, gH109Ticks);
      if (gH114GicProbe) {
        IOLog ("NATIVE_GIC: typer=%llx isen=%llx igrp=%llx ipend=%llx ipri=%llx\n",
               (unsigned long long) armcpu_ppi_probe[0],
               (unsigned long long) armcpu_ppi_probe[1],
               (unsigned long long) armcpu_ppi_probe[2],
               (unsigned long long) armcpu_ppi_probe[3],
               (unsigned long long) armcpu_ppi_probe[4]);
        // H120: DS bit, redistributor wake state, Group 0 enable and the retried
        // group assignment. The ICC_HPPIR/HPPIR0/RPR reads are gone: they crash.
        IOLog ("NATIVE_GIC2: dctl=%llx gtyper=%llx waker=%llx grp0=%llu igrp2=%llx\n",
               (unsigned long long) armcpu_ppi_probe[5],
               (unsigned long long) armcpu_ppi_probe[9],
               (unsigned long long) armcpu_ppi_probe[6],
               (unsigned long long) armcpu_ppi_probe[7],
               (unsigned long long) armcpu_ppi_probe[8]);
        IOLog ("NATIVE_GIC3: isen_off=%llx isen_on=%llx grp27=%llx\n",
               (unsigned long long) armcpu_ppi_probe[10],
               (unsigned long long) armcpu_ppi_probe[11],
               (unsigned long long) armcpu_ppi_probe[12]);
      }
    }
  }
  // H112: one forced dump + flush 120 s after the UFS start, independent of
  // any exec flag. The last three boots froze near 197 s before any gated
  // flush, so this guarantees one written record before that point.
  {
    static volatile int forcedDone;
    if (!forcedDone && gH103StartTicks &&
        mach_absolute_time () - gH103StartTicks >= 120 * 19200000ULL) {
      forcedDone = 1;
      // H127g: PPI 19 has driven the clock since the first interrupt, so this
      // point only records the state. clock19/forced in NATIVE_IRQ carry the
      // durable evidence.
      IOLog ("NATIVE_TIMER_STATE: t19=%llu t27=%llu clock19=%llu forced=%llu\n",
             (unsigned long long) armcpu_timer19_interrupts,
             (unsigned long long) armcpu_timer27_interrupts,
             (unsigned long long) armcpu_timer19_rtclock_count,
             (unsigned long long) armcpu_timer19_forced_count);
      IOLog ("NATIVE_TICK: forced dump at uptime 120 s flags=%x\n", native_boot_flags);
      native_proc_state_dump ();
      native_proc_wait_graph ();
      if (gH103SlotMode) {
        h103FlushNow ();
      }
    }
  }
  if (!gH103SlotMode) {
    return;
  }
  h103ReportMetaSerialize ();
  h103ProbeIomfb ();
  // H156: a boot that dies in launchd never reaches backboardd, so the h107
  // gate would keep the persistent log empty (H153-H155 all died there and
  // left the H151 slots untouched).  Flush on the normal interval even before
  // the gate opens.
  if (!h107GateOpen ()) {
    // H157 kept the pre-gate log alive; H162 caps it at one write per 10 s and
    // never writes while a window request is in flight, because the slot write
    // takes the same fLock and UFS queue as the data path (this is what turned
    // H155-H161 into err 60 storms).
    static uint64_t lastEarlyFlush;
    uint64_t now = mach_absolute_time ();
    if (!gWindowInFlight &&
        (!lastEarlyFlush || now - lastEarlyFlush >= 10 * 19200000ULL)) {
      lastEarlyFlush = now;
      h103FlushNow ();
    }
    return;
  }
  h103MaybeLoadIogpu ();
  h103MaybePublishIogpu ();
  // H104f/H107: no slot/header writes before backboardd actual exec. When
  // backboardd never runs, SpringBoard + 60 s opens the same gate so the dump
  // and one flush still record why it stopped.
  if (!h107GateOpen ()) {
    return;
  }
  // H115: flush on the first display-side event, so the record exists before
  // backboardd replaces the console with its first frame (or before a crash).
  static volatile uint32_t seenEvents;
  uint32_t events = 0;
  if (native_boot_flags & 4) {
    events |= 1;      // backboardd exec
  }
  if (native_boot_flags & 0x1000) {
    events |= 2;      // IOMobile match succeeded for backboardd
  }
  if (native_legacy_client_calls) {
    events |= 4;      // display user client opened
  }
  if (native_legacy_transfer_calls) {
    events |= 8;      // first client frame reached the scanout
  }
  bool force = (events & ~seenEvents) != 0;
  if (force) {
    seenEvents |= events;
    IOLog ("NATIVE_FRAME: event=%x flags=%x client=%u xfer=%u frames=%u\n",
           events, native_boot_flags, native_legacy_client_calls,
           native_legacy_transfer_calls, native_legacy_frame_count);
  }
  uint64_t interval = (gH103SlotMode == 2 ? 2u : gH103LogSecs) * 19200000ULL;
  uint64_t now = mach_absolute_time ();
  // H107: run any due dump before reading the message cursor, and force the
  // flush on the same pass so the stage lines reach the disk.
  bool dumped = h105DumpTick ();
  if (!gH103LastSlotTicks) {
    gH103LastSlotTicks = now;
    if (!dumped && !force) {
      return;
    }
  }
  if (__atomic_load_n (&gH103SlotBusy, __ATOMIC_ACQUIRE)) {
    return;
  }
  if (!dumped && !force && now - gH103LastSlotTicks < interval) {
    return;
  }
  struct H103MsgBuf *m = msgbufp;
  if (!m || !m->msg_bufc || m->msg_size <= 0) {
    return;
  }
  int x = m->msg_bufx;
  if (x < 0 || x >= m->msg_size) {
    x = 0;
  }
  if (x == gH103LastSlotMsgX) {
    // H106: ios-sweet 0069's rule - nothing new to persist means no data
    // write. Only the header is rewritten, at the same slow interval, so
    // uptime_s still shows whether the read path is alive.
    if (now - gH103LastHeaderTicks >= interval) {
      int expected = 0;
      if (!__atomic_compare_exchange_n (&gH103SlotBusy, &expected, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return;
      }
      gH103LastSlotTicks = now;
      gH103LastHeaderTicks = now;
      h103HeaderWriteThunk ();
      __atomic_store_n (&gH103SlotBusy, 0, __ATOMIC_RELEASE);
    }
    return;
  }
  gH103LastSlotTicks = now;
  // H115: record what the client frame actually contained before the flush, so
  // a black screen can be told from a lost display path.
  {
    static volatile uint32_t printedFrames;
    uint32_t frames = native_legacy_frame_count;
    if (frames && frames != printedFrames) {
      printedFrames = frames;
      uint32_t k = frames > 32 ? 31 : frames - 1;
      IOLog ("NATIVE_FRAME: frames=%u kind=%u nonblack=%llu bbox=%llu,%llu-%llu,%llu\n",
             frames, native_legacy_frame_kind[k],
             (unsigned long long) native_legacy_frame_stats[k][0],
             (unsigned long long) native_legacy_frame_stats[k][1],
             (unsigned long long) native_legacy_frame_stats[k][2],
             (unsigned long long) native_legacy_frame_stats[k][3],
             (unsigned long long) native_legacy_frame_stats[k][4]);
    }
  }
  // H108: h103FlushNow owns the busy flag and the cursor update; the writer
  // leaves gH103LastSlotMsgX at the previous value until the delta is written.
  // H162: skip the write while the block-storage path is busy; the next beat
  // retries.  This keeps the log from stalling SystemOS reads.
  if (!gWindowInFlight) {
    h103FlushNow ();
  }
}

// H109: the timer interrupt fires this every second. It flips the 16x16 cell
// (a plain framebuffer fill, no locks), counts ticks and wakes the dump worker.
// Printing, task locks and UFS writes stay in the worker's thread context.
static void
h109TimerFunc (timer_call_param_t, timer_call_param_t)
{
  gH109Ticks = gH109Ticks + 1;
  uint32_t n = gH109Ticks;
  if (n <= 3 || (n % 10) == 0) {
    IOLog ("NATIVE_TICK: fire=%u flags=%x\n", n, native_boot_flags);
  }
  native_legacy_heartbeat_cell ();
  if (gH103SlotMode && gH105DumpStage < 3 && h107GateOpen ()) {
    thread_wakeup ((event_t) &gH109Chan);
  }
  if (gH109TimerCall) {
    timer_call_enter (gH109TimerCall, mach_absolute_time () + 19200000ULL, 0);
  }
}

// H109: the dump worker sleeps on an explicit wakeup, never on a thread timer,
// so it does not depend on the timer wakeup path that IOSleep and tsleep both
// missed. It runs any due dump stage and flushes that dump.
static void
h109WorkerThread (void *, wait_result_t)
{
  thread_set_thread_name (current_thread (), "h109_dump");
  for (;;) {
    assert_wait (&gH109Chan, THREAD_INTERRUPTIBLE);
    thread_block (nullptr);
    gH110WorkerWakes = gH110WorkerWakes + 1;
    IOLog ("NATIVE_TICK: worker wake=%u flags=%x\n",
           gH110WorkerWakes, native_boot_flags);
    if (!gH103SlotMode || !h107GateOpen ()) {
      continue;
    }
    if (h105DumpTick ()) {
      h103FlushNow ();
    }
  }
}

OSDefineMetaClassAndStructors (NativeUfsProbe, IOService)
OSDefineMetaClassAndStructors (NativeUfsLun, IOBlockStorageDevice)

OSDefineMetaClassAndStructors (NativeUfsWindow, IOBlockStorageDevice)

static uint32_t
read32 (uintptr_t base, unsigned offset)
{
  uint32_t value = *reinterpret_cast<volatile uint32_t *> (base + offset);
  __asm__ volatile ("dmb oshld" ::: "memory");
  return value;
}

static void
write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  *reinterpret_cast<volatile uint32_t *> (base + offset) = value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

static uint32_t
getBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) |
         (uint32_t (at[2]) << 8) | at[3];
}

static uint64_t
getBe64 (const uint8_t *at)
{
  return (uint64_t (getBe32 (at)) << 32) | getBe32 (at + 4);
}

static uint32_t
getLe32 (const uint8_t *at)
{
  return uint32_t (at[0]) | (uint32_t (at[1]) << 8) |
         (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
}

static uint64_t
getLe64 (const uint8_t *at)
{
  return uint64_t (getLe32 (at)) | (uint64_t (getLe32 (at + 4)) << 32);
}

static void
putBe32 (uint8_t *at, uint32_t value)
{
  at[0] = value >> 24;
  at[1] = value >> 16;
  at[2] = value >> 8;
  at[3] = value;
}

static void
zero32 (void *start, size_t length)
{
  volatile uint32_t *word = static_cast<volatile uint32_t *> (start);
  for (size_t index = 0; index < length / 4; index++) {
    word[index] = 0;
  }
}

static void
cacheClean (void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  __asm__ volatile ("dsb sy" ::: "memory");
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void
cacheInvalidate (void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  __asm__ volatile ("dsb sy" ::: "memory");
  for (; address < end; address += 64) {
    __asm__ volatile ("dc ivac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static uintptr_t
mapPhys (uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, length);
  if (!memory) {
    return 0;
  }
  keep = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere), OSNoRetain);
  memory->release ();
  return keep ? static_cast<uintptr_t> (keep->getVirtualAddress ()) : 0;
}

static void
dumpSmmu (uintptr_t smmu, const char *name)
{
  uint32_t idr0 = read32 (smmu, 0x20);
  uint32_t idr1 = read32 (smmu, 0x24);
  uint32_t idr2 = read32 (smmu, 0x28);
  IOLog ("NATIVE_UFS_GPT: smmu %s idr0=%08x idr1=%08x idr2=%08x sCR0=%08x\n",
         name, idr0, idr1, idr2, read32 (smmu, 0x00));
  unsigned groups = idr0 & 0xff;
  if (groups > 64) {
    groups = 64;
  }
  for (unsigned index = 0; index < groups; index++) {
    uint32_t smr = read32 (smmu, 0x800 + 4 * index);
    if (!(smr & 0x80000000U)) {
      continue;
    }
    uint32_t s2cr = read32 (smmu, 0xc00 + 4 * index);
    IOLog ("NATIVE_UFS_GPT: smmu %s smr[%u]=%08x s2cr=%08x\n",
           name, index, smr, s2cr);
  }
}

static bool
waitRegister (uintptr_t base, unsigned offset, uint32_t mask, uint32_t expected,
              unsigned microseconds);

static bool
uicCommand (uintptr_t hc, uint32_t opcode, uint32_t arg1, uint32_t arg2,
            uint32_t *result)
{
  uint32_t pending = read32 (hc, UFS_REG_IS);
  if (pending & (UFS_IS_UCCS | UFS_IS_UEE)) {
    write32 (hc, UFS_REG_IS, pending & (UFS_IS_UCCS | UFS_IS_UEE));
  }
  write32 (hc, UFS_REG_UICARG1, arg1);
  write32 (hc, UFS_REG_UICARG2, arg2);
  write32 (hc, UFS_REG_UICARG3, 0);
  write32 (hc, UFS_REG_UICCMD, opcode);
  if (!waitRegister (hc, UFS_REG_IS, UFS_IS_UCCS, UFS_IS_UCCS, 100000)) {
    return false;
  }
  uint32_t status = read32 (hc, UFS_REG_IS);
  write32 (hc, UFS_REG_IS, UFS_IS_UCCS | UFS_IS_UEE);
  if (result) {
    *result = read32 (hc, UFS_REG_UICARG3);
  }
  return (status & UFS_IS_UEE) == 0;
}

static void
spinTicks (uint64_t ticks)
{
  uint64_t start = mach_absolute_time ();
  while (mach_absolute_time () - start < ticks) {
    __asm__ volatile ("yield" ::: "memory");
  }
}

static bool
waitRegister (uintptr_t base, unsigned offset, uint32_t mask, uint32_t expected,
              unsigned microseconds)
{
  uint64_t deadline = mach_absolute_time () + uint64_t (microseconds) * 20ULL;
  while (mach_absolute_time () < deadline) {
    if ((read32 (base, offset) & mask) == expected) {
      return true;
    }
    __asm__ volatile ("yield" ::: "memory");
  }
  return false;
}

static bool
compatibleContains (OSData *compatible, const char *needle)
{
  if (!compatible) {
    return false;
  }
  const uint8_t *bytes = static_cast<const uint8_t *> (compatible->getBytesNoCopy ());
  size_t length = compatible->getLength ();
  size_t wanted = strlen (needle) + 1;
  if (!bytes || length < wanted) {
    return false;
  }
  for (size_t offset = 0; offset + wanted <= length; offset++) {
    if (memcmp (bytes + offset, needle, wanted) == 0) {
      return true;
    }
  }
  return false;
}

/* H290: ufsCommand() refuses a new command while the previous doorbell is still set
 * (UFS_REG_UTRLDBR), and refuses outright when the queue is poisoned.  The refusal
 * surfaced as EIO, which APFS turned into a failed page-in and SIGBUS in the paging
 * process.  Wait for the doorbell and retry a bounded number of times; block reads and
 * writes are idempotent, so a resend is safe.  Only a poisoned queue gives up at once. */
static bool ufsCommand (UfsQueue *q, uint8_t lun, const uint8_t *cdb, unsigned bytes, void *dst);

/* H323: one OCS timeout used to poison the queue for the rest of the boot.  Every later read then
 * returned EIO straight away, APFS failed the page-in, and the process that touched the page died
 * - SpringBoard among them, which is the "got err 5 / too many corpses" screen.  A timeout leaves
 * the transfer list wedged, so recovery is a host-controller reset; block reads are idempotent, so
 * the caller can simply retry once the controller is ready again. */
static bool
ufsRecover (UfsQueue *q)
{
  if (!q || !q->hc) {
    return false;
  }
  const uint32_t beforeIs = read32 (q->hc, UFS_REG_IS);
  const uint32_t beforeDb = read32 (q->hc, UFS_REG_UTRLDBR);
  uint32_t hcs = 0;
  bool ready = false;
  // Bit 0 is HCS.DP (device present). HCS.UCRDY is bit 3 and only indicates that
  // UIC commands may be issued. Keep the existing device-presence check here.
  for (unsigned attempt = 0; attempt < 4 && !ready; ++attempt) {
    write32 (q->hc, UFS_REG_HCE, read32 (q->hc, UFS_REG_HCE) & ~1U);
    spinTicks (38400);                                /* 2 ms to quiesce */
    write32 (q->hc, UFS_REG_IS, 0xffffffffU);
    write32 (q->hc, UFS_REG_UTRLDBR, 0);              /* drop the stuck transfer */
    write32 (q->hc, UFS_REG_HCE, read32 (q->hc, UFS_REG_HCE) | 1U);
    for (unsigned i = 0; i < 200; ++i) {
      hcs = read32 (q->hc, UFS_REG_HCS);
      if (hcs & 1U) {
        ready = true;
        break;
      }
      spinTicks (1920);                               /* 100 us */
    }
    if (!ready) {
      spinTicks (192000);                             /* 10 ms before the next toggle */
    }
  }
  q->poisoned = false;
  gUfsInFlight = 0;
  if (ready) {
    gUfsRecovered = gUfsRecovered + 1;
  }
  IOLog ("NATIVE_UFS_RECOVER: is=%x db=%x hcs=%x ready=%u\n",
         beforeIs, beforeDb, hcs, ready ? 1u : 0u);
  return ready;
}

static bool
ufsCommandRetry (UfsQueue *q, uint8_t lun, const uint8_t *cdb, unsigned bytes, void *dst,
                 const char *who)
{
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    if (ufsCommand (q, lun, cdb, bytes, dst)) {
      if (attempt) {
        IOLog ("NATIVE_UFS_RETRY: %s ok attempt=%u\n", who, attempt);
      }
      return true;
    }
    if (!q) {
      return false;
    }
    if (q->poisoned) {
      // H323: recover instead of abandoning the read for the rest of the boot.
      IOLog ("NATIVE_UFS_RETRY: %s poisoned at attempt=%u, recovering\n", who, attempt);
      if (!ufsRecover (q)) {
        // H361: a controller that is not ready yet is not a reason to fail a page-in (EIO kills
        // the process that touched the page). Wait and try the toggle again instead.
        IOSleep (5);
        continue;
      }
      continue;
    }
    if (attempt == 0 || attempt == 7) {
      IOLog ("NATIVE_UFS_RETRY: %s wait attempt=%u doorbell=%u bytes=%u\n", who, attempt,
             (unsigned) read32 (q->hc, UFS_REG_UTRLDBR), bytes);
    }
    IOSleep (2);
  }
  IOLog ("NATIVE_UFS_RETRY: %s gave up bytes=%u\n", who, bytes);
  return false;
}

static bool
readCommandAllowed (unsigned opcode, unsigned direction, unsigned bytes)
{
  if (bytes > UFS_DATA_MAX || (bytes & 3)) {
    return false;
  }
  switch (opcode) {
    case 0x00: return direction == 0 && bytes == 0; // TEST UNIT READY
    case 0x25: return direction == 2 && bytes == 8; // READ CAPACITY(10)
    case 0x9e: return direction == 2 && bytes == 32; // READ CAPACITY(16)
    case 0x28: return direction == 2 && bytes != 0; // READ(10)
    case 0x2a: return direction == 1 && bytes != 0; // WRITE(10), persistent log
    case 0x35: return direction == 0 && bytes == 0; // SYNCHRONIZE CACHE, log
    default: return false;
  }
}

static bool
waitForCompletion (uint8_t *list, uint32_t *ocs, unsigned *loops,
                   uint64_t timeoutTicks)
{
  uint64_t start = mach_absolute_time ();
  unsigned loop = 0;
  for (;;) {
    cacheInvalidate (list, 64);
    *ocs = read32 (reinterpret_cast<uintptr_t> (list), 8) & 0xff;
    if (*ocs != 0x0f) {
      *loops = loop;
      return true;
    }
    if (timeoutTicks && mach_absolute_time () - start > timeoutTicks) {
      *loops = loop;
      return false;
    }
    if (loop++ > 100000000U) {
      *loops = loop;
      return false;
    }
    __asm__ volatile ("yield" ::: "memory");
  }
}

// H128: the data path is clean now (no dumps, no flushes, no printf while a
// storage lock is held).  H129: the bound must still exceed UFS_TIMEOUT_TICKS
// (5.2 s), otherwise the lock expires while a command is legitimately running;
// that is what turned the slow read at LBA 0x5b17b0 into repeated 2 s
// kIOReturnTimeout failures.  115200000 ticks is 6 s at 19.2 MHz.
/* H285: a 6 s bound turned transiently slow UFS commands into hard I/O errors
 * (disk5/disk6 err 60/5 -> decmpfs page-in failure -> process SIGBUS -> ipc_space
 * panic).  The holder keeps the lock across one synchronous command, so a waiter
 * timing out says the command took longer than the bound, not that the queue is
 * wedged. Give it enough room to survive a slow command before failing. */
#define UFS_DATA_LOCK_TICKS  (576000000ULL)  /* 30 s at 19.2 MHz */
/* H317: the log is a diagnostic, so it must never make the main I/O path wait.  A flush that
 * cannot take the lock in 100 ms is dropped and the next two seconds are skipped. */
#define UFS_LOG_LOCK_TICKS   (1920000ULL)    /* 100 ms at 19.2 MHz */
#define UFS_LOG_BACKOFF_TICKS (38400000ULL)  /* 2 s */
static volatile uint64_t gLockOwnerTid;
static volatile uint64_t gLockAcquiredTicks;
static volatile uint32_t gLockWaiters;
// Bounded lock acquisition: a stuck holder must not turn a diagnostic read into
// an unobservable hang. 19.2 MHz timebase, so 38400000 ticks is two seconds.
// H128: the wait must sleep, not spin.  `yield` is only a pipeline hint and
// never reschedules; on the single-CPU baseline a spinning waiter can starve
// the lock holder, which is how a 2 s bound turned into 20 s stalls.
static bool
lockWithTimeout (IOLock *lock, const char *who, uint64_t timeoutTicks, bool track)
{
  /* H272ad: a timed-out lock tells us nothing about who held it, which is exactly what
   * the "count=8, nothing started" storms need.  Track the holder and how long it has
   * been holding, and print both when a waiter gives up. */
  if (track) gLockWaiters = gLockWaiters + 1;
  uint64_t start = mach_absolute_time ();
  unsigned spins = 0;
  while (!IOLockTryLock (lock)) {
    if (mach_absolute_time () - start > timeoutTicks) {
      const uint64_t held = gLockAcquiredTicks ? (mach_absolute_time () - gLockAcquiredTicks) : 0;
      IOLog ("NATIVE_UFS_LOCK: %s timeout ticks=%llu owner=%llu held_ms=%llu waiters=%u\n", who,
             (unsigned long long) (mach_absolute_time () - start),
             (unsigned long long) gLockOwnerTid,
             (unsigned long long) (held / 19200ULL),
             (unsigned) gLockWaiters);
      if (track) gLockWaiters = gLockWaiters - 1;
      return false;
    }
    if (++spins < 64) {
      __asm__ volatile ("yield" ::: "memory");
    } else {
      IOSleep (1);
    }
  }
  if (track) {
    /* H272ae: thread_tid is a plain uint64_t function; the previous form treated the
     * result as a function pointer and killed early boot (UEFI green line).  Only the
     * data paths pay for this bookkeeping - logWrite/logSync pass track=false and stay
     * byte-identical to the baseline. */
    gLockWaiters = gLockWaiters - 1;
    /* This kernel exports no thread_tid(); the thread pointer itself is a stable
     * enough owner marker for "same holder or not". */
    gLockOwnerTid = (uint64_t) (uintptr_t) current_thread ();
    gLockAcquiredTicks = mach_absolute_time ();
  }
  return true;
}

static bool
blockRange (uint64_t block, uint64_t count, uint64_t last, uint32_t blockSize)
{
  return count && blockSize >= 512 && blockSize <= 4096 &&
         (blockSize & (blockSize - 1)) == 0 && block <= last &&
         count - 1 <= last - block;
}

static void
dumpMediaServices (void)
{
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  unsigned count = 0;
  while (iterator && count < 16) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) {
      break;
    }
    OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty ("BSD Name"));
    OSString *uuid = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    OSNumber *base = OSDynamicCast (OSNumber, media->getProperty ("Base"));
    IOLog ("NATIVE_UFS_MEDIA: media[%u] content=%s bsd=%s size=%llx base=%llx uuid=%s\n",
           count,
           content ? content->getCStringNoCopy () : "-",
           bsd ? bsd->getCStringNoCopy () : "-",
           size ? size->unsigned64BitValue () : 0ULL,
           base ? base->unsigned64BitValue () : 0ULL,
           uuid ? uuid->getCStringNoCopy () : "-");
    count++;
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
  IOLog ("NATIVE_UFS_MEDIA: total=%u\n", count);
}

static bool
rootMediaRelevant (OSString *content, OSString *uuid, OSNumber *size)
{
  if (content) {
    const char *value = content->getCStringNoCopy ();
    if (value && (strcmp (value, "Apple_APFS") == 0 ||
                  strcmp (value, "Apple_HFS") == 0)) {
      return true;
    }
  }
  if (uuid) {
    const char *value = uuid->getCStringNoCopy ();
    if (value && (strcmp (value, "04769D26-2CA0-4825-B443-E2E5F88300F2") == 0 ||
                  strcmp (value, "8846FE78-2910-4297-AF97-D59BB6F8FC16") == 0)) {
      return true;
    }
  }
  if (size) {
    uint64_t value = size->unsigned64BitValue ();
    return value == 6077580800ULL || value == 3303014400ULL;
  }
  return false;
}

static void
dumpRootMedia (void)
{
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  unsigned total = 0;
  unsigned shown = 0;
  while (iterator && total < 512) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) {
      break;
    }
    total++;
    OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty ("BSD Name"));
    OSString *uuid = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    OSNumber *base = OSDynamicCast (OSNumber, media->getProperty ("Base"));
    if (shown < 32 && rootMediaRelevant (content, uuid, size)) {
      IOLog ("NATIVE_UFS_ROOT_MEDIA: media[%u] content=%s bsd=%s size=%llx base=%llx uuid=%s\n",
             shown,
             content ? content->getCStringNoCopy () : "-",
             bsd ? bsd->getCStringNoCopy () : "-",
             size ? size->unsigned64BitValue () : 0ULL,
             base ? base->unsigned64BitValue () : 0ULL,
             uuid ? uuid->getCStringNoCopy () : "-");
      shown++;
    }
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
  IOLog ("NATIVE_UFS_ROOT_MEDIA: total=%u shown=%u\n", total, shown);
}

// Temporary B5 bring-up authority: the windows are read-only and their content
// came from the verified SHNGROOT image.  Replace with VerifiedRoot before any
// final configuration.
extern "C" int
native_verified_block_root_authorized (dev_t device)
{
  return device != NODEV;
}

static int
native_volume_lookup_once (const char *uuid, dev_t *device, char *path,
                           size_t size, unsigned *matches_out)
{
  if (matches_out) {
    *matches_out = 0;
  }
  if (!uuid || !device || !path || size < 6 || strlen (uuid) != 36) {
    return 0;
  }
  *device = NODEV;
  path[0] = 0;
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity (1);
  OSString *wanted = OSString::withCString (uuid);
  if (!matching || !properties || !wanted) {
    OSSafeReleaseNULL (wanted);
    OSSafeReleaseNULL (properties);
    OSSafeReleaseNULL (matching);
    return 0;
  }
  bool ready = properties->setObject ("UUID", wanted) &&
               matching->setObject ("IOPropertyMatch", properties);
  OSSafeReleaseNULL (wanted);
  OSSafeReleaseNULL (properties);
  OSIterator *iterator = ready ? IOService::getMatchingServices (matching) : nullptr;
  OSSafeReleaseNULL (matching);
  if (!iterator) {
    return 0;
  }
  unsigned matches = 0;
  bool authorized = false;
  if (matches_out) {
    *matches_out = 0;
  }
  dev_t candidate = NODEV;
  char candidatePath[64] = {};
  while (OSObject *object = iterator->getNextObject ()) {
    IOService *media = OSDynamicCast (IOService, object);
    if (!media) {
      continue;
    }
    if (++matches != 1) {
      authorized = false;
      break;
    }
    OSString *identity = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *name = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSNumber *major = OSDynamicCast (OSNumber, media->getProperty (kIOBSDMajorKey));
    OSNumber *minor = OSDynamicCast (OSNumber, media->getProperty (kIOBSDMinorKey));
    if (!identity || strcmp (identity->getCStringNoCopy (), uuid) || !name ||
        !major || !minor || !name->getLength () ||
        name->getLength () >= sizeof (candidatePath) - 6 ||
        name->getLength () >= size - 6 ||
        major->unsigned64BitValue () > 255 ||
        minor->unsigned64BitValue () > 0xffffff) {
      continue;
    }
    candidate = static_cast<dev_t> (makedev (major->unsigned32BitValue (),
                                             minor->unsigned32BitValue ()));
    if (!native_verified_block_root_authorized (candidate)) {
      continue;
    }
    snprintf (candidatePath, sizeof (candidatePath), "/dev/%s",
              name->getCStringNoCopy ());
    authorized = true;
  }
  OSSafeReleaseNULL (iterator);
  if (matches_out) {
    *matches_out = matches;
  }
  if (!authorized || matches != 1) {
    return 0;
  }
  *device = candidate;
  strlcpy (path, candidatePath, size);
  return 1;
}

// H159: APFS publishes the SystemOS volume media asynchronously; the one-shot
// lookup lost the race (disk3 checkpoint search happened, but disk3s1 had not
// appeared yet) and native_owner_cache_prepare() reported EAUTH.  Retry briefly
// and log every attempt so the next log names the reason.
extern "C" int
native_verified_block_volume_device (const char *uuid, dev_t *device,
                                     char *path, size_t size)
{
  for (unsigned attempt = 0; attempt < 10; attempt++) {
    unsigned matches = 0;
    if (native_volume_lookup_once (uuid, device, path, size, &matches)) {
      if (attempt) {
        IOLog ("NATIVE_SYSTEMOS_WAIT: found after %u attempts\n", attempt + 1);
      }
      return 1;
    }
    IOLog ("NATIVE_SYSTEMOS_WAIT: attempt=%u matches=%u\n", attempt + 1, matches);
    IOSleep (200);
  }
  return 0;
}

static bool
ufsSubmit (UfsQueue *q, unsigned direction, unsigned bytes,
           uint64_t timeoutTicks = UFS_TIMEOUT_TICKS)
{
  if (!q || q->poisoned || bytes > q->dataCapacity ||
      (read32 (q->hc, UFS_REG_IS) & UFS_FATAL_STATUS)) {
    return false;
  }
  uint8_t *ucd = q->ucd;
  if (ucd[0] == 1) {
    if (!readCommandAllowed (ucd[16], direction, bytes)) {
      return false;
    }
  } else if (ucd[0] != 0 || bytes || direction) {
    return false;
  }
  uint32_t *utrd = reinterpret_cast<uint32_t *> (q->list);
  zero32 (utrd, 32);
  // Vendor ufshci.h/ufshcd.c: SCSI is CT=0, NOP OUT is UTP_CMD_TYPE_DEV_MANAGE
  // (CT=2); both carry UTP_REQ_DESC_INT_CMD.  0x10000000 is native UFS CT=1 and
  // leaves this controller's UTP engine stuck.
  utrd[0] = (ucd[0] == 0) ? 0x21000000U : (0x01000000U | (direction << 25));
  utrd[2] = 0x0fU;
  utrd[4] = static_cast<uint32_t> (q->ucdPhys);
  utrd[5] = static_cast<uint32_t> (q->ucdPhys >> 32);
  utrd[6] = (512U >> 2) | ((UFS_RSP_OFFSET >> 2) << 16);
  utrd[7] = (bytes ? 1U : 0U) | ((UFS_PRDT_OFFSET >> 2) << 16);
  if (bytes) {
    uint32_t *prd = reinterpret_cast<uint32_t *> (ucd + UFS_PRDT_OFFSET);
    prd[0] = static_cast<uint32_t> (q->dataPhys);
    prd[1] = static_cast<uint32_t> (q->dataPhys >> 32);
    prd[2] = 0;
    prd[3] = bytes - 1;
  }
  gUfsCommands = gUfsCommands + 1;
  gUfsInFlight = 1;
  if (gNopActive) {
    IOLog ("NATIVE_UFS_NOP: submit before cacheClean is=%x hcs=%x db=%x\n",
           read32 (q->hc, UFS_REG_IS), read32 (q->hc, UFS_REG_HCS),
           read32 (q->hc, UFS_REG_UTRLDBR));
  }
  cacheClean (q->list, 4096);
  cacheClean (q->ucd, UFS_FW_UCD_SIZE);
  if (gNopActive) {
    IOLog ("NATIVE_UFS_NOP: submit after cacheClean is=%x hcs=%x db=%x\n",
           read32 (q->hc, UFS_REG_IS), read32 (q->hc, UFS_REG_HCS),
           read32 (q->hc, UFS_REG_UTRLDBR));
    spinTicks (19200);
    IOLog ("NATIVE_UFS_NOP: submit before doorbell\n");
  }
  // H360: the controller clears UTRLDBR bit 0 when it consumes the command.  A bit still set
  // from the previous command is a doorbell timeout in ufshcd terms, not a permanently unusable
  // queue: wait 10 ms for it, then poison and let the caller's recovery reset the controller.
  {
    uint64_t dbStart = mach_absolute_time ();
    while (read32 (q->hc, UFS_REG_UTRLDBR) & 1U) {
      if (mach_absolute_time () - dbStart > 192000ULL) {         /* 10 ms */
        gUfsDbTimeouts = gUfsDbTimeouts + 1;
        IOLog ("NATIVE_UFS_DBTO: db=%x is=%x hcs=%x hce=%x\n",
               read32 (q->hc, UFS_REG_UTRLDBR), read32 (q->hc, UFS_REG_IS),
               read32 (q->hc, UFS_REG_HCS), read32 (q->hc, UFS_REG_HCE));
        q->poisoned = true;
        gUfsInFlight = 0;
        return false;
      }
      __asm__ volatile ("yield" ::: "memory");
    }
  }
  write32 (q->hc, UFS_REG_IS, 0xffffffffU);
  write32 (q->hc, UFS_REG_UTRLDBR, 1);
  if (gNopActive) {
    IOLog ("NATIVE_UFS_NOP: submit after doorbell is=%x hcs=%x db=%x\n",
           read32 (q->hc, UFS_REG_IS), read32 (q->hc, UFS_REG_HCS),
           read32 (q->hc, UFS_REG_UTRLDBR));
    spinTicks (19200);
  }
  uint64_t commandStart = mach_absolute_time ();
  uint32_t ocs = 0x0f;
  unsigned loops = 0;
  if (gNopActive) {
    IOLog ("NATIVE_UFS_NOP: waiting timeout=%llu\n",
           (unsigned long long) timeoutTicks);
  }
  if (!waitForCompletion (q->list, &ocs, &loops, timeoutTicks)) {
    q->poisoned = true;
    gUfsInFlight = 0;
    gUfsLastLoops = loops;
    gUfsLastTicks = mach_absolute_time () - commandStart;
    IOLog ("NATIVE_UFS_GPT: OCS timeout type=%08x ocs=%x loops=%u "
           "db=%x is=%x hcs=%x hce=%x ucd0=%x cdb=%02x%02x%02x%02x%02x%02x\n",
           utrd[0], ocs, loops,
           read32 (q->hc, UFS_REG_UTRLDBR), read32 (q->hc, UFS_REG_IS),
           read32 (q->hc, UFS_REG_HCS), read32 (q->hc, UFS_REG_HCE),
           q->ucd[0], q->ucd[16], q->ucd[17], q->ucd[18], q->ucd[19],
           q->ucd[20], q->ucd[21]);
    return false;
  }
  cacheInvalidate (q->list, 4096);
  cacheInvalidate (q->ucd, UFS_FW_UCD_SIZE);
  if (ocs) {
    gUfsInFlight = 0;
    gUfsLastLoops = loops;
    gUfsLastTicks = mach_absolute_time () - commandStart;
    IOLog ("NATIVE_UFS_GPT: OCS=%x type=%08x loops=%u\n", ocs, utrd[0], loops);
    return false;
  }
  gUfsInFlight = 0;
  gUfsLastLoops = loops;
  gUfsLastTicks = mach_absolute_time () - commandStart;
  if ((gUfsCommands & 63) == 0) {
    // H128: nothing but counters here.  This block used to call
    // h105DumpTick() (proc_iterate() plus ~100 lines of printf) and repaint
    // the 2x2 while fLock was held, inside a block-storage request.  Both now
    // run from the heartbeat thread, which the working PPI 19 timer wakes.
    if (!gH103Quiet) {
      IOLog ("NATIVE_UFS_PROGRESS: cmds=%llu last=%llx/%llu loops=%llu ticks=%llu "
             "winReq=%llu winBytes=%llu winFlight=%llu lastWin=%llx/%llu "
             "req=%llx/%llu dbto=%llu rec=%llu wfail=%llu\n",
             (unsigned long long) gUfsCommands,
             (unsigned long long) gUfsLastLba,
             (unsigned long long) gUfsLastBytes,
             (unsigned long long) gUfsLastLoops,
             (unsigned long long) gUfsLastTicks,
             (unsigned long long) gWindowRequests,
             (unsigned long long) gWindowBytes,
             (unsigned long long) gWindowInFlight,
             (unsigned long long) gWindowLastBlock,
             (unsigned long long) gWindowLastCount,
             (unsigned long long) gWindowLastReqBlock,
             (unsigned long long) gWindowLastReqCount,
             (unsigned long long) gUfsDbTimeouts,
             (unsigned long long) gUfsRecovered,
             (unsigned long long) gUfsWriteFailed);
    }
  }
  return true;
}

static bool
ufsCommand (UfsQueue *q, uint8_t lun, const uint8_t *cdb, unsigned bytes, void *dst)
{
  if (!q || !readCommandAllowed (cdb[0], bytes ? 2 : 0, bytes) || q->poisoned ||
      bytes > q->dataCapacity) {
    return false;
  }
  zero32 (q->ucd, UFS_FW_UCD_SIZE);
  uint8_t *req = q->ucd;
  req[0] = 1;
  req[1] = bytes ? 0x40 : 0;
  req[2] = lun;
  req[3] = 1;
  putBe32 (req + 12, bytes);
  memcpy (req + 16, cdb, 16);
  cacheInvalidate (q->data, bytes);
  if (!ufsSubmit (q, bytes ? 2 : 0, bytes)) {
    return false;
  }
  cacheInvalidate (q->data, bytes);
  const uint8_t *response = req + UFS_RSP_OFFSET;
  if (response[0] != 0x21 || response[2] != lun || response[3] != 1 ||
      response[4] || response[6] || response[7] || response[10] ||
      response[11] || getBe32 (response + 12)) {
    IOLog ("NATIVE_UFS_GPT: bad response type=%x lun=%x tag=%x flags=%x r6=%x status=%x r10=%x r11=%x residual=%x\n",
           response[0], response[2], response[3], response[4], response[6],
           response[7], response[10], response[11], getBe32 (response + 12));
    return false;
  }
  if (bytes && dst && dst != q->data) {
    memcpy (dst, q->data, bytes);
  }
  return true;
}

static bool
ufsWriteCommand (UfsQueue *q, uint8_t lun, const uint8_t *cdb, unsigned bytes,
                 const void *src)
{
  if (!q || q->poisoned || !src || !bytes || bytes > q->dataCapacity ||
      (bytes & 3)) {
    // H360: a failed write is never retried or replayed; only reads may be resent.
    gUfsWriteFailed = gUfsWriteFailed + 1;
    return false;
  }
  if (src != q->data) {
    memcpy (q->data, src, bytes);
  }
  zero32 (q->ucd, UFS_FW_UCD_SIZE);
  uint8_t *req = q->ucd;
  req[0] = 1;
  req[1] = 0x20;
  req[2] = lun;
  req[3] = 1;
  putBe32 (req + 12, bytes);
  memcpy (req + 16, cdb, 16);
  cacheClean (q->data, bytes);
  if (!ufsSubmit (q, 1, bytes)) {
    return false;
  }
  const uint8_t *response = req + UFS_RSP_OFFSET;
  if (response[0] != 0x21 || response[2] != lun || response[3] != 1 ||
      response[4] || response[6] || response[7] || response[10] ||
      response[11] || getBe32 (response + 12)) {
    IOLog ("NATIVE_UFS_LOG: bad write response type=%x lun=%x tag=%x flags=%x r6=%x status=%x residual=%x\n",
           response[0], response[2], response[3], response[4], response[6],
           response[7], getBe32 (response + 12));
    return false;
  }
  return true;
}

static bool
ufsSync (UfsQueue *q)
{
  if (!q || q->poisoned) {
    return false;
  }
  zero32 (q->ucd, UFS_FW_UCD_SIZE);
  uint8_t *req = q->ucd;
  req[0] = 1;
  req[1] = 0;
  req[2] = 0;
  req[3] = 1;
  req[16] = 0x35;
  if (!ufsSubmit (q, 0, 0)) {
    return false;
  }
  const uint8_t *response = req + UFS_RSP_OFFSET;
  if (response[0] != 0x21 || response[3] != 1 || response[4] ||
      response[6] || response[7] || response[10] || response[11]) {
    return false;
  }
  return true;
}

static bool
ufsNop (UfsQueue *q)
{
  IOLog ("NATIVE_UFS_NOP: enter\n");
  if (!q) {
    return false;
  }
  IOLog ("NATIVE_UFS_NOP: precheck\n");
  uint32_t doorbell = read32 (q->hc, UFS_REG_UTRLDBR);
  IOLog ("NATIVE_UFS_NOP: precheck poisoned=%u db=%x\n",
         q->poisoned ? 1 : 0, doorbell);
  if (q->poisoned || doorbell) {
    return false;
  }
  zero32 (q->ucd, UFS_FW_UCD_SIZE);
  q->ucd[3] = 1;
  gNopActive = true;
  IOLog ("NATIVE_UFS_NOP: before submit\n");
  bool submitted = ufsSubmit (q, 0, 0, UFS_TIMEOUT_TICKS);
  gNopActive = false;
  IOLog ("NATIVE_UFS_NOP: after submit ok=%u poisoned=%u\n",
         submitted ? 1 : 0, q->poisoned ? 1 : 0);
  if (!submitted) {
    return false;
  }
  const uint8_t *response = q->ucd + UFS_RSP_OFFSET;
  return response[0] == 0x20 && response[3] == 1;
}

// H138: is the Legacy framebuffer service visible to IOKit matching at all?
// backboardd never issues an IOMobile* match in the logs, so first prove the
// service can be found from the kernel side, and under which class name.
static void
h103ProbeIomfb (void)
{
  static volatile uint64_t lastTicks;
  uint64_t now = mach_absolute_time ();
  if (!gH103StartTicks) {
    return;
  }
  if (now - gH103StartTicks < 60 * 19200000ULL) {
    return;
  }
  if (lastTicks && now - lastTicks < 60 * 19200000ULL) {
    return;
  }
  lastTicks = now;
  // H139: the verdict also goes into native_boot_flags, which every NATIVE_IRQ
  // line prints.  A one-shot IOLog lands in the dyld-cache flood and is
  // routinely trimmed by the slot delta, which is what happened in H138.
  // 0x80000 is a control: backboardd itself matches IOPlatformExpertDevice
  // with ok=1, so this separates "our service is not matchable" from
  // "this API does not match by class name at all".
  // 0x100000 asks for the concrete class name, separating "not in the
  // registry" from "present but not matchable as an IOMobileFramebuffer".
  const uint32_t bits[4] = { 0x20000U, 0x40000U, 0x80000U, 0x100000U };
  const char *names[4] = { "IOMobileFramebuffer", "IOMobileFramebufferLegacy",
                           "IOPlatformExpertDevice", "NativeLegacyDisplay" };
  for (unsigned i = 0; i < 4; i++) {
    OSDictionary *matching = IOService::serviceMatching (names[i]);
    IOService *service = matching ? IOService::copyMatchingService (matching) : nullptr;
    if (service) {
      native_boot_flags |= bits[i];
    } else {
      native_boot_flags &= ~bits[i];
    }
    IOLog ("NATIVE_IOMFB_PROBE: name=%s match=%u service=%p class=%s flags=%x\n",
           names[i], matching ? 1 : 0, service,
           service ? service->getMetaClass ()->getClassName () : "-",
           native_boot_flags);
    OSSafeReleaseNULL (service);
    OSSafeReleaseNULL (matching);
  }
}

bool
NativeUfsProbe::readBlocks (uint8_t unit, uint64_t block, uint64_t count,
                            uint32_t blockSize, IOMemoryDescriptor *dst,
                            uint64_t *done)
{
  if (!fReady || !fLock || !dst || dst->getDirection () != kIODirectionIn ||
      !blockSize || block > 0xffffffffULL) {
    return false;
  }
  uint64_t total = 0;
  bool ok = true;
  while (count) {
    unsigned chunk = static_cast<unsigned> (fQueue.dataCapacity / blockSize);
    if (chunk == 0) {
      ok = false;
      break;
    }
    if (count < chunk) {
      chunk = static_cast<unsigned> (count);
    }
    // H128: one command per lock.  The old code held fLock across the whole
    // request, so a 1 MiB read blocked every other UFS user for eight chunks.
    if (!lockWithTimeout (fLock, "readBlocks", UFS_DATA_LOCK_TICKS, true)) {
      IOLog ("NATIVE_UFS_LOCK: readBlocks blocked block=%llx count=%llu\n",
             block, count);
      ok = false;
      break;
    }
    uint8_t cdb[16] = { 0 };
    cdb[0] = 0x28;
    putBe32 (cdb + 2, static_cast<uint32_t> (block));
    cdb[7] = static_cast<uint8_t> (chunk >> 8);
    cdb[8] = static_cast<uint8_t> (chunk);
    unsigned bytes = chunk * blockSize;
    gUfsLastLba = block;
    gUfsLastBytes = bytes;
    bool commandOk = ufsCommandRetry (&fQueue, unit, cdb, bytes, nullptr, "readBlocks");
    bool copyOk = commandOk &&
        dst->writeBytes (static_cast<IOByteCount> (total), fQueue.data,
                         static_cast<IOByteCount> (bytes)) ==
        static_cast<IOByteCount> (bytes);
    IOLockUnlock (fLock);
    if (!commandOk || !copyOk) {
      ok = false;
      break;
    }
    total += bytes;
    block += chunk;
    count -= chunk;
  }
  if (done) {
    *done = total;
  }
  return ok;
}

bool
NativeUfsProbe::readWindowBulk (uint64_t lba, uint32_t blocks, void *out)
{
  if (!fReady || !fLock || !out || !blocks ||
      blocks > UFS_DATA_MAX / 4096 ||
      lba > 0xffffffffULL - (blocks - 1)) {
    return false;
  }
  // H172: one READ(10) may not exceed the queue's DMA capacity.  H171 can
  // hand us a smaller low-memory buffer, and the fixed 128 KB command then
  // failed instantly (ufsCommand rejects bytes > dataCapacity), which is why
  // every window read returned ok=0 and mountroot never saw APFS.  Split the
  // request into capacity-sized commands, one lock per command (H128).
  const uint32_t chunkBlocks = fQueue.dataCapacity / 4096U;
  if (!chunkBlocks) {
    return false;
  }
  uint8_t *dst = static_cast<uint8_t *> (out);
  uint32_t remaining = blocks;
  uint64_t at = lba;
  bool allOk = true;
  uint64_t firstLock = 0, firstCommand = 0;
  unsigned calls = 0;
  while (remaining) {
    uint32_t chunk = remaining < chunkBlocks ? remaining : chunkBlocks;
    uint64_t lockStart = mach_absolute_time ();
    if (!lockWithTimeout (fLock, "readWindowBulk", UFS_DATA_LOCK_TICKS, true)) {
      IOLog ("NATIVE_UFS_LOCK: readWindowBulk blocked lba=%llx blocks=%u wait=%llu\n",
             at, chunk, (unsigned long long) (mach_absolute_time () - lockStart));
      allOk = false;
      break;
    }
    uint64_t lockTicks = mach_absolute_time () - lockStart;
    uint8_t cdb[16] = { 0x28 };
    putBe32 (cdb + 2, static_cast<uint32_t> (at));
    cdb[7] = static_cast<uint8_t> (chunk >> 8);
    cdb[8] = static_cast<uint8_t> (chunk);
    unsigned bytes = chunk * 4096U;
    gUfsLastLba = at;
    gUfsLastBytes = bytes;
    uint64_t commandStart = mach_absolute_time ();
    bool ok = ufsCommandRetry (&fQueue, 0, cdb, bytes, dst, "readWindow");
    uint64_t commandTicks = mach_absolute_time () - commandStart;
    IOLockUnlock (fLock);
    if (!calls) {
      firstLock = lockTicks;
      firstCommand = commandTicks;
    }
    calls++;
    if (!ok) {
      allOk = false;
      // H305: a window read that fails is what APFS reports as err 5 and what a process later
      // faults on (SIGBUS). Count it and name the LBA, rate-limited so a storm stays readable.
      extern volatile uint32_t native_ufs_read_failures;
      native_ufs_read_failures = native_ufs_read_failures + 1;
      if (native_ufs_read_failures <= 8 || (native_ufs_read_failures % 64) == 0) {
        IOLog ("NATIVE_UFS_READFAIL: n=%u lba=0x%llx blocks=%u\n",
               (unsigned) native_ufs_read_failures, (unsigned long long) at,
               (unsigned) chunkBlocks);
      }
      break;
    }
    dst += bytes;
    at += chunk;
    remaining -= chunk;
  }
  static unsigned gBulkPhases;
  unsigned call = gBulkPhases++;
  if (!gH103Quiet &&
      (call < 8 || firstCommand > 19200000ULL || firstLock > 19200000ULL)) {
    IOLog ("NATIVE_UFS_BULK: lba=%llx blocks=%u chunk=%u calls=%u lock=%llu cmd=%llu ok=%u\n",
           lba, blocks, chunkBlocks, calls,
           (unsigned long long) firstLock, (unsigned long long) firstCommand,
           allOk ? 1 : 0);
  }
  return allOk;
}

bool
NativeUfsProbe::readWindow4k (uint64_t lba, void *out)
{
  return readWindowBulk (lba, 1, out);
}

bool
NativeUfsProbe::logWrite (uint64_t rel, uint32_t blocks, const void *src,
                          uint64_t lockTicks)
{
  if (!fReady || !fLock || !src || !blocks || rel >= fLogBlocks ||
      blocks > fLogBlocks - rel || blocks > UFS_DATA_MAX / 4096) {
    return false;
  }
  if (!fLogDma || !fLogData || !fLogDataPhys) {
    return false;
  }
  if (blocks > fLogCapacity / 4096U) {
    blocks = fLogCapacity / 4096U;
    if (!blocks) {
      return false;
    }
  }
  const unsigned bytes = blocks * 4096U;
  // H319: never wait for the I/O lock in the log path.  A single try-lock means a busy queue costs
  // the log one slot and costs the main path nothing; the caller backs off on failure.
  (void) lockTicks;
  if (!IOLockTryLock (fLock)) {
    return false;
  }
  uint8_t cdb[16] = { 0x2a };
  cdb[1] = 0x08;  // FUA: commit this log write without a SYNCHRONIZE CACHE
  putBe32 (cdb + 2, static_cast<uint32_t> (fLogFirst + rel));
  cdb[7] = static_cast<uint8_t> (blocks >> 8);
  cdb[8] = static_cast<uint8_t> (blocks);
  // getBytesNoCopy() on the DMA descriptor is not a CPU-write mapping here.
  // Fill it through the IOMemoryDescriptor writeBytes() path instead, then
  // pass the DMA pointer as src so ufsWriteCommand() skips its own memcpy.
  IOByteCount copied = fLogDma->writeBytes (0, src, bytes);
  if (copied != static_cast<IOByteCount> (bytes)) {
    IOLog ("NATIVE_UFS_LOG: log DMA writeBytes short=%llu want=%u lba=%llx\n",
           (unsigned long long) copied, bytes,
           (unsigned long long) (fLogFirst + rel));
    IOLockUnlock (fLock);
    return false;
  }
  uint8_t *savedData = fQueue.data;
  uint64_t savedPhys = fQueue.dataPhys;
  uint32_t savedCapacity = fQueue.dataCapacity;
  fQueue.data = fLogData;
  fQueue.dataPhys = fLogDataPhys;
  fQueue.dataCapacity = fLogCapacity;
  bool ok = ufsWriteCommand (&fQueue, 0, cdb, bytes, fLogData);
  fQueue.data = savedData;
  fQueue.dataPhys = savedPhys;
  fQueue.dataCapacity = savedCapacity;
  IOLockUnlock (fLock);
  return ok;
}

// The reserved marker shares the normal queue and DMA buffers. Every caller must
// acquire the I/O lock without waiting. Read it at offset 250*4096 of the log pull.
bool
NativeUfsProbe::logMarker (const char *kind, bool)
{
  if (!fReady || !fLock || !fLogData || !fLogDma || !fLogDataPhys || !fLogCapacity || !fQueue.data) {
    return false;
  }
  if (fLogBlocks <= 250) {
    return false;
  }
  if (!IOLockTryLock (fLock)) {
    return false;
  }
  if (fQueue.poisoned) {
    IOLockUnlock (fLock);
    return false;
  }
  static char block[4096];
  memset (block, 0, sizeof block);
  const uint32_t uptime = gH103StartTicks ?
      static_cast<uint32_t> ((mach_absolute_time () - gH103StartTicks) / 19200000ULL) : 0;
  // H312: how long ago the heartbeat last moved is the single most useful field here, so record
  // it rather than making the reader compare timestamps from two different records.
  const uint64_t hbAgoTicks = gH103BeatTicks ?
      (mach_absolute_time () - gH103BeatTicks) : 0;
  // H341: the ring's first write is gated on a storage request and the console stops painting once
  // the display driver owns the panel, so neither can answer "is CoreAnimation submitting?".
  // This record is written every minute regardless: val/xf/fr are the display-side counters.
  snprintf (block, sizeof block,
      "NXMARK1 kind=%s uptime=%u hbago=%llu beat=%u cmds=%llu flags=%x build=%s "
      "val=%u xf=%u fr=%u\n",
      kind ? kind : "?", (unsigned) uptime,
      (unsigned long long) (hbAgoTicks / 19200000ULL), (unsigned) gH103SlotCursor,
      (unsigned long long) gUfsCommands, (unsigned) native_boot_flags, H103_BUILD,
      (unsigned) native_legacy_swap_validation_calls,
      (unsigned) native_legacy_transfer_calls,
      (unsigned) native_legacy_frame_count);
  // H314: the ring flush can lose the race for fLock at panic time, and the ring is what normally
  // carries NATIVE_PANIC_STR.  The message buffer is readable from this context, so the marker
  // block carries its tail as well - that is the panic reason without needing a second boot.
  {
    struct H103MsgBuf *m = msgbufp;
    int size = (m && m->msg_size > 0) ? m->msg_size : 0;
    int cursor = (m && m->msg_bufx >= 0 && m->msg_bufx < m->msg_size) ? m->msg_bufx : 0;
    size_t used = strlen (block);
    const int want = 1200;
    if (size > want && used + want + 2 < sizeof block) {
      block[used++] = '\n';
      for (int i = 0; i < want; ++i) {
        char ch = m->msg_bufc[(cursor + size - want + i) % size];
        block[used + i] = (ch == '\n' || (ch >= 32 && ch < 127)) ? ch : ' ';
      }
      used += want;
      block[used] = 0;
    }
  }
  if (fLogDma->writeBytes (0, block, sizeof block) !=
      static_cast<IOByteCount> (sizeof block)) {
    IOLockUnlock (fLock);
    return false;
  }
  uint8_t cdb[16] = { 0x2a };
  cdb[1] = 0x08;  // FUA: nothing else is going to sync this write
  putBe32 (cdb + 2, static_cast<uint32_t> (fLogFirst + 250));
  cdb[7] = 0;
  cdb[8] = 1;
  uint8_t *savedData = fQueue.data;
  uint64_t savedPhys = fQueue.dataPhys;
  uint32_t savedCapacity = fQueue.dataCapacity;
  fQueue.data = fLogData;
  fQueue.dataPhys = fLogDataPhys;
  fQueue.dataCapacity = fLogCapacity;
  bool ok = ufsWriteCommand (&fQueue, 0, cdb, 4096, fLogData);
  fQueue.data = savedData;
  fQueue.dataPhys = savedPhys;
  fQueue.dataCapacity = savedCapacity;
  IOLockUnlock (fLock);
  return ok;
}

bool
NativeUfsProbe::logSync (void)
{
  if (!fReady || !fLock) {
    return false;
  }
  if (!lockWithTimeout (fLock, "logSync", 38400000ULL, false)) {
    return false;
  }
  bool ok = ufsSync (&fQueue);
  IOLockUnlock (fLock);
  return ok;
}

void
NativeUfsProbe::logThread (void *raw, wait_result_t)
{
  NativeUfsProbe *self = static_cast<NativeUfsProbe *> (raw);
  if (!self) {
    return;
  }
  thread_set_thread_name (current_thread (), "h103_log");
  // Let root mount and the early display/root messages enter the kernel ring.
  IOSleep (20000);
  self->h103Snapshot ("thread", false, false);
}

bool
NativeUfsProbe::h103Snapshot (const char *tag, bool pause, bool verify)
{
  if (!fLogFirst || !fLogBlocks || !fReady || !fLock) {
    IOLog ("NATIVE_UFS_LOG: snapshot skipped tag=%s first=%llx blocks=%u "
           "ready=%u lock=%u\n", tag ? tag : "?", (unsigned long long) fLogFirst,
           fLogBlocks, fReady ? 1 : 0, fLock ? 1 : 0);
    return false;
  }

  // Write the self-test before inspecting the window. The old code returned
  // early when block 0 held stale data, which made every retry after a partial
  // write impossible to distinguish from a dead writer.
  //
  // Keep the two 4 KiB self-test buffers off the stack: start() already uses a
  // 9 KiB frame and h103Snapshot() used to add another 8 KiB, overflowing the
  // 16 KiB kernel stack before the first WRITE(10).
  uint8_t *test = static_cast<uint8_t *> (IOMallocAligned (4096, 4096));
  uint8_t *check = static_cast<uint8_t *> (IOMallocAligned (4096, 4096));
  if (!test || !check) {
    if (test) {
      IOFreeAligned (test, 4096);
    }
    if (check) {
      IOFreeAligned (check, 4096);
    }
    IOLog ("NATIVE_UFS_LOG: snapshot test allocation failed tag=%s\n",
           tag ? tag : "?");
    if (pause) {
      IOSleep (30000);
    }
    return false;
  }
  memset (test, 0, 4096);
  memset (check, 0, 4096);
  memcpy (test, "H103TEST", 8);
  snprintf (reinterpret_cast<char *> (test + 16), 128,
            "tag=%s lba=%llx wrote=0 read=0", tag ? tag : "?",
            (unsigned long long) fLogFirst);
  bool testWrite = logWrite (0, 1, test);
  bool testRead = false;
  if (verify) {
    testRead = testWrite && readWindow4k (fLogFirst, check) &&
               memcmp (check, "H103TEST", 8) == 0;
  }
  snprintf (reinterpret_cast<char *> (test + 16), 128,
            "tag=%s lba=%llx wrote=%u read=%u", tag ? tag : "?",
            (unsigned long long) fLogFirst, testWrite ? 1 : 0,
            testRead ? 1 : 0);
  logWrite (0, 1, test);
  bool endWrite = logWrite (H103_LOG_BLOCKS - 1, 1, test);
  IOLog ("NATIVE_UFS_LOG: write test tag=%s verify=%u wrote=%u read=%u end=%u "
         "lba=%llx\n", tag ? tag : "?", verify ? 1 : 0, testWrite ? 1 : 0,
         testRead ? 1 : 0, endWrite ? 1 : 0,
         (unsigned long long) fLogFirst);
  if (!testWrite || (verify && !testRead)) {
    IOFreeAligned (test, 4096);
    IOFreeAligned (check, 4096);
    if (pause) {
      IOSleep (30000);
    }
    return false;
  }
  IOFreeAligned (check, 4096);
  check = nullptr;
  IOFreeAligned (test, 4096);
  test = nullptr;

  uint8_t *body = static_cast<uint8_t *> (
    IOMallocAligned (H103_LOG_MSG_BLOCKS * 4096U, 4096));
  if (!body) {
    IOLog ("NATIVE_UFS_LOG: snapshot body allocation failed tag=%s\n",
           tag ? tag : "?");
    if (pause) {
      IOSleep (30000);
    }
    return false;
  }
  uint32_t msgBytes = h103CopyMsgbuf (body);
  bool ok = true;
  for (uint32_t offset = 0; ok && offset < H103_LOG_MSG_BLOCKS; offset += 32) {
    ok = logWrite (1 + offset, 32, body + offset * 4096U);
  }

  uint8_t *head = static_cast<uint8_t *> (IOMallocAligned (4096, 4096));
  if (!head) {
    IOFreeAligned (body, H103_LOG_MSG_BLOCKS * 4096U);
    IOLog ("NATIVE_UFS_LOG: snapshot header allocation failed tag=%s\n",
           tag ? tag : "?");
    if (pause) {
      IOSleep (30000);
    }
    return false;
  }
  memset (head, 0, 4096);
  H103LogHeader h;
  memset (&h, 0, sizeof h);
  memcpy (h.magic, "SHLOG001", 8);
  h.version = 3;
  h.header_bytes = sizeof h;
  h.boot_id = h103LogTicks ();
  h.first_lba = fLogFirst;
  h.window_blocks = fLogBlocks;
  h.msg_first = 1;
  h.msg_blocks = H103_LOG_MSG_BLOCKS;
  h.msg_bytes = msgBytes;
  h.slot_first = 257;
  h.slot_blocks = 32;
  h.slot_count = (fLogBlocks > 257) ? (fLogBlocks - 257) / 32 : 0;
  h.slots_written = gH103SlotCursor;
  h.rounds = gH103SlotCursor ? gH103SlotCursor : 1;
  h.uptime_s = gH103StartTicks ?
      static_cast<uint32_t> ((mach_absolute_time () - gH103StartTicks) / 19200000ULL) : 0;
  h.write_errors = gH103SlotWriteErrors;
  h.export_first = static_cast<uint32_t> (gUfsCommands);
  h.export_blocks = static_cast<uint32_t> (gUfsLastLba);
  h.export_bytes = static_cast<uint32_t> (gUfsLastBytes);
  h.export_generation = ((gUfsInFlight ? 1U : 0U) | (gWindowInFlight ? 2U : 0U) |
                         ((gH103TimerFires & 0xffffffU) << 8));
  h.export_below = static_cast<uint32_t> (gUfsLastLoops);
  h.state = gH103SlotCursor ? 3 : 2;
  strlcpy (h.tag, "cheeseburger-H103", sizeof h.tag);
  memcpy (head, &h, sizeof h);
  ok = ok && logWrite (0, 1, head);
  // Do not issue SYNCHRONIZE CACHE here. The WRITE(10) uses FUA; the UFS sync
  // command has been observed to hold fLock indefinitely, which also blocks
  // the heartbeat late snapshot and 2x2 redraw.
  IOLog ("NATIVE_UFS_LOG: snapshot tag=%s ok=%u msg=%u first=%llx blocks=%u\n",
         tag ? tag : "?", ok ? 1 : 0, msgBytes,
         (unsigned long long) fLogFirst, fLogBlocks);
  IOFreeAligned (head, 4096);
  IOFreeAligned (body, H103_LOG_MSG_BLOCKS * 4096U);
  if (pause) {
    IOSleep (30000);
  }
  return ok;
}

void
NativeUfsProbe::h103SlotWrite (void)
{
  static uint32_t attempts;
  bool ready = fLogFirst && fLogBlocks && fReady && fLock && fLogDma;
  if (!ready) {
    if (attempts++ < 8) {
      IOLog ("NATIVE_UFS_SLOT: wait=%u first=%llx blocks=%u ready=%u "
             "lock=%u dma=%u\n", attempts,
             (unsigned long long) fLogFirst, fLogBlocks,
             fReady ? 1 : 0, fLock ? 1 : 0, fLogDma ? 1 : 0);
    }
    return;
  }
  const uint32_t slotCount = (fLogBlocks > 257) ? (fLogBlocks - 257) / 32 : 0;
  if (!slotCount) {
    if (attempts++ < 8) {
      IOLog ("NATIVE_UFS_SLOT: no slots blocks=%u\n", fLogBlocks);
    }
    return;
  }
  native_h103_slot_state = 1;
  uint8_t *body = gH103SlotBody;
  // H106/H179: write only the bytes produced since the previous flush.  A
  // delta larger than one 32-block slot is split across consecutive slots
  // instead of dropping its older half (that is how the mount and display
  // probe lines disappeared from earlier logs).
  struct H103MsgBuf *m = msgbufp;
  uint32_t size = (m && m->msg_size > 0) ? static_cast<uint32_t> (m->msg_size) : 0;
  uint32_t cursor = (m && m->msg_bufx >= 0 && m->msg_bufx < m->msg_size) ?
      static_cast<uint32_t> (m->msg_bufx) : 0;
  uint32_t previous = gH103LastSlotMsgX < 0 ? 0 : static_cast<uint32_t> (gH103LastSlotMsgX);
  uint32_t delta = size ? (cursor + size - previous) % size : 0;
  gH103FlushRounds++;
  if (!delta) {
    gH103FlushIdleRounds++;
  } else {
    gH103FlushIdleRounds = 0;
  }
  const uint32_t stampBytes = 128;
  const uint32_t chunkPayload = 32U * 4096U - stampBytes;
  const uint32_t uptime = gH103StartTicks ?
      static_cast<uint32_t> ((mach_absolute_time () - gH103StartTicks) / 19200000ULL) : 0;
  uint32_t offset = size ? previous % size : 0;
  uint32_t remaining = delta;
  do {
    uint32_t n = remaining < chunkPayload ? remaining : chunkPayload;
    uint32_t blocks = (stampBytes + n + 4095U) / 4096U;
    if (!blocks) {
      blocks = 1;
    }
    char stamp[128];
    int stampLen = snprintf (stamp, sizeof stamp,
        "SHLOGS v2 boot=%llx seq=%u build=%s uptime=%u bytes=%u",
        (unsigned long long) gH103BootId, (unsigned) gH103SlotCursor, H103_BUILD,
        (unsigned) uptime, (unsigned) n);
    if (stampLen < 0 || static_cast<uint32_t> (stampLen) >= stampBytes) {
      stampLen = 0;
    }
    memset (body, ' ', stampBytes);
    memcpy (body, stamp, static_cast<size_t> (stampLen));
    body[stampBytes - 1] = '\n';
    for (uint32_t i = 0; i < n; ++i) {
      body[stampBytes + i] =
          static_cast<uint8_t> (m->msg_bufc[(offset + i) % size]);
    }
    uint32_t slot = gH103SlotCursor % slotCount;
    // H317: a contended lock drops this slot instead of stalling the heartbeat, and the next
    // flushes back off briefly so the main I/O path keeps the queue to itself.
    bool wrote = logWrite (257U + slot * 32U, blocks, body, UFS_LOG_LOCK_TICKS);
    if (!wrote) {
      gH103FlushBackoffTicks = mach_absolute_time () + UFS_LOG_BACKOFF_TICKS;
    }
    if (!wrote) {
      gH103SlotWriteErrors = gH103SlotWriteErrors + 1;
      // H366: the ring going quiet is the one event we cannot read back from the ring, so also
      // stamp it into the lock-free reserved block while the disk still lets us.
      if ((gH103SlotWriteErrors % 8) == 1) {
        h103Marker ("logstop", true);
      }
      if (gH103SlotWriteErrors <= 8 || (gH103SlotWriteErrors % 32) == 0) {
        IOLog ("NATIVE_UFS_LOG_FAIL: slot=%u seq=%u errors=%u delta=%u n=%u blocks=%u\n",
               slot, (unsigned) gH103SlotCursor, (unsigned) gH103SlotWriteErrors,
               delta, n, blocks);
      }
    }
    if (attempts++ < 8) {
      IOLog ("NATIVE_UFS_SLOT: attempt=%u slot=%u lba=%llx body=%u blocks=%u "
             "delta=%u n=%u fires=%u\n",
             attempts, slot, (unsigned long long) (fLogFirst + 257U + slot * 32U),
             wrote ? 1 : 0, blocks, delta, n, gH103TimerFires);
    }
    if (!wrote) {
      break;
    }
    if ((gH103FlushRounds % 60) == 1) {
      IOLog ("NATIVE_UFS_LOG_STAT: rounds=%u seq=%u errors=%u idle=%u skips=%u delta=%u n=%u\n",
             (unsigned) gH103FlushRounds, (unsigned) (gH103SlotCursor + 1),
             (unsigned) gH103SlotWriteErrors, (unsigned) gH103FlushIdleRounds,
             (unsigned) gH103FlushSkips, delta, n);
    }
    H103LogHeader h;
    memset (&h, 0, sizeof h);
    memcpy (h.magic, "SHLOG001", 8);
    h.version = 3;
    h.header_bytes = sizeof h;
    h.boot_id = h103LogTicks ();
    h.first_lba = fLogFirst;
    h.window_blocks = fLogBlocks;
    h.msg_first = 1;
    h.msg_blocks = H103_LOG_MSG_BLOCKS;
    h.msg_bytes = n;
    h.slot_first = 257;
    h.slot_blocks = 32;
    h.slot_count = slotCount;
    h.slots_written = gH103SlotCursor + 1;
    h.rounds = gH103SlotCursor + 1;
    h.uptime_s = gH103StartTicks ?
        static_cast<uint32_t> ((mach_absolute_time () - gH103StartTicks) / 19200000ULL) : 0;
    h.write_errors = gH103SlotWriteErrors;
    h.export_first = static_cast<uint32_t> (gUfsCommands);
    h.export_blocks = static_cast<uint32_t> (gUfsLastLba);
    h.export_bytes = static_cast<uint32_t> (gUfsLastBytes);
    h.export_generation = ((gUfsInFlight ? 1U : 0U) | (gWindowInFlight ? 2U : 0U) |
                         ((gH103TimerFires & 0xffffffU) << 8));
    h.export_below = static_cast<uint32_t> (gUfsLastLoops);
    h.state = 3;
    strlcpy (h.tag, "cheeseburger-H103", sizeof h.tag);
    uint8_t *head = gH103SlotHeader;
    memset (head, 0, 4096);
    memcpy (head, &h, sizeof h);
    bool headOk = logWrite (0, 1, head);
    if (attempts < 8) {
      IOLog ("NATIVE_UFS_SLOT: header=%u cursor=%u\n",
             headOk ? 1 : 0, gH103SlotCursor);
    }
    if (!headOk) {
      gH103SlotWriteErrors = gH103SlotWriteErrors + 1;
      break;
    }
    gH103SlotCursor++;
    gH103LastMsgBytes = n;
    gH103LastHeaderTicks = mach_absolute_time ();
    gH103LastSlotSuccessTicks = mach_absolute_time ();
    native_h103_slot_state = 2U;
    offset = size ? (offset + n) % size : 0;
    // Advance only after both this slot and its header were persisted. A busy
    // queue or failed write leaves the remaining delta available to retry.
    gH103LastSlotMsgX = static_cast<int> (offset);
    remaining -= n;
  } while (remaining);
}

void
NativeUfsProbe::h103HeaderOnly (void)
{
  const uint32_t slotCount = (fLogBlocks > 257) ? (fLogBlocks - 257) / 32 : 0;
  if (!fLogFirst || !fLogBlocks || !fLogDma || !fLogData || !fLogDataPhys ||
      !slotCount) {
    return;
  }
  H103LogHeader h;
  memset (&h, 0, sizeof h);
  memcpy (h.magic, "SHLOG001", 8);
  h.version = 3;
  h.header_bytes = sizeof h;
  h.boot_id = h103LogTicks ();
  h.first_lba = fLogFirst;
  h.window_blocks = fLogBlocks;
  h.msg_first = 1;
  h.msg_blocks = H103_LOG_MSG_BLOCKS;
  h.msg_bytes = gH103LastMsgBytes;
  h.slot_first = 257;
  h.slot_blocks = 32;
  h.slot_count = slotCount;
  h.slots_written = gH103SlotCursor;
  h.rounds = gH103SlotCursor ? gH103SlotCursor : 1;
  h.uptime_s = gH103StartTicks ?
      static_cast<uint32_t> ((mach_absolute_time () - gH103StartTicks) / 19200000ULL) : 0;
  h.write_errors = gH103SlotWriteErrors;
  h.export_first = static_cast<uint32_t> (gUfsCommands);
  h.export_blocks = static_cast<uint32_t> (gUfsLastLba);
  h.export_bytes = static_cast<uint32_t> (gUfsLastBytes);
  h.export_generation = ((gUfsInFlight ? 1U : 0U) | (gWindowInFlight ? 2U : 0U) |
                         ((gH103TimerFires & 0xffffffU) << 8));
  h.export_below = static_cast<uint32_t> (gUfsLastLoops);
  h.state = gH103SlotCursor ? 3 : 2;
  strlcpy (h.tag, "cheeseburger-H103", sizeof h.tag);
  uint8_t *head = gH103SlotHeader;
  memset (head, 0, 4096);
  memcpy (head, &h, sizeof h);
  bool ok = logWrite (0, 1, head);
  if (!ok) {
    gH103SlotWriteErrors = gH103SlotWriteErrors + 1;
  } else {
    gH103LastSlotSuccessTicks = mach_absolute_time ();
    gH103LastHeaderTicks = mach_absolute_time ();
  }
}

void
NativeUfsProbe::free (void)
{
  if (gH103TimerCall) {
    timer_call_cancel (gH103TimerCall);
    timer_call_free (gH103TimerCall);
    gH103TimerCall = nullptr;
  }
  if (fHbTimer) {
    fHbTimer->cancelTimeout ();
    if (fHbLoop) {
      fHbLoop->removeEventSource (fHbTimer);
    }
    fHbTimer->release ();
    fHbTimer = nullptr;
  }
  if (fHbLoop) {
    fHbLoop->release ();
    fHbLoop = nullptr;
  }
  IOService::free ();
}

void
NativeUfsProbe::hbTimerFired (OSObject *owner, IOTimerEventSource *sender)
{
  NativeUfsProbe *self = static_cast<NativeUfsProbe *> (owner);
  if (self) {
    self->hbTimerTick (sender);
  }
}

void
NativeUfsProbe::hbTimerTick (IOTimerEventSource *sender)
{
  static unsigned fires;
  static int stallLogged;
  fires++;
  if (fires <= 4) {
    IOLog ("NATIVE_UFS_HB_TIMER: fire=%u cmds=%llu\n", fires,
           (unsigned long long) gUfsCommands);
  }
  if (!gH103SkipPattern) {
    native_legacy_test_pattern ();
  }
  uint64_t now = mach_absolute_time ();
  if (gUfsInFlight && gWindowStartTicks &&
      now - gWindowStartTicks > 5 * 19200000ULL) {
    if (!stallLogged) {
      stallLogged = 1;
      IOLog ("NATIVE_UFS_HB_TIMER: stall cmds=%llu last=%llx/%llu "
             "req=%llx/%llu winFlight=%llu ufsFlight=%llu loops=%llu\n",
             (unsigned long long) gUfsCommands,
             (unsigned long long) gUfsLastLba,
             (unsigned long long) gUfsLastBytes,
             (unsigned long long) gWindowLastReqBlock,
             (unsigned long long) gWindowLastReqCount,
             (unsigned long long) gWindowInFlight,
             (unsigned long long) gUfsInFlight,
             (unsigned long long) gUfsLastLoops);
    }
  } else {
    stallLogged = 0;
  }
  fHbNext += 19200000ULL;
  if (fHbNext <= now) {
    fHbNext = now + 19200000ULL;
  }
  if (sender) {
    sender->wakeAtTime (fHbNext);
  }
}

void
NativeUfsProbe::h103WindowRead (void)
{
  // The real-time slot writer in the heartbeat owns the persistent log now.
  // Do not write a 1 MiB body from the storage completion path.
}

static uint8_t gH103Test[4096] __attribute__((aligned(4096)));

void
NativeUfsProbe::h103SingleWrite (void)
{
  if (!fLogFirst || !fLogBlocks || !fReady || !fLock ||
      !fLogData || !fLogDataPhys) {
    IOLog ("NATIVE_UFS_LOG: single write skipped first=%llx ready=%u "
           "lock=%u dma=%u\n", (unsigned long long) fLogFirst,
           fReady ? 1 : 0, fLock ? 1 : 0,
           (fLogData && fLogDataPhys) ? 1 : 0);
    return;
  }
  memset (gH103Test, 0, sizeof gH103Test);
  memcpy (gH103Test, "H103TEST", 8);
  snprintf (reinterpret_cast<char *> (gH103Test + 16), 128,
            "single lba=%llx", (unsigned long long) fLogFirst);
  IOLog ("NATIVE_UFS_LOG: single write begin lba=%llx\n",
         (unsigned long long) fLogFirst);
  bool ok = logWrite (0, 1, gH103Test);
  IOLog ("NATIVE_UFS_LOG: single write result=%u lba=%llx\n",
         ok ? 1 : 0, (unsigned long long) fLogFirst);
  IOSleep (30000);
}

bool
NativeUfsWindow::configure (NativeUfsProbe *parent, const char *name,
                            uint64_t absoluteByte, uint64_t length)
{
  host = parent;
  absolute = absoluteByte;
  bytes = length;
  bounce = static_cast<uint8_t *> (IOMalloc (UFS_DATA_MAX));
  readAhead = static_cast<uint8_t *> (IOMalloc (UFS_DATA_MAX));
  readAheadLba = 0;
  readAheadBytes = 0;
  lock = IOLockAlloc ();
  if (name) {
    strlcpy (label, name, sizeof (label));
  }
  setProperty ("B5Window", label);
  setProperty ("B5Offset", absolute, 64);
  setProperty ("B5Bytes", bytes, 64);
  setProperty ("B5ReadOnly", true);
  if (readAhead) {
    IOLog ("NATIVE_UFS_CACHE: %s read-ahead=%u bytes\n", label, UFS_DATA_MAX);
  }
  return host && absolute && bytes && !(absolute & 4095) && !(bytes & 511) &&
         bounce && readAhead && lock;
}

IOReturn
NativeUfsWindow::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block,
                                   UInt64 count, IOStorageAttributes *,
                                   IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count ||
      block >= bytes / 512 || count > bytes / 512 - block ||
      buffer->getLength () < count * 512) {
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection () != kIODirectionIn) {
    return kIOReturnNotWritable;
  }
  if (!online || !bounce || !readAhead || !lock) {
    return kIOReturnNotReady;
  }
  uint64_t request = gWindowRequests;
  gWindowRequests = request + 1;
  gWindowBytes = gWindowBytes + count * 512;
  gWindowLastBlock = block;
  gWindowLastCount = count;
  gWindowLastReqBlock = block;
  gWindowLastReqCount = count;
  gWindowStartTicks = mach_absolute_time ();
  gWindowInFlight = 1;
  if (!gH103Quiet && (request < 8 || (request & 1023) == 0)) {
    IOLog ("NATIVE_UFS_WINDOW: %s req=%llu block=%llx count=%llu\n",
           label, request, block, count);
  }
  uint64_t requestStart = mach_absolute_time ();
  uint64_t cursor = absolute + block * 512;
  uint64_t end = cursor + count * 512;
  uint64_t done = 0;
  uint64_t readTicks = 0;
  uint64_t writeTicks = 0;
  bool ok = true;
  uint64_t lockStart = mach_absolute_time ();
  if (!lockWithTimeout (lock, label, UFS_DATA_LOCK_TICKS, true)) {
    IOLog ("NATIVE_UFS_LOCK: window %s blocked req=%llu block=%llx count=%llu\n",
           label, request, block, count);
    gWindowInFlight = 0;
    return kIOReturnTimeout;
  }
  uint64_t lockTicks = mach_absolute_time () - lockStart;
  while (cursor < end) {
    uint64_t page = cursor & ~4095ULL;
    unsigned inPage = static_cast<unsigned> (cursor - page);
    // APFS issues many sequential 4 KiB reads. Serve them from a page-aligned
    // 128 KiB read-ahead window so one UFS command covers up to 32 requests.
    if (readAheadBytes && page >= readAheadLba &&
        page + 4096 <= readAheadLba + readAheadBytes) {
      uint64_t offset = cursor - readAheadLba;
      uint64_t available = readAheadBytes - offset;
      unsigned wanted = static_cast<unsigned> (end - cursor);
      if (wanted > available) {
        wanted = static_cast<unsigned> (available);
      }
      uint64_t phase = mach_absolute_time ();
      IOByteCount copied = buffer->writeBytes (done, readAhead + offset, wanted);
      writeTicks += mach_absolute_time () - phase;
      if (copied != wanted) {
        ok = false;
        break;
      }
      cursor += wanted;
      done += wanted;
      continue;
    }
    uint64_t windowEnd = absolute + bytes;
    uint64_t available = windowEnd > page ? windowEnd - page : 0;
    if (available > UFS_DATA_MAX) {
      available = UFS_DATA_MAX;
    }
    uint32_t blocks = static_cast<uint32_t> ((available + 4095) / 4096);
    if (!blocks) {
      blocks = 1;
    }
    if (blocks > UFS_DATA_MAX / 4096) {
      blocks = UFS_DATA_MAX / 4096;
    }
    uint64_t phase = mach_absolute_time ();
    bool readOk = host->readWindowBulk (page / 4096, blocks, readAhead);
    readTicks += mach_absolute_time () - phase;
    if (!readOk) {
      ok = false;
      readAheadBytes = 0;
      break;
    }
    readAheadLba = page;
    readAheadBytes = static_cast<uint64_t> (blocks) * 4096;
    unsigned wanted = static_cast<unsigned> (end - cursor);
    uint64_t cached = readAheadBytes - inPage;
    if (wanted > cached) {
      wanted = static_cast<unsigned> (cached);
    }
    phase = mach_absolute_time ();
    IOByteCount copied = buffer->writeBytes (done, readAhead + inPage, wanted);
    writeTicks += mach_absolute_time () - phase;
    if (copied != wanted) {
      ok = false;
      break;
    }
    cursor += wanted;
    done += wanted;
  }
  uint64_t unlockStart = mach_absolute_time ();
  IOLockUnlock (lock);
  uint64_t unlockTicks = mach_absolute_time () - unlockStart;
  gWindowInFlight = 0;
  gWindowStartTicks = 0;
  uint64_t requestTicks = mach_absolute_time () - requestStart;
  if (!gH103Quiet && (request < 8 || (request & 1023) == 0 || requestTicks > 19200000ULL)) {
    IOLog ("NATIVE_UFS_WINDOW: %s req=%llu done=%llu ok=%u ticks=%llu "
           "read=%llu write=%llu\n",
           label, request, done, ok ? 1 : 0,
           (unsigned long long) requestTicks,
           (unsigned long long) readTicks,
           (unsigned long long) writeTicks);
  }
  if (!gH103Quiet && request < 4) {
    IOLog ("NATIVE_UFS_PHASE: %s req=%llu lock=%llu read=%llu write=%llu unlock=%llu\n",
           label, request, (unsigned long long) lockTicks,
           (unsigned long long) readTicks, (unsigned long long) writeTicks,
           (unsigned long long) unlockTicks);
  }
  completion->action (completion->target, completion->parameter,
                      ok ? kIOReturnSuccess : kIOReturnIOError, done);
  if (ok && host) {
    host->h103WindowRead ();
  }
  return kIOReturnSuccess;
}

bool
NativeUfsLun::configure (NativeUfsProbe *parent, uint8_t unit, uint64_t lastLba,
                         uint32_t blockSize)
{
  host = parent;
  lun = unit;
  last = lastLba;
  sector = blockSize;
  setProperty ("B5Lun", unit, 32);
  setProperty ("B5ReadOnly", true);
  return true;
}

static unsigned gIoRequestLog;
static unsigned gIoRequests;
static unsigned gBlock0Requests;
static bool gMediaDumped;

IOReturn
NativeUfsLun::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block,
                                UInt64 count, IOStorageAttributes *,
                                IOStorageCompletion *completion)
{
  if (!buffer || buffer->getDirection () != kIODirectionIn) {
    return kIOReturnNotWritable;
  }
  if (!host || !blockRange (block, count, last, sector) ||
      count > ~uint64_t (0) / sector ||
      buffer->getLength () != count * sector) {
    return kIOReturnBadArgument;
  }
  gIoRequests++;
  if (block == 0) {
    gBlock0Requests++;
  }
  if (!gH103Quiet && gIoRequestLog < 16) {
    IOLog ("NATIVE_UFS_IO: req block=%llx count=%llu sector=%x len=%llx\n",
           block, count, sector, buffer->getLength ());
    gIoRequestLog++;
  }
  uint64_t started = mach_absolute_time ();
  uint64_t done = 0;
  bool ok = host->readBlocks (lun, block, count, sector, buffer, &done);
  uint64_t finished = mach_absolute_time ();
  IOReturn status = ok ? kIOReturnSuccess : kIOReturnIOError;
  if (!gH103Quiet && gIoRequestLog < 32) {
    IOLog ("NATIVE_UFS_IO: done block=%llx count=%llu ok=%u done=%llx status=%x\n",
           block, count, ok ? 1 : 0, done, status);
    gIoRequestLog++;
  }
  if (!gH103Quiet && gIoRequests <= 8) {
    IOLog ("NATIVE_UFS_IO: timing block=%llx ufs_ticks=%llu total_ticks=%llu\n",
           block, finished - started, mach_absolute_time () - started);
  }
  if (gIoRequests >= 4 && !gMediaDumped) {
    gMediaDumped = true;
    IOLog ("NATIVE_UFS_IO: one-shot IOMedia snapshot before completion at request %u\n",
           gIoRequests);
    dumpMediaServices ();
  }
  if (completion && completion->action) {
    if (!gH103Quiet && gIoRequests <= 8) {
      IOLog ("NATIVE_UFS_IO: completion enter block=%llx\n", block);
    }
    completion->action (completion->target, completion->parameter, status, done);
    if (!gH103Quiet && gIoRequests <= 8) {
      IOLog ("NATIVE_UFS_IO: completion leave block=%llx\n", block);
    }
  }
  if (!gH103Quiet && (gIoRequests & 31) == 0) {
    IOLog ("NATIVE_UFS_IO: requests=%u block0=%u last_block=%llx\n",
           gIoRequests, gBlock0Requests, block);
  }
  return kIOReturnSuccess;
}

bool
NativeUfsProbe::start (IOService *provider)
{
  if (!IOService::start (provider)) {
    return false;
  }
  // H358: remember the provider the keystore is attached to when it is published later.
  gNativeProvider = provider;

  OSData *compatible = provider ?
      OSDynamicCast (OSData, provider->getProperty ("compatible")) : nullptr;
  OSData *profile = provider ?
      OSDynamicCast (OSData, provider->getProperty ("p8,msm8998-direct-dma")) : nullptr;
  // H152/H153: the GOP display nub is also an IOPlatformDevice, so matching can
  // offer this driver a provider that is not the UFS node.  The provider
  // properties only exist after IOService::start(), so validate afterwards and
  // reject instead of panicking.
  if (!compatibleContains (compatible, "qcom,ufshc") || !profile ||
      profile->getLength () != sizeof (uint32_t) ||
      *static_cast<const uint32_t *> (profile->getBytesNoCopy ()) != 1) {
    IOLog ("NATIVE_UFS_GPT: not the UFS node provider=%s, skipping\n",
           provider ? provider->getName () : "-");
    return false;
  }
  IOLog ("NATIVE_UFS_GPT: warm-link probe start\n");
  if (cheeseburger_usb_probe_start != nullptr) {
    cheeseburger_usb_probe_start ();
  }

  IOMemoryDescriptor *gccDesc = IOMemoryDescriptor::withPhysicalAddress (
    MSM8998_GCC_BASE, MSM8998_GCC_SIZE, kIODirectionInOut);
  if (!gccDesc) {
    panic ("NATIVE_UFS_GPT: GCC map descriptor failed");
  }
  IOMemoryMap *gccMap = gccDesc->map ();
  gccDesc->release ();
  if (!gccMap) {
    panic ("NATIVE_UFS_GPT: GCC map failed");
  }
  uintptr_t gcc = static_cast<uintptr_t> (gccMap->getVirtualAddress ());
  const unsigned clockOffsets[] = {
    0x75004, // UFS GDSC
    0x7500c, // gcc_ufs_ahb_clk
    0x75008, // gcc_ufs_axi_clk
    0x76008, // gcc_ufs_unipro_core_clk
    0x82028, // gcc_aggre1_ufs_axi_clk
    0x7600c, // gcc_ufs_ice_core_clk
    0x76040, // gcc_ufs_phy_aux_clk
    0x75010, // gcc_ufs_tx_symbol_0_clk
    0x75014, // gcc_ufs_rx_symbol_0_clk
    0x7605c, // gcc_ufs_rx_symbol_1_clk
  };
  const unsigned clockCount = sizeof (clockOffsets) / sizeof (clockOffsets[0]);
  for (unsigned index = 0; index < clockCount; index++) {
    uint32_t value = read32 (gcc, clockOffsets[index]);
    IOLog ("NATIVE_UFS_GPT: gcc %x=%08x\n", clockOffsets[index], value);
  }
  // The three branches checked by P8/P11 are not the whole UFS AXI path on
  // MSM8998: gcc_aggre1_ufs_axi_clk (0x82028) gates the UFS bus master's
  // access to DDR.  If the hand-off left that branch disabled, the CPU can
  // still read the host registers while every DMA command hangs forever.
  // Enable only the branch bit (BIT0), preserve all other fields, and require
  // the branch to report enabled and not halted (BIT31 clear).
  const unsigned criticalClocks[] = { 0x7500c, 0x75008, 0x76008, 0x82028 };
  for (unsigned index = 0; index < sizeof (criticalClocks) / sizeof (criticalClocks[0]); index++) {
    unsigned offset = criticalClocks[index];
    uint32_t value = read32 (gcc, offset);
    if ((value & 0x80000001U) == 0x00000001U) {
      continue;
    }
    IOLog ("NATIVE_UFS_GPT: enabling gcc %x old=%08x\n", offset, value);
    write32 (gcc, offset, value | 0x00000001U);
    if (!waitRegister (gcc, offset, 0x80000001U, 0x00000001U, 20000)) {
      panic ("NATIVE_UFS_GPT: UFS clock not running at %x=%08x",
             offset, read32 (gcc, offset));
    }
    IOLog ("NATIVE_UFS_GPT: gcc %x now=%08x\n", offset, read32 (gcc, offset));
  }

  IOMemoryDescriptor *hcDesc = IOMemoryDescriptor::withPhysicalAddress (
    MSM8998_UFS_HC_BASE, MSM8998_UFS_HC_SIZE, kIODirectionInOut);
  if (!hcDesc) {
    panic ("NATIVE_UFS_GPT: HC descriptor failed");
  }
  IOMemoryMap *hcMap = hcDesc->map ();
  hcDesc->release ();
  if (!hcMap) {
    panic ("NATIVE_UFS_GPT: HC map failed");
  }
  uintptr_t hc = static_cast<uintptr_t> (hcMap->getVirtualAddress ());
  uint32_t cap = read32 (hc, 0x00);
  uint32_t version = read32 (hc, 0x08);
  uint32_t is = read32 (hc, UFS_REG_IS);
  uint32_t hcs = read32 (hc, UFS_REG_HCS);
  uint32_t hce = read32 (hc, UFS_REG_HCE);
  uint32_t doorbell = read32 (hc, UFS_REG_UTRLDBR);
  uint32_t taskDoorbell = read32 (hc, UFS_REG_TASK_DOORBELL);
  IOLog ("NATIVE_UFS_GPT: HC cap=%08x ver=%08x is=%08x hcs=%08x hce=%08x\n",
         cap, version, is, hcs, hce);
  if (!(hce & 1) || ((hcs & 3) != 3) || (is & UFS_FATAL_STATUS) ||
      doorbell || taskDoorbell) {
    panic ("NATIVE_UFS_GPT: controller not idle hce=%08x hcs=%08x is=%08x db=%08x tdb=%08x",
           hce, hcs, is, doorbell, taskDoorbell);
  }

  uint32_t ahit = read32 (hc, UFS_REG_AHIT);
  IOLog ("NATIVE_UFS_GPT: uic probe ahit=%08x is=%08x hcs=%08x\n",
         ahit, read32 (hc, UFS_REG_IS), read32 (hc, UFS_REG_HCS));
  uint32_t powerMode = 0;
  bool gotPowerMode = uicCommand (hc, UFS_UIC_DME_GET, 0x15710000U, 0, &powerMode);
  IOLog ("NATIVE_UFS_GPT: uic ahit=%08x get=%u mode=%08x is=%08x hcs=%08x\n",
         ahit, gotPowerMode, powerMode, read32 (hc, UFS_REG_IS),
         read32 (hc, UFS_REG_HCS));
  if (!gotPowerMode) {
    uint32_t wakeResult = 0;
    bool woke = uicCommand (hc, UFS_UIC_DME_HIBERNATE_EXIT, 0, 0, &wakeResult);
    IOLog ("NATIVE_UFS_GPT: uic hibernate-exit=%u result=%08x is=%08x hcs=%08x\n",
           woke, wakeResult, read32 (hc, UFS_REG_IS), read32 (hc, UFS_REG_HCS));
    powerMode = 0;
    gotPowerMode = uicCommand (hc, UFS_UIC_DME_GET, 0x15710000U, 0, &powerMode);
    IOLog ("NATIVE_UFS_GPT: uic after-wake get=%u mode=%08x is=%08x hcs=%08x\n",
           gotPowerMode, powerMode, read32 (hc, UFS_REG_IS),
           read32 (hc, UFS_REG_HCS));
  }

  OSSharedPtr<IOMemoryMap> smmu1Map, smmu2Map;
  uintptr_t smmu1 = mapPhys (0x1680000ULL, 0x10000ULL, smmu1Map);
  if (smmu1) {
    dumpSmmu (smmu1, "anoc1");
  } else {
    IOLog ("NATIVE_UFS_GPT: smmu anoc1 map failed\n");
  }
  uintptr_t smmu2 = mapPhys (0x16c0000ULL, 0x40000ULL, smmu2Map);
  if (smmu2) {
    dumpSmmu (smmu2, "anoc2");
  } else {
    IOLog ("NATIVE_UFS_GPT: smmu anoc2 map failed\n");
  }

  // The firmware's own UTRL/UCD page is the only low physical DMA region that
  // the controller is known to have used.  Reuse that page (not the firmware
  // high data buffer): list at +0, UCD at +0x100, response at +0x200, PRDT at
  // +0x400 and a small low scratch at +0x800.  The page must be cache-cleaned
  // because MSM8998 UFS is not coherent (ACPI _CCA=0); writing it through a
  // cacheable alias without cleaning leaves the controller reading the old
  // firmware descriptors.
  uint64_t listPhys = (uint64_t (read32 (hc, UFS_REG_UTRLBAU)) << 32) |
                      read32 (hc, UFS_REG_UTRLBA);
  if (!listPhys || (listPhys & 0x3ff)) {
    panic ("NATIVE_UFS_GPT: no usable firmware list base=%llx", listPhys);
  }
  OSSharedPtr<IOMemoryMap> listMap, ucdMap;
  uintptr_t listVirt = mapPhys (listPhys, 4096, listMap);
  if (!listVirt) {
    panic ("NATIVE_UFS_GPT: firmware UTRL map failed base=%llx", listPhys);
  }
  uint64_t ucdPhys = listPhys + UFS_UCD_OFFSET;
  uintptr_t ucdVirt = mapPhys (ucdPhys, UFS_FW_UCD_SIZE, ucdMap);
  if (!ucdVirt) {
    panic ("NATIVE_UFS_GPT: firmware UCD map failed base=%llx", ucdPhys);
  }
  uint8_t *list = reinterpret_cast<uint8_t *> (listVirt);
  uint8_t *ucd = reinterpret_cast<uint8_t *> (ucdVirt);
  uint8_t *data = ucd + UFS_FW_DATA_OFFSET;
  uint64_t dataPhys = ucdPhys + UFS_FW_DATA_OFFSET;
  uint32_t dataCapacity = UFS_FW_DATA_MAX;
  uint32_t fw0 = read32 (listVirt, 0);
  uint32_t fw2 = read32 (listVirt, 8);
  cacheInvalidate (list, 4096);
  cacheInvalidate (ucd, UFS_FW_UCD_SIZE);
  zero32 (list, 4096);
  zero32 (ucd, UFS_FW_UCD_SIZE);
  cacheClean (list, 4096);
  cacheClean (ucd, UFS_FW_UCD_SIZE);
  IOLog ("NATIVE_UFS_GPT: firmware page zero list0=%08x list2=%08x ucd0=%08x\n",
         read32 (listVirt, 0), read32 (listVirt, 8), read32 (ucdVirt, 0));
  IOLog ("NATIVE_UFS_GPT: firmware page list=%llx ucd=%llx data=%llx cap=%x fw=%08x/%08x\n",
         listPhys, ucdPhys, dataPhys, dataCapacity, fw0, fw2);

  // Install the private queue, preserving the firmware values only for a
  // diagnostic rollback-free log.  The controller is not reset.
  uint32_t oldRun = read32 (hc, UFS_REG_UTRLRSR);
  uint32_t oldLo = read32 (hc, UFS_REG_UTRLBA);
  uint32_t oldHi = read32 (hc, UFS_REG_UTRLBAU);
  uint32_t oldIE = read32 (hc, UFS_REG_IE);
  IOLog ("NATIVE_UFS_GPT: old queue run=%x ba=%x/%x ie=%x\n", oldRun, oldLo, oldHi, oldIE);
  write32 (hc, UFS_REG_UTRLRSR, 0);
  if (!waitRegister (hc, UFS_REG_UTRLRSR, 1, 0, 10000) || read32 (hc, UFS_REG_UTRLDBR)) {
    panic ("NATIVE_UFS_GPT: queue did not stop rsr=%x db=%x",
           read32 (hc, UFS_REG_UTRLRSR), read32 (hc, UFS_REG_UTRLDBR));
  }
  write32 (hc, UFS_REG_IE, 0);
  write32 (hc, UFS_REG_UTRLBA, static_cast<uint32_t> (listPhys));
  write32 (hc, UFS_REG_UTRLBAU, static_cast<uint32_t> (listPhys >> 32));
  write32 (hc, UFS_REG_IS, 0xffffffffU);
  write32 (hc, UFS_REG_UTRLRSR, 1);
  if (!waitRegister (hc, UFS_REG_UTRLRSR, 1, 1, 10000) ||
      !waitRegister (hc, UFS_REG_HCS, 2, 2, 10000)) {
    panic ("NATIVE_UFS_GPT: queue did not start rsr=%x hcs=%x",
           read32 (hc, UFS_REG_UTRLRSR), read32 (hc, UFS_REG_HCS));
  }
  if (read32 (hc, UFS_REG_UTRLBA) != static_cast<uint32_t> (listPhys) ||
      read32 (hc, UFS_REG_UTRLBAU) != static_cast<uint32_t> (listPhys >> 32)) {
    panic ("NATIVE_UFS_GPT: queue base readback ba=%x/%x want=%llx",
           read32 (hc, UFS_REG_UTRLBA), read32 (hc, UFS_REG_UTRLBAU),
           listPhys);
  }
  IOLog ("NATIVE_UFS_GPT: private queue installed ba=%llx\n", listPhys);

  UfsQueue queue = { hc, list, ucd, data, ucdPhys, dataPhys, dataCapacity, false };

  IOLog ("NATIVE_UFS_NOP: settle spin before first command\n");
  spinTicks (192000);
  IOLog ("NATIVE_UFS_NOP: settle complete\n");
  if (!ufsNop (&queue)) {
    panic ("NATIVE_UFS_GPT: firmware-page NOP failed; see OCS/type log");
  }
  IOLog ("NATIVE_UFS_GPT: firmware-page NOP completed\n");

  const unsigned maxLun = 6;
  uint64_t lastLba[maxLun];
  uint32_t blockSize[maxLun];
  uint32_t lunStatus[maxLun];
  for (unsigned lun = 0; lun < maxLun; lun++) {
    lastLba[lun] = 0;
    blockSize[lun] = 0;
    lunStatus[lun] = 0xee;
    uint8_t cap16[16] = { 0x9e, 0x10 };
    putBe32 (cap16 + 10, 32);
    if (ufsCommand (&queue, static_cast<uint8_t> (lun), cap16, 32, data)) {
      lastLba[lun] = getBe64 (data);
      blockSize[lun] = getBe32 (data + 8);
      lunStatus[lun] = 0;
      continue;
    }
    uint8_t cap10[16] = { 0x25 };
    if (ufsCommand (&queue, static_cast<uint8_t> (lun), cap10, 8, data)) {
      lastLba[lun] = getBe32 (data);
      blockSize[lun] = getBe32 (data + 4);
      lunStatus[lun] = 0;
    }
  }
  for (unsigned lun = 0; lun < maxLun; lun++) {
    IOLog ("NATIVE_UFS_GPT: L%u st=%02x last=%llx bs=%x\n", lun,
           lunStatus[lun], lastLba[lun], blockSize[lun]);
  }
  if (lunStatus[0] || blockSize[0] != 4096) {
    panic ("NATIVE_UFS_GPT: LUN0 capacity failed status=%x last=%llx bs=%x",
           lunStatus[0], lastLba[0], blockSize[0]);
  }
  IOOptionBits gptOptions = static_cast<IOOptionBits> (kIODirectionInOut) |
                             static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
                             static_cast<IOOptionBits> (kIOMemoryMapperNone);
  IOBufferMemoryDescriptor *gptDma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask (
    kernel_task, gptOptions, blockSize[0], 0x00000000ffffffffULL);
  if (!gptDma) {
    IOLog ("NATIVE_UFS_GPT: GPT low DMA failed; high fallback\n");
    gptDma = IOBufferMemoryDescriptor::inTaskWithOptions (
      kernel_task, gptOptions, blockSize[0], 4096);
  }
  if (!gptDma) {
    panic ("NATIVE_UFS_GPT: GPT DMA allocation failed");
  }
  uint8_t *gptData = static_cast<uint8_t *> (gptDma->getBytesNoCopy ());
  IOByteCount gptContiguous = 0;
  uint64_t gptPhys = gptDma->getPhysicalSegment (0, &gptContiguous, kIOMemoryMapperNone);
  if (!gptData || !gptPhys || (gptPhys & 0xfff) || gptContiguous < blockSize[0]) {
    panic ("NATIVE_UFS_GPT: GPT DMA invalid phys=%llx cont=%llx", gptPhys,
           uint64_t (gptContiguous));
  }
  data = gptData;
  dataPhys = gptPhys;
  dataCapacity = blockSize[0];
  queue.data = data;
  queue.dataPhys = dataPhys;
  queue.dataCapacity = dataCapacity;
  IOLog ("NATIVE_UFS_GPT: GPT data phys=%llx size=%x\n", gptPhys, dataCapacity);

  uint8_t read10[16] = { 0x28 };
  putBe32 (read10 + 2, 1);
  read10[7] = 0;
  read10[8] = 1;
  if (!ufsCommand (&queue, 0, read10, blockSize[0], data)) {
    panic ("NATIVE_UFS_GPT: LBA1 header READ10 failed");
  }
  const uint8_t *header = data;
  uint32_t signature0 = getLe32 (header);
  uint32_t signature1 = getLe32 (header + 4);
  uint32_t headerCrc = getLe32 (header + 16);
  uint32_t entriesCrc = getLe32 (header + 88);
  uint64_t currentLba = getLe64 (header + 24);
  uint64_t backupLba = getLe64 (header + 32);
  uint64_t firstUsable = getLe64 (header + 40);
  uint64_t lastUsable = getLe64 (header + 48);
  uint64_t entriesLba = getLe64 (header + 72);
  uint32_t entryCount = getLe32 (header + 80);
  uint32_t entrySize = getLe32 (header + 84);
  if (signature0 != 0x20494645U || signature1 != 0x54524150U ||
      entrySize < 128 || entrySize > 512 || !entryCount || entryCount > 128 ||
      entriesLba > 0xffffffffULL) {
    panic ("NATIVE_UFS_GPT: bad GPT sig=%08x/%08x n=%u esz=%u elba=%llx",
           signature0, signature1, entryCount, entrySize, entriesLba);
  }
  uint32_t entryBytes = entryCount * entrySize;
  uint32_t entryBlocks = (entryBytes + blockSize[0] - 1) / blockSize[0];
  if (entryBlocks * blockSize[0] > dataCapacity) {
    panic ("NATIVE_UFS_GPT: GPT entries too large n=%u esz=%u", entryCount, entrySize);
  }
  memset (read10, 0, sizeof (read10));
  read10[0] = 0x28;
  putBe32 (read10 + 2, static_cast<uint32_t> (entriesLba));
  read10[7] = static_cast<uint8_t> (entryBlocks >> 8);
  read10[8] = static_cast<uint8_t> (entryBlocks);
  if (!ufsCommand (&queue, 0, read10, entryBlocks * blockSize[0], data)) {
    panic ("NATIVE_UFS_GPT: GPT entries READ10 failed");
  }

  char text[4096];
  int used = 0;
#define ADD(...) do { \
  if (used < int (sizeof (text))) { \
    int _n = snprintf (text + used, size_t (sizeof (text) - size_t (used)), __VA_ARGS__); \
    if (_n > 0) used += _n; \
    if (used >= int (sizeof (text))) used = int (sizeof (text)) - 1; \
  } \
} while (0)
  ADD ("NATIVE_UFS_GPT warm-link read-only\n");
  ADD ("cap=%08x ver=%08x is=%08x hcs=%08x hce=%08x\n",
       cap, version, is, hcs, hce);
  ADD ("queue=%llx ucd=%llx data=%llx cap=%x\n", listPhys, ucdPhys, dataPhys, dataCapacity);
  for (unsigned lun = 0; lun < maxLun; lun++) {
    ADD ("L%u st=%02x last=%llx bs=%x\n", lun, lunStatus[lun],
         lastLba[lun], blockSize[lun]);
  }
  ADD ("GPT hdr=%08x ent=%08x n=%u esz=%u\n",
       headerCrc, entriesCrc, entryCount, entrySize);
  ADD ("cur=%llx bak=%llx usable=%llx..%llx\n",
       currentLba, backupLba, firstUsable, lastUsable);
  const char *const keys[] = {
    "userdata_ios", "persist", "cache", "misc", "metadata", "system", "vendor",
    "boot", "recovery", "modem", "modemst1", "modemst2", "fsg", "fsc",
    "keymaster", "abl", "xbl", "tz", "devcfg", "devinfo"
  };
  uint64_t rootFirst = 0, rootLast = 0;
  unsigned listed = 0;
  for (uint32_t index = 0; index < entryCount; index++) {
    const uint8_t *entry = data + index * entrySize;
    uint64_t first = getLe64 (entry + 32);
    uint64_t last = getLe64 (entry + 40);
    if (!first) {
      continue;
    }
    char name[40];
    unsigned length = 0;
    for (unsigned c = 0; c < 36 && c < entrySize - 56; c += 2) {
      uint8_t lo = entry[56 + c];
      uint8_t hi = entry[57 + c];
      if (!lo && !hi) {
        break;
      }
      name[length++] = (hi == 0 && lo >= 32 && lo < 127) ? char (lo) : '?';
    }
    name[length] = 0;
    if (!__builtin_strcmp (name, "userdata_ios")) {
      rootFirst = first;
      rootLast = last;
    }
    bool key = false;
    for (unsigned k = 0; k < sizeof (keys) / sizeof (keys[0]); k++) {
      if (!__builtin_strcmp (name, keys[k])) {
        key = true;
        break;
      }
    }
    if (!key) {
      continue;
    }
    uint64_t mib = ((last - first + 1) * blockSize[0]) >> 20;
    ADD ("%s %llx-%llx %lluM\n", name, first, last, mib);
    listed++;
  }
  ADD ("partitions=%u listed=%u\n", entryCount, listed);
  IOLog ("%s", text);
  IOLog ("NATIVE_UFS_ROOT: userdata_ios LBA %llx..%llx\n", rootFirst, rootLast);

  IOOptionBits readOptions = static_cast<IOOptionBits> (kIODirectionInOut) |
                             static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
                             static_cast<IOOptionBits> (kIOMemoryMapperNone);
  uint32_t readCapacity = 0;
  uint64_t readPhys = 0;
  IOByteCount readContiguous = 0;
  IOBufferMemoryDescriptor *readDma = allocLowContiguous (
    readOptions, UFS_DATA_MAX, &readCapacity, &readPhys, &readContiguous);
  if (!readDma) {
    panic ("NATIVE_UFS_GPT: no low read DMA buffer (wanted %x)", UFS_DATA_MAX);
  }
  uint8_t *readData = static_cast<uint8_t *> (readDma->getBytesNoCopy ());
  if (!readData) {
    panic ("NATIVE_UFS_GPT: read DMA mapping missing phys=%llx",
           (unsigned long long) readPhys);
  }
  IOLog ("NATIVE_UFS_GPT: read DMA cap=%x phys=%llx cont=%llx\n",
         readCapacity, (unsigned long long) readPhys,
         (unsigned long long) readContiguous);
  queue.data = readData;
  queue.dataPhys = readPhys;
  queue.dataCapacity = readCapacity;
  queue.poisoned = false;
  fQueue = queue;

  IOOptionBits logOptions = static_cast<IOOptionBits> (kIODirectionIn) |
                            static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
                            static_cast<IOOptionBits> (kIOMemoryMapperNone);
  uint32_t logCapacity = 0;
  uint64_t logPhys = 0;
  IOByteCount logContiguous = 0;
  IOBufferMemoryDescriptor *logDma = allocLowContiguous (
    logOptions, UFS_DATA_MAX, &logCapacity, &logPhys, &logContiguous);
  if (logDma) {
    fLogDma = logDma;
    fLogData = static_cast<uint8_t *> (logDma->getBytesNoCopy ());
    fLogDataPhys = logPhys;
    fLogCapacity = logCapacity;
    IOLog ("NATIVE_UFS_LOG: log DMA cap=%x phys=%llx cont=%llx\n",
           logCapacity, (unsigned long long) logPhys,
           (unsigned long long) logContiguous);
    if (!fLogData) {
      IOLog ("NATIVE_UFS_LOG: log DMA mapping missing phys=%llx\n",
             (unsigned long long) logPhys);
      fLogData = nullptr;
      fLogDataPhys = 0;
      fLogCapacity = 0;
    }
  } else {
    IOLog ("NATIVE_UFS_LOG: log DMA allocation failed\n");
  }
  fListMap = listMap;
  fUcdMap = ucdMap;
  fLock = IOLockAlloc ();
  if (!fLock) {
    panic ("NATIVE_UFS_GPT: read lock allocation failed");
  }
  fReady = true;
  char rootName[4][33] = {};
  uint64_t rootOffset[4] = {};
  uint64_t rootSize[4] = {};
  uint64_t rootImageBytes = 0;
  unsigned rootWindows = 0;
  {
    uint8_t header[4096] = {};
    uint8_t readUserdata[16] = { 0x28 };
    putBe32 (readUserdata + 2, static_cast<uint32_t> (rootFirst));
    readUserdata[8] = 1;
    if (ufsCommand (&queue, 0, readUserdata, sizeof (header), header)) {
      uint32_t magic = getLe32 (header);
      uint32_t magic2 = getLe32 (header + 4);
      uint32_t version = getLe32 (header + 8);
      uint32_t windows = getLe32 (header + 12);
      uint64_t imageBytes = getLe64 (header + 16);
      IOLog ("NATIVE_UFS_ROOT: userdata_ios magic=%08x/%08x ver=%u windows=%u image=%llx\n",
             magic, magic2, version, windows, imageBytes);
      if (magic == 0x474e4853U && magic2 == 0x544f4f52U && windows <= 4) {
        rootImageBytes = imageBytes;
        for (uint32_t index = 0; index < windows; index++) {
          const uint8_t *entry = header + 24 + index * 128;
          char name[33];
          memcpy (name, entry, 32);
          name[32] = 0;
          uint64_t offset = getLe64 (entry + 32);
          uint64_t size = getLe64 (entry + 40);
          strlcpy (rootName[index], name, sizeof (rootName[index]));
          rootOffset[index] = offset;
          rootSize[index] = size;
          rootWindows++;
          IOLog ("NATIVE_UFS_ROOT: window[%u]=%s off=%llx size=%llx\n",
                 index, name, offset, size);
        }
      } else {
        IOLog ("NATIVE_UFS_ROOT: SHNGROOT header not recognized\n");
      }
    } else {
      IOLog ("NATIVE_UFS_ROOT: userdata first-block READ10 failed\n");
    }
  }
  IOLog ("NATIVE_UFS_GPT: read DMA phys=%llx size=%x; publishing LUN0\n",
         readPhys, UFS_DATA_MAX);

  bool published = false;
  NativeUfsLun *child = new NativeUfsLun;
  if (child && child->init (nullptr) &&
      child->configure (this, 0, lastLba[0], blockSize[0]) &&
      child->attach (this) && child->start (this)) {
    IOLog ("NATIVE_UFS_PUB: before LUN0 register\n");
    child->registerService ();
    IOLog ("NATIVE_UFS_PUB: after LUN0 register\n");
    published = true;
    IOLog ("NATIVE_UFS_GPT: published LUN0 last=%llx bs=%x read-only\n",
           lastLba[0], blockSize[0]);
  } else {
    IOLog ("NATIVE_UFS_GPT: LUN0 publication failed\n");
  }
  if (child) {
    child->release ();
  }

  unsigned windowsPublished = 0;
  for (unsigned index = 0; index < rootWindows; index++) {
    uint64_t absolute = rootFirst * 4096ULL + rootOffset[index];
    IOLog ("NATIVE_UFS_ROOT: preparing window[%u]=%s abs=%llx bytes=%llx\n",
           index, rootName[index], absolute, rootSize[index]);
    NativeUfsWindow *window = new NativeUfsWindow;
    bool started = window && window->init (nullptr) &&
                   window->configure (this, rootName[index], absolute,
                                      rootSize[index]) &&
                   window->attach (this) && window->start (this);
    if (started) {
      IOLog ("NATIVE_UFS_PUB: before window[%u]=%s register\n",
             index, rootName[index]);
      window->registerService ();
      IOLog ("NATIVE_UFS_PUB: after window[%u]=%s register\n",
             index, rootName[index]);
      windowsPublished++;
      IOLog ("NATIVE_UFS_ROOT: published window[%u]=%s abs=%llx bytes=%llx\n",
             index, rootName[index], absolute, rootSize[index]);
      // Keep the reference: the window owns its bounce buffer and lock.
    } else {
      IOLog ("NATIVE_UFS_ROOT: window[%u]=%s publication failed\n",
             index, rootName[index]);
      if (window) {
        window->release ();
      }
    }
  }
  IOLog ("NATIVE_UFS_ROOT_MEDIA: post-publication snapshot\n");
  dumpRootMedia ();
  IOLog ("NATIVE_UFS_GPT: start returning published=%u windows=%u\n",
         published ? 1 : 0, windowsPublished);
  gH103Quiet = 1;
  gH103Probe = this;
  uint32_t skipPattern = 0;
  PE_parse_boot_argn ("h103-skip-pattern", &skipPattern, sizeof skipPattern);
  gH103SkipPattern = skipPattern ? 1 : 0;
  uint32_t slotMode = 1;
  PE_parse_boot_argn ("h103-slot", &slotMode, sizeof slotMode);
  gH103SlotMode = slotMode <= 2 ? static_cast<int> (slotMode) : 1;
  // H106: ios-sweet 0069's flush interval (seconds), 1..60.
  uint32_t logSecs = 10;
  PE_parse_boot_argn ("h103-log-secs", &logSecs, sizeof logSecs);
  gH103LogSecs = (logSecs >= 1 && logSecs <= 60) ? logSecs : 10;
  // H104f: do not touch userdata_ios until backboardd has actually exec'd.
  // The early slot writes contend with driverkitd/backboardd's UFS reads and
  // make the launchd ordering race lose; slot0 boots are stable. The message
  // buffer keeps accumulating, so the first post-backboardd write still dumps
  // the early boot log. H106 keeps that gate and adds the ios-sweet 0069
  // cadence: one delta write per h103-log-secs, and none while quiet.
  uint32_t hbTimer = 0;
  PE_parse_boot_argn ("h103-hb-timer", &hbTimer, sizeof hbTimer);
  gH103HbTimerOn = hbTimer <= 2 ? static_cast<int> (hbTimer) : 1;
  uint32_t loadIogpu = 0;
  PE_parse_boot_argn ("h103-load-iogpu", &loadIogpu, sizeof loadIogpu);
  gH103LoadIogpu = loadIogpu ? 1 : 0;
  gH103StartTicks = mach_absolute_time ();
  // H336: publishing AppleKeyStore from start() made launchd/securityd talk to it immediately and
  // the boot stopped right after the first log flush.  Remember the provider and publish from the
  // watchdog thread half a minute in, once launchd is up.
  // H178: a warm reboot leaves the kernel message ring in place, and its write
  // cursor does not necessarily start at zero.  With previous=0 the first
  // flush therefore copied the whole ring up to the current cursor, i.e. the
  // previous boot's console text (that is how H170-era "read DMA low failed"
  // lines kept appearing inside H177 slots).  Start the delta at the cursor
  // that exists when this probe runs, so every slot only holds bytes produced
  // after it.
  if (msgbufp && msgbufp->msg_bufx >= 0 && msgbufp->msg_bufx < msgbufp->msg_size) {
    gH103LastSlotMsgX = msgbufp->msg_bufx;
    IOLog ("NATIVE_UFS_LOG: base cursor=%d size=%d\n", msgbufp->msg_bufx,
           msgbufp->msg_size);
  }
  // H163: the first start wins.  A mountroot retry starts the probe twice in
  // one boot (seen at 59 s and 64 s); the stamp must not change with it.
  if (!gH103BootId) {
    gH103BootId = h103LogTicks ();
  }
  IOLog ("NATIVE_BUILD: %s boot=%llx\n", H103_BUILD,
         (unsigned long long) gH103BootId);
  IOLog ("NATIVE_UFS_HB: slotMode=%d logSecs=%u hbTimer=%d loadIogpu=%d\n",
         gH103SlotMode, gH103LogSecs, gH103HbTimerOn, gH103LoadIogpu);
  thread_t heartbeat = nullptr;
  kern_return_t heartbeatRet = kernel_thread_start (heartbeatThread, nullptr, &heartbeat);
  IOLog ("NATIVE_UFS_HB: start rc=%x skipPattern=%u\n",
         heartbeatRet, gH103SkipPattern);
  if (heartbeatRet == KERN_SUCCESS) {
    thread_deallocate (heartbeat);
  }
  {
    thread_t sbSampler = THREAD_NULL;
    kern_return_t sbRet = kernel_thread_start (a540SpringBoardSamplerThread, nullptr, &sbSampler);
    IOLog ("NATIVE_SBPC: start rc=%x\n", sbRet);
    if (sbRet == KERN_SUCCESS) {
      thread_deallocate (sbSampler);
    }
  }
  {
    // H312: outlives the sampler, so a late stall or a late panic always has a writer.
    thread_t watchdog = THREAD_NULL;
    kern_return_t wdRet = kernel_thread_start (nativeWatchdogThread, nullptr, &watchdog);
    IOLog ("NATIVE_UFS_WD: start rc=%x\n", wdRet);
    if (wdRet == KERN_SUCCESS) {
      thread_deallocate (watchdog);
    }
  }
  if (gH103SlotMode) {
    // H128: the 2-second slot thread is now opt-in.  It ignored
    // h103-log-secs (it wrote a 16 KiB slot every 2 s, ~170 writes per boot)
    // and its delta cursor never advanced, so it duplicated the heartbeat's
    // flush and added a write burst every 2 s to the very path that was
    // starving the file system.  The heartbeat drives h103SlotTick() now.
    uint32_t slotThread = 0;
    PE_parse_boot_argn ("h103-slot-thread", &slotThread, sizeof slotThread);
    if (slotThread) {
      thread_t slotThreadHandle = nullptr;
      IOLog ("NATIVE_UFS_HB: slot start\n");
      kern_return_t slotRet =
          kernel_thread_start (h103SlotThread, nullptr, &slotThreadHandle);
      IOLog ("NATIVE_UFS_HB: slot rc=%x\n", slotRet);
      if (slotRet == KERN_SUCCESS) {
        thread_deallocate (slotThreadHandle);
      }
    } else {
      IOLog ("NATIVE_UFS_HB: slot thread off (heartbeat owns the flush)\n");
    }
  } else {
    IOLog ("NATIVE_UFS_HB: slot disabled\n");
  }
  // H109: the 1 Hz heartbeat and the dump wakeup come from a timer_call (timer
  // interrupt), because thread timers never fire on this port. The worker
  // thread only sleeps on an explicit wakeup.
  thread_t dumpThread = nullptr;
  kern_return_t dumpRet = kernel_thread_start (h109WorkerThread, nullptr, &dumpThread);
  IOLog ("NATIVE_TICK: worker rc=%x\n", dumpRet);
  if (dumpRet == KERN_SUCCESS) {
    thread_deallocate (dumpThread);
  }
  uint32_t tickEnabled = 1;
  PE_parse_boot_argn ("h109-tick", &tickEnabled, sizeof tickEnabled);
  gH110TickEnabled = tickEnabled ? 1 : 0;
  uint32_t gicProbe = 1;
  PE_parse_boot_argn ("h114-gic", &gicProbe, sizeof gicProbe);
  gH114GicProbe = gicProbe ? 1 : 0;
  if (!gH110TickEnabled) {
    IOLog ("NATIVE_TICK: timer disabled\n");
  } else {
    gH109TimerCall = timer_call_alloc (h109TimerFunc, nullptr);
    if (gH109TimerCall) {
      timer_call_enter (gH109TimerCall, mach_absolute_time () + 19200000ULL, 0);
      IOLog ("NATIVE_TICK: timer armed\n");
    } else {
      IOLog ("NATIVE_TICK: timer alloc failed\n");
    }
  }
  if (gH103HbTimerOn == 1) {
    fHbLoop = IOWorkLoop::workLoop ();
    if (fHbLoop) {
      fHbTimer = IOTimerEventSource::timerEventSource (this,
                                                       &NativeUfsProbe::hbTimerFired);
      if (fHbTimer && fHbLoop->addEventSource (fHbTimer) == kIOReturnSuccess) {
        fHbNext = mach_absolute_time () + 19200000ULL;
        fHbTimer->wakeAtTime (fHbNext);
        IOLog ("NATIVE_UFS_HB_TIMER: start loop=%p timer=%p\n", fHbLoop, fHbTimer);
      } else {
        IOLog ("NATIVE_UFS_HB_TIMER: add failed loop=%p timer=%p\n",
               fHbLoop, fHbTimer);
        if (fHbTimer) {
          fHbTimer->release ();
          fHbTimer = nullptr;
        }
      }
    } else {
      IOLog ("NATIVE_UFS_HB_TIMER: no workloop\n");
    }
  } else if (gH103HbTimerOn == 2) {
    gH103TimerCall = timer_call_alloc (h103TimerCallFunc, nullptr);
    if (gH103TimerCall) {
      timer_call_enter (gH103TimerCall, mach_absolute_time () + 19200000ULL, 0);
      IOLog ("NATIVE_UFS_HB_TIMER: timer_call start %p\n", gH103TimerCall);
    } else {
      IOLog ("NATIVE_UFS_HB_TIMER: timer_call alloc failed\n");
    }
  }

  // Persistent log window: the last 32 MiB of userdata_ios. Write one
  // snapshot synchronously from start (the root path is already proven) and
  // another from the first 64 MiB of window reads. Neither depends on the
  // heartbeat thread or on a separately created log thread.
  fLogBlocks = H103_LOG_BLOCKS;
  fLogFirst = rootLast >= H103_LOG_BLOCKS ? rootLast + 1 - H103_LOG_BLOCKS : 0;
  fLogImageBytes = rootImageBytes;
  uint32_t logMode = 0;
  PE_parse_boot_argn ("sheng-log", &logMode, sizeof logMode);
  bool logSafe = logMode == 1 && fLogFirst && rootFirst &&
                 fLogFirst >= rootFirst &&
                 fLogFirst + fLogBlocks <= rootLast + 1;
  IOLog ("NATIVE_UFS_LOG: setup mode=%u first=%llx blocks=%u root=%llx..%llx "
         "image=%llx safe=%u\n",
         logMode, (unsigned long long) fLogFirst, fLogBlocks,
         (unsigned long long) rootFirst, (unsigned long long) rootLast,
         (unsigned long long) rootImageBytes, logSafe ? 1 : 0);
  gH103LogMode = logMode;
  gH103LogFirst = fLogFirst;
  gH103RootFirst = rootFirst;
  gH103RootLast = rootLast;
  gH391RootImageBytes = rootImageBytes;
  if (logSafe) {
    IOLog ("NATIVE_UFS_LOG: window lba=%llx blocks=%u image=%llx\n",
           (unsigned long long) fLogFirst, fLogBlocks,
           (unsigned long long) fLogImageBytes);
    // Do not write from start(): the synchronous 1 MiB snapshot raced the
    // early LUN/root reads and faulted in readBlocks. The first window read
    // snapshot runs after root, on the storage request path.
    IOLog ("NATIVE_UFS_LOG: deferred snapshot to window read\n");
    gH103LogFirst = fLogFirst;
  } else {
    IOLog ("NATIVE_UFS_LOG: disabled mode=%u first=%llx root=%llx..%llx image=%llx\n",
           logMode, (unsigned long long) fLogFirst,
           (unsigned long long) rootFirst, (unsigned long long) rootLast,
           (unsigned long long) rootImageBytes);
    fLogFirst = 0;
    fLogBlocks = 0;
    gH103LogFirst = 0;
  }
  return published;
}
