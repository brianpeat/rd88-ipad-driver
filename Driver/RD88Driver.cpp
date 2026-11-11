//
//  RD88Driver.cpp
//  USB audio driver for the Roland RD-88 (vendor-specific audio, 0582:024a).
//
//  What the keyboard exposes at high speed (read from its descriptors):
//    interface 1, alt 1: isochronous OUT 0x0D  - audio to the keyboard
//    interface 2, alt 1: isochronous IN  0x8E  - audio from the keyboard
//    interface 3:        bulk 0x03 / 0x84      - MIDI (not handled here yet)
//  Both audio streams: 2 channels, 32-bit, 48 kHz, one packet per 125 us microframe.
//  The IN stream is the clock: we send back exactly as many frames as we receive.
//

#include <os/log.h>
#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSCollections.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/USBDriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>

#include "RD88Driver.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "RD88Driver: " fmt, ##__VA_ARGS__)

// ---- Things most likely to need changing during bring-up -------------------

// Sample layout on the wire. The descriptor says format tag 3 (IEEE float) and
// Roland's Mac driver copies samples straight through, so float is the best guess.
// If audio comes out as loud noise, set this to 0 to try 32-bit signed integer.
#define RD88_WIRE_IS_FLOAT 1

// A forum report says iPadOS hides AudioDriverKit devices that say "USB" here,
// and that "Thunderbolt" works. If the device never shows up, try the other one.
static const IOUserAudioTransportType kTransportType = IOUserAudioTransportType::Thunderbolt;

// How many milliseconds ahead of the matching input we schedule output packets.
// Lower = less latency, but too low and packets miss their slot.
static const uint32_t kOutDelayFrames = 6;

// ---- Fixed properties of the device ---------------------------------------

static const double   kSampleRate        = 48000.0;
static const uint32_t kChannels          = 2;
static const uint32_t kBytesPerFrame     = 8;      // 2 channels x 4 bytes
static const uint8_t  kOutInterface      = 1;
static const uint8_t  kInInterface       = 2;
static const uint8_t  kOutEndpoint       = 0x0D;
static const uint8_t  kInEndpoint        = 0x8E;
static const uint8_t  kStreamingAlt      = 1;
static const uint32_t kMaxPacketBytes    = 60;
static const uint32_t kNominalPktFrames  = 6;      // 48000 / 8000 microframes
static const uint32_t kPacketsPerRequest = 8;      // one USB frame (1 ms)
static const uint32_t kInRequests        = 4;      // kept queued ahead
static const uint32_t kOutRequests       = 16;
static const uint32_t kRingFrames        = 19200;  // 400 ms; also the zero-timestamp period
static const uint32_t kSafetyOffset      = 144;    // same value Roland's Mac driver reports

struct IsoRequest
{
	IOBufferMemoryDescriptor* data;
	IOBufferMemoryDescriptor* frameList;
	uint8_t*                  dataPtr;
	IOUSBIsochronousFrame*    frames;
	OSAction*                 action;
	uint64_t                  frameNumber;
	bool                      busy;
};

struct RD88Driver_IVars
{
	IOUSBHostDevice*    usbDevice;
	IOUSBHostInterface* outInterface;
	IOUSBHostInterface* inInterface;
	IOUSBHostPipe*      outPipe;
	IOUSBHostPipe*      inPipe;

	OSSharedPtr<IOUserAudioDevice> audioDevice;
	OSSharedPtr<IOUserAudioStream> outStream;
	OSSharedPtr<IOUserAudioStream> inStream;
	IOBufferMemoryDescriptor* outRing;
	IOBufferMemoryDescriptor* inRing;
	uint8_t* outRingPtr;
	uint8_t* inRingPtr;

	IsoRequest inReq[kInRequests];
	IsoRequest outReq[kOutRequests];
	uint32_t   nextOutReq;
	uint64_t   nextInFrame;

