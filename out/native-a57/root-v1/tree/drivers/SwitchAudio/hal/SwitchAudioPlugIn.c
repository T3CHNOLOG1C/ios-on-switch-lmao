// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Switch's audio devices for iPadOS 17's audio server (audiomxd). Samples move through rings shared with the kernel
// service SwitchAudio (../SwitchAudioAbi.h). Three devices, as VirtualAudio (VirtualAudio.plugin, which turns HAL
// devices into the ports routes are made of) knows a J120's by their UIDs (its DeviceFactory_Aspen):
//  - "Speaker" (built in): the stereo output to the RT5639 (its speakers or the jack) and a silent stereo tap input.
//    VirtualAudio makes it Device_Speaker_Aspen, whose port is the built-in speaker (pspk, "Speaker"): the route
//    AVAudioSession calls BuiltInSpeaker and Control Center and the route picker show as the iPad itself. On 21H461 a
//    speaker device must have a tap stream (an input whose 'taps' is 1): every product's speaker policy sets
//    "tap stream required" when VirtualAudio's expanseEnabled feature is on, as it is, and without one the speaker
//    device throws ("TapStream is enabled by policy but not found") and plays nothing. Real hardware carries what
//    the speaker plays there, for echo cancellation; this one is silence, as there was no reference before.
//  - "SwitchAudioMic" (Thunderbolt): the mono input, the headset microphone on the jack (the Switch has no built-in
//    one). An unknown UID with the Thunderbolt transport is a driver device with a generic input port (ptbi), as the
//    single device was before; built-in devices with unknown UIDs would need Apple port descriptions, and a
//    "Codec"-UID device is Apple's own codec (headphones, data-source microphones), not published here.
//  - "Hawking" (built in): one silent mono input, the Hawking port (phki) of the J120's routing tables, which
//    otherwise put the null input in its place.
// Speaker and microphone run on the service's one timeline (one clock domain, so their aggregate needs no drift
// correction); Hawking keeps its own on the host clock and never starts the hardware.
// Objects: the plug-in (1); Speaker (2) with its output (3), tap (4) and output volume (5); the microphone device (6)
// and its input (7); Hawking (8) and its input (9).
// VirtualAudio runs a port's DSP chain after the mix: for pspk on J120 that is /Library/Audio/Tunings/J120/VAD/
// j120_speaker_*.austrip, tuned for its four MAX98723 drivers. ../tunings has stereo pass-through strips under the same
// names, which the Data overlay shows over those (tree/overlay/manifest.json).
//
// Derived from UniXNU-sheng's hal/ShengAudioPlugIn.c (BSD-2-Clause-Patent), which plays on iPadOS 17.7.11 hardware;
// its diagnostics, property trace and VirtualAudio rebinding are left out. What it established and this keeps:
//  - 'accs', 'auto' and empty custom-property, box, clock-device and 'taps' lists must be answered or the device is
//    dropped (a tap stream answers 'taps' with 1 instead),
//  - CoreAudio's 24 MHz host clock constant must be set to the board's timer (19.2 MHz here, as on sheng),
//  - zero time stamps start at the frame IO starts on, with a period longer than safety offset plus one IO cycle.
//
// Build (the iOS SDK has no AudioServerPlugIn headers; the macOS ones declare the same interface):
//   mkdir -p H/CoreAudio; for h in AudioServerPlugIn.h AudioHardwareBase.h AudioHardware.h; do
//     ln -sf "$(xcrun --sdk macosx --show-sdk-path)/System/Library/Frameworks/CoreAudio.framework/Headers/$h" H/CoreAudio/; done
//   xcrun clang -target arm64-apple-ios17.0 -isysroot "$(xcrun --sdk iphoneos --show-sdk-path)" -IH -I.. -O2 -Wall \
//     -Wextra -Werror -bundle -fvisibility=hidden -framework CoreFoundation -framework IOKit SwitchAudioPlugIn.c \
//     -o SwitchAudio.driver/SwitchAudio
//   codesign -f -s - --pagesize 4096 --identifier com.ioswitch.audio SwitchAudio.driver/SwitchAudio
// The executable alone (the Data overlay ships it and Info.plist, no resource seal), with 4 KiB code pages as the
// system's own binaries here; scripts/package_raw_boot.py puts its CDHash in the boot trust cache.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include "SwitchAudioAbi.h"

