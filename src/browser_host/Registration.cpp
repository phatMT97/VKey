// VKey browser native-messaging registration
// SPDX-License-Identifier: GPL-3.0-only

#include "Registration.h"

#include "core/config/ConfigManager.h"
#include "core/security/ExtensionTrust.h"

#include <Windows.h>

#include <string>
#include <string_view>

namespace NextKey::BrowserHost {
namespace {

constexpr wchar_t kHostName[] = L"io.github.phatmt97.vkey";
constexpr char kChromiumId[] = "ccmggbcabaknpjielbiioolpfnpfgkbi";
constexpr char kFirefoxId[] = "browser@vkey.phatmt97.github.io";

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring result(path, length);
    const auto slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    result.resize(slash);
    return result;
}

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::string JsonPath(std::wstring_view value) {
    std::string result;
    for (const char ch : Utf8(value)) {
        if (ch == '\\' || ch == '"') result.push_back('\\');
        result.push_back(ch);
    }
    return result;
}

bool WriteTextFile(const std::wstring& path, std::string_view content) noexcept {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()),
                              &written, nullptr) != FALSE
        && written == content.size();
    FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

bool SetManifestRegistryPath(std::wstring_view vendorPath,
                             const std::wstring& manifestPath) noexcept {
    std::wstring key(vendorPath);
    key += L"\\";
    key += kHostName;
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &handle, nullptr) != ERROR_SUCCESS)
        return false;
    const DWORD bytes = static_cast<DWORD>((manifestPath.size() + 1) * sizeof(wchar_t));
    const bool ok = RegSetValueExW(handle, nullptr, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(manifestPath.c_str()),
                                   bytes) == ERROR_SUCCESS;
    RegCloseKey(handle);
    return ok;
}

bool DeleteManifestRegistryPath(std::wstring_view vendorPath) noexcept {
    std::wstring key(vendorPath);
    key += L"\\";
    key += kHostName;
    return RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS;
}

bool CheckManifestRegistryPath(std::wstring_view vendorPath) noexcept {
    std::wstring key(vendorPath);
    key += L"\\";
    key += kHostName;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &handle) == ERROR_SUCCESS) {
        RegCloseKey(handle);
        return true;
    }
    return false;
}

} // namespace

bool RegisterNativeMessagingHost() noexcept {
    try {
        const std::wstring moduleDir = ModuleDirectory();
        if (moduleDir.empty()) return false;
        const std::wstring hostPath = moduleDir + L"\\VKeyBrowserHost.exe";
        if (GetFileAttributesW(hostPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

        // Security gate: verify binary integrity and signature before registering
        if (!Security::IsExtensionTrusted(hostPath)) return false;

        const std::wstring root = ConfigManager::GetAppDataDirectory()
            + L"\\native-messaging";
        CreateDirectoryW(root.c_str(), nullptr);
        const std::wstring chromiumManifest = root + L"\\vkey-browser-chromium.json";
        const std::wstring firefoxManifest = root + L"\\vkey-browser-firefox.json";
        const std::string escapedHost = JsonPath(hostPath);
        const std::string chromiumJson =
            "{\n  \"name\": \"io.github.phatmt97.vkey\",\n"
            "  \"description\": \"VKey browser routing host\",\n"
            "  \"path\": \"" + escapedHost + "\",\n"
            "  \"type\": \"stdio\",\n"
            "  \"allowed_origins\": [\"chrome-extension://" +
            std::string(kChromiumId) + "/\"]\n}\n";
        const std::string firefoxJson =
            "{\n  \"name\": \"io.github.phatmt97.vkey\",\n"
            "  \"description\": \"VKey browser routing host\",\n"
            "  \"path\": \"" + escapedHost + "\",\n"
            "  \"type\": \"stdio\",\n"
            "  \"allowed_extensions\": [\"" + std::string(kFirefoxId) + "\"]\n}\n";
        if (!WriteTextFile(chromiumManifest, chromiumJson)
            || !WriteTextFile(firefoxManifest, firefoxJson))
            return false;

        bool registered = false;
        registered |= SetManifestRegistryPath(
            L"Software\\Google\\Chrome\\NativeMessagingHosts", chromiumManifest);
        registered |= SetManifestRegistryPath(
            L"Software\\Microsoft\\Edge\\NativeMessagingHosts", chromiumManifest);
        registered |= SetManifestRegistryPath(
            L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts", chromiumManifest);
        registered |= SetManifestRegistryPath(
            L"Software\\Vivaldi\\NativeMessagingHosts", chromiumManifest);
        registered |= SetManifestRegistryPath(
            L"Software\\Opera Software\\NativeMessagingHosts", chromiumManifest);
        registered |= SetManifestRegistryPath(
            L"Software\\Mozilla\\NativeMessagingHosts", firefoxManifest);
        return registered;
    } catch (...) {
        return false;
    }
}

bool UnregisterNativeMessagingHost() noexcept {
    try {
        bool unregistered = false;
        unregistered |= DeleteManifestRegistryPath(L"Software\\Google\\Chrome\\NativeMessagingHosts");
        unregistered |= DeleteManifestRegistryPath(L"Software\\Microsoft\\Edge\\NativeMessagingHosts");
        unregistered |= DeleteManifestRegistryPath(L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts");
        unregistered |= DeleteManifestRegistryPath(L"Software\\Vivaldi\\NativeMessagingHosts");
        unregistered |= DeleteManifestRegistryPath(L"Software\\Opera Software\\NativeMessagingHosts");
        unregistered |= DeleteManifestRegistryPath(L"Software\\Mozilla\\NativeMessagingHosts");

        const std::wstring root = ConfigManager::GetAppDataDirectory() + L"\\native-messaging";
        DeleteFileW((root + L"\\vkey-browser-chromium.json").c_str());
        DeleteFileW((root + L"\\vkey-browser-firefox.json").c_str());
        return unregistered;
    } catch (...) {
        return false;
    }
}

bool IsNativeMessagingHostRegistered() noexcept {
    return CheckManifestRegistryPath(L"Software\\Google\\Chrome\\NativeMessagingHosts")
        || CheckManifestRegistryPath(L"Software\\Microsoft\\Edge\\NativeMessagingHosts")
        || CheckManifestRegistryPath(L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts")
        || CheckManifestRegistryPath(L"Software\\Mozilla\\NativeMessagingHosts");
}

} // namespace NextKey::BrowserHost
