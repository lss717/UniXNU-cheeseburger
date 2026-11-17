// SPDX-License-Identifier: BSD-2-Clause-Patent
// OnePlus 5 USB handoff probe, EP0 validation and opt-in CDC ACM live log.
// Mode 1 is read-only. Mode 2 owns DWC3 EP0 and the CDC data endpoints.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/task.h>
#include <kern/clock.h>
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
constexpr uint32_t kQusb2PortPowerDown = 0x210;
constexpr uint32_t kQusb2PortTune2 = 0x240;
constexpr uint32_t kQusb2PortTest1 = 0x24c;

constexpr uint32_t kDmaBytes = 0x4000;
constexpr uint32_t kEventBytes = 0x1000;
constexpr uint32_t kEventOffset = 0x0000;
constexpr uint32_t kSetupTrbOffset = 0x1000;
constexpr uint32_t kDataTrbOffset = 0x1040;
constexpr uint32_t kStatusInTrbOffset = 0x1080;
constexpr uint32_t kStatusOutTrbOffset = 0x10c0;
constexpr uint32_t kRxTrbOffset = 0x1100;
constexpr uint32_t kBulkInTrbOffset = 0x1140;
constexpr uint32_t kNotifyTrbOffset = 0x1180;
constexpr uint32_t kSetupOffset = 0x1200;
constexpr uint32_t kDataOffset = 0x1300;
constexpr uint32_t kNotifyOffset = 0x1500;
constexpr uint32_t kRxOffset = 0x1800;
constexpr uint32_t kTxOffset = 0x2000;
constexpr uint32_t kRxBytes = 512;
constexpr uint32_t kLogPayloadMax = 1984;
constexpr uint32_t kLogHeaderBytes = 32;

constexpr uint32_t kDctlRunStop = 1U << 31;
constexpr uint32_t kDctlCsftrst = 1U << 30;
constexpr uint32_t kDstsDevctrlhlt = 1U << 22;
constexpr uint32_t kGctlPrtcapdirShift = 12;
constexpr uint32_t kGctlPrtcapdirMask = 3U << kGctlPrtcapdirShift;
constexpr uint32_t kGctlPrtcapdirDevice = 2;
constexpr uint32_t kDepcmdCmdact = 1U << 10;
constexpr uint32_t kDepcmdTypeSetEpConfig = 1U;
constexpr uint32_t kDepcmdTypeSetTransferResource = 2U;
constexpr uint32_t kDepcmdTypeSetStall = 4U;
constexpr uint32_t kDepcmdTypeStartTransfer = 6U;
constexpr uint32_t kDepcmdTypeEndTransfer = 8U;
constexpr uint32_t kDepcmdTypeStartConfig = 9U;
constexpr uint32_t kDepcmdCmdIoc = 1U << 8;
constexpr uint32_t kTrbHwo = 1U << 0;
constexpr uint32_t kTrbLst = 1U << 1;
constexpr uint32_t kTrbIspImi = 1U << 10;
constexpr uint32_t kTrbIoc = 1U << 11;
constexpr uint32_t kTrbCtlSetup = 2U << 4;
constexpr uint32_t kTrbCtlStatus2 = 3U << 4;
constexpr uint32_t kTrbCtlStatus3 = 4U << 4;
constexpr uint32_t kTrbCtlData = 5U << 4;
constexpr uint32_t kTrbCtlNormal = 1U << 4;
constexpr uint32_t kDepevtStatusBusErr = 1U << 0;
constexpr uint32_t kTrbStatusSetupPending = 2;
constexpr int kCompletionSetupPending = -2;

constexpr uint8_t kDeviceDescriptor[] = {
	18, 1, 0x00, 0x02, 0x02, 0x02, 0x01, 64,
	0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1
};
constexpr uint8_t kConfigDescriptor[] = {
	9, 2, 67, 0, 2, 1, 0, 0x80, 250,
	9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
	5, 0x24, 0, 0x10, 1,
	5, 0x24, 1, 0, 1,
	4, 0x24, 2, 2,
	5, 0x24, 6, 0, 1,
	7, 5, 0x82, 3, 16, 0, 9,
	9, 4, 1, 0, 2, 0x0a, 0, 0, 0,
	7, 5, 0x01, 2, 0, 2, 0,
	7, 5, 0x81, 2, 0, 2, 0
};
constexpr uint8_t kQualifierDescriptor[] = {
	10, 6, 0x00, 0x02, 0x02, 0x02, 0x01, 64, 1, 0
};
constexpr uint8_t kBosDescriptor[] = {
	5, 15, 5, 0, 0
};

static_assert(sizeof(kDeviceDescriptor) == 18);
static_assert(kDeviceDescriptor[4] == 0x02 && kDeviceDescriptor[5] == 0x02 &&
    kDeviceDescriptor[6] == 0x01);
static_assert(sizeof(kConfigDescriptor) == 67);
static_assert((kConfigDescriptor[2] | (kConfigDescriptor[3] << 8)) ==
    sizeof(kConfigDescriptor));
static_assert(kConfigDescriptor[4] == 2);
static_assert(kConfigDescriptor[9] == 9 && kConfigDescriptor[10] == 4 &&
    kConfigDescriptor[11] == 0 && kConfigDescriptor[13] == 1 &&
    kConfigDescriptor[14] == 0x02 && kConfigDescriptor[15] == 0x02 &&
    kConfigDescriptor[16] == 0x01);
static_assert(kConfigDescriptor[44] == 9 && kConfigDescriptor[45] == 4 &&
    kConfigDescriptor[46] == 1 && kConfigDescriptor[48] == 2 &&
    kConfigDescriptor[49] == 0x0a);
static_assert(kConfigDescriptor[39] == 0x82 && kConfigDescriptor[55] == 0x01 &&
    kConfigDescriptor[62] == 0x81);

struct CheeseburgerMsgBuf {
	int msg_magic;
	int msg_size;
	int msg_bufx;
	int msg_bufr;
	char *msg_bufc;
};

extern "C" struct CheeseburgerMsgBuf *msgbufp;
extern "C" void native_h103_log_freeze(void);

uint32_t
crc32(const uint8_t *data, size_t length)
{
	uint32_t crc = 0xffffffffU;
	for (size_t index = 0; index < length; index++) {
		crc ^= data[index];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
		}
	}
	return ~crc;
}

