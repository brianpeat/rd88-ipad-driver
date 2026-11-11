//
//  RD88MIDIDriver.cpp
//  USB MIDI for the Roland RD-88 (interface 3 of 0582:024a).
//
//  Interface 3, alt 0: bulk OUT 0x03 (to the keyboard), bulk IN 0x84 (from the keyboard).
//  Assumed wire format: standard 4-byte USB-MIDI event packets, as on other Roland
//  vendor-specific MIDI interfaces. The system side speaks Universal MIDI Packets.
//

#include <os/log.h>
#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSCollections.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/USBDriverKit.h>
#include <MIDIDriverKit/MIDIDriverKit.h>

#include "RD88MIDIDriver.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "RD88MIDIDriver: " fmt, ##__VA_ARGS__)

static const uint8_t  kMidiOutEndpoint = 0x03;
static const uint8_t  kMidiInEndpoint  = 0x84;
static const uint32_t kReadBytes       = 512;
static const uint32_t kWriteBytes      = 256;
static const uint32_t kWriteBuffers    = 8;
static const uint32_t kSysexMax        = 4096;

struct WriteBuffer
{
	IOBufferMemoryDescriptor* data;
	uint8_t*                  ptr;
	OSAction*                 action;
	volatile bool             busy;
};

struct RD88MIDIDriver_IVars
{
	IOUSBHostInterface* interface;
	IOUSBHostPipe*      inPipe;
	IOUSBHostPipe*      outPipe;

	OSSharedPtr<IOUserMIDIDevice>      device;
	OSSharedPtr<IOUserMIDIEntity>      entity;
	OSSharedPtr<IOUserMIDISource>      source;
	OSSharedPtr<IOUserMIDIDestination> destination;

	IOBufferMemoryDescriptor* readData;
	uint8_t*                  readPtr;
	OSAction*                 readAction;

	WriteBuffer write[kWriteBuffers];

	bool     stopping;
	bool     ioRunning;
	uint32_t logBudget;

	// System-exclusive from the keyboard, collected until its end byte.
	uint8_t  sysexIn[kSysexMax];
	uint32_t sysexInCount;
	bool     sysexInActive;

	// System-exclusive to the keyboard, packed three bytes per USB packet.
	uint8_t  sysexOut[3];
	uint32_t sysexOutCount;
};

// ---------------------------------------------------------------------------
// Keyboard -> system (USB-MIDI packets to Universal MIDI Packets)

static void SendSysex(RD88MIDIDriver_IVars* ivars)
{
	const uint32_t total = ivars->sysexInCount;
	uint32_t offset = 0;
	do
	{
		const uint32_t n = (total - offset > 6) ? 6 : (total - offset);
		const bool first = (offset == 0);
		const bool last  = (offset + n >= total);
		const uint32_t status = (first && last) ? 0 : first ? 1 : last ? 3 : 2;
		uint8_t b[6] = {};
		for (uint32_t i = 0; i < n; i++) b[i] = ivars->sysexIn[offset + i];
		IOUserMIDIUMPWord words[2];
		words[0] = 0x30000000u | (status << 20) | (n << 16) | ((uint32_t)b[0] << 8) | b[1];
		words[1] = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];
		ivars->source->Send(words, 2);
		offset += n;
	} while (offset < total);
}

static void SysexInByte(RD88MIDIDriver_IVars* ivars, uint8_t byte)
{
	if (byte == 0xF0) { ivars->sysexInActive = true; ivars->sysexInCount = 0; return; }
	if (!ivars->sysexInActive) return;
	if (byte == 0xF7) { SendSysex(ivars); ivars->sysexInActive = false; return; }
	if (byte & 0x80) { ivars->sysexInActive = false; return; }   // broken message; drop it
	if (ivars->sysexInCount < kSysexMax) ivars->sysexIn[ivars->sysexInCount++] = byte;
}