enum {
    kPlugIn = kAudioObjectPlugInObject,
    kSpeaker = 2,
    kSpeakerOut = 3,
    kSpeakerTap = 4,
    kVolume = 5,
    kMic = 6,
    kMicIn = 7,
    kHawking = 8,
    kHawkingIn = 9,
};
#define kSpeakerUID "Speaker"  // VirtualAudio's names for a J120's devices (DeviceFactory_Aspen)
#define kHawkingUID "Hawking"
#define kMicUID     "SwitchAudioMic"
#define kModelUID   "SwitchAudioModel"
static const UInt32 kClockDomain = SWITCH_AUDIO_MAGIC;  // speaker and microphone: the service's one clock
static const Float64 kRate = SWITCH_AUDIO_RATE;
static const UInt32 kZeroPeriod = SWITCH_AUDIO_RING_FRAMES;

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs = 1;
static io_connect_t gConnect;
static SwitchAudioControl *gControl;
static int16_t *gOut, *gIn;
static UInt32 gSpeakerUsers, gMicUsers, gHawkingUsers;  // IO clients per device (gLock)
static Float32 gVolume = 1.0f;
static Float64 gTicksPerFrame;      // the service's clock (its control page), nominal until it is mapped
static Float64 gHostTicksPerFrame;  // the host clock's nominal rate: Hawking's
static uint64_t gZeroOrigin, gZeroSeed, gHawkingOrigin, gHawkingSeed;

// ---- Kernel service ----------------------------------------------------------------------------------------------
static int mapType(uint32_t type, void **out)
{
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(gConnect, type, mach_task_self(), &address, &size, kIOMapAnywhere) != kIOReturnSuccess)
        return -1;
    *out = (void *)(uintptr_t)address;
    return 0;
}

// CoreAudio's host clock frequency is a 24 MHz constant that AudioGetHostClockFrequency loads with adrp + ldr d0; its
// address is decoded from those two instructions and the kernel service rewrites it (the page is read-only here).
static void setHostClock(void)
{
    // audiomxd is arm64e: dlsym's pointer is signed, and this arm64 code only reads through the address bits.
    const uintptr_t symbol = (uintptr_t)dlsym(RTLD_DEFAULT, "AudioGetHostClockFrequency") & (((uintptr_t)1 << 47) - 1);
    if (!symbol) return;
    const uint32_t *code = (const uint32_t *)symbol, adrp = code[0], ldr = code[1];
    if ((adrp & 0x9f000000u) != 0x90000000u || (ldr & 0xffc00000u) != 0xfd400000u || ((ldr >> 5) & 31) != (adrp & 31)) {
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: AudioGetHostClockFrequency has an unexpected shape");
        return;
    }
    int64_t page = (int64_t)(((adrp >> 5) & 0x7ffffu) << 2 | ((adrp >> 29) & 3u));
    if (page & (1 << 20)) page -= 1 << 21;
    const uint64_t address =
        ((uintptr_t)code & ~(uintptr_t)0xfff) + (uint64_t)(page << 12) + ((ldr >> 10) & 0xfffu) * 8;
    if (IOConnectCallScalarMethod(gConnect, SwitchAudioSetHostClock, &address, 1, NULL, NULL) != KERN_SUCCESS)
        os_log_error(OS_LOG_DEFAULT, "SwitchAudio: host clock constant not set");
}

