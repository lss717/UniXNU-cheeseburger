// SPDX-License-Identifier: BSD-2-Clause-Patent
// OnePlus 5 USB handoff probe. This is deliberately read-only: it never
// changes GCC/GDSC state, resets DWC3, allocates DMA, or enables endpoints.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

namespace {

constexpr uint64_t kGccBase = 0x00100000;
constexpr uint64_t kGccSize = 0x000b0000;
constexpr uint64_t kDwc3Base = 0x0a800000;
constexpr uint64_t kDwc3Size = 0x0000d000;
constexpr uint64_t kQscratchBase = 0x0a8f8800;
constexpr uint64_t kQscratchSize = 0x00000400;
constexpr uint64_t kQusb2Base = 0x0c012000;
constexpr uint64_t kQusb2Size = 0x00000400;

constexpr uint32_t kUsbGdsc = 0x0f004;
constexpr uint32_t kUsbBcr = 0x0f000;
constexpr uint32_t kUsbMasterCbcr = 0x0f008;
constexpr uint32_t kUsbSleepCbcr = 0x0f00c;
constexpr uint32_t kUsbMockUtmiCbcr = 0x0f010;
constexpr uint32_t kCfgNocUsb3AxiCbcr = 0x05018;
constexpr uint32_t kAggre1Usb3AxiCbcr = 0x82024;
constexpr uint32_t kUsbPhyCfgAhb2PhyCbcr = 0x6a004;
constexpr uint32_t kQusb2PhyPrimBcr = 0x12000;
constexpr uint32_t kRx1Usb2Clkref = 0x88014;

constexpr uint32_t kGsnpsid = 0xc120;
constexpr uint32_t kGctl = 0xc110;
constexpr uint32_t kGsts = 0xc118;
constexpr uint32_t kGhwparams0 = 0xc140;
constexpr uint32_t kGhwparams2 = 0xc148;
constexpr uint32_t kGhwparams3 = 0xc14c;
constexpr uint32_t kGusb2PhyCfg = 0xc200;
constexpr uint32_t kDcfg = 0xc700;
constexpr uint32_t kDctl = 0xc704;
constexpr uint32_t kDevten = 0xc708;
constexpr uint32_t kDsts = 0xc70c;
constexpr uint32_t kDalepena = 0xc720;
constexpr uint32_t kGevntadrlo = 0xc400;
constexpr uint32_t kGevntadrhi = 0xc404;
constexpr uint32_t kGevntsiz = 0xc408;
constexpr uint32_t kGevntcount = 0xc40c;

constexpr uint32_t kQscratchGeneralCfg = 0x08;
constexpr uint32_t kQscratchHsPhyCtrl = 0x10;
constexpr uint32_t kQscratchSsPhyCtrl = 0x30;

constexpr uint32_t kQusb2PllStatus = 0x1a0;
constexpr uint32_t kQusb2PwrCtrl1 = 0x210;
constexpr uint32_t kQusb2PortTune1 = 0x240;
constexpr uint32_t kQusb2Test1 = 0x254;

struct Mapping {
	IOMemoryMap *map = nullptr;
	uintptr_t address = 0;