static void HandleUSBPacket(RD88MIDIDriver_IVars* ivars, const uint8_t* p)
{
	if (ivars->source.get() == nullptr) return;
	const uint8_t cin = p[0] & 0x0F;
	IOUserMIDIUMPWord word = 0;
	switch (cin)
	{
		case 0x8: case 0x9: case 0xA: case 0xB: case 0xE:   // 3-byte channel messages
			word = 0x20000000u | ((uint32_t)p[1] << 16) | ((uint32_t)(p[2] & 0x7F) << 8) | (p[3] & 0x7F);
			break;
		case 0xC: case 0xD:                                 // 2-byte channel messages
			word = 0x20000000u | ((uint32_t)p[1] << 16) | ((uint32_t)(p[2] & 0x7F) << 8);
			break;
		case 0xF:                                           // single byte (clock, start, stop...)
			if (p[1] < 0xF8) return;
			word = 0x10000000u | ((uint32_t)p[1] << 16);
			break;
		case 0x2:                                           // 2-byte system common
			word = 0x10000000u | ((uint32_t)p[1] << 16) | ((uint32_t)(p[2] & 0x7F) << 8);
			break;
		case 0x3:                                           // 3-byte system common
			word = 0x10000000u | ((uint32_t)p[1] << 16) | ((uint32_t)(p[2] & 0x7F) << 8) | (p[3] & 0x7F);
			break;
		case 0x4:                                           // sysex start / continue
			SysexInByte(ivars, p[1]); SysexInByte(ivars, p[2]); SysexInByte(ivars, p[3]);
			return;
		case 0x5:                                           // sysex end (1 byte) or 1-byte system common
			if (p[1] == 0xF7 || ivars->sysexInActive) { SysexInByte(ivars, p[1]); return; }
			if (p[1] < 0xF0) return;
			word = 0x10000000u | ((uint32_t)p[1] << 16);
			break;
		case 0x6:                                           // sysex end (2 bytes)
			SysexInByte(ivars, p[1]); SysexInByte(ivars, p[2]);
			return;
		case 0x7:                                           // sysex end (3 bytes)
			SysexInByte(ivars, p[1]); SysexInByte(ivars, p[2]); SysexInByte(ivars, p[3]);
			return;
		default:
			return;
	}
	ivars->source->Send(&word, 1);
}

// ---------------------------------------------------------------------------
// System -> keyboard (Universal MIDI Packets to USB-MIDI packets)

