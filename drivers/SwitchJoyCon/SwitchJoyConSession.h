// SPDX-License-Identifier: BSD-2-Clause-Patent
// When a rail Joy-Con's HID device has to be published again (SwitchJoyConHID.cpp keeps the session,
// SwitchJoyConRail.cpp republishes; test/switch_joycon_session_test.cpp drives this header on the host and models the
// daemons around it).
//
// gamecontrollerd takes exactly one JoyConHIDServicePlugin session per HID service (21H461 GameController.framework):
//   * a service enters -[_GCNintendoJoyConDeviceManager claimHIDService:] only from its HID monitor's matching callback
//     ("HID Service Added"), which puts a _GCNintendoJoyConDevicePending in _pendingDevices[service];
//   * -[... acceptDriverConnection:forHIDService:] / acceptFilterConnection: hand the plugin's (and
//     GamepadHIDServiceFilter's) connection to that pending device, and when it has become the physical device
//     (claimHIDService: block .109: _physicalDevices[service] = device, removeObjectForKey: from _pendingDevices) every
//     later connection for the service finds no pending device and is invalidated (block: objectForKey: nil ->
//     -[connection invalidate]);
//   * the physical device's driver-connection invalidation handler only clears the connection; only "HID Service
//     Removed" (-[... relinquishHIDService:]) or a new device with the same identifier (block .110, SerialNumber)
//     drops it.
// backboardd's plugin and filter reconnect at once when gamecontrollerd cancels them ("interrupted.  Attempting to
// re-establish."), so a second plugin session on a service gamecontrollerd has already prepared is a connection storm:
// tens of thousands of connections in minutes, with gamecontrollerd the top CPU user.
//
// That second session is what a backboardd restart gives a device that outlives it: the kernel device and its registry
// ID stay, and gamecontrollerd's IOHIDEventSystemClient, reconnecting to the new backboardd
// (__IOHIDEventSystemClientRefresh -> IOHIDEventSystemClientSetMatchingMultiple), keeps a service whose ID it already
// knew without calling the matching or removal callbacks (__IOHIDEventSystemClientServiceReplaceCallback; only IDs it
// did not know reach __IOHIDEventSystemClientRefreshServiceCallback). bluetoothd's device for a Bluetooth Joy-Con stays
// across a backboardd restart as well, so by the same code it would meet the same refusal; this driver can renew only
// the devices it publishes. So when the backboardd session serving a rail device ends while the device stays, the
// rail publishes a new device (new registry ID): gamecontrollerd claims that one afresh, and block .110 retires the
// stale physical device by its SerialNumber.
//
// A session is the first user client from backboardd that opens the device (JoyConHIDServicePlugin's HIDDevice; the
// kernel's own IOHIDEventDummyService and other processes' clients do not count). It ends when that client closes the
// device (backboardd exited: its user clients close as the task goes; the rail's own destroy has detached the
// callback first), or when a different backboardd opens the device (a close that was not seen, or comes late).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum switch_joycon_session_event {
    kSwitchJoyConSessionNone = 0,
    kSwitchJoyConSessionClosed = 1,    // the serving backboardd's client closed the device
    kSwitchJoyConSessionReplaced = 2,  // a different backboardd opened it
};

// One device's backboardd session. Zero-initialised for a new device. The caller serialises calls (the device's lock).
struct switch_joycon_session {
    uintptr_t client;  // the serving user client, compared only (never dereferenced); 0 before the first one
    int32_t pid;       // its backboardd's pid
    bool ended;        // reported once; a device whose session ended is about to be replaced
};

// An open of the device by `client` (an IOUserClient; 0 for a kernel client) from process `pid`, `backboardd` when that
// process is backboardd. Returns the event to report (the ended session's pid is session->pid).
static inline enum switch_joycon_session_event
switch_joycon_session_open(struct switch_joycon_session *session, uintptr_t client, int32_t pid, bool backboardd)
{
    if (!client || !backboardd || session->ended)
        return kSwitchJoyConSessionNone;
    if (!session->client) {
        session->client = client;
        session->pid = pid;
        return kSwitchJoyConSessionNone;
    }
    if (pid == session->pid)
        return kSwitchJoyConSessionNone;  // another client in the same backboardd
    session->ended = true;
    return kSwitchJoyConSessionReplaced;
}

// A close of the device by `client` (as for open).
static inline enum switch_joycon_session_event switch_joycon_session_close(struct switch_joycon_session *session,
                                                                           uintptr_t client)
{
    if (!client || session->ended || client != session->client)
        return kSwitchJoyConSessionNone;
    session->ended = true;
    return kSwitchJoyConSessionClosed;
}

// The rail's loop check (non-fatal: log one line and turn the feature off). Every backboardd restart is a new
// pid, so republishing for each one is fine however often it comes; two sessions ended by the same backboardd within
// kSwitchJoyConRepublishGapMS of each other would be a plugin that drops every new device, which republishing would
// only feed.
enum {
    kSwitchJoyConRepublishGapMS = 10000
};

struct switch_joycon_republish_guard {
    uint64_t at;  // ms of the last republish, 0 if none
    int32_t pid;  // the backboardd whose session that was
};

// True: publish the device again (recorded). False: refuse; the caller logs once and stops republishing.
static inline bool switch_joycon_republish_allowed(struct switch_joycon_republish_guard *guard, int32_t pid,
                                                   uint64_t now_ms)
{
    if (guard->at && guard->pid == pid && now_ms - guard->at < kSwitchJoyConRepublishGapMS)
        return false;
    guard->at = now_ms ? now_ms : 1;
    guard->pid = pid;
    return true;
}

#ifdef __cplusplus
}
#endif
