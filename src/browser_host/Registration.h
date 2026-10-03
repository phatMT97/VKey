// VKey browser native-messaging registration
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace NextKey::BrowserHost {

/// Register the per-user native-messaging manifests for supported browsers.
/// Safe and idempotent; returns false when the companion executable is absent
/// or no browser manifest could be registered.
[[nodiscard]] bool RegisterNativeMessagingHost() noexcept;
/// True when Chrome/Firefox launched this VKey executable as a native host.
[[nodiscard]] bool IsNativeMessagingInvocation() noexcept;
/// Verify and launch the companion with the browser's standard I/O handles.
[[nodiscard]] int RunTrustedNativeMessagingHost() noexcept;

/// Unregister the per-user native-messaging manifests from all supported browsers.
[[nodiscard]] bool UnregisterNativeMessagingHost() noexcept;

/// Check if the native-messaging host is currently registered in browser registry.
[[nodiscard]] bool IsNativeMessagingHostRegistered() noexcept;

} // namespace NextKey::BrowserHost