	bool     running;
	uint64_t sampleTime;      // frames received from the keyboard since start
	uint64_t ticksPerMs;      // mach ticks in one USB frame
	uint32_t logBudget;       // limits repeated error logging
};

// ---------------------------------------------------------------------------

static uint32_t* ActionIndex(OSAction* action)
{
	return (uint32_t*)action->GetReference();
}

static void FreeRequest(IsoRequest& r)
{
	OSSafeReleaseNULL(r.action);
	OSSafeReleaseNULL(r.data);
	OSSafeReleaseNULL(r.frameList);
	r.dataPtr = nullptr;
	r.frames = nullptr;
}

static kern_return_t AllocRequest(IsoRequest& r, IOUSBHostInterface* interface, uint64_t direction)
{
	kern_return_t ret = interface->CreateIOBuffer(direction, kPacketsPerRequest * kMaxPacketBytes, &r.data);
	if (ret != kIOReturnSuccess) return ret;
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
										   kPacketsPerRequest * sizeof(IOUSBIsochronousFrame), 0, &r.frameList);
	if (ret != kIOReturnSuccess) return ret;

	IOAddressSegment seg = {};
	ret = r.data->GetAddressRange(&seg);
	if (ret != kIOReturnSuccess) return ret;
	r.dataPtr = (uint8_t*)seg.address;
	ret = r.frameList->GetAddressRange(&seg);
	if (ret != kIOReturnSuccess) return ret;
	r.frames = (IOUSBIsochronousFrame*)seg.address;
	r.busy = false;
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------

bool RD88Driver::init()
{
	if (!super::init()) return false;
	ivars = IONewZero(RD88Driver_IVars, 1);
	return ivars != nullptr;
}

void RD88Driver::free()
{
	if (ivars != nullptr)
	{
		for (auto& r : ivars->inReq) FreeRequest(r);
		for (auto& r : ivars->outReq) FreeRequest(r);
		OSSafeReleaseNULL(ivars->outPipe);
		OSSafeReleaseNULL(ivars->inPipe);
		OSSafeReleaseNULL(ivars->outInterface);
		OSSafeReleaseNULL(ivars->inInterface);
		OSSafeReleaseNULL(ivars->usbDevice);
		OSSafeReleaseNULL(ivars->outRing);
		OSSafeReleaseNULL(ivars->inRing);
		ivars->audioDevice.reset();
		ivars->outStream.reset();
		ivars->inStream.reset();
	}
	IOSafeDeleteNULL(ivars, RD88Driver_IVars, 1);
	super::free();
}