struct PacketWriter
{
	uint8_t  bytes[kWriteBytes];
	uint32_t count;
	void Add(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
	{
		if (count + 4 > kWriteBytes) return;
		bytes[count++] = cin; bytes[count++] = b1; bytes[count++] = b2; bytes[count++] = b3;
	}
};

static void SysexOutByte(RD88MIDIDriver_IVars* ivars, PacketWriter& w, uint8_t byte)
{
	ivars->sysexOut[ivars->sysexOutCount++] = byte;
	const uint32_t n = ivars->sysexOutCount;
	if (byte == 0xF7)
	{
		w.Add((uint8_t)(0x4 + n), ivars->sysexOut[0], n > 1 ? ivars->sysexOut[1] : 0, n > 2 ? ivars->sysexOut[2] : 0);
		ivars->sysexOutCount = 0;
	}
	else if (n == 3)
	{
		w.Add(0x4, ivars->sysexOut[0], ivars->sysexOut[1], ivars->sysexOut[2]);
		ivars->sysexOutCount = 0;
	}
}

static uint32_t WordsInPacket(uint32_t type)
{
	switch (type)
	{
		case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
		case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;
		case 0xB: case 0xC: return 3;
		default: return 4;
	}
}

static kern_return_t HandleUMP(RD88MIDIDriver_IVars* ivars, const IOUserMIDIUMPWord* words, size_t numWords)
{
	if (ivars->stopping || ivars->outPipe == nullptr) return kIOReturnSuccess;

	PacketWriter w;
	w.count = 0;

	size_t i = 0;
	while (i < numWords)
	{
		const uint32_t word = words[i];
		const uint32_t type = word >> 28;
		const uint32_t len = WordsInPacket(type);
		if (i + len > numWords) break;

		const uint8_t status = (uint8_t)(word >> 16);
		const uint8_t d1 = (uint8_t)((word >> 8) & 0x7F);
		const uint8_t d2 = (uint8_t)(word & 0x7F);

		if (type == 0x2)                    // MIDI 1.0 channel message
		{
			w.Add(status >> 4, status, d1, d2);
		}
		else if (type == 0x1)               // system common / real time
		{
			if (status >= 0xF8) w.Add(0xF, status, 0, 0);
			else if (status == 0xF1 || status == 0xF3) w.Add(0x2, status, d1, 0);
			else if (status == 0xF2) w.Add(0x3, status, d1, d2);
			else if (status == 0xF6) w.Add(0x5, status, 0, 0);
		}
		else if (type == 0x3)               // 7-bit system exclusive
		{
			const uint32_t sx = (word >> 20) & 0xF;
			uint32_t n = (word >> 16) & 0xF;
			if (n > 6) n = 6;
			const uint32_t second = words[i + 1];
			const uint8_t b[6] = { (uint8_t)(word >> 8), (uint8_t)word, (uint8_t)(second >> 24),
								   (uint8_t)(second >> 16), (uint8_t)(second >> 8), (uint8_t)second };
			if (sx == 0 || sx == 1) { ivars->sysexOutCount = 0; SysexOutByte(ivars, w, 0xF0); }
			for (uint32_t k = 0; k < n; k++) SysexOutByte(ivars, w, b[k] & 0x7F);
			if (sx == 0 || sx == 3) SysexOutByte(ivars, w, 0xF7);
		}
		i += len;
	}

	if (w.count == 0) return kIOReturnSuccess;

	for (uint32_t k = 0; k < kWriteBuffers; k++)
	{
		WriteBuffer& buffer = ivars->write[k];
		if (buffer.busy) continue;
		buffer.busy = true;
		memcpy(buffer.ptr, w.bytes, w.count);
		kern_return_t ret = ivars->outPipe->AsyncIO(buffer.data, w.count, buffer.action, 0);
		if (ret != kIOReturnSuccess) buffer.busy = false;
		return kIOReturnSuccess;
	}
	return kIOReturnSuccess;   // all buffers in flight: drop rather than block
}

// ---------------------------------------------------------------------------

bool RD88MIDIDriver::init()
{
	if (!super::init()) return false;
	ivars = IONewZero(RD88MIDIDriver_IVars, 1);
	return ivars != nullptr;
}

void RD88MIDIDriver::free()
{
	if (ivars != nullptr)
	{
		for (auto& b : ivars->write) { OSSafeReleaseNULL(b.action); OSSafeReleaseNULL(b.data); }
		OSSafeReleaseNULL(ivars->readAction);
		OSSafeReleaseNULL(ivars->readData);
		OSSafeReleaseNULL(ivars->inPipe);
		OSSafeReleaseNULL(ivars->outPipe);
		OSSafeReleaseNULL(ivars->interface);
		ivars->source.reset();
		ivars->destination.reset();
		ivars->entity.reset();
		ivars->device.reset();
	}
	IOSafeDeleteNULL(ivars, RD88MIDIDriver_IVars, 1);
	super::free();
}

kern_return_t IMPL(RD88MIDIDriver, Start)
{
	kern_return_t ret = Start(provider, SUPERDISPATCH);
	if (ret != kIOReturnSuccess) { LOG("super Start failed 0x%x", ret); return ret; }

	ivars->logBudget = 20;

	ivars->interface = OSDynamicCast(IOUSBHostInterface, provider);
	if (ivars->interface == nullptr) { LOG("provider is not a USB interface"); return kIOReturnNoDevice; }
	ivars->interface->retain();

	ret = ivars->interface->Open(this, 0, nullptr);
	if (ret != kIOReturnSuccess) { LOG("interface Open failed 0x%x", ret); return ret; }

	ret = ivars->interface->CopyPipe(kMidiInEndpoint, &ivars->inPipe);
	if (ret != kIOReturnSuccess) { LOG("CopyPipe (in) failed 0x%x", ret); return ret; }
	ret = ivars->interface->CopyPipe(kMidiOutEndpoint, &ivars->outPipe);
	if (ret != kIOReturnSuccess) { LOG("CopyPipe (out) failed 0x%x", ret); return ret; }

	IOAddressSegment seg = {};
	ret = ivars->interface->CreateIOBuffer(kIOMemoryDirectionIn, kReadBytes, &ivars->readData);
	if (ret == kIOReturnSuccess) ret = ivars->readData->GetAddressRange(&seg);
	if (ret == kIOReturnSuccess) ret = CreateActionReadComplete(0, &ivars->readAction);
	if (ret != kIOReturnSuccess) { LOG("read buffer setup failed 0x%x", ret); return ret; }
	ivars->readPtr = (uint8_t*)seg.address;

	for (uint32_t k = 0; k < kWriteBuffers; k++)
	{
		WriteBuffer& buffer = ivars->write[k];
		// InOut so the driver can fill it (an Out-only buffer is read-only here).
		ret = ivars->interface->CreateIOBuffer(kIOMemoryDirectionInOut, kWriteBytes, &buffer.data);
		if (ret == kIOReturnSuccess) ret = buffer.data->GetAddressRange(&seg);
		if (ret == kIOReturnSuccess) ret = CreateActionWriteComplete(sizeof(uint32_t), &buffer.action);
		if (ret != kIOReturnSuccess) { LOG("write buffer setup failed 0x%x", ret); return ret; }
		buffer.ptr = (uint8_t*)seg.address;
		buffer.busy = false;
		*(uint32_t*)buffer.action->GetReference() = k;
	}

	auto deviceUID    = OSSharedPtr(OSString::withCString("RD88-USB-MIDI"), OSNoRetain);
	auto modelUID     = OSSharedPtr(OSString::withCString("RD-88"), OSNoRetain);
	auto manufacturer = OSSharedPtr(OSString::withCString("Roland"), OSNoRetain);
	auto name         = OSSharedPtr(OSString::withCString("RD-88"), OSNoRetain);

	ivars->device = IOUserMIDIDevice::Create(this, deviceUID.get(), modelUID.get(), manufacturer.get());
	if (ivars->device.get() == nullptr) { LOG("MIDI device Create failed"); return kIOReturnNoMemory; }
	ivars->device->SetName(name.get());

	ivars->entity = IOUserMIDIEntity::Create(this, ivars->device.get(), name.get(),
											 IOUserMIDIProtocolID::MIDIProtocol_1_0, 1, 1);
	if (ivars->entity.get() == nullptr) { LOG("MIDI entity Create failed"); return kIOReturnNoMemory; }

	ivars->source = ivars->entity->GetSource(0);
	ivars->destination = ivars->entity->GetDestination(0);
	if (ivars->source.get() == nullptr || ivars->destination.get() == nullptr)
	{
		LOG("entity has no source/destination");
		return kIOReturnNotFound;
	}

	ret = ivars->device->AddEntity(ivars->entity.get());
	if (ret != kIOReturnSuccess) LOG("AddEntity returned 0x%x (continuing)", ret);

	// Plain names, so apps list the port as "RD-88" instead of "RD-88 RD-88 Source 0",
	// and fixed IDs, so the keyboard looks like the same device after every replug.
	ivars->source->SetName(name.get());
	ivars->destination->SetName(name.get());
	ivars->source->SetProperty(IOUserMIDIProperty::DisplayName, name.get());
	ivars->destination->SetProperty(IOUserMIDIProperty::DisplayName, name.get());
	auto deviceID = OSSharedPtr(OSNumber::withNumber((uint64_t)0x52443830, 32), OSNoRetain);
	auto entityID = OSSharedPtr(OSNumber::withNumber((uint64_t)0x52443831, 32), OSNoRetain);
	auto sourceID = OSSharedPtr(OSNumber::withNumber((uint64_t)0x52443832, 32), OSNoRetain);
	auto destID   = OSSharedPtr(OSNumber::withNumber((uint64_t)0x52443833, 32), OSNoRetain);
	ivars->device->SetProperty(IOUserMIDIProperty::UniqueID, deviceID.get());
	ivars->entity->SetProperty(IOUserMIDIProperty::UniqueID, entityID.get());
	ivars->source->SetProperty(IOUserMIDIProperty::UniqueID, sourceID.get());
	ivars->destination->SetProperty(IOUserMIDIProperty::UniqueID, destID.get());

	// Offline until the system starts MIDI IO on the device; StartIO clears it.
	auto offline = OSSharedPtr(OSNumber::withNumber((uint64_t)1, 32), OSNoRetain);
	ivars->device->SetProperty(IOUserMIDIProperty::Offline, offline.get());

	ret = AddObject(ivars->device.get());
	if (ret != kIOReturnSuccess) { LOG("AddObject failed 0x%x", ret); return ret; }

	RD88MIDIDriver_IVars* state = ivars;
	ivars->destination->SetIOBlock(^kern_return_t(IOUserMIDIUMPWord const* umpWords, size_t numWords) {
		return HandleUMP(state, umpWords, numWords);
	});


	ret = RegisterService();
	if (ret != kIOReturnSuccess) { LOG("RegisterService failed 0x%x", ret); return ret; }

	ret = ivars->inPipe->AsyncIO(ivars->readData, kReadBytes, ivars->readAction, 0);
	if (ret != kIOReturnSuccess) LOG("first read failed 0x%x", ret);

	LOG("started");
	return kIOReturnSuccess;
}

kern_return_t IMPL(RD88MIDIDriver, Stop)
{
	LOG("stopping");
	ivars->stopping = true;
	if (ivars->inPipe != nullptr) ivars->inPipe->Abort(0, kIOReturnAborted, this);
	if (ivars->outPipe != nullptr) ivars->outPipe->Abort(0, kIOReturnAborted, this);
	if (ivars->device.get() != nullptr)
	{
		if (ivars->ioRunning) { ivars->ioRunning = false; ivars->device->StopIO(); }
		RemoveObject(ivars->device.get());
	}
	// Each action holds a reference to this driver; release them so the driver can be freed.
	OSSafeReleaseNULL(ivars->readAction);
	for (auto& b : ivars->write) OSSafeReleaseNULL(b.action);
	OSSafeReleaseNULL(ivars->inPipe);
	OSSafeReleaseNULL(ivars->outPipe);
	if (ivars->interface != nullptr) ivars->interface->Close(this, 0);
	kern_return_t ret = Stop(provider, SUPERDISPATCH);
	// The MIDI objects hold a reference back to this driver. Drop them here, not in
	// free(), or the driver is never freed and its process outlives the unplug.
	ivars->source.reset();
	ivars->destination.reset();
	ivars->entity.reset();
	ivars->device.reset();
	return ret;
}

kern_return_t RD88MIDIDriver::StartIO(OSArray* deviceList)
{
	kern_return_t ret = super::StartIO(deviceList);
	if (ret != kIOReturnSuccess) { LOG("super StartIO failed 0x%x", ret); return ret; }
	if (ivars->device.get() == nullptr) return kIOReturnSuccess;

	ret = ivars->device->StartIO();
	if (ret != kIOReturnSuccess) { LOG("device StartIO failed 0x%x", ret); return ret; }
	auto online = OSSharedPtr(OSNumber::withNumber((uint64_t)0, 32), OSNoRetain);
	ivars->device->SetProperty(IOUserMIDIProperty::Offline, online.get());
	ivars->ioRunning = true;
	LOG("MIDI IO started");
	return kIOReturnSuccess;
}

kern_return_t RD88MIDIDriver::StopIO()
{
	if (ivars->device.get() != nullptr && ivars->ioRunning)
	{
		ivars->ioRunning = false;
		auto offline = OSSharedPtr(OSNumber::withNumber((uint64_t)1, 32), OSNoRetain);
		ivars->device->SetProperty(IOUserMIDIProperty::Offline, offline.get());
		ivars->device->StopIO();
	}
	return super::StopIO();
}

void IMPL(RD88MIDIDriver, ReadComplete)
{
	if (ivars->stopping) return;

	if (status == kIOReturnSuccess)
	{
		const uint32_t count = (actualByteCount > kReadBytes) ? kReadBytes : actualByteCount;
		for (uint32_t offset = 0; offset + 4 <= count; offset += 4)
			HandleUSBPacket(ivars, ivars->readPtr + offset);
	}
	else if (status == kIOReturnAborted || status == kIOReturnNotResponding || status == kIOReturnNoDevice)
	{
		return;   // unplugged or shutting down: stop reading
	}
	else if (ivars->logBudget > 0)
	{
		ivars->logBudget--;
		LOG("read status 0x%x", status);
		if (ivars->logBudget == 0) return;   // persistent error: give up rather than spin
		ivars->inPipe->ClearStall(true);
	}

	kern_return_t ret = ivars->inPipe->AsyncIO(ivars->readData, kReadBytes, ivars->readAction, 0);
	if (ret != kIOReturnSuccess && ivars->logBudget > 0) { ivars->logBudget--; LOG("read resubmit failed 0x%x", ret); }
}

void IMPL(RD88MIDIDriver, WriteComplete)
{
	const uint32_t index = *(uint32_t*)action->GetReference();
	if (index < kWriteBuffers) ivars->write[index].busy = false;
	if (status != kIOReturnSuccess && status != kIOReturnAborted && ivars->logBudget > 0)
	{
		ivars->logBudget--;
		LOG("write status 0x%x", status);
	}
}