static int connectService(void)
{
    if (gControl) return 0;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SWITCH_AUDIO_CLASS));
    if (!service) return -1;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConnect);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) return -1;
    void *control = NULL, *out = NULL, *in = NULL;
    if (mapType(SwitchAudioMemoryControl, &control) || mapType(SwitchAudioMemoryOutput, &out) ||
        mapType(SwitchAudioMemoryInput, &in) || ((SwitchAudioControl *)control)->magic != SWITCH_AUDIO_MAGIC ||
        ((SwitchAudioControl *)control)->version != SWITCH_AUDIO_VERSION) {
        IOServiceClose(gConnect);
        gConnect = 0;
        return -1;
    }
    gOut = out;
    gIn = in;
    gControl = control;
    gTicksPerFrame = (Float64)gControl->ticks_per_frame_q32 / 4294967296.0;
    setHostClock();
    return 0;
}
static kern_return_t call(uint32_t selector)
{
    return gConnect ? IOConnectCallScalarMethod(gConnect, selector, NULL, 0, NULL, NULL) : KERN_FAILURE;
}
static UInt32 latencyFrames(void)
{
    return gControl ? gControl->lead_frames : 2 * SWITCH_AUDIO_PERIOD_FRAMES;
}
// Capture becomes valid a period at a time (in_frames), so input is read at least that much plus the lead behind now.
static UInt32 inputSafetyFrames(void)
{
    return latencyFrames() + (gControl ? gControl->period_frames : SWITCH_AUDIO_PERIOD_FRAMES);
}
// The Tegra backend's DMA reads the ring from memory and does not snoop the caches: write back what was mixed.
static void cleanToMemory(const void *start, size_t bytes)
{
    for (uintptr_t at = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + bytes; at < end; at += 64)
        __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- Helpers -----------------------------------------------------------------------------------------------------
static AudioStreamBasicDescription format(UInt32 channels)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    return f;
}
#define REPLY(type, value)                                                        \
    do {                                                                          \
        if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; \
        *(type *)outData = (value);                                               \
        *outDataSize = sizeof(type);                                              \
        return 0;                                                                 \
    } while (0)
#define REPLY_STR(text) REPLY(CFStringRef, CFSTR(text))

// The output volume spans kMinDb..0 dB on a squared curve over the scalar; scalar 0 mutes (sheng's tuned curve).
static const Float32 kMinDb = -36.0f;
static Float32 toDb(Float32 scalar)
{
    const Float32 r = 1.0f - (scalar < 0 ? 0 : scalar > 1 ? 1 : scalar);
    return kMinDb * r * r;
}
static Float32 toScalar(Float32 db)
{
    return db <= kMinDb ? 0.0f : db >= 0 ? 1.0f : 1.0f - sqrtf(db / kMinDb);
}
static Float32 gain(void)
{
    return gVolume <= 0.0f ? 0.0f : powf(10.0f, toDb(gVolume) / 20.0f);
}

// ---- Object model --------------------------------------------------------------------------------------------
static Boolean isDevice(AudioObjectID o)
{
    return o == kSpeaker || o == kMic || o == kHawking;
}
static Boolean isStream(AudioObjectID o)
{
    return o == kSpeakerOut || o == kSpeakerTap || o == kMicIn || o == kHawkingIn;
}
static AudioObjectID deviceOf(AudioObjectID o)  // the device a stream or control belongs to
{
    return o == kMicIn ? kMic : o == kHawkingIn ? kHawking : kSpeaker;
}
static AudioObjectID outputOf(AudioObjectID device)
{
    return device == kSpeaker ? kSpeakerOut : 0;
}
static AudioObjectID inputOf(AudioObjectID device)
{
    return device == kSpeaker ? kSpeakerTap : device == kMic ? kMicIn : kHawkingIn;
}
static UInt32 channelsOf(AudioObjectID stream)
{
    return stream == kSpeakerOut || stream == kSpeakerTap ? 2 : 1;
}
static UInt32 usersOf(AudioObjectID device)
{
    return device == kSpeaker ? gSpeakerUsers : device == kMic ? gMicUsers : gHawkingUsers;
}

// ---- IUnknown and lifecycle --------------------------------------------------------------------------------------
static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *self)
{
    (void)self;
    return atomic_fetch_add(&gRefs, 1) + 1;
}
static ULONG Release(void *self)
{
    (void)self;
    UInt32 r = atomic_load(&gRefs);
    if (r) r = atomic_fetch_sub(&gRefs, 1) - 1;
    return r;
}