kern_return_t IMPL(RD88Driver, Start)
{
	kern_return_t ret = Start(provider, SUPERDISPATCH);
	if (ret != kIOReturnSuccess) { LOG("super Start failed 0x%x", ret); return ret; }

	mach_timebase_info_data_t tb = {};
	mach_timebase_info(&tb);
	ivars->ticksPerMs = (1000000ull * tb.denom) / tb.numer;

	// ---- USB side ----
	ivars->usbDevice = OSDynamicCast(IOUSBHostDevice, provider);
	if (ivars->usbDevice == nullptr) { LOG("provider is not a USB device"); return kIOReturnNoDevice; }
	ivars->usbDevice->retain();

	ret = ivars->usbDevice->Open(this, 0, 0);
	if (ret != kIOReturnSuccess) { LOG("device Open failed 0x%x", ret); return ret; }

	uint8_t speed = 0;
	ivars->usbDevice->GetSpeed(&speed);
	LOG("device opened, speed code %u", speed);

	// true: publish the interfaces so the MIDI driver can attach to interface 3.
	ret = ivars->usbDevice->SetConfiguration(1, true);
	if (ret != kIOReturnSuccess) { LOG("SetConfiguration failed 0x%x", ret); return ret; }

	uintptr_t iterator = 0;
	ret = ivars->usbDevice->CreateInterfaceIterator(&iterator);
	if (ret != kIOReturnSuccess) { LOG("CreateInterfaceIterator failed 0x%x", ret); return ret; }
	for (;;)
	{
		IOUSBHostInterface* interface = nullptr;
		if (ivars->usbDevice->CopyInterface(iterator, &interface) != kIOReturnSuccess || interface == nullptr) break;

		uint8_t number = 0xFF;
		const IOUSBConfigurationDescriptor* config = interface->CopyConfigurationDescriptor();
		if (config != nullptr)
		{
			const IOUSBInterfaceDescriptor* desc = interface->GetInterfaceDescriptor(config);
			if (desc != nullptr) number = desc->bInterfaceNumber;
			IOUSBHostFreeDescriptor(config);
		}

		if (number == kOutInterface && ivars->outInterface == nullptr) ivars->outInterface = interface;
		else if (number == kInInterface && ivars->inInterface == nullptr) ivars->inInterface = interface;
		else interface->release();
	}
	ivars->usbDevice->DestroyInterfaceIterator(iterator);

	if (ivars->outInterface == nullptr || ivars->inInterface == nullptr)
	{
		LOG("audio interfaces not found (out %d, in %d) - is the keyboard at high speed and in VENDOR mode?",
			ivars->outInterface != nullptr, ivars->inInterface != nullptr);
		return kIOReturnNotFound;
	}

	ret = ivars->outInterface->Open(this, 0, nullptr);
	if (ret != kIOReturnSuccess) { LOG("out interface Open failed 0x%x", ret); return ret; }
	ret = ivars->inInterface->Open(this, 0, nullptr);
	if (ret != kIOReturnSuccess) { LOG("in interface Open failed 0x%x", ret); return ret; }

	for (uint32_t i = 0; i < kInRequests; i++)
	{
		ret = AllocRequest(ivars->inReq[i], ivars->inInterface, kIOMemoryDirectionIn);
		if (ret == kIOReturnSuccess) ret = CreateActionInComplete(sizeof(uint32_t), &ivars->inReq[i].action);
		if (ret != kIOReturnSuccess) { LOG("in request alloc failed 0x%x", ret); return ret; }
		*ActionIndex(ivars->inReq[i].action) = i;
	}
	for (uint32_t i = 0; i < kOutRequests; i++)
	{
		// InOut, not Out: an Out-only buffer is mapped read-only in the driver, and we fill it.
		ret = AllocRequest(ivars->outReq[i], ivars->outInterface, kIOMemoryDirectionInOut);
		if (ret == kIOReturnSuccess) ret = CreateActionOutComplete(sizeof(uint32_t), &ivars->outReq[i].action);
		if (ret != kIOReturnSuccess) { LOG("out request alloc failed 0x%x", ret); return ret; }
		*ActionIndex(ivars->outReq[i].action) = i;
	}

	// ---- Audio side ----
	auto deviceUID    = OSSharedPtr(OSString::withCString("RD88-USB-Audio"), OSNoRetain);
	auto modelUID     = OSSharedPtr(OSString::withCString("RD-88"), OSNoRetain);
	auto manufacturer = OSSharedPtr(OSString::withCString("Roland"), OSNoRetain);
	auto deviceName   = OSSharedPtr(OSString::withCString("RD-88"), OSNoRetain);
	auto outName      = OSSharedPtr(OSString::withCString("RD-88 Output"), OSNoRetain);
	auto inName       = OSSharedPtr(OSString::withCString("RD-88 Input"), OSNoRetain);

	ivars->audioDevice = IOUserAudioDevice::Create(this, false, deviceUID.get(), modelUID.get(),
												   manufacturer.get(), kRingFrames);
	if (ivars->audioDevice.get() == nullptr) { LOG("audio device Create failed"); return kIOReturnNoMemory; }
	ivars->audioDevice->SetName(deviceName.get());
	ivars->audioDevice->SetTransportType(kTransportType);

	double rate = kSampleRate;
	ivars->audioDevice->SetAvailableSampleRates(&rate, 1);
	ivars->audioDevice->SetSampleRate(kSampleRate);

	const uint64_t ringBytes = (uint64_t)kRingFrames * kBytesPerFrame;
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, ringBytes, 0, &ivars->outRing);
	if (ret == kIOReturnSuccess) ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, ringBytes, 0, &ivars->inRing);
	if (ret != kIOReturnSuccess) { LOG("ring alloc failed 0x%x", ret); return ret; }
	IOAddressSegment seg = {};
	ivars->outRing->GetAddressRange(&seg);
	ivars->outRingPtr = (uint8_t*)seg.address;
	ivars->inRing->GetAddressRange(&seg);
	ivars->inRingPtr = (uint8_t*)seg.address;
	memset(ivars->outRingPtr, 0, ringBytes);
	memset(ivars->inRingPtr, 0, ringBytes);

	IOUserAudioStreamBasicDescription format = {};
	format.mSampleRate       = kSampleRate;
	format.mFormatID         = IOUserAudioFormatID::LinearPCM;