	~Mapping()
	{
		OSSafeReleaseNULL(map);
	}
};

struct GateState {
	bool gdscOn;
	bool dwc3Readable;
	bool phyReadable;
};

bool
mapPhysical(uint64_t physical, uint64_t length, Mapping &mapping)
{
	IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress(
	    physical, length, kIODirectionIn);
	if (descriptor == nullptr) {
		return false;
	}
	mapping.map = descriptor->map();
	descriptor->release();
	if (mapping.map == nullptr || mapping.map->getVirtualAddress() == 0) {
		return false;
	}
	mapping.address = mapping.map->getVirtualAddress();
	return true;
}

uint32_t
read32(uintptr_t base, uint32_t offset)
{
	uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

bool
branchLive(uint32_t value)
{
	// Qualcomm branch clocks report BIT(31) while halted. The exact enable bit
	// can be voted by firmware, so the halt bit is the conservative read gate.
	return (value & 0x80000000U) == 0;
}

void
probeGcc(GateState &gates)
{
	gates = {};

	Mapping gcc;
	if (!mapPhysical(kGccBase, kGccSize, gcc)) {
		IOLog("CHEESEBURGER_USB: GCC map failed\n");
		return;
	}

	const uint32_t gdsc = read32(gcc.address, kUsbGdsc);
	const uint32_t bcr = read32(gcc.address, kUsbBcr);
	const uint32_t master = read32(gcc.address, kUsbMasterCbcr);
	const uint32_t sleep = read32(gcc.address, kUsbSleepCbcr);
	const uint32_t mockUtmi = read32(gcc.address, kUsbMockUtmiCbcr);
	const uint32_t cfgNoc = read32(gcc.address, kCfgNocUsb3AxiCbcr);
	const uint32_t aggre1 = read32(gcc.address, kAggre1Usb3AxiCbcr);
	const uint32_t ahb2Phy = read32(gcc.address, kUsbPhyCfgAhb2PhyCbcr);
	const uint32_t qusb2Reset = read32(gcc.address, kQusb2PhyPrimBcr);
	const uint32_t rx1Clkref = read32(gcc.address, kRx1Usb2Clkref);

	IOLog("CHEESEBURGER_USB: gdsc=%08x bcr=%08x master=%08x sleep=%08x "
	    "mock_utmi=%08x\n", gdsc, bcr, master, sleep, mockUtmi);
	IOLog("CHEESEBURGER_USB: cfg_noc=%08x aggre1=%08x ahb2phy=%08x "
	    "qusb2_reset=%08x rx1_clkref=%08x\n",
	    cfgNoc, aggre1, ahb2Phy, qusb2Reset, rx1Clkref);

	gates.gdscOn = (gdsc & 0x80000000U) != 0;
	gates.dwc3Readable = gates.gdscOn && branchLive(master) && branchLive(cfgNoc);
	gates.phyReadable = gates.dwc3Readable && branchLive(ahb2Phy);
	IOLog("CHEESEBURGER_USB: gates gdsc_on=%u dwc3_readable=%u phy_readable=%u\n",
	    gates.gdscOn ? 1U : 0U, gates.dwc3Readable ? 1U : 0U,
	    gates.phyReadable ? 1U : 0U);
}

void
probeDwc3(const GateState &gates)
{
	if (!gates.dwc3Readable) {
		IOLog("CHEESEBURGER_USB: DWC3/QSCRATCH untouched (clock or power gate closed)\n");
		return;
	}

	Mapping dwc3;
	if (!mapPhysical(kDwc3Base, kDwc3Size, dwc3)) {
		IOLog("CHEESEBURGER_USB: DWC3 map failed\n");
		return;
	}

	const uint32_t gsnpsid = read32(dwc3.address, kGsnpsid);
	const uint32_t gctl = read32(dwc3.address, kGctl);
	const uint32_t gsts = read32(dwc3.address, kGsts);
	const uint32_t hw0 = read32(dwc3.address, kGhwparams0);
	const uint32_t hw2 = read32(dwc3.address, kGhwparams2);
	const uint32_t hw3 = read32(dwc3.address, kGhwparams3);
	const uint32_t phyCfg = read32(dwc3.address, kGusb2PhyCfg);
	const uint32_t dcfg = read32(dwc3.address, kDcfg);
	const uint32_t dctl = read32(dwc3.address, kDctl);
	const uint32_t devten = read32(dwc3.address, kDevten);
	const uint32_t dsts = read32(dwc3.address, kDsts);
	const uint32_t dalepena = read32(dwc3.address, kDalepena);
	const uint32_t evtLo = read32(dwc3.address, kGevntadrlo);
	const uint32_t evtHi = read32(dwc3.address, kGevntadrhi);
	const uint32_t evtSiz = read32(dwc3.address, kGevntsiz);
	const uint32_t evtCount = read32(dwc3.address, kGevntcount);

	IOLog("CHEESEBURGER_USB: dwc3 id=%08x hw0=%08x hw2=%08x hw3=%08x "
	    "addr_width=%u\n", gsnpsid, hw0, hw2, hw3, hw0 >> 24);
	IOLog("CHEESEBURGER_USB: gctl=%08x role=%u gsts=%08x gusb2phycfg=%08x\n",
	    gctl, (gctl >> 12) & 3U, gsts, phyCfg);
	IOLog("CHEESEBURGER_USB: dcfg=%08x dctl=%08x devten=%08x dsts=%08x "
	    "dalepena=%08x\n", dcfg, dctl, devten, dsts, dalepena);
	IOLog("CHEESEBURGER_USB: event addr=%08x%08x size=%08x count=%08x\n",
	    evtHi, evtLo, evtSiz, evtCount);

	Mapping qscratch;
	if (mapPhysical(kQscratchBase, kQscratchSize, qscratch)) {
		IOLog("CHEESEBURGER_USB: qscratch general=%08x hs_phy_ctrl=%08x "
		    "ss_phy_ctrl=%08x\n",
		    read32(qscratch.address, kQscratchGeneralCfg),
		    read32(qscratch.address, kQscratchHsPhyCtrl),
		    read32(qscratch.address, kQscratchSsPhyCtrl));
	} else {
		IOLog("CHEESEBURGER_USB: QSCRATCH map failed\n");
	}
}

void
probeQusb2(const GateState &gates)
{
	if (!gates.phyReadable) {
		IOLog("CHEESEBURGER_USB: QUSB2 untouched (AHB2PHY clock gate closed)\n");
		return;
	}

	Mapping qusb2;
	if (!mapPhysical(kQusb2Base, kQusb2Size, qusb2)) {
		IOLog("CHEESEBURGER_USB: QUSB2 map failed\n");
		return;
	}
	IOLog("CHEESEBURGER_USB: qusb2 pll=%08x pwr1=%08x tune1=%08x test1=%08x\n",
	    read32(qusb2.address, kQusb2PllStatus),
	    read32(qusb2.address, kQusb2PwrCtrl1),
	    read32(qusb2.address, kQusb2PortTune1),
	    read32(qusb2.address, kQusb2Test1));
}

void
usbProbeThread(void *, wait_result_t)
{
	uint32_t delaySeconds = 130;
	if (PE_parse_boot_argn("cheeseburger-usb-delay", &delaySeconds,
	    sizeof delaySeconds) && delaySeconds > 600) {
		delaySeconds = 600;
	}
	if (delaySeconds != 0) {
		IOLog("CHEESEBURGER_USB: delaying probe %u seconds for persistent logging\n",
		    delaySeconds);
		IOSleep(delaySeconds * 1000U);
	}

	IOLog("CHEESEBURGER_USB: build=port-r1 mode=probe\n");
	GateState gates{};
	probeGcc(gates);
	probeDwc3(gates);
	probeQusb2(gates);
	IOLog("CHEESEBURGER_USB: probe complete\n");
	thread_terminate(current_thread());
}

} // namespace

extern "C" void
cheeseburger_usb_probe_start(void)
{
	static uint32_t attempted;
	uint32_t mode = 0;

	if (!PE_parse_boot_argn("cheeseburger-usb", &mode, sizeof mode) || mode == 0) {
		return;
	}
	if (mode != 1) {
		IOLog("CHEESEBURGER_USB: mode=%u unsupported; this build is probe-only\n", mode);
		return;
	}
	uint32_t expected = 0;
	if (!__atomic_compare_exchange_n(&attempted, &expected, 1, false,
	    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return;
	}

	IOLog("CHEESEBURGER_USB: probe requested\n");
	thread_t thread = nullptr;
	kern_return_t result = kernel_thread_start(usbProbeThread, nullptr, &thread);
	if (result != KERN_SUCCESS) {
		IOLog("CHEESEBURGER_USB: thread start failed %x\n", result);
		return;
	}
	thread_deallocate(thread);
}
