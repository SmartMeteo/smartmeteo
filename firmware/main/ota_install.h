#pragma once

#include <stdint.h>
#include <time.h>

// Downloading and installing an update. Deliberately separate from ota_check.cpp: deciding that a
// new version EXISTS and deciding to overwrite the other half of the flash are different acts with
// different consequences, and the second one should not be reachable by accident from the first.
namespace OtaInstall {

enum class Result {
    Idle,        // nothing to do — no offer, or not allowed right now
    Installed,   // written, verified and armed; the caller must reboot
    Failed,      // attempted and did not finish; why_ says what happened
};

// True when there is a newer version on offer AND this device is willing to spend the charge and
// the radio time on it right now. Cheap; safe to call on any wake.
//
// `automatic` separates the two callers, and getting that wrong made the Install button refuse to
// work. The scheduled path must obey the owner's automatic-updates switch; pressing Install is
// itself the consent that switch stands for, so the manual path must not. The switch is about
// whether the device may decide, not about whether it may install.
bool due(bool automatic = true);

// Why not, in words fit to show someone — or nullptr when nothing is blocking. Exists because the
// endpoint used to answer "not allowed now", which is the same as saying nothing.
const char *blockedReason(bool automatic);

// Fetch, verify and arm. MUST be called with the radio already up — like the check, this never
// raises it, because an update is not worth a session of its own on a device sold on months.
// On Result::Installed the caller is responsible for the calibration hand-off and the restart:
// this function deliberately does not reboot, because the state that has to survive the update
// lives in the cycle, not here.
// `automatic` as for due(): the scheduled caller leaves it true, the Install button's task passes
// false. Every return after the run has been announced leaves the state at StFailed or StDone,
// never at StRunning.
Result run(bool automatic = true);

// Why the last attempt ended the way it did — short, for the console and the log.
extern char why[32];

// Bytes fetched in the last attempt, and how many were expected. Updated as the transfer runs, so
// a console watching /api/status sees a bar move rather than a spinner that might mean anything.
extern volatile uint32_t fetched;
extern volatile uint32_t total;

// What the installer is doing RIGHT NOW, in words fit to show someone. `state` alone is too
// coarse: it says "running" through two multi-second stretches where nothing moves — erasing the
// far slot before the download, and verifying the signature over 1.7 MB after it. The owner sees a
// bar at 0 % or stuck at 100 %, then the device vanishes and reboots with no explanation. A
// progress bar that stops moving and says nothing is indistinguishable from a hang, and was
// reported as one.
extern char phase[24];

// Where the current attempt is. Read by the status endpoint; written only by the worker.
enum State : uint8_t { StIdle = 0, StRunning = 1, StDone = 2, StFailed = 3 };
extern volatile State state;

// An image being pushed into the same slot over the local network (/api/ota). That path does not go
// through run() at all, so `state` says nothing about it, and until 2026-09-18 nothing did: the
// upload refused to start while an install was running, but an install would happily start while an
// upload was streaming — two writers, one slot, and esp_ota_end() then rejecting the mixture. It
// also keeps the setup window open for the duration, which the upload needs and did not have: a
// 3 MB push over a weak access-point link outlasts the three-minute window that carries its radio.
extern volatile bool local_upload;

// Start an install in its own task and return at once, so the request that asked for it can be
// answered instead of holding an HTTP connection open for a 1.7 MB transfer. The task performs the
// calibration hand-off and reboots on success — the same ending the scheduled path has, because a
// device that installed but did not hand over its calibration arrives at the new image blind.
// Returns false when there is nothing to install or one is already running.
bool start(void);

}  // namespace OtaInstall