#if RD88_WIRE_IS_FLOAT
	format.mFormatFlags      = (IOUserAudioFormatFlags)(IOUserAudioFormatFlags::FormatFlagIsFloat |
														 IOUserAudioFormatFlags::FormatFlagIsPacked);
#else
	format.mFormatFlags      = (IOUserAudioFormatFlags)(IOUserAudioFormatFlags::FormatFlagIsSignedInteger |
														 IOUserAudioFormatFlags::FormatFlagIsPacked);
#endif
	format.mBytesPerPacket   = kBytesPerFrame;
	format.mFramesPerPacket  = 1;
	format.mBytesPerFrame    = kBytesPerFrame;
	format.mChannelsPerFrame = kChannels;
	format.mBitsPerChannel   = 32;

	ivars->outStream = IOUserAudioStream::Create(this, IOUserAudioStreamDirection::Output, ivars->outRing);
	ivars->inStream  = IOUserAudioStream::Create(this, IOUserAudioStreamDirection::Input, ivars->inRing);
	if (ivars->outStream.get() == nullptr || ivars->inStream.get() == nullptr)
	{
		LOG("stream Create failed");
		return kIOReturnNoMemory;
	}
	ivars->outStream->SetName(outName.get());
	ivars->outStream->SetAvailableStreamFormats(&format, 1);
	ivars->outStream->SetCurrentStreamFormat(&format);
	ivars->outStream->SetTerminalType(IOUserAudioStreamTerminalType::Line);
	ivars->inStream->SetName(inName.get());
	ivars->inStream->SetAvailableStreamFormats(&format, 1);
	ivars->inStream->SetCurrentStreamFormat(&format);
	ivars->inStream->SetTerminalType(IOUserAudioStreamTerminalType::Line);

	ret = ivars->audioDevice->AddStream(ivars->outStream.get());
	if (ret == kIOReturnSuccess) ret = ivars->audioDevice->AddStream(ivars->inStream.get());
	if (ret != kIOReturnSuccess) { LOG("AddStream failed 0x%x", ret); return ret; }

	ivars->audioDevice->SetCanBeDefaultOutputDevice(true);
	ivars->audioDevice->SetCanBeDefaultInputDevice(true);
	ivars->audioDevice->SetCanBeDefaultSystemOutputDevice(true);
	ivars->audioDevice->SetInputSafetyOffset(kSafetyOffset);
	ivars->audioDevice->SetOutputSafetyOffset(kSafetyOffset);
	ivars->audioDevice->SetOutputLatency(kOutDelayFrames * 48);
	ivars->audioDevice->SetInputLatency(kNominalPktFrames * kPacketsPerRequest);
	ivars->audioDevice->SetClockAlgorithm(IOUserAudioClockAlgorithm::TwelvePtMovingWindowAverage);

	// The system reads/writes the ring buffers itself; nothing to do per IO cycle.
	ivars->audioDevice->SetIOOperationHandler(
		^kern_return_t(IOUserAudioObjectID, IOUserAudioIOOperation, uint32_t, uint64_t, uint64_t) {
			return kIOReturnSuccess;
		});

	ret = AddObject(ivars->audioDevice.get());
	if (ret != kIOReturnSuccess) { LOG("AddObject failed 0x%x", ret); return ret; }

	ret = RegisterService();
	if (ret != kIOReturnSuccess) { LOG("RegisterService failed 0x%x", ret); return ret; }

	LOG("started");
	return kIOReturnSuccess;
}