static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host)
{
    (void)d;
    gHost = host;
    pthread_mutex_lock(&gLock);
    if (connectService()) os_log_error(OS_LOG_DEFAULT, "SwitchAudio: kernel service not available yet");
    pthread_mutex_unlock(&gLock);
    return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c,
                             AudioObjectID *o)
{
    (void)d;
    (void)desc;
    (void)c;
    (void)o;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o)
{
    (void)d;
    (void)o;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c)
{
    (void)d;
    (void)o;
    (void)c;
    return 0;
}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c)
{
    (void)d;
    (void)o;
    (void)c;
    return 0;
}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{
    (void)d;
    (void)o;
    (void)a;
    (void)i;
    return 0;
}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i)
{
    (void)d;
    (void)o;
    (void)a;
    (void)i;
    return 0;
}

// ---- Properties --------------------------------------------------------------------------------------------------
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                                UInt32 *outDataSize, void *outData);

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size)
{
    UInt8 scratch[1024];
    UInt32 written = 0;
    const OSStatus r = GetPropertyData(d, o, pid, a, qs, q, sizeof scratch, &written, scratch);
    if (r == 0) *size = written;
    return r;
}
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                           const AudioObjectPropertyAddress *a)
{
    UInt32 size = 0;
    return GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
}
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                   const AudioObjectPropertyAddress *a, Boolean *out)
{
    const AudioObjectPropertySelector s = a->mSelector;
    *out =
        (o == kVolume && (s == kAudioLevelControlPropertyScalarValue || s == kAudioLevelControlPropertyDecibelValue)) ||
        (isStream(o) && (s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat ||
                         s == kAudioStreamPropertyIsActive)) ||
        (isDevice(o) && s == kAudioDevicePropertyNominalSampleRate);
    return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size,
                                const void *data)
{
    (void)d;
    (void)pid;
    (void)qs;
    (void)q;
    if (o == kVolume && size >= sizeof(Float32)) {
        Float32 v = *(const Float32 *)data;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue)
            v = toScalar(v);
        else if (a->mSelector != kAudioLevelControlPropertyScalarValue)
            return kAudioHardwareUnknownPropertyError;
        gVolume = v < 0 ? 0 : v > 1 ? 1 : v;
        const AudioObjectPropertyAddress changed[2] = {
            {kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
            {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
        gHost->PropertiesChanged(gHost, kVolume, 2, changed);
        return 0;
    }
    if (isStream(o) &&
        (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        return size >= sizeof *f && f->mSampleRate == kRate && f->mChannelsPerFrame == channelsOf(o)
                   ? 0
                   : kAudioDeviceUnsupportedFormatError;
    }
    if (isDevice(o) && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioDeviceUnsupportedFormatError;
    // The audio server turns streams it does not use off and on; they stay running, so this only acknowledges it.
    if (isStream(o) && a->mSelector == kAudioStreamPropertyIsActive)
        return size >= sizeof(UInt32) ? 0 : kAudioHardwareBadPropertySizeError;
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                                UInt32 *outDataSize, void *outData)
{
    (void)d;
    (void)pid;
    const AudioObjectPropertySelector s = a->mSelector;
    // Lists the audio server reads for each new device and that must answer, empty: custom properties on every
    // object, boxes and clock devices on the plug-in, taps on the streams (an unknown-property error drops the device).
    // The speaker's tap stream answers 'taps' with 1 (top of file).
    if (o == kSpeakerTap && s == 'taps') REPLY(UInt32, 1);
    if (o <= kHawkingIn &&
        (s == kAudioObjectPropertyCustomPropertyInfoList ||
         ((s == kAudioPlugInPropertyBoxList || s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn) ||
         (s == 'taps' && isStream(o)))) {
        *outDataSize = 0;
        return 0;
    }
    if (o == kPlugIn) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("IOSwitch");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            static const AudioObjectID devices[] = {kSpeaker, kMic, kHawking};
            UInt32 n = 3;
            if (inDataSize / sizeof(AudioObjectID) < n) n = inDataSize / (UInt32)sizeof(AudioObjectID);
            memcpy(outData, devices, n * sizeof(AudioObjectID));
            *outDataSize = n * (UInt32)sizeof(AudioObjectID);
            return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            AudioObjectID device = kAudioObjectUnknown;
            if (uid && CFStringCompare(uid, CFSTR(kSpeakerUID), 0) == kCFCompareEqualTo) device = kSpeaker;
            if (uid && CFStringCompare(uid, CFSTR(kMicUID), 0) == kCFCompareEqualTo) device = kMic;
            if (uid && CFStringCompare(uid, CFSTR(kHawkingUID), 0) == kCFCompareEqualTo) device = kHawking;
            REPLY(AudioObjectID, device);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (isDevice(o)) {
        const AudioObjectID output = outputOf(o), input = inputOf(o);
        const Boolean scopeIn = a->mScope == kAudioObjectPropertyScopeInput;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyModelName:
            if (o == kSpeaker) REPLY_STR("Speaker");
            if (o == kMic) REPLY_STR("Microphone");
            REPLY_STR("Hawking");
        case kAudioObjectPropertyManufacturer: REPLY_STR("IOSwitch");
        case kAudioDevicePropertyDeviceUID:
            if (o == kSpeaker) REPLY_STR(kSpeakerUID);
            if (o == kMic) REPLY_STR(kMicUID);
            REPLY_STR(kHawkingUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        case 'accs': REPLY(UInt32, 0);  // private device properties the audio server reads for each new device
        case 'auto': REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        case kAudioDevicePropertyTransportType:
            REPLY(UInt32, o == kMic ? kAudioDeviceTransportTypeThunderbolt : kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, o);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, o == kHawking ? 0 : kClockDomain);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, usersOf(o) > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, o != kHawking);
        case kAudioDevicePropertyLatency: REPLY(UInt32, o == kHawking ? 0 : latencyFrames());
        case kAudioDevicePropertySafetyOffset:
            if (o == kHawking) REPLY(UInt32, SWITCH_AUDIO_PERIOD_FRAMES);
            REPLY(UInt32, scopeIn ? inputSafetyFrames() : latencyFrames() + 2 * SWITCH_AUDIO_PERIOD_FRAMES);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            const UInt32 channels = channelsOf(scopeIn || !output ? input : output);
            ((UInt32 *)outData)[0] = 1;
            ((UInt32 *)outData)[1] = channels;
            *outDataSize = 2 * sizeof(UInt32);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = channelsOf(scopeIn || !output ? input : output);
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) +
                                          channels * sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = channels;
            l->mChannelDescriptions[0].mChannelLabel =
                channels == 1 ? kAudioChannelLabel_Mono : kAudioChannelLabel_Left;
            if (channels == 2) l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outDataSize = bytes;
            return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[3];
            UInt32 n = 0;
            const Boolean streams = s != kAudioObjectPropertyControlList, controls = s != kAudioDevicePropertyStreams;
            if (streams && output && !scopeIn) list[n++] = output;
            if (streams && a->mScope != kAudioObjectPropertyScopeOutput) list[n++] = input;
            if (controls && o == kSpeaker && !scopeIn) list[n++] = kVolume;
            if (inDataSize / sizeof(AudioObjectID) < n) n = inDataSize / (UInt32)sizeof(AudioObjectID);
            memcpy(outData, list, n * sizeof(AudioObjectID));
            *outDataSize = n * (UInt32)sizeof(AudioObjectID);
            return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (isStream(o)) {
        const UInt32 channels = channelsOf(o);
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, deviceOf(o));
        case kAudioObjectPropertyName:
            if (o == kSpeakerOut) REPLY_STR("Speaker");
            if (o == kSpeakerTap) REPLY_STR("Speaker Tap");
            if (o == kMicIn) REPLY_STR("Microphone");
            REPLY_STR("Hawking Input");
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, 1);
        case kAudioStreamPropertyDirection: REPLY(UInt32, o != kSpeakerOut);
        case kAudioStreamPropertyTerminalType:
            REPLY(UInt32, o == kSpeakerOut   ? kAudioStreamTerminalTypeSpeaker
                          : o == kSpeakerTap ? kAudioStreamTerminalTypeUnknown
                                             : kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format(channels));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            REPLY(AudioStreamRangedDescription, ((AudioStreamRangedDescription){format(channels), {kRate, kRate}}));
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    if (o == kVolume) {
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kSpeaker);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: REPLY(Float32, gVolume);
        case kAudioLevelControlPropertyDecibelValue: REPLY(Float32, toDb(gVolume));
        case kAudioLevelControlPropertyDecibelRange: REPLY(AudioValueRange, ((AudioValueRange){kMinDb, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toDb(*(Float32 *)outData);
            *outDataSize = sizeof(Float32);
            return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toScalar(*(Float32 *)outData);
            *outDataSize = sizeof(Float32);
            return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    return kAudioHardwareBadObjectError;
}

// ---- IO ----------------------------------------------------------------------------------------------------------
// The play position now, in sample time on the service's timeline.
static uint64_t nowFrames(void)
{
    const uint64_t host = mach_absolute_time(), anchor = gControl->anchor_host;
    return host > anchor ? (uint64_t)((Float64)(host - anchor) / gTicksPerFrame) : 0;
}

// The speaker starts the service's output and the microphone its input; the hardware runs while either does, on one
// timeline that starts (new origin, new seed) when the first of them starts. Hawking only starts its own clock.
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client)
{
    (void)d;
    (void)client;
    if (!isDevice(o)) return kAudioHardwareBadObjectError;
    OSStatus r = 0;
    pthread_mutex_lock(&gLock);
    if (o == kHawking) {
        if (!gHawkingUsers++) {
            gHawkingOrigin = mach_absolute_time();
            gHawkingSeed++;
        }
        pthread_mutex_unlock(&gLock);
        return 0;
    }
    UInt32 *users = o == kSpeaker ? &gSpeakerUsers : &gMicUsers;
    if (*users == 0) {
        const Boolean first = !gSpeakerUsers && !gMicUsers;
        if (connectService() || call(o == kSpeaker ? SwitchAudioStartOutput : SwitchAudioStartInput) != KERN_SUCCESS)
            r = kAudioHardwareNotRunningError;
        else if (first) {
            setHostClock();  // again: the shared page may have been read back from disk since
            gZeroOrigin = nowFrames();
            gZeroSeed++;
        }
    }
    if (!r) ++*users;
    pthread_mutex_unlock(&gLock);
    return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client)
{
    (void)d;
    (void)client;
    if (!isDevice(o)) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (o == kHawking) {
        if (gHawkingUsers) --gHawkingUsers;
    } else {
        UInt32 *users = o == kSpeaker ? &gSpeakerUsers : &gMicUsers;
        if (*users && --*users == 0) call(o == kSpeaker ? SwitchAudioStopOutput : SwitchAudioStopInput);
    }
    pthread_mutex_unlock(&gLock);
    return 0;
}
// Time stamps start at the frame IO started on and repeat every kZeroPeriod frames: on the service's timeline for the
// speaker and microphone, on the host clock at the nominal rate for Hawking.
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, Float64 *sampleTime,
                                 UInt64 *hostTime, UInt64 *seed)
{
    (void)d;
    (void)client;
    const uint64_t now = mach_absolute_time();
    if (o == kHawking) {
        const Float64 period = gHostTicksPerFrame * kZeroPeriod;
        const uint64_t wraps = now > gHawkingOrigin ? (uint64_t)((Float64)(now - gHawkingOrigin) / period) : 0;
        *sampleTime = (Float64)(wraps * kZeroPeriod);
        *hostTime = gHawkingOrigin + (uint64_t)((Float64)wraps * period);
        *seed = gHawkingSeed;
        return 0;
    }
    if (!gControl) return kAudioHardwareNotRunningError;
    const Float64 period = gTicksPerFrame * kZeroPeriod;
    const uint64_t origin = gControl->anchor_host + (uint64_t)((Float64)gZeroOrigin * gTicksPerFrame);
    const uint64_t wraps = now > origin ? (uint64_t)((Float64)(now - origin) / period) : 0;
    *sampleTime = (Float64)(gZeroOrigin + wraps * kZeroPeriod);
    *hostTime = origin + (uint64_t)((Float64)wraps * period);
    *seed = gZeroSeed;
    return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op,
                                  Boolean *will, Boolean *inPlace)
{
    (void)d;
    (void)client;
    *will =
        (op == kAudioServerPlugInIOOperationWriteMix && o == kSpeaker) || op == kAudioServerPlugInIOOperationReadInput;
    *inPlace = true;
    return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames,
                                 const AudioServerPlugInIOCycleInfo *info)
{
    (void)d;
    (void)o;
    (void)client;
    (void)op;
    (void)frames;
    (void)info;
    return 0;
}
static inline int16_t toS16(Float32 v)
{
    v *= 32767.0f;
    return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 client,
                              UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info, void *main,
                              void *secondary)
{
    (void)d;
    (void)o;
    (void)client;
    (void)secondary;
    if (op == kAudioServerPlugInIOOperationReadInput && (stream == kSpeakerTap || stream == kHawkingIn)) {
        memset(main, 0, (size_t)frames * channelsOf(stream) * sizeof(Float32));  // silent inputs (top of file)
        return 0;
    }
    if (!gControl) return 0;
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kSpeakerOut) {
        // The audio server can run more than one IO context on the device, each with its own mix: add to what the ring
        // holds (the service clears every period it consumes) so one context's silence does not erase another's sound.
        const Float32 *src = main, g = gain();
        const uint64_t first = (uint64_t)info->mOutputTime.mSampleTime;
        uint64_t t = first;
        for (UInt32 i = 0; i < frames; ++i, ++t) {
            int16_t *dst = gOut + 2 * (t % SWITCH_AUDIO_RING_FRAMES);
            dst[0] = toS16(dst[0] / 32767.0f + src[2 * i] * g);
            dst[1] = toS16(dst[1] / 32767.0f + src[2 * i + 1] * g);
        }
        if (gControl->backend == SwitchAudioBackendTegraI2S && frames) {
            const uint64_t at = first % SWITCH_AUDIO_RING_FRAMES;
            const uint64_t total = frames < SWITCH_AUDIO_RING_FRAMES ? frames : SWITCH_AUDIO_RING_FRAMES;
            const uint64_t run = total < SWITCH_AUDIO_RING_FRAMES - at ? total : SWITCH_AUDIO_RING_FRAMES - at;
            cleanToMemory(gOut + 2 * at, run * 2 * sizeof(int16_t));
            if (run < total) cleanToMemory(gOut, (total - run) * 2 * sizeof(int16_t));
        }
        if (t > gControl->out_written) gControl->out_written = t;
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kMicIn) {
        Float32 *dst = main;
        uint64_t t = (uint64_t)info->mInputTime.mSampleTime;
        const uint64_t captured = gControl->in_frames;
        for (UInt32 i = 0; i < frames; ++i, ++t)
            dst[i] = t < captured ? gIn[t % SWITCH_AUDIO_RING_FRAMES] / 32768.0f : 0.0f;
    }
    return 0;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames,
                               const AudioServerPlugInIOCycleInfo *info)
{
    (void)d;
    (void)o;
    (void)client;
    (void)op;
    (void)frames;
    (void)info;
    return 0;
}

static AudioServerPlugInDriverInterface gInterface = {NULL,
                                                      QueryInterface,
                                                      AddRef,
                                                      Release,
                                                      Initialize,
                                                      CreateDevice,
                                                      DestroyDevice,
                                                      AddDeviceClient,
                                                      RemoveDeviceClient,
                                                      PerformDeviceConfigurationChange,
                                                      AbortDeviceConfigurationChange,
                                                      HasProperty,
                                                      IsPropertySettable,
                                                      GetPropertyDataSize,
                                                      GetPropertyData,
                                                      SetPropertyData,
                                                      StartIO,
                                                      StopIO,
                                                      GetZeroTimeStamp,
                                                      WillDoIOOperation,
                                                      BeginIOOperation,
                                                      DoIOOperation,
                                                      EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out)
{
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        AddRef(self);
        *out = &gInterfacePtr;
        r = S_OK;
    }
    CFRelease(requested);
    return r;
}

__attribute__((visibility("default"))) void *SwitchAudio_Create(CFAllocatorRef allocator, CFUUIDRef type)
{
    (void)allocator;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return NULL;
    if (!gHostTicksPerFrame) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        gHostTicksPerFrame = 1e9 / kRate * tb.denom / tb.numer;
        if (!gTicksPerFrame) gTicksPerFrame = gHostTicksPerFrame;
    }
    return &gInterfacePtr;
}
