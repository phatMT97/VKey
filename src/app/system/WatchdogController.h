// VKey - Watchdog Controller (Windows-only)
// SPDX-License-Identifier: GPL-3.0-only
//
// Owns the watchdog lifecycle: heartbeat thread + Task Scheduler entry +
// VKeyWatchdog.exe child process.

#pragma once

#ifdef _WIN32

#include "system/HeartbeatPublisher.h"

#include <Windows.h>
#include <atomic>

namespace NextKey {

struct SystemConfig;  // forward decl — definition in core/SystemConfig.h

class WatchdogController {
public:
    WatchdogController() = default;
    ~WatchdogController() = default;

    WatchdogController(const WatchdogController&) = delete;
    WatchdogController& operator=(const WatchdogController&) = delete;

    /// Restore enabled state from `systemConfig`. If previously enabled,
    /// launch VKeyWatchdog.exe and start the heartbeat (Task Scheduler
    /// entry is assumed to exist from a prior Toggle ON).
    void Init(const SystemConfig& systemConfig);

    /// Flip on/off in response to a user click: persist to TOML, manage
    /// Task Scheduler entry, child process, and heartbeat.
    /// @param notifyHwnd Parent HWND for the confirm MessageBox (may be NULL).
    void Toggle(HWND notifyHwnd);

    /// Idempotent — signals "user quit, do not respawn" to the watchdog.
    void SignalGracefulShutdown() noexcept;

    /// Task Scheduler entry point: check the saved setting and the embedded
    /// companion hash before starting the watchdog.
    [[nodiscard]] static int RunScheduledTask() noexcept;

    [[nodiscard]] bool IsEnabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] static bool LaunchWatchdogProcess() noexcept;
    static void KillWatchdogProcess() noexcept;

    HeartbeatPublisher heartbeat_;
    std::atomic<bool> enabled_{false};
};

}  // namespace NextKey

#endif  // _WIN32