kern_return_t IMPL(RD88Driver, Stop)
{
	LOG("stopping");
	ivars->running = false;
	if (ivars->inPipe != nullptr) ivars->inPipe->Abort(0, kIOReturnAborted, this);
	if (ivars->outPipe != nullptr) ivars->outPipe->Abort(0, kIOReturnAborted, this);
	if (ivars->audioDevice.get() != nullptr) RemoveObject(ivars->audioDevice.get());
	// Each action holds a reference to this driver. Unless they are released here the
	// driver is never freed and its process stays behind after every unplug.
	for (auto& r : ivars->inReq) OSSafeReleaseNULL(r.action);
	for (auto& r : ivars->outReq) OSSafeReleaseNULL(r.action);
	OSSafeReleaseNULL(ivars->inPipe);
	OSSafeReleaseNULL(ivars->outPipe);
	if (ivars->outInterface != nullptr) ivars->outInterface->Close(this, 0);
	if (ivars->inInterface != nullptr) ivars->inInterface->Close(this, 0);
	if (ivars->usbDevice != nullptr) ivars->usbDevice->Close(this, 0);
	kern_return_t ret = Stop(provider, SUPERDISPATCH);
	// The audio objects hold a reference back to this driver. Drop them here, not in
	// free(), or the driver is never freed and its process outlives the unplug.
	ivars->outStream.reset();
	ivars->inStream.reset();
	ivars->audioDevice.reset();
	return ret;
}

// ---------------------------------------------------------------------------
// Starting and stopping the audio stream

static kern_return_t SubmitIn(RD88Driver_IVars* ivars, IsoRequest& r, uint64_t frameNumber)
{
	for (uint32_t j = 0; j < kPacketsPerRequest; j++)
	{
		r.frames[j].status        = kIOReturnInvalid;
		r.frames[j].requestCount  = kMaxPacketBytes;
		r.frames[j].completeCount = 0;
		r.frames[j].timeStamp     = 0;
	}
	r.frameNumber = frameNumber;
	r.busy = true;
	kern_return_t ret = ivars->inPipe->IsochIO(r.data, r.frameList, frameNumber, r.action);
	if (ret != kIOReturnSuccess) r.busy = false;
	return ret;
}