void
putLe16(uint8_t *data, uint16_t value)
{
	data[0] = static_cast<uint8_t>(value);
	data[1] = static_cast<uint8_t>(value >> 8);
}

void
putLe32(uint8_t *data, uint32_t value)
{
	data[0] = static_cast<uint8_t>(value);
	data[1] = static_cast<uint8_t>(value >> 8);
	data[2] = static_cast<uint8_t>(value >> 16);
	data[3] = static_cast<uint8_t>(value >> 24);
}

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
mapPhysical(uint64_t physical, uint64_t length, Mapping &mapping,
    IODirection direction = kIODirectionIn)
{
	IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress(
	    physical, length, direction);
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
	IOLog("CHEESEBURGER_USB: qusb2 pll=%08x powerdown=%08x tune2=%08x "
	    "test1=%08x\n",
	    read32(qusb2.address, kQusb2PllStatus),
	    read32(qusb2.address, kQusb2PortPowerDown),
	    read32(qusb2.address, kQusb2PortTune2),
	    read32(qusb2.address, kQusb2PortTest1));
}

void
write32(uintptr_t base, uint32_t offset, uint32_t value)
{
	*reinterpret_cast<volatile uint32_t *>(base + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

void
syncDma(void *address, size_t length, bool invalidate)
{
	uintptr_t begin = reinterpret_cast<uintptr_t>(address) & ~uintptr_t(63);
	uintptr_t end = (reinterpret_cast<uintptr_t>(address) + length + 63) &
	    ~uintptr_t(63);
	for (uintptr_t line = begin; line < end; line += 64) {
		if (invalidate) {
			__asm__ volatile("dc ivac, %0" : : "r"(line) : "memory");
		} else {
			__asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

constexpr uint32_t
depcmdPar0(unsigned endpoint)
{
	return 0xc808 + 0x10 * endpoint;
}

constexpr uint32_t
depcmdPar1(unsigned endpoint)
{
	return 0xc804 + 0x10 * endpoint;
}

constexpr uint32_t
depcmdPar2(unsigned endpoint)
{
	return 0xc800 + 0x10 * endpoint;
}

constexpr uint32_t
depcmd(unsigned endpoint)
{
	return 0xc80c + 0x10 * endpoint;
}

unsigned
stringDescriptor(unsigned index, uint8_t *out)
{
	static const char *const strings[] = {
		nullptr, "OnePlus 5", "UniXNU USB CDC", "OP5CDCH408"
	};
	if (index == 0) {
		out[0] = 4;
		out[1] = 3;
		out[2] = 0x09;
		out[3] = 0x04;
		return 4;
	}
	if (index >= sizeof(strings) / sizeof(strings[0])) {
		return 0;
	}
	const char *text = strings[index];
	unsigned length = 2;
	for (; *text != '\0' && length < 126; text++) {
		out[length++] = static_cast<uint8_t>(*text);
		out[length++] = 0;
	}
	out[0] = static_cast<uint8_t>(length);
	out[1] = 3;
	return length;
}

struct Ep0Device {
	enum class Phase {
		Idle,
		Setup,
		DataIn,
		DataOut,
		WaitStatus,
		Status
	};

	Mapping dwc3;
	Mapping qscratch;
	IOBufferMemoryDescriptor *dmaDescriptor = nullptr;
	uint8_t *dma = nullptr;
	uint64_t dmaPhysical = 0;
	uint32_t eventPosition = 0;
	uint32_t eventCount = 0;
	uint32_t setupCount = 0;
	uint32_t connectGeneration = 0;
	uint32_t lastSetupType = 0;
	uint32_t lastSetupRequest = 0;
	uint32_t lastSetupLength = 0;
	uint32_t lastSetupResponse = 0;
	bool lastSetupDataIn = false;
	bool lastSetupDataOut = false;
	Phase phase = Phase::Idle;
	bool configured = false;
	bool dataEndpoints = false;
	bool setupArmed = false;
	bool dataIn = false;
	bool dtr = false;
	bool notifyPending = false;
	bool fault = false;
	bool transferBusy[6] = {};
	uint32_t transferRequested[6] = {};
	uint32_t transferResource[6] = {};
	int pendingStatusEndpoint = -1;
	bool endTransferEventPending = false;
	uint32_t bootId = 0;
	uint32_t logSequence = 0;
	uint32_t txFrames = 0;
	uint64_t txBytes = 0;
	uint64_t rxBytes = 0;
	uint64_t droppedBytes = 0;
	int logCursor = -1;
	uint8_t lineCoding[7] = {0x00, 0xc2, 0x01, 0x00, 0x00, 0x00, 0x08};

	~Ep0Device()
	{
		if (dmaDescriptor != nullptr) {
			dmaDescriptor->complete();
			dmaDescriptor->release();
		}
	}

	uint32_t
	read(uint32_t offset)
	{
		return read32(dwc3.address, offset);
	}

	void
	write(uint32_t offset, uint32_t value)
	{
		write32(dwc3.address, offset, value);
	}

	bool
	waitFor(uint32_t offset, uint32_t mask, uint32_t expected, unsigned usec)
	{
		for (unsigned elapsed = 0; elapsed < usec; elapsed += 10) {
			if ((read(offset) & mask) == expected) {
				return true;
			}
			IODelay(10);
		}
		return false;
	}

	bool
	command(unsigned endpoint, uint32_t type, uint32_t p0, uint32_t p1,
	    uint32_t p2)
	{
		write(depcmdPar0(endpoint), p0);
		write(depcmdPar1(endpoint), p1);
		write(depcmdPar2(endpoint), p2);
		write(depcmd(endpoint), type | kDepcmdCmdact);
		for (unsigned elapsed = 0; elapsed < 1000000; elapsed++) {
			uint32_t value = read(depcmd(endpoint));
			if ((value & kDepcmdCmdact) == 0) {
				uint32_t status = (value >> 12) & 0xf;
				if (status != 0) {
					IOLog("CHEESEBURGER_USB: ep%u command %u failed status=%u\n",
					    endpoint, type, status);
					return false;
				}
				if ((type & 0xfU) == kDepcmdTypeStartTransfer) {
					transferResource[endpoint] = (value >> 16) & 0x7fU;
				}
				return true;
			}
			IODelay(1);
		}
		IOLog("CHEESEBURGER_USB: ep%u command %u timeout value=%08x\n",
		    endpoint, type, read(depcmd(endpoint)));
		return false;
	}

	bool
	startTransfer(unsigned endpoint, uint32_t trbOffset, uint32_t dataOffset,
	    uint32_t length, uint32_t control)
	{
		if (endpoint >= 6 || transferBusy[endpoint]) {
			return false;
		}
		const uint64_t buffer = dmaPhysical + dataOffset;
		const uint64_t trbPhysical = dmaPhysical + trbOffset;
		volatile uint32_t *trb = reinterpret_cast<volatile uint32_t *>(
		    dma + trbOffset);
		trb[0] = static_cast<uint32_t>(buffer);
		trb[1] = static_cast<uint32_t>(buffer >> 32);
		trb[2] = length;
		trb[3] = control | kTrbHwo | kTrbLst | kTrbIspImi | kTrbIoc;
		if (length != 0 && (control == kTrbCtlData || control == kTrbCtlNormal)) {
			syncDma(dma + dataOffset, length, false);
		}
		syncDma(dma + trbOffset, sizeof(uint32_t) * 4, false);
		transferBusy[endpoint] = true;
		transferRequested[endpoint] = length;
		if (command(endpoint, kDepcmdTypeStartTransfer,
		    static_cast<uint32_t>(trbPhysical >> 32),
		    static_cast<uint32_t>(trbPhysical), 0)) {
			return true;
		}
		transferBusy[endpoint] = false;
		return false;
	}

	int
	completeTransfer(unsigned endpoint, uint32_t trbOffset)
	{
		if (endpoint >= 6 || !transferBusy[endpoint]) {
			IOLog("CHEESEBURGER_USB: unexpected completion ep=%u\n", endpoint);
			fault = true;
			return -1;
		}
		syncDma(dma + trbOffset, sizeof(uint32_t) * 4, true);
		volatile uint32_t *trb = reinterpret_cast<volatile uint32_t *>(
		    dma + trbOffset);
		const uint32_t remaining = trb[2] & 0xffffffU;
		const uint32_t status = trb[2] >> 28;
		if ((trb[3] & kTrbHwo) != 0 ||
		    remaining > transferRequested[endpoint]) {
			IOLog("CHEESEBURGER_USB: invalid completion ep=%u trb=%08x/%08x\n",
			    endpoint, trb[2], trb[3]);
			fault = true;
			return -1;
		}
		transferBusy[endpoint] = false;
		if (endpoint <= 1 && status == kTrbStatusSetupPending) {
			return kCompletionSetupPending;
		}
		if (status != 0) {
			IOLog("CHEESEBURGER_USB: invalid completion ep=%u trb=%08x/%08x\n",
			    endpoint, trb[2], trb[3]);
			fault = true;
			return -1;
		}
		return static_cast<int>(transferRequested[endpoint] - remaining);
	}

	uint32_t
	endControlData()
	{
		if (!transferBusy[1]) {
			return 1;
		}
		const uint32_t resource = transferResource[1];
		if (resource == 0) {
			IOLog("CHEESEBURGER_USB: control data-in missing resource index\n");
			fault = true;
			return 0;
		}
		for (unsigned attempt = 0; attempt < 3; attempt++) {
			if (command(1, kDepcmdTypeEndTransfer | kDepcmdCmdIoc |
			    (resource << 16), 0, 0, 0)) {
				transferBusy[1] = false;
				transferResource[1] = 0;
				endTransferEventPending = true;
				IOLog("CHEESEBURGER_USB: control data-in end requested resource=%u\n",
				    resource);
				return 1;
			}
			IODelay(100);
		}
		fault = true;
		return 0;
	}

	uint32_t
	startControlStatus(unsigned endpoint)
	{
		phase = Phase::Status;
		pendingStatusEndpoint = -1;
		IOLog("CHEESEBURGER_USB: control status start ep=%u data_in=%u\n",
		    endpoint, dataIn ? 1U : 0U);
		unsigned started = 0;
		for (unsigned attempt = 0; attempt < 8 && !started; attempt++) {
			if (attempt != 0) {
				IODelay(100);
			}
			const uint32_t statusControl = dataIn ?
			    kTrbCtlStatus3 : kTrbCtlStatus2;
			started = startTransfer(endpoint,
			    endpoint == 0 ? kStatusOutTrbOffset : kStatusInTrbOffset,
			    kDataOffset, 0, statusControl);
		}
		if (!started) {
			IOLog("CHEESEBURGER_USB: control status retries exhausted ep=%u\n",
			    endpoint);
			fault = true;
			return 0;
		}
		return 1;
	}

	uint32_t
	consumeSetupPendingDataIn(unsigned endpoint)
	{
		if (endpoint != 0 || phase != Phase::DataIn || !transferBusy[1]) {
			return 0;
		}
		syncDma(dma + kSetupTrbOffset, sizeof(uint32_t) * 4 * 4, true);
		volatile uint32_t *setupTrb = reinterpret_cast<volatile uint32_t *>(
		    dma + kSetupTrbOffset);
		volatile uint32_t *dataTrb = reinterpret_cast<volatile uint32_t *>(
		    dma + kDataTrbOffset);
		volatile uint32_t *statusInTrb = reinterpret_cast<volatile uint32_t *>(
		    dma + kStatusInTrbOffset);
		volatile uint32_t *statusOutTrb = reinterpret_cast<volatile uint32_t *>(
		    dma + kStatusOutTrbOffset);
		syncDma(dma + kSetupOffset, 8, true);
		const uint8_t *pendingSetup = dma + kSetupOffset;
		IOLog("CHEESEBURGER_USB: control abnormal ep0 complete resource=%u "
		    "setup=%08x/%08x data=%08x/%08x in=%08x/%08x out=%08x/%08x "
		    "setup-bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
		    transferResource[1], setupTrb[2], setupTrb[3], dataTrb[2], dataTrb[3],
		    statusInTrb[2], statusInTrb[3], statusOutTrb[2], statusOutTrb[3],
		    pendingSetup[0], pendingSetup[1], pendingSetup[2], pendingSetup[3],
		    pendingSetup[4], pendingSetup[5], pendingSetup[6], pendingSetup[7]);
		if (!endControlData()) {
			return 1;
		}
		phase = Phase::WaitStatus;
		IOLog("CHEESEBURGER_USB: control waiting for end-transfer completion\n");
		return 1;
	}

	bool
	armReceive()
	{
		if (!dataEndpoints || fault || transferBusy[2]) {
			return !fault;
		}
		if (!startTransfer(2, kRxTrbOffset, kRxOffset, kRxBytes,
		    kTrbCtlNormal)) {
			fault = true;
			return false;
		}
		return true;
	}

	bool
	configureDataEndpoints()
	{
		if (dataEndpoints || fault) {
			return !fault;
		}
		const uint32_t bulkIn = (2U << 1) | (512U << 3) | (1U << 17);
		const uint32_t bulkOut = (2U << 1) | (512U << 3);
		const uint32_t interruptIn = (3U << 1) | (16U << 3) | (2U << 17);
		if (!command(0, kDepcmdTypeStartConfig | (2U << 16), 0, 0, 0) ||
		    !command(2, kDepcmdTypeSetEpConfig, bulkOut,
		    (1U << 8) | (2U << 25), 0) ||
		    !command(2, kDepcmdTypeSetTransferResource, 1, 0, 0) ||
		    !command(3, kDepcmdTypeSetEpConfig, bulkIn,
		    (1U << 8) | (3U << 25), 0) ||
		    !command(3, kDepcmdTypeSetTransferResource, 1, 0, 0) ||
		    !command(5, kDepcmdTypeSetEpConfig, interruptIn,
		    (1U << 8) | (8U << 16) | (5U << 25), 0) ||
		    !command(5, kDepcmdTypeSetTransferResource, 1, 0, 0)) {
			fault = true;
			return false;
		}
		write(kDalepena, read(kDalepena) | 3U | (1U << 2) | (1U << 3) |
		    (1U << 5));
		dataEndpoints = true;
		if (!armReceive()) {
			return false;
		}
		IOLog("CHEESEBURGER_USB: CDC data endpoints enabled out=%u in=%u irq=%u\n",
		    2U, 3U, 5U);
		return true;
	}

	bool
	configureEp0()
	{
		if (fault) {
			return false;
		}
		if (!command(0, kDepcmdTypeStartConfig, 0, 0, 0)) {
			return false;
		}
		const uint32_t endpoint0Out = (0U << 1) | (64U << 3);
		const uint32_t endpoint0In = (0U << 1) | (64U << 3);
		const uint32_t outParams = (1U << 8) | (1U << 10) | (0U << 25);
		const uint32_t inParams = (1U << 8) | (1U << 10) | (1U << 25);
		if (!command(0, kDepcmdTypeSetEpConfig, endpoint0Out, outParams, 0) ||
		    !command(0, kDepcmdTypeSetTransferResource, 1, 0, 0) ||
		    !command(1, kDepcmdTypeSetEpConfig, endpoint0In, inParams, 0) ||
		    !command(1, kDepcmdTypeSetTransferResource, 1, 0, 0)) {
			return false;
		}
		write(kDalepena, 3U);
		dataEndpoints = false;
		dtr = false;
		notifyPending = false;
		pendingStatusEndpoint = -1;
		bzero(transferBusy, sizeof(transferBusy));
		bzero(transferResource, sizeof(transferResource));
		return true;
	}

	bool
	armSetup()
	{
		if (fault || setupArmed) {
			return !fault;
		}
		phase = Phase::Setup;
		setupArmed = false;
		for (unsigned attempt = 0; attempt < 8 && !setupArmed; attempt++) {
			if (attempt != 0) {
				IODelay(100);
			}
			setupArmed = startTransfer(0, kSetupTrbOffset, kSetupOffset, 8,
			    kTrbCtlSetup);
		}
		if (!setupArmed) {
			IOLog("CHEESEBURGER_USB: EP0 setup arm retries exhausted\n");
			fault = true;
		}
		return setupArmed;
	}

	void
	stall()
	{
		if (!command(0, kDepcmdTypeSetStall, 0, 0, 0)) {
			fault = true;
			return;
		}
		setupArmed = false;
		armSetup();
	}

	uint32_t
	collectLog(uint8_t *output, uint32_t capacity)
	{
		struct CheeseburgerMsgBuf *buffer = msgbufp;
		if (buffer == nullptr || buffer->msg_bufc == nullptr ||
		    buffer->msg_size <= 0 || buffer->msg_bufx < 0 ||
		    buffer->msg_bufx >= buffer->msg_size) {
			return 0;
		}
		const uint32_t size = static_cast<uint32_t>(buffer->msg_size);
		const uint32_t current = static_cast<uint32_t>(buffer->msg_bufx);
		if (logCursor < 0 || static_cast<uint32_t>(logCursor) >= size) {
			logCursor = static_cast<int>(current);
			return 0;
		}
		const uint32_t previous = static_cast<uint32_t>(logCursor);
		uint32_t length = (current + size - previous) % size;
		if (length == 0) {
			return 0;
		}
		uint32_t offset = previous;
		if (length > capacity) {
			droppedBytes += length - capacity;
			length = capacity;
			offset = (current + size - length) % size;
		}
		for (uint32_t index = 0; index < length; index++) {
			output[index] = static_cast<uint8_t>(buffer->msg_bufc[(offset + index) % size]);
		}
		logCursor = static_cast<int>(current);
		return length;
	}

	bool
	sendNotification()
	{
		if (!notifyPending || transferBusy[5]) {
			return !fault;
		}
		uint8_t *data = dma + kNotifyOffset;
		const uint8_t serialState[10] = {0xa1, 0x20, 0, 0, 0, 0, 2, 0, 3, 0};
		memcpy(data, serialState, sizeof(serialState));
		if (!startTransfer(5, kNotifyTrbOffset, kNotifyOffset,
		    sizeof(serialState), kTrbCtlNormal)) {
			fault = true;
			return false;
		}
		notifyPending = false;
		return true;
	}

	bool
	sendLogFrame()
	{
		if (!configured || !dataEndpoints || !dtr || fault || transferBusy[3]) {
			return !fault;
		}
		uint8_t *frame = dma + kTxOffset;
		uint8_t *payload = frame + kLogHeaderBytes;
		const uint32_t payloadLength = collectLog(payload, kLogPayloadMax);
		if (payloadLength == 0) {
			return true;
		}
		bzero(frame, kLogHeaderBytes);
		frame[0] = 'U';
		frame[1] = 'X';
		frame[2] = 'L';
		frame[3] = '1';
		frame[4] = 1;
		frame[5] = kLogHeaderBytes;
		putLe32(frame + 8, bootId);
		putLe32(frame + 12, logSequence);
		putLe32(frame + 16, static_cast<uint32_t>(droppedBytes));
		frame[20] = 'H';
		frame[21] = '4';
		frame[22] = '0';
		frame[23] = '8';
		putLe16(frame + 24, static_cast<uint16_t>(payloadLength));
		putLe32(frame + 28, crc32(payload, payloadLength));
		const uint32_t length = kLogHeaderBytes + payloadLength;
		if (!startTransfer(3, kBulkInTrbOffset, kTxOffset, length,
		    kTrbCtlNormal)) {
			IOSleep(10);
			return true;
		}
		logSequence++;
		txFrames++;
		txBytes += payloadLength;
		return true;
	}

	void
	transmit()
	{
		if (!configured || fault) {
			return;
		}
		sendNotification();
		sendLogFrame();
	}

	void
	handleSetup()
	{
		syncDma(dma + kSetupOffset, 8, true);
		const uint8_t *setup = dma + kSetupOffset;
		uint8_t *data = dma + kDataOffset;
		const uint8_t requestType = setup[0];
		const uint8_t request = setup[1];
		const uint16_t value = static_cast<uint16_t>(setup[2] |
		    (setup[3] << 8));
		const uint16_t index = static_cast<uint16_t>(setup[4] |
		    (setup[5] << 8));
		const uint16_t length = static_cast<uint16_t>(setup[6] |
		    (setup[7] << 8));
		unsigned responseLength = 0;
		bool ok = true;
		bool dataStageOut = false;

		dataIn = (requestType & 0x80) != 0;
		setupCount++;
		if (setupCount <= 64) {
			IOLog("CHEESEBURGER_USB: setup %02x %02x value=%04x "
			    "index=%04x length=%u\n", requestType, request, value,
			    index, length);
		}
		if ((requestType & 0x60) == 0 && request == 6) {
			switch (value >> 8) {
			case 1:
				if ((value & 0xff) == 0) {
					memcpy(data, kDeviceDescriptor, sizeof(kDeviceDescriptor));
					responseLength = sizeof(kDeviceDescriptor);
				} else {
					ok = false;
				}
				break;
			case 2:
			case 7:
				if ((value & 0xff) == 0) {
					memcpy(data, kConfigDescriptor, sizeof(kConfigDescriptor));
					responseLength = sizeof(kConfigDescriptor);
				} else {
					ok = false;
				}
				break;
			case 3:
				responseLength = stringDescriptor(value & 0xff, data);
				ok = responseLength != 0;
				break;
			case 6:
				if ((value & 0xff) == 0) {
					memcpy(data, kQualifierDescriptor,
					    sizeof(kQualifierDescriptor));
					responseLength = sizeof(kQualifierDescriptor);
				} else {
					ok = false;
				}
				break;
			case 15:
				if ((value & 0xff) == 0) {
					memcpy(data, kBosDescriptor, sizeof(kBosDescriptor));
					responseLength = sizeof(kBosDescriptor);
				} else {
					ok = false;
				}
				break;
			default:
				ok = false;
				break;
			}
		} else if ((requestType & 0x60) == 0 && request == 5) {
			const uint32_t dcfg = read(kDcfg) & ~(0x7fU << 3);
			write(kDcfg, dcfg | (static_cast<uint32_t>(value & 0x7f) << 3));
		} else if ((requestType & 0x60) == 0 && request == 9) {
			if (index != 0 || length != 0 || value > 1) {
				ok = false;
			} else if (value == 1) {
				if (!configureDataEndpoints()) {
					ok = false;
				} else {
					configured = true;
					dtr = false;
					IOLog("CHEESEBURGER_USB: host configured CDC ACM device\n");
				}
			} else {
				configured = false;
				dataEndpoints = false;
				dtr = false;
			}
		} else if ((requestType & 0x60) == 0 && request == 8) {
			data[0] = configured ? 1 : 0;
			responseLength = 1;
		} else if ((requestType & 0x60) == 0 && request == 0) {
			data[0] = 0;
			data[1] = 0;
			responseLength = 2;
		} else if ((requestType & 0x7f) == 0x01 && request == 10) {
			if (length != 1 || value != 0 || index > 1) {
				ok = false;
			} else {
				data[0] = 0;
				responseLength = 1;
			}
		} else if ((requestType & 0x7f) == 0x01 && request == 11) {
			if (length != 0 || value != 0 || index > 1) {
				ok = false;
			}
		} else if ((requestType & 0x7f) == 0x02 && request == 1) {
			if (length != 0 || (index != 0x01 && index != 0x81 && index != 0x82)) {
				ok = false;
			}
		} else if (requestType == 0x21 && request == 0x20) {
			if (value != 0 || index != 0 || length != 7) {
				ok = false;
			} else {
				dataStageOut = true;
			}
		} else if (requestType == 0xa1 && request == 0x21) {
			if (value != 0 || index != 0 || length != 7) {
				ok = false;
			} else {
				memcpy(data, lineCoding, sizeof(lineCoding));
				responseLength = sizeof(lineCoding);
			}
		} else if (requestType == 0x21 && request == 0x22) {
			if (index != 0 || length != 0 || (value & ~3U) != 0) {
				ok = false;
			} else {
				dtr = configured && ((value & 1U) != 0);
				if (dtr) {
					notifyPending = true;
				}
			}
		} else if (requestType == 0x21 && request == 0x23) {
			if (length != 0) {
				ok = false;
			}
		} else {
			ok = false;
		}

		lastSetupType = requestType;
		lastSetupRequest = request;
		lastSetupLength = length;
		lastSetupResponse = responseLength;
		lastSetupDataIn = dataIn;
		lastSetupDataOut = dataStageOut;
		if ((requestType & 0x60) != 0 || setupCount <= 16) {
			IOLog("CHEESEBURGER_USB: control parse type=%02x request=%02x "
			    "length=%u response=%u in=%u out=%u ok=%u\n",
			    requestType, request, length, responseLength,
			    dataIn ? 1U : 0U, dataStageOut ? 1U : 0U, ok ? 1U : 0U);
		}
		if (!ok) {
			stall();
			return;
		}
		if (dataStageOut) {
			phase = Phase::DataOut;
			const uint32_t transferLength = (length + 63U) & ~63U;
			IOLog("CHEESEBURGER_USB: control data-out start length=%u "
			    "transfer=%u busy0=%u\n", length, transferLength,
			    transferBusy[0] ? 1U : 0U);
			if (!startTransfer(0, kDataTrbOffset, kDataOffset, transferLength,
			    kTrbCtlData)) {
				fault = true;
			}
			return;
		}
		if (length == 0) {
			phase = Phase::WaitStatus;
			IOLog("CHEESEBURGER_USB: control wait-status no data\n");
			return;
		}
		if (!dataIn) {
			stall();
			return;
		}
		if (responseLength > length) {
			responseLength = length;
		}
		if (responseLength == 0) {
			stall();
			return;
		}
		phase = Phase::DataIn;
		IOLog("CHEESEBURGER_USB: control data-in start length=%u busy1=%u\n",
		    responseLength, transferBusy[1] ? 1U : 0U);
		if (!startTransfer(1, kDataTrbOffset, kDataOffset, responseLength,
		    kTrbCtlData)) {
			fault = true;
		}
	}

	void
	handleDeviceEvent(uint32_t event)
	{
		const unsigned type = (event >> 8) & 0xf;
		if (type == 0) {
			configured = false;
			dataEndpoints = false;
			dtr = false;
			notifyPending = false;
			setupArmed = false;
			phase = Phase::Idle;
			pendingStatusEndpoint = -1;
			endTransferEventPending = false;
			bzero(transferBusy, sizeof(transferBusy));
			bzero(transferResource, sizeof(transferResource));
			IOLog("CHEESEBURGER_USB: disconnect\n");
		} else if (type == 1) {
			const uint32_t dcfg = read(kDcfg) & ~(0x7fU << 3);
			write(kDcfg, dcfg);
			configured = false;
			dataEndpoints = false;
			dtr = false;
			notifyPending = false;
			setupArmed = false;
			phase = Phase::Idle;
			pendingStatusEndpoint = -1;
			endTransferEventPending = false;
			bzero(transferBusy, sizeof(transferBusy));
			bzero(transferResource, sizeof(transferResource));
			IOLog("CHEESEBURGER_USB: bus reset\n");
			if (!configureEp0()) {
				fault = true;
				return;
			}
			armSetup();
		} else if (type == 2) {
			connectGeneration++;
			IOLog("CHEESEBURGER_USB: connect done speed=%u generation=%u\n",
			    read(kDsts) & 7U, connectGeneration);
			if (phase == Phase::Idle && !setupArmed) {
				armSetup();
			}
		} else if (type == 3) {
			IOLog("CHEESEBURGER_USB: link state change state=%u\n",
			    (event >> 16) & 0xf);
		} else if (type == 4 || type == 6) {
			IOLog("CHEESEBURGER_USB: suspend\n");
		} else if (type == 5) {
			IOLog("CHEESEBURGER_USB: resume\n");
		}
	}

	void
	handleEvent(uint32_t event)
	{
		if ((event & 1U) != 0) {
			handleDeviceEvent(event);
			return;
		}
		const unsigned endpoint = (event >> 1) & 0x1f;
		const unsigned type = (event >> 6) & 0xf;
		const unsigned status = (event >> 12) & 0xf;
		if (endpoint != 0 && endpoint != 1 && endpoint != 2 &&
		    endpoint != 3 && endpoint != 5) {
			return;
		}
		if (type == 1) {
			if ((endpoint == 0 || endpoint == 1) && setupCount <= 32) {
				IOLog("CHEESEBURGER_USB: control complete ep=%u status=%u "
				    "phase=%u busy0=%u busy1=%u\n", endpoint, status,
				    static_cast<unsigned>(phase), transferBusy[0] ? 1U : 0U,
				    transferBusy[1] ? 1U : 0U);
			}
			if (consumeSetupPendingDataIn(endpoint)) {
				return;
			}
			if ((status & kDepevtStatusBusErr) != 0) {
				IOLog("CHEESEBURGER_USB: transfer error ep=%u status=%u\n",
				    endpoint, status);
				fault = true;
				return;
			}
			uint32_t trbOffset = 0;
			if (endpoint == 0 && phase == Phase::Setup) {
				trbOffset = kSetupTrbOffset;
			} else if (endpoint == 0 && phase == Phase::DataOut) {
				trbOffset = kDataTrbOffset;
			} else if (endpoint == 1 && phase == Phase::DataIn) {
				trbOffset = kDataTrbOffset;
			} else if (phase == Phase::Status) {
				trbOffset = endpoint == 0 ? kStatusOutTrbOffset : kStatusInTrbOffset;
			} else if (endpoint == 2) {
				trbOffset = kRxTrbOffset;
			} else if (endpoint == 3) {
				trbOffset = kBulkInTrbOffset;
			} else if (endpoint == 5) {
				trbOffset = kNotifyTrbOffset;
			}
			if (endpoint == 0 && phase == Phase::Status &&
			    !transferBusy[0] && transferBusy[1]) {
				IOLog("CHEESEBURGER_USB: control setup-pending during status\n");
				if (endControlData()) {
					phase = Phase::Idle;
					armSetup();
				}
				return;
			}
			const int completed = trbOffset ? completeTransfer(endpoint, trbOffset) : -1;
			if (completed == kCompletionSetupPending) {
				IOLog("CHEESEBURGER_USB: control setup-pending rearm ep=%u\n",
				    endpoint);
				pendingStatusEndpoint = -1;
				phase = Phase::Idle;
				armSetup();
				return;
			}
			if (completed < 0) {
				return;
			}
			if (endpoint == 2) {
				rxBytes += static_cast<uint32_t>(completed);
				armReceive();
				return;
			}
			if (endpoint == 3 || endpoint == 5) {
				return;
			}
			if (phase == Phase::Setup && endpoint == 0) {
				setupArmed = false;
				handleSetup();
			} else if (phase == Phase::DataIn && endpoint == 1) {
				phase = Phase::WaitStatus;
			} else if (phase == Phase::DataOut && endpoint == 0) {
				if (completed == 7) {
					syncDma(dma + kDataOffset, 7, true);
					memcpy(lineCoding, dma + kDataOffset, 7);
				} else {
					IOLog("CHEESEBURGER_USB: bad line coding length=%d\n",
					    completed);
					fault = true;
					return;
				}
				phase = Phase::WaitStatus;
			} else if (phase == Phase::Status) {
				phase = Phase::Idle;
				armSetup();
			}
		} else if (type == 3 && (endpoint == 0 || endpoint == 1)) {
			if (setupCount <= 32) {
				IOLog("CHEESEBURGER_USB: control not-ready ep=%u status=%u "
				    "phase=%u busy0=%u busy1=%u request=%02x length=%u "
				    "response=%u in=%u out=%u\n", endpoint, status,
				    static_cast<unsigned>(phase), transferBusy[0] ? 1U : 0U,
				    transferBusy[1] ? 1U : 0U, lastSetupRequest,
				    lastSetupLength, lastSetupResponse,
				    lastSetupDataIn ? 1U : 0U, lastSetupDataOut ? 1U : 0U);
			}
			if (status == 2 && phase == Phase::DataIn) {
				pendingStatusEndpoint = static_cast<int>(endpoint);
				IOLog("CHEESEBURGER_USB: control status arrived before data "
				    "complete ep=%u\n", endpoint);
			} else if (status == 2 && phase == Phase::WaitStatus &&
			    !endTransferEventPending) {
				(void)startControlStatus(endpoint);
			}
		} else if (type == 7 && endpoint == 1 &&
		    ((event >> 24) & 0xfU) == kDepcmdTypeEndTransfer &&
		    endTransferEventPending) {
			endTransferEventPending = false;
			IOLog("CHEESEBURGER_USB: control end-transfer complete\n");
			if (phase == Phase::WaitStatus && pendingStatusEndpoint >= 0) {
				(void)startControlStatus(
				    static_cast<unsigned>(pendingStatusEndpoint));
			}
		}
	}

	void
	poll()
	{
		const uint32_t count = read(kGevntcount) & 0xffff;
		if (count > kEventBytes || (count & 3) != 0) {
			IOLog("CHEESEBURGER_USB: invalid event count=%u\n", count);
			fault = true;
			return;
		}
		if (count == 0) {
			return;
		}
		syncDma(dma + kEventOffset, kEventBytes, true);
		for (uint32_t consumed = 0; consumed < count && !fault;
		    consumed += 4) {
			const uint32_t event = *reinterpret_cast<uint32_t *>(
			    dma + kEventOffset + eventPosition);
			eventPosition = (eventPosition + 4) % kEventBytes;
			eventCount++;
			if (eventCount <= 128) {
				IOLog("CHEESEBURGER_USB: event=%08x\n", event);
			}
			handleEvent(event);
		}
		write(kGevntcount, count);
	}

	bool
	begin()
	{
		if (!mapPhysical(kDwc3Base, kDwc3Size, dwc3, kIODirectionInOut) ||
		    !mapPhysical(kQscratchBase, kQscratchSize, qscratch,
		    kIODirectionInOut)) {
			IOLog("CHEESEBURGER_USB: mode2 register map failed\n");
			return false;
		}
		const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
		    static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
		    static_cast<IOOptionBits>(kIOMemoryMapperNone);
		dmaDescriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		    kernel_task, options, kDmaBytes, 0x00000000ffffffffULL);
		bool lowDma = dmaDescriptor != nullptr;
		if (dmaDescriptor == nullptr) {
			IOLog("CHEESEBURGER_USB: low DMA allocation failed; high fallback\n");
			dmaDescriptor = IOBufferMemoryDescriptor::inTaskWithOptions(
			    kernel_task, options, kDmaBytes, 4096);
		}
		if (dmaDescriptor == nullptr || dmaDescriptor->prepare() != kIOReturnSuccess) {
			IOLog("CHEESEBURGER_USB: DMA allocation/prepare failed\n");
			return false;
		}
		dma = static_cast<uint8_t *>(dmaDescriptor->getBytesNoCopy());
		IOByteCount contiguous = 0;
		dmaPhysical = dmaDescriptor->getPhysicalSegment(0, &contiguous,
		    kIOMemoryMapperNone);
		if (dma == nullptr || dmaPhysical == 0 ||
		    (dmaPhysical & 0xfff) != 0 || contiguous < kDmaBytes) {
			IOLog("CHEESEBURGER_USB: invalid DMA phys=%llx contiguous=%llx\n",
			    dmaPhysical, static_cast<uint64_t>(contiguous));
			return false;
		}
		bzero(dma, kDmaBytes);
		syncDma(dma, kDmaBytes, false);
		const uint64_t ticks = mach_absolute_time();
		bootId = static_cast<uint32_t>(ticks) ^
		    static_cast<uint32_t>(ticks >> 32);
		if (bootId == 0) {
			bootId = 1;
		}
		logCursor = -1;

		write(kDevten, 0);
		write(kDctl, read(kDctl) & ~kDctlRunStop);
		if (!waitFor(kDsts, kDstsDevctrlhlt, kDstsDevctrlhlt, 500000)) {
			IOLog("CHEESEBURGER_USB: controller halt timeout dsts=%08x\n",
			    read(kDsts));
			return false;
		}
		write(kDctl, kDctlCsftrst);
		if (!waitFor(kDctl, kDctlCsftrst, 0, 500000)) {
			IOLog("CHEESEBURGER_USB: controller reset timeout dctl=%08x\n",
			    read(kDctl));
			return false;
		}
		IOSleep(10);
		uint32_t gctl = read(kGctl);
		gctl = (gctl & ~kGctlPrtcapdirMask) |
		    (kGctlPrtcapdirDevice << kGctlPrtcapdirShift);
		write(kGctl, gctl);
		if (((read(kGctl) >> kGctlPrtcapdirShift) & 3U) !=
		    kGctlPrtcapdirDevice) {
			IOLog("CHEESEBURGER_USB: device mode set failed gctl=%08x\n",
			    read(kGctl));
			return false;
		}
		IOLog("CHEESEBURGER_USB: device mode gctl=%08x\n", read(kGctl));
		IOSleep(10);

		const uint64_t eventPhysical = dmaPhysical + kEventOffset;
		write(kGevntadrlo, static_cast<uint32_t>(eventPhysical));
		write(kGevntadrhi, static_cast<uint32_t>(eventPhysical >> 32));
		write(kGevntsiz, 0x80000000U | kEventBytes);
		write(kGevntcount, read(kGevntcount) & 0xffff);
		if (read(kGevntadrhi) != static_cast<uint32_t>(eventPhysical >> 32)) {
			IOLog("CHEESEBURGER_USB: event address readback failed\n");
			return false;
		}

		const uint32_t dcfg = read(kDcfg);
		write(kDcfg, dcfg & ~(3U | (0x7fU << 3)));
		write(kDalepena, 0);
		if (!configureEp0() || !armSetup()) {
			IOLog("CHEESEBURGER_USB: EP0 configuration failed\n");
			return false;
		}

		const uint32_t hs = read32(qscratch.address, kQscratchHsPhyCtrl);
		const uint32_t ss = read32(qscratch.address, kQscratchSsPhyCtrl);
		write32(qscratch.address, kQscratchHsPhyCtrl,
		    hs | (1U << 20) | (1U << 28));
		write32(qscratch.address, kQscratchSsPhyCtrl, ss | (1U << 24));
		write(kDevten, 0x7);
		write(kDctl, read(kDctl) | kDctlRunStop);
		if (!waitFor(kDsts, kDstsDevctrlhlt, 0, 500000)) {
			IOLog("CHEESEBURGER_USB: controller run timeout dsts=%08x\n",
			    read(kDsts));
			return false;
		}
		IOLog("CHEESEBURGER_USB: EP0 running dma=%llx low=%u dsts=%08x "
		    "qscratch_hs=%08x\n", dmaPhysical, lowDma ? 1U : 0U,
		    read(kDsts), read32(qscratch.address, kQscratchHsPhyCtrl));
		IOLog("CHEESEBURGER_USB: build=H408 cdc-acm boot=%08x\n", bootId);
		return true;
	}

	void
	run()
	{
		uint32_t summaryTicks = 0;
		while (!fault) {
			poll();
			transmit();
			if (++summaryTicks >= 60000) {
				summaryTicks = 0;
				IOLog("CHEESEBURGER_USB: alive configured=%u dtr=%u "
				    "events=%u setups=%u frames=%u tx=%llu rx=%llu "
				    "dropped=%llu dsts=%08x\n",
				    configured ? 1U : 0U, dtr ? 1U : 0U,
				    eventCount, setupCount, txFrames,
				    static_cast<unsigned long long>(txBytes),
				    static_cast<unsigned long long>(rxBytes),
				    static_cast<unsigned long long>(droppedBytes),
				    read(kDsts));
			}
			IOSleep(1);
		}
		IOLog("CHEESEBURGER_USB: EP0 fault event=%u setup=%u dsts=%08x "
		    "devten=%08x\n", eventCount, setupCount, read(kDsts),
		    read(kDevten));
	}
};

void
usbEp0Thread(void *, wait_result_t)
{
	uint32_t delaySeconds = 240;
	if (PE_parse_boot_argn("cheeseburger-usb-delay", &delaySeconds,
	    sizeof delaySeconds)) {
		delaySeconds = delaySeconds > 600 ? 600 : delaySeconds;
	}
	IOLog("CHEESEBURGER_USB: mode=ep0 scheduled delay=%u\n", delaySeconds);
	if (delaySeconds != 0) {
		IOSleep(delaySeconds * 1000U);
	}

	GateState gates{};
	probeGcc(gates);
	if (!gates.dwc3Readable) {
		IOLog("CHEESEBURGER_USB: mode2 refused; DWC3 not readable\n");
		thread_terminate(current_thread());
	}
	probeDwc3(gates);
	probeQusb2(gates);

	Ep0Device *device = new Ep0Device;
	if (device == nullptr || !device->begin()) {
		IOLog("CHEESEBURGER_USB: mode2 EP0 bring-up failed\n");
		delete device;
		thread_terminate(current_thread());
	}
	device->run();
	thread_terminate(current_thread());
}

void
usbProbeThread(void *, wait_result_t)
{
	uint32_t rounds = 6;
	uint32_t intervalSeconds = 60;
	if (PE_parse_boot_argn("cheeseburger-usb-rounds", &rounds, sizeof rounds)) {
		rounds = rounds == 0 ? 1 : (rounds > 12 ? 12 : rounds);
	}
	if (PE_parse_boot_argn("cheeseburger-usb-interval", &intervalSeconds,
	    sizeof intervalSeconds)) {
		intervalSeconds = intervalSeconds > 600 ? 600 : intervalSeconds;
	}

	for (uint32_t round = 0; round < rounds; round++) {
		if (round != 0 && intervalSeconds != 0) {
			IOSleep(intervalSeconds * 1000U);
		}
		IOLog("CHEESEBURGER_USB: build=port-r2 mode=probe round=%u/%u\n",
		    round + 1, rounds);
		GateState gates{};
		probeGcc(gates);
		probeDwc3(gates);
		probeQusb2(gates);
	}
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
	if (mode != 1 && mode != 2) {
		IOLog("CHEESEBURGER_USB: mode=%u unsupported (1=probe, 2=EP0)\n",
		    mode);
		return;
	}
	uint32_t expected = 0;
	if (!__atomic_compare_exchange_n(&attempted, &expected, 1, false,
	    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return;
	}

	IOLog("CHEESEBURGER_USB: mode=%u requested\n", mode);
	thread_t thread = nullptr;
	void (*worker)(void *, wait_result_t) =
	    mode == 1 ? usbProbeThread : usbEp0Thread;
	kern_return_t result = kernel_thread_start(worker, nullptr, &thread);
	if (result != KERN_SUCCESS) {
		IOLog("CHEESEBURGER_USB: thread start failed %x\n", result);
		return;
	}
	thread_deallocate(thread);
}