kern_return_t RD88Driver::StartDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (ivars->audioDevice.get() == nullptr || in_object_id != ivars->audioDevice->GetObjectID())
		return kIOReturnBadArgument;

	LOG("StartDevice");

	kern_return_t ret = ivars->inInterface->SelectAlternateSetting(kStreamingAlt);
	if (ret != kIOReturnSuccess) { LOG("select alt (in) failed 0x%x", ret); return ret; }
	ret = ivars->outInterface->SelectAlternateSetting(kStreamingAlt);
	if (ret != kIOReturnSuccess) { LOG("select alt (out) failed 0x%x", ret); return ret; }

	OSSafeReleaseNULL(ivars->inPipe);
	OSSafeReleaseNULL(ivars->outPipe);
	ret = ivars->inInterface->CopyPipe(kInEndpoint, &ivars->inPipe);
	if (ret != kIOReturnSuccess) { LOG("CopyPipe (in) failed 0x%x", ret); return ret; }
	ret = ivars->outInterface->CopyPipe(kOutEndpoint, &ivars->outPipe);
	if (ret != kIOReturnSuccess) { LOG("CopyPipe (out) failed 0x%x", ret); return ret; }

	memset(ivars->outRingPtr, 0, (size_t)kRingFrames * kBytesPerFrame);
	memset(ivars->inRingPtr, 0, (size_t)kRingFrames * kBytesPerFrame);
	ivars->sampleTime = 0;
	ivars->nextOutReq = 0;
	ivars->logBudget = 20;
	for (auto& r : ivars->outReq) r.busy = false;

	uint64_t frameNow = 0, timeNow = 0;
	ret = ivars->inInterface->GetFrameNumber(&frameNow, &timeNow);
	if (ret != kIOReturnSuccess) { LOG("GetFrameNumber failed 0x%x", ret); return ret; }

	// Sample 0 is the first sample of the first packet we ask for.
	const uint64_t firstFrame = frameNow + 8;
	ivars->audioDevice->UpdateCurrentZeroTimestamp(0, timeNow + 8 * ivars->ticksPerMs);

	ivars->running = true;
	ivars->nextInFrame = firstFrame;
	for (uint32_t i = 0; i < kInRequests; i++)
	{
		ret = SubmitIn(ivars, ivars->inReq[i], ivars->nextInFrame);
		if (ret != kIOReturnSuccess)
		{
			LOG("initial IsochIO (in) failed 0x%x", ret);
			ivars->running = false;
			ivars->inPipe->Abort(0, kIOReturnAborted, this);
			return ret;
		}
		ivars->nextInFrame++;
	}

	ret = super::StartDevice(in_object_id, in_flags);
	if (ret != kIOReturnSuccess)
	{
		LOG("super StartDevice failed 0x%x", ret);
		ivars->running = false;
		ivars->inPipe->Abort(0, kIOReturnAborted, this);
	}
	return ret;
}

kern_return_t RD88Driver::StopDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (ivars->audioDevice.get() == nullptr || in_object_id != ivars->audioDevice->GetObjectID())
		return kIOReturnBadArgument;

	LOG("StopDevice");
	ivars->running = false;
	if (ivars->inPipe != nullptr) ivars->inPipe->Abort(0, kIOReturnAborted, this);
	if (ivars->outPipe != nullptr) ivars->outPipe->Abort(0, kIOReturnAborted, this);
	OSSafeReleaseNULL(ivars->inPipe);
	OSSafeReleaseNULL(ivars->outPipe);
	ivars->inInterface->SelectAlternateSetting(0);
	ivars->outInterface->SelectAlternateSetting(0);

	return super::StopDevice(in_object_id, in_flags);
}

// ---------------------------------------------------------------------------
// Streaming

static void CopyToRing(uint8_t* ring, uint64_t sampleTime, const uint8_t* src, uint32_t frames)
{
	uint32_t pos = (uint32_t)(sampleTime % kRingFrames);
	uint32_t first = (frames < kRingFrames - pos) ? frames : (kRingFrames - pos);
	memcpy(ring + (size_t)pos * kBytesPerFrame, src, (size_t)first * kBytesPerFrame);
	if (first < frames) memcpy(ring, src + (size_t)first * kBytesPerFrame, (size_t)(frames - first) * kBytesPerFrame);
}

static void CopyFromRing(const uint8_t* ring, uint64_t sampleTime, uint8_t* dst, uint32_t frames)
{
	uint32_t pos = (uint32_t)(sampleTime % kRingFrames);
	uint32_t first = (frames < kRingFrames - pos) ? frames : (kRingFrames - pos);
	memcpy(dst, ring + (size_t)pos * kBytesPerFrame, (size_t)first * kBytesPerFrame);
	if (first < frames) memcpy(dst + (size_t)first * kBytesPerFrame, ring, (size_t)(frames - first) * kBytesPerFrame);
}

void IMPL(RD88Driver, InComplete)
{
	const uint32_t index = *ActionIndex(action);
	if (index >= kInRequests) return;
	IsoRequest& in = ivars->inReq[index];
	in.busy = false;
	if (!ivars->running || ivars->audioDevice.get() == nullptr) return;

	if (status != kIOReturnSuccess && ivars->logBudget > 0)
	{
		ivars->logBudget--;
		LOG("in request status 0x%x", status);
	}

	// Host time of the start of this request's USB frame.
	uint64_t frameNow = 0, timeNow = 0;
	ivars->inInterface->GetFrameNumber(&frameNow, &timeNow);
	const uint64_t frameStart = timeNow - (frameNow - in.frameNumber) * ivars->ticksPerMs;
	const uint64_t ticksPerPacket = ivars->ticksPerMs / kPacketsPerRequest;

	// Matching output request: same packet sizes, sent kOutDelayFrames later.
	IsoRequest& out = ivars->outReq[ivars->nextOutReq];
	const bool sendOut = !out.busy && ivars->outPipe != nullptr;
	ivars->nextOutReq = (ivars->nextOutReq + 1) % kOutRequests;
	uint32_t outOffset = 0;

	for (uint32_t j = 0; j < kPacketsPerRequest; j++)
	{
		const uint8_t* packet = in.dataPtr + j * kMaxPacketBytes;
		uint32_t frames = in.frames[j].completeCount / kBytesPerFrame;
		static const uint8_t silence[kMaxPacketBytes] = {};
		if (in.frames[j].status != kIOReturnSuccess || frames == 0 || frames > kMaxPacketBytes / kBytesPerFrame)
		{
			// Keep the timeline moving on a dropped packet.
			packet = silence;
			frames = kNominalPktFrames;
		}

		CopyToRing(ivars->inRingPtr, ivars->sampleTime, packet, frames);

		if (sendOut)
		{
			CopyFromRing(ivars->outRingPtr, ivars->sampleTime, out.dataPtr + outOffset, frames);
			out.frames[j].status        = kIOReturnInvalid;
			out.frames[j].requestCount  = frames * kBytesPerFrame;
			out.frames[j].completeCount = 0;
			out.frames[j].timeStamp     = 0;
			outOffset += frames * kBytesPerFrame;
		}

		// Tell the system each time the sample counter wraps the ring.
		const uint64_t before = ivars->sampleTime / kRingFrames;
		const uint64_t after  = (ivars->sampleTime + frames) / kRingFrames;
		if (after != before)
		{
			const uint64_t boundary = after * kRingFrames;
			const uint64_t into = boundary - ivars->sampleTime;
			const uint64_t when = frameStart + j * ticksPerPacket + (into * ticksPerPacket) / frames;
			ivars->audioDevice->UpdateCurrentZeroTimestamp(boundary, when);
		}
		ivars->sampleTime += frames;
	}

	if (sendOut)
	{
		out.frameNumber = in.frameNumber + kOutDelayFrames;
		out.busy = true;
		kern_return_t ret = ivars->outPipe->IsochIO(out.data, out.frameList, out.frameNumber, out.action);
		if (ret != kIOReturnSuccess)
		{
			out.busy = false;
			if (ivars->logBudget > 0) { ivars->logBudget--; LOG("IsochIO (out) failed 0x%x", ret); }
		}
	}

	// Queue this request again for the next free frame.
	kern_return_t ret = SubmitIn(ivars, in, ivars->nextInFrame);
	if (ret != kIOReturnSuccess)
	{
		// Most likely we fell behind; jump ahead of the current frame and retry.
		ivars->nextInFrame = frameNow + 4;
		ret = SubmitIn(ivars, in, ivars->nextInFrame);
		if (ret != kIOReturnSuccess && ivars->logBudget > 0) { ivars->logBudget--; LOG("IsochIO (in) failed 0x%x", ret); }
	}
	ivars->nextInFrame++;
}

void IMPL(RD88Driver, OutComplete)
{
	const uint32_t index = *ActionIndex(action);
	if (index >= kOutRequests) return;
	ivars->outReq[index].busy = false;
	if (status != kIOReturnSuccess && status != kIOReturnAborted && ivars->running && ivars->logBudget > 0)
	{
		ivars->logBudget--;
		LOG("out request status 0x%x", status);
	}
}
