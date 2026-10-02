// VKey - Configuration Manager Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "ConfigManager.h"
#include "core/Debug.h"
#include "core/hotkey/HotkeyLabel.h"

#define TOML_HEADER_ONLY 1
#include "toml.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <filesystem>

#ifdef _WIN32
#include "core/WinStrings.h"
#include <ShlObj.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace NextKey {

namespace {

// ─── Wave 2 (2026-05-23) — TomlFileCache ───────────────────────────────
//
// Lazy parse for the 7 TOML files re-read on every config bump (TODO
// #1074). Cache keys on the UTF-8 path string; entries hold the mtime
// at parse time + a shared_ptr<const toml::table>. If a Load() call
// observes the same mtime as the cached entry, returns the cached
// table — no file I/O, no parse work (~5ms saved per skipped file on
// cold cache).
//
// Failure caching: a parse error caches a null table for that mtime;
// repeated callers don't hammer the broken file until it changes.
//
// Thread safety: callable from main thread (settings save / dialog),
// worker thread (ReloadFromToml drain), and startup thread (Start).
// Internal mutex serialises map mutation. The shared_ptr<const table>
// is RCU-style — readers hold a snapshot via the returned shared_ptr
// even if a concurrent re-parse swaps the cache entry.
//
// Intra-second edit race: filesystem mtime resolution is typically
// 1 second on FAT/exFAT and ~100ns on NTFS. On NTFS, edits within
// the same tick window would miss; in practice each TOML write goes
// through `WriteToml`'s rename-and-replace which always produces a
// fresh mtime tick.
class TomlFileCache {
public:
    /// Returns the cached or freshly-parsed table. nullptr on file
    /// missing or parse error.
    [[nodiscard]] std::shared_ptr<const toml::table> Load(const std::string& utf8Path) {
        std::error_code ec;
        // C++20 deprecated std::filesystem::u8path. Construct from wstring on
        // Windows (native encoding) so non-ASCII paths work; on Linux the
        // native encoding IS UTF-8 so direct construction from std::string
        // is correct.
#ifdef _WIN32
        const std::filesystem::path fsPath(Utf8ToWide(utf8Path));
#else
        const std::filesystem::path fsPath(utf8Path);
#endif
        const auto mtime = std::filesystem::last_write_time(fsPath, ec);
        if (ec) {
            // File missing — drop any stale cache entry so a future
            // re-create picks up clean.
            std::lock_guard lk(mu_);
            entries_.erase(utf8Path);
            return nullptr;
        }
        {
            std::lock_guard lk(mu_);
            auto it = entries_.find(utf8Path);
            if (it != entries_.end() && it->second.mtime == mtime) {
                return it->second.table;  // shared_ptr copy (may be null)
            }
        }
        // Re-parse. Release the cache lock during file I/O so a
        // concurrent Load() on a different file doesn't serialise.
        std::shared_ptr<const toml::table> parsed;
        try {
            parsed = std::make_shared<const toml::table>(toml::parse_file(utf8Path));
        } catch (...) {
            parsed = nullptr;
        }
        std::lock_guard lk(mu_);
        entries_[utf8Path] = Entry{mtime, parsed};
        return parsed;
    }

    /// Drop the cache entry for a single path. Called by `WriteToml`
    /// after a successful rename so a subsequent `Load()` re-parses
    /// even when filesystem mtime resolution is too coarse to tick
    /// between a same-tick Save→Load pair (Linux ext4 1s default,
    /// FAT/exFAT 2s, NTFS occasionally same-tick under load).
    void Invalidate(const std::string& utf8Path) {
        std::lock_guard lk(mu_);
        entries_.erase(utf8Path);
    }

    /// Drop all cached entries (test helper).
    void Clear() {
        std::lock_guard lk(mu_);
        entries_.clear();
    }

private:
    struct Entry {
        std::filesystem::file_time_type mtime;
        std::shared_ptr<const toml::table> table;  // null = parse failed at this mtime
    };
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> entries_;
};

/// Immortal (never-destroyed) accessor for the process-wide TOML cache.
///
/// MUST NOT be a plain global / Meyers static: at process exit the CRT
/// runs static destructors on the main thread while OTHER threads are
/// still alive — detached auto-update / toast threads in `main.cpp`
/// (Sleep 30s / 1s, never joined) and any later-destructed global in a
/// different TU whose dtor calls `ConfigManager::Load*`. A destructed
/// `unordered_map` has its bucket-array pointer nulled by `~vector`
/// while `_Mask` keeps the default 8-bucket value (7), so a post-dtor
/// `find()` dereferences `[null + (hash & 7) * 16 + 8]` → access
/// violation reading 0x78 (observed crash, RVA 0x73906, v4.1.0.0).
///
/// Leaking the instance keeps `entries_` valid for the full process
/// lifetime — the OS reclaims the memory at exit, so there is no real
/// leak and no use-after-destruction window.
/// Not `noexcept`: the one-time `new` can throw `std::bad_alloc`, which
/// must propagate to the caller's try/catch (LoadFromFile et al.) so a
/// config read degrades to defaults rather than terminating the process.
TomlFileCache& TomlCache() {
    static TomlFileCache* const instance = new TomlFileCache();
    return *instance;
}

/// Cached replacement for `toml::parse_file`. Returns a copy of the
/// cached table on hit, fresh parse on miss. Throws to mirror the
/// original `toml::parse_file` semantics — callers' try/catch blocks
/// continue to handle errors uniformly without source changes.
toml::table ParseTomlCached(const std::string& utf8Path) {
    auto cached = TomlCache().Load(utf8Path);
    if (cached) return *cached;
    // Cache miss (file missing or parse error) — re-attempt parse so
    // the caller sees a real toml::parse_error exception rather than
    // swallowed details. Cheap because the cache lookup already
    // confirmed parse failure; the second attempt fails the same way.
    return toml::parse_file(utf8Path);
}

#ifdef _WIN32
/// Named mutex to serialize TOML read-modify-write across processes.
/// Prevents race where Settings deferred save overwrites Macro/ExcludedApps changes.
class ConfigFileLock {
public:
    ConfigFileLock() noexcept {
        hMutex_ = CreateMutexW(nullptr, FALSE, L"Local\\VKeyConfigLock");
        if (hMutex_) {
            DWORD result = WaitForSingleObject(hMutex_, INFINITE);
            // WAIT_OBJECT_0: acquired normally
            // WAIT_ABANDONED: previous owner crashed — we now own it
            owned_ = (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED);
        }
    }
    ~ConfigFileLock() noexcept {
        if (hMutex_) {
            if (owned_) ReleaseMutex(hMutex_);
            CloseHandle(hMutex_);
        }
    }
    ConfigFileLock(const ConfigFileLock&) = delete;
    ConfigFileLock& operator=(const ConfigFileLock&) = delete;
private:
    HANDLE hMutex_ = nullptr;
    bool owned_ = false;
};
#else
class ConfigFileLock {
public:
    ConfigFileLock() noexcept {
        fd_ = ::open("/tmp/vkey_config.lock", O_CREAT | O_RDWR, 0666);
        if (fd_ >= 0) owned_ = (::flock(fd_, LOCK_EX) == 0);
    }
    ~ConfigFileLock() noexcept {
        if (fd_ < 0) return;
        if (owned_) (void)::flock(fd_, LOCK_UN);
        (void)::close(fd_);
    }
    ConfigFileLock(const ConfigFileLock&) = delete;
    ConfigFileLock& operator=(const ConfigFileLock&) = delete;
private:
    int fd_ = -1;
    bool owned_ = false;
};
#endif

/// Load existing TOML file or return empty table (for merge-and-save pattern)
toml::table LoadExistingToml(const std::string& utf8Path) {
    try { return toml::parse_file(utf8Path); } catch (...) { return {}; }
}

/// Write TOML table to file atomically
bool WriteToml(const std::string& utf8Path, const toml::table& tbl) {
    std::string tempPath = utf8Path + ".tmp";
#ifdef _WIN32
    std::ofstream file(Utf8ToWide(tempPath));
#else
    std::ofstream file(tempPath);
#endif
    if (!file.is_open()) return false;
    file << tbl;
    file.close();
    if (file.fail()) return false;

#ifdef _WIN32
    std::wstring wDest = Utf8ToWide(utf8Path);
    std::wstring wTemp = Utf8ToWide(tempPath);
    if (!MoveFileExW(wTemp.c_str(), wDest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(wTemp.c_str());
        return false;
    }
#else
    if (std::rename(tempPath.c_str(), utf8Path.c_str()) != 0) {
        std::remove(tempPath.c_str());
        return false;
    }
#endif
    TomlCache().Invalidate(utf8Path);
    return true;
}

// Security limits — shared across all Load* functions
constexpr uintmax_t kMaxConfigFileSizeBytes = 5 * 1024 * 1024;  // 5 MB
constexpr size_t    kMaxMacroKeyLen         = 32;
constexpr size_t    kMaxMacroValueLen       = 20480;
constexpr size_t    kMaxPerAppEntries       = 256;
constexpr size_t    kMaxAppListEntries      = 1000;

}  // namespace

std::optional<TypingConfig> ConfigManager::LoadFromFile(const std::wstring& path) {
    // Guard: reject config files larger than 5 MB to prevent OOM via crafted macros
    std::error_code sizeEc;
    auto fileSize = std::filesystem::file_size(path, sizeEc);
    if (sizeEc || fileSize > kMaxConfigFileSizeBytes) {
        NEXTKEY_LOG(L"[ConfigManager] Config file too large or unreadable (%llu bytes), using defaults",
                    sizeEc ? 0ULL : static_cast<unsigned long long>(fileSize));
        return std::nullopt;
    }

    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);
        
        TypingConfig config;
        
        // [input] section
        if (auto input = table["input"].as_table()) {
            if (auto method = input->get("method")) {
                std::string methodStr = method->value_or<std::string>("telex");
                if (methodStr == "vni") {
                    config.inputMethod = InputMethod::VNI;
                } else if (methodStr == "simple_telex") {
                    config.inputMethod = InputMethod::SimpleTelex;
                } else if (methodStr == "combined") {
                    config.inputMethod = InputMethod::Combined;
                } else if (methodStr == "user_defined") {
                    config.inputMethod = InputMethod::UserDefined;
                } else {
                    config.inputMethod = InputMethod::Telex;
                }
            }
            if (auto ct = input->get("code_table")) {
                auto val = ct->value_or(0);
                if (val >= 0 && val <= 4) {
                    config.codeTable = static_cast<CodeTable>(val);
                }
            }
        }

        // [UserDefinedKeyMap] section
        if (auto km = table["UserDefinedKeyMap"].as_table()) {
            LoadCustomKeyMap(km, config);
        }

        // [features] section — use node_view [] operator for safe access to optional keys
        if (auto features = table["features"].as_table()) {
            config.spellCheckEnabled = (*features)["spell_check"].value_or(true);
            config.spellSuggestEnabled = (*features)["spell_suggest"].value_or(false);
            config.beepOnSwitch = (*features)["beep_on_switch"].value_or(false);
            config.smartSwitch = (*features)["smart_switch"].value_or(false);
            config.excludeApps = (*features)["exclude_apps"].value_or(false);
            config.tsfApps = (*features)["tsf_apps"].value_or(false);
            config.hidePreeditUnderline =
                (*features)["hide_preedit_underline"].value_or(false);
            config.optimizeLevel = static_cast<uint8_t>(
                (*features)["optimize_level"].value_or(0)
            );
            config.modernOrtho = (*features)["modern_ortho"].value_or(true);
            config.autoCaps = (*features)["auto_caps"].value_or(false);
            config.allowZwjf = (*features)["allow_zwjf"].value_or(false);
            config.autoRestoreEnabled = (*features)["auto_restore"].value_or(true);
            // Default false: opt-in. Most users don't have CJK layouts active.
            config.cjkAutoSwitch = (*features)["cjk_auto_switch"].value_or(false);
            // v3 cleanup: `temp_off_method` / `temp_off_macro_esc` no longer
            // loaded into TypingConfig — `MigrateLegacyHotkeysIfNeeded` reads
            // them directly from TOML when building the registry on first run.
            config.macroEnabled = (*features)["macro_enabled"].value_or(false);
            config.macroInEnglish = (*features)["macro_in_english"].value_or(false);
            config.quickConsonant = (*features)["quick_consonant"].value_or(false);
            config.quickStartConsonant = (*features)["quick_start_consonant"].value_or(false);
            config.quickEndConsonant = (*features)["quick_end_consonant"].value_or(false);
            // escRestoreRawEnabled kept temporarily — TSF EngineController still
            // reads it as the ESC restore-raw gate. Phase 2: route TSF through
            // HotkeyRegistry, then drop both the field and this loader.
            config.escRestoreRawEnabled = (*features)["esc_restore_raw"].value_or(false);
            config.autoCapsMacro = (*features)["auto_caps_macro"].value_or(false);
            config.allowEnglishBypass = (*features)["allow_english_bypass"].value_or(false);
            config.suggestKeepChars = (*features)["suggest_keep_chars"].value_or(false);
            config.debugLogEnabled = (*features)["debug_log"].value_or(false);
            config.enableToast = (*features)["enable_toast"].value_or(true);
            config.macroTriggerSpace = (*features)["macro_trigger_space"].value_or(true);
            config.macroTriggerEnter = (*features)["macro_trigger_enter"].value_or(true);
            config.macroTriggerTab = (*features)["macro_trigger_tab"].value_or(true);
            config.macroTriggerDir = (*features)["macro_trigger_dir"].value_or(true);
            // Spell exclusion prefixes (e.g. ["hđ", "đp"]) — stored pre-lowercased
            if (auto arr = (*features)["spell_exclusions"].as_array()) {
                for (auto& item : *arr) {
                    if (auto str = item.value<std::string>()) {
                        auto wide = Utf8ToWide(*str);
                        if (wide.size() >= 2) {
                            for (auto& ch : wide) ch = towlower(ch);
                            config.spellExclusions.push_back(std::move(wide));
                        }
                    }
                }
            }
        }

        // Hidden TOML `[debug] perf_histogram` — Phase 1 histogram gate.
        // Lives in its own [debug] table (not [features]) so it stays out of
        // the user-facing surface; Settings UI does not enumerate this section.
        // Read outside the [features] block so it works even on configs that
        // omit [features] entirely.
        if (auto dbg = table["debug"].as_table()) {
            config.perfHistogramEnabled = (*dbg)["perf_histogram"].value_or(false);
        }

        return config;
    } catch (const toml::parse_error&) {
        // Fall through to return nullopt
    } catch (const std::exception&) {
        // Fall through to return nullopt
    }
    return std::nullopt;
}

bool ConfigManager::SaveToFile(const std::wstring& path, const TypingConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table input;
        const char* methodStr = "telex";
        if (config.inputMethod == InputMethod::VNI) methodStr = "vni";
        else if (config.inputMethod == InputMethod::SimpleTelex) methodStr = "simple_telex";
        else if (config.inputMethod == InputMethod::Combined) methodStr = "combined";
        else if (config.inputMethod == InputMethod::UserDefined) methodStr = "user_defined";
        input.insert_or_assign("method", methodStr);
        input.insert_or_assign("code_table", static_cast<int64_t>(config.codeTable));
        tbl.insert_or_assign("input", std::move(input));

        // [UserDefinedKeyMap] section
        toml::table km;
        SaveCustomKeyMap(&km, config);
        if (!km.empty()) {
            tbl.insert_or_assign("UserDefinedKeyMap", std::move(km));
        } else {
            tbl.erase("UserDefinedKeyMap");
        }

        // Update [features] section
        toml::table features;
        features.insert_or_assign("spell_check", config.spellCheckEnabled);
        features.insert_or_assign("spell_suggest", config.spellSuggestEnabled);
        features.insert_or_assign("beep_on_switch", config.beepOnSwitch);
        features.insert_or_assign("smart_switch", config.smartSwitch);
        features.insert_or_assign("exclude_apps", config.excludeApps);
        features.insert_or_assign("tsf_apps", config.tsfApps);
        features.insert_or_assign("hide_preedit_underline", config.hidePreeditUnderline);
        features.insert_or_assign("optimize_level", static_cast<int64_t>(config.optimizeLevel));
        features.insert_or_assign("modern_ortho", config.modernOrtho);
        features.insert_or_assign("auto_caps", config.autoCaps);
        features.insert_or_assign("allow_zwjf", config.allowZwjf);
        features.insert_or_assign("auto_restore", config.autoRestoreEnabled);
        features.insert_or_assign("cjk_auto_switch", config.cjkAutoSwitch);
        // v3 cleanup: stop writing `temp_off_method` / `temp_off_macro_esc` —
        // HotkeyRegistry owns these triggers now. Stale entries on disk are
        // harmless (loader ignores them) but we explicitly erase to keep
        // config.toml lean for new saves.
        features.erase("temp_off_method");
        features.erase("temp_off_macro_esc");
        features.erase("temp_off_by_alt");  // Pre-v2 legacy key
        features.insert_or_assign("macro_enabled", config.macroEnabled);
        features.insert_or_assign("macro_in_english", config.macroInEnglish);
        features.insert_or_assign("quick_consonant", config.quickConsonant);
        features.insert_or_assign("quick_start_consonant", config.quickStartConsonant);
        features.insert_or_assign("quick_end_consonant", config.quickEndConsonant);
        features.insert_or_assign("esc_restore_raw", config.escRestoreRawEnabled);
        features.insert_or_assign("auto_caps_macro", config.autoCapsMacro);
        features.insert_or_assign("allow_english_bypass", config.allowEnglishBypass);
        features.insert_or_assign("suggest_keep_chars", config.suggestKeepChars);
        features.insert_or_assign("debug_log", config.debugLogEnabled);
        features.insert_or_assign("enable_toast", config.enableToast);
        features.insert_or_assign("macro_trigger_space", config.macroTriggerSpace);
        features.insert_or_assign("macro_trigger_enter", config.macroTriggerEnter);
        features.insert_or_assign("macro_trigger_tab", config.macroTriggerTab);
        features.insert_or_assign("macro_trigger_dir", config.macroTriggerDir);
        // Spell exclusion prefixes
        toml::array exclArr;
        for (const auto& excl : config.spellExclusions) {
            if (excl.size() >= 2) {
                exclArr.push_back(WideToUtf8(excl));
            }
        }
        features.insert_or_assign("spell_exclusions", std::move(exclArr));
        tbl.insert_or_assign("features", std::move(features));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::wstring ConfigManager::GetConfigPath(void* moduleHint) {
    // Primary: exe directory
    std::wstring exeDir = GetExeDirectory(moduleHint);
    if (DirectoryWritable(exeDir)) {
        return exeDir + L"\\config.toml";
    }
    
    // Fallback: %APPDATA%/VKey/
    std::wstring appDataDir = GetAppDataDirectory();
    return appDataDir + L"\\config.toml";
}

TypingConfig ConfigManager::LoadOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadFromFile(configPath);
    if (config) {
        return *config;
    }
    
    // Return compiled defaults (FR8 - engine autonomy)
    return TypingConfig{};
}

std::wstring ConfigManager::GetExeDirectory(void* moduleHint) {
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(static_cast<HMODULE>(moduleHint), path, MAX_PATH);
    if (len > 0) {
        std::wstring fullPath(path);
        size_t lastSlash = fullPath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return fullPath.substr(0, lastSlash);
        }
    }
#endif
    return L".";
}

std::wstring ConfigManager::GetAppDataDirectory() {
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::wstring appData(path);
        std::wstring vkeyDir = appData + L"\\VKey";

        // Create directory if it doesn't exist
        CreateDirectoryW(vkeyDir.c_str(), nullptr);
        return vkeyDir;
    }
#endif
    return L".";
}

bool ConfigManager::DirectoryWritable(const std::wstring& path) {
#ifdef _WIN32
    // Try to create a temp file
    std::wstring testFile = path + L"\\__vkey_test_write__.tmp";
    HANDLE hFile = CreateFileW(
        testFile.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr
    );
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        return true;
    }
#endif
    return false;
}

std::optional<UIConfig> ConfigManager::LoadUIConfig(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        UIConfig config;

        // [ui] section — use [] for safe access to optional keys
        if (auto ui = table["ui"].as_table()) {
            config.showAdvanced = (*ui)["show_advanced"].value_or(false);
            config.backgroundOpacity = static_cast<uint8_t>(
                (*ui)["background_opacity"].value_or(80)
            );
            config.pinned = (*ui)["pinned"].value_or(false);
        }

        return config;
    } catch (...) {
        return std::nullopt;
    }
}

bool ConfigManager::SaveUIConfig(const std::wstring& path, const UIConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table ui;
        ui.insert_or_assign("show_advanced", config.showAdvanced);
        ui.insert_or_assign("background_opacity", static_cast<int64_t>(config.backgroundOpacity));
        ui.insert_or_assign("pinned", config.pinned);
        tbl.insert_or_assign("ui", std::move(ui));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

UIConfig ConfigManager::LoadUIConfigOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadUIConfig(configPath);
    if (config) {
        return *config;
    }
    return UIConfig{};
}

std::optional<HotkeyConfig> ConfigManager::LoadHotkeyConfig(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        // Default E-V switch = Ctrl+Shift (UniKey-style). Applies both when the
        // [hotkey] table is absent and when ctrl/shift keys are missing inside it
        // (value_or(true) below). An explicit `ctrl=false`/`shift=false` on disk
        // still wins — clearing the switch remains possible.
        HotkeyConfig config;
        config.ctrl  = true;
        config.shift = true;

        if (auto hotkey = table["hotkey"].as_table()) {
            config.ctrl  = (*hotkey)["ctrl"].value_or(true);
            config.shift = (*hotkey)["shift"].value_or(true);
            config.alt   = (*hotkey)["alt"].value_or(false);
            config.win   = (*hotkey)["win"].value_or(false);

            if (auto vkNode = (*hotkey)["vk"]; vkNode.is_integer()) {
                config.vk = static_cast<uint32_t>(vkNode.value_or<int64_t>(0));
            } else {
                // Legacy schema (pre-2026-05): `key = "Z"`. See LegacyKeyCharToVk
                // docs — A-Z/0-9 migrate cleanly, anything else drops to 0 and
                // user rebinds via the new capture overlay.
                auto keyStr = (*hotkey)["key"].value_or<std::string>("");
                if (!keyStr.empty()) {
                    config.vk = LegacyKeyCharToVk(Utf8ToWide(keyStr));
                }
            }
        }

        return config;
    } catch (...) {
        return std::nullopt;
    }
}

bool ConfigManager::SaveHotkeyConfig(const std::wstring& path, const HotkeyConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table hotkey;
        hotkey.insert_or_assign("ctrl",  config.ctrl);
        hotkey.insert_or_assign("shift", config.shift);
        hotkey.insert_or_assign("alt",   config.alt);
        hotkey.insert_or_assign("win",   config.win);
        // New schema: write `vk` as integer. Legacy `key = "..."` is dropped
        // when we replace the sub-table below.
        hotkey.insert_or_assign("vk", static_cast<int64_t>(config.vk));

        tbl.insert_or_assign("hotkey", std::move(hotkey));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

HotkeyConfig ConfigManager::LoadHotkeyConfigOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadHotkeyConfig(configPath);
    if (config) {
        return *config;
    }
    // No config file → default E-V switch = Ctrl+Shift (UniKey-style), matching
    // the in-file default in LoadHotkeyConfig. vk=0 means modifier-combo, no main
    // key — HotkeyManager registers it as a Ctrl+Shift gesture.
    HotkeyConfig defaults;
    defaults.ctrl  = true;
    defaults.shift = true;
    return defaults;
}

// ─────────────────────────── Unified HotkeyRegistry ──────────────────────
// Stored as `[[hotkeys]]` array-of-tables (distinct from `[hotkey]` table
// used by the V/E toggle config above). Each entry: { intent = "...", trigger = { vk, mods, double_tap? } }.

std::optional<HotkeyRegistry> ConfigManager::LoadHotkeyRegistry(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        HotkeyRegistry registry;
        if (auto arr = table["hotkeys"].as_array()) {
            registry.Load(*arr);
        }
        if (auto stateTbl = table["hotkey_state"].as_table()) {
            registry.LoadEnabled(*stateTbl);
        }
        // Sections missing is NOT a failure — caller decides defaults vs. empty.
        return registry;
    } catch (...) {
        return std::nullopt;
    }
}

bool ConfigManager::SaveHotkeyRegistry(const std::wstring& path, const HotkeyRegistry& registry) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::array arr;
        registry.Save(arr);
        tbl.insert_or_assign("hotkeys", std::move(arr));

        toml::table state;
        registry.SaveEnabled(state);
        tbl.insert_or_assign("hotkey_state", std::move(state));

        // TSF mirror: TSF EngineController gates ESC restore-raw on
        // SharedState ESC_RESTORE_RAW, populated from
        // [features].esc_restore_raw via TypingConfig. Without this sync,
        // disabling cancel-composition in the Hotkeys UI doesn't propagate
        // to TSF hosts (Word, Edge in TSF mode, etc.) — TSF would keep
        // restoring raw keys on Esc. Mirror reflects "Esc bare-tap is bound
        // to CancelComposition AND the intent is enabled".
        // Phase 2: route TSF through HotkeyRegistry directly and drop both
        // the mirror and the legacy field.
        constexpr uint32_t kVkEscape = 0x1B;
        const bool escIsCancelTrigger = registry.Matches(
            Intent::CancelComposition, kVkEscape, /*mods=*/0,
            /*isDoubleTap=*/false, /*keyUp=*/false);
        if (auto* features = tbl["features"].as_table()) {
            features->insert_or_assign("esc_restore_raw", escIsCancelTrigger);
        } else {
            toml::table newFeatures;
            newFeatures.insert_or_assign("esc_restore_raw", escIsCancelTrigger);
            tbl.insert_or_assign("features", std::move(newFeatures));
        }

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

namespace {

// "User has touched the v3 schema" sentinel — presence of `[hotkey_state]`
// in config.toml. SaveHotkeyRegistry always writes it, so once the user has
// opened the Hotkeys dialog or migration ran, this returns true on disk.
// Used by both LoadHotkeyRegistryOrDefault and MigrateLegacyHotkeysIfNeeded
// to distinguish "fresh / pre-v3 install" from "user explicitly cleared".
[[nodiscard]] bool HasHotkeyStateSection(const std::wstring& path) noexcept {
    try {
        auto table = ParseTomlCached(WideToUtf8(path));
        return table.contains("hotkey_state");
    } catch (...) {
        return false;
    }
}

}  // namespace

HotkeyRegistry ConfigManager::LoadHotkeyRegistryOrDefault() {
    const std::wstring configPath = GetConfigPath();
    auto registry = LoadHotkeyRegistry(configPath);
    if (!registry) {
        return HotkeyRegistry::Defaults();
    }
    // If the user has the v3 schema on disk, honor what's there — including an
    // explicit empty registry (= "I cleared everything intentionally").
    if (HasHotkeyStateSection(configPath)) {
        return *registry;
    }
    // No state section → never-touched-new-UI install. Empty result here means
    // either fresh install or pre-v3 with no [[hotkeys]] — both cases want
    // factory bindings as the UI's starting state.
    const bool empty =
        registry->TriggersFor(Intent::CancelComposition).empty() &&
        registry->TriggersFor(Intent::SkipMacro).empty() &&
        registry->TriggersFor(Intent::ToggleEnabled).empty();
    if (empty) {
        return HotkeyRegistry::Defaults();
    }
    return *registry;
}

namespace {

/// Read the 3 pre-v3 hotkey toggles from the `[features]` TOML table without
/// going through TypingConfig (those fields were dropped in v3 cleanup).
/// Returns all-default values when file/section is unreadable.
struct LegacyHotkeyToggles {
    bool    escRestoreRaw    = false;
    bool    tempOffMacroEsc  = false;
    uint8_t tempOffMethod    = 0;  // 0=None, 1=DupAlt, 2=Ctrl
};

[[nodiscard]] LegacyHotkeyToggles ReadLegacyHotkeyToggles(const std::wstring& path) noexcept {
    LegacyHotkeyToggles out;
    try {
        auto table = ParseTomlCached(WideToUtf8(path));
        if (auto features = table["features"].as_table()) {
            out.escRestoreRaw   = (*features)["esc_restore_raw"].value_or(false);
            out.tempOffMacroEsc = (*features)["temp_off_macro_esc"].value_or(false);
            if (auto m = (*features)["temp_off_method"].value<int64_t>()) {
                int v = static_cast<int>(*m);
                if (v >= 0 && v <= 2) out.tempOffMethod = static_cast<uint8_t>(v);
            } else if ((*features)["temp_off_by_alt"].value_or(false)) {
                // Pre-v2 fallback key.
                out.tempOffMethod = 1;  // DupAlt
            }
        }
    } catch (...) {
        // Defaults already set on `out`.
    }
    return out;
}

}  // namespace

HotkeyRegistry ConfigManager::MigrateLegacyHotkeysIfNeeded(const std::wstring& path) {
    // Fresh install (no config file) → factory defaults, nothing to persist.
    if (!std::filesystem::exists(path)) {
        return HotkeyRegistry::Defaults();
    }

    // User has v3 schema on disk → honor as-is, including explicit empty.
    // SaveHotkeyRegistry writes `[hotkey_state]` on every save, so anyone who
    // has used the new Hotkeys UI passes through here without further migration.
    if (HasHotkeyStateSection(path)) {
        return LoadHotkeyRegistry(path).value_or(HotkeyRegistry{});
    }

    // [hotkey_state] missing → pre-v3 install. If existing bindings sit in the
    // [[hotkeys]] array (e.g., from earlier buggy persists or partial migration),
    // honor them. Otherwise fall through to legacy-field migration.
    auto existing = LoadHotkeyRegistry(path);
    const bool populated = existing && (
        !existing->TriggersFor(Intent::CancelComposition).empty() ||
        !existing->TriggersFor(Intent::SkipMacro).empty() ||
        !existing->TriggersFor(Intent::ToggleEnabled).empty());
    if (populated) {
        return *existing;
    }

    // Empty registry on disk (either `[[hotkeys]]` absent OR present-but-empty).
    // Without a distinguishing sentinel we can't tell "explicit user clear"
    // from "leftover from a buggy earlier persist" — and the latter actually
    // happened in pre-758f2b4 builds. Pragmatic v1 choice: derive from legacy
    // toggles, fall back to Defaults() when legacy fields are all at v2 defaults.
    // The UI then matches what runtime fires; users who genuinely want "no
    // shortcuts" can delete each binding individually (a future schema sentinel
    // can recover the explicit-clear semantic).
    const auto legacy = ReadLegacyHotkeyToggles(path);
    const bool legacyAllDefault = !legacy.escRestoreRaw
                               && !legacy.tempOffMacroEsc
                               && legacy.tempOffMethod == 0;
    auto migrated = legacyAllDefault
        ? HotkeyRegistry::Defaults()
        : HotkeyRegistry::FromLegacyFields(
              legacy.escRestoreRaw, legacy.tempOffMacroEsc, legacy.tempOffMethod);
    NEXTKEY_LOG(L"[ConfigManager] Migrated v2→v3 hotkeys (esc=%d macro=%d method=%u legacyDefault=%d)",
                legacy.escRestoreRaw, legacy.tempOffMacroEsc,
                static_cast<unsigned>(legacy.tempOffMethod), legacyAllDefault);
    if (!SaveHotkeyRegistry(path, migrated)) {
        NEXTKEY_LOG(L"[ConfigManager] Failed to persist migrated hotkeys to %s", path.c_str());
    }
    return migrated;
}

std::vector<std::wstring> ConfigManager::LoadAllExcludedApps(const std::wstring& path) {
    std::vector<std::wstring> apps;
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        auto lower = [](std::wstring s) {
            for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
            return s;
        };
        auto mergeUnique = [&](std::wstring wide) {
            if (apps.size() >= kMaxAppListEntries) return;
            wide = lower(std::move(wide));
            if (std::find(apps.begin(), apps.end(), wide) == apps.end()) {
                apps.push_back(std::move(wide));
            }
        };

        if (auto section = table["excluded_apps"].as_table()) {
            // Load [excluded_apps].list
            if (auto arr = (*section)["list"].as_array()) {
                for (auto& item : *arr) {
                    if (apps.size() >= kMaxAppListEntries) break;
                    if (auto str = item.value<std::string>()) {
                        apps.push_back(Utf8ToWide(*str));
                    }
                }
            }
            // Backward compat: merge [excluded_apps].soft into the same list
            if (auto arr = (*section)["soft"].as_array()) {
                for (auto& item : *arr) {
                    if (apps.size() >= kMaxAppListEntries) break;
                    if (auto str = item.value<std::string>()) {
                        auto wide = Utf8ToWide(*str);
                        if (std::find(apps.begin(), apps.end(), wide) == apps.end()) {
                            apps.push_back(std::move(wide));
                        }
                    }
                }
            }
        }

        // Pre-v3 (Feb 2026, 713f862f) schema used `[excludedApps]` (camelCase),
        // gated on its own `enabled` flag. That rename dropped the list itself —
        // unlike hotkeys (MigrateLegacyHotkeysIfNeeded), nothing ever carried the
        // old entries over, so any app added before the rename silently fell out
        // of enforcement (#224: user's live `[excluded_apps]` lacked an app that
        // was still excluded per the old `[excludedApps]`, so VKey kept processing
        // keystrokes in it). Merge forever on load (mirrors the `soft` merge
        // above) — lowercased, since old-UI entries predate the lowercase-on-save
        // normalization ExcludedAppsDialog applies today.
        if (auto legacy = table["excludedApps"].as_table()) {
            const auto* enabledNode = legacy->get_as<bool>("enabled");
            if (!enabledNode || enabledNode->get()) {
                if (auto arr = (*legacy)["list"].as_array()) {
                    for (auto& item : *arr) {
                        if (apps.size() >= kMaxAppListEntries) break;
                        if (auto str = item.value<std::string>()) {
                            mergeUnique(Utf8ToWide(*str));
                        }
                    }
                }
            }
        }
    } catch (...) {}
    return apps;
}

bool ConfigManager::SaveExcludedApps(const std::wstring& path,
                                      const std::vector<std::wstring>& apps) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::array arr;
        for (auto& app : apps) {
            arr.push_back(WideToUtf8(app));
        }
        // Read-modify-write the [excluded_apps] section so the sibling
        // `force_vn` array (per-app hard-V list) survives an E-list save.
        // Drop the legacy `soft` key — it is merged into `list` on load.
        if (auto* section = tbl["excluded_apps"].as_table()) {
            section->insert_or_assign("list", std::move(arr));
            section->erase("soft");
        } else {
            toml::table newSection;
            newSection.insert_or_assign("list", std::move(arr));
            tbl.insert_or_assign("excluded_apps", std::move(newSection));
        }
        // Drop the pre-v3 `[excludedApps]` section — its `list` is merged in
        // on every load (see LoadAllExcludedApps), so once a save round-trips
        // that merge into `[excluded_apps]`, the old section is dead weight.
        tbl.erase("excludedApps");

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::vector<std::wstring> ConfigManager::LoadForcedVnApps(const std::wstring& path) {
    std::vector<std::wstring> apps;
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        // [excluded_apps].force_vn — apps locked to Vietnamese (hard-V).
        if (auto section = table["excluded_apps"].as_table()) {
            if (auto arr = (*section)["force_vn"].as_array()) {
                for (auto& item : *arr) {
                    if (apps.size() >= kMaxAppListEntries) break;
                    if (auto str = item.value<std::string>()) {
                        apps.push_back(Utf8ToWide(*str));
                    }
                }
            }
        }
    } catch (...) {}
    return apps;
}

bool ConfigManager::SaveForcedVnApps(const std::wstring& path,
                                      const std::vector<std::wstring>& apps) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::array arr;
        for (auto& app : apps) {
            arr.push_back(WideToUtf8(app));
        }
        // Read-modify-write so the sibling `list` (E) array survives a V save.
        if (auto* section = tbl["excluded_apps"].as_table()) {
            section->insert_or_assign("force_vn", std::move(arr));
        } else {
            toml::table newSection;
            newSection.insert_or_assign("force_vn", std::move(arr));
            tbl.insert_or_assign("excluded_apps", std::move(newSection));
        }

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::unordered_map<std::wstring, bool>
ConfigManager::LoadSmartSwitchApps(const std::wstring& path) {
    std::unordered_map<std::wstring, bool> apps;
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        auto lower = [](std::wstring s) {
            for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
            return s;
        };

        // 1. V2 schema: [smart_switch.apps] inline map.
        if (auto smart = table["smart_switch"].as_table()) {
            if (auto appsTbl = (*smart)["apps"].as_table()) {
                bool warnedUnknown = false;
                bool warnedCap = false;
                for (const auto& [keyView, node] : *appsTbl) {
                    if (apps.size() >= kMaxAppListEntries) {
                        if (!warnedCap) {
                            NEXTKEY_LOG(L"[ConfigManager] LoadSmartSwitchApps: cap %zu hit, surplus dropped",
                                        kMaxAppListEntries);
                            warnedCap = true;
                        }
                        break;
                    }
                    auto modeStr = node.value<std::string>();
                    if (!modeStr) continue;
                    bool isVietnamese;
                    if (*modeStr == "english")          isVietnamese = false;
                    else if (*modeStr == "vietnamese")  isVietnamese = true;
                    else {
                        if (!warnedUnknown) {
                            NEXTKEY_LOG(L"[ConfigManager] LoadSmartSwitchApps: unknown mode string, entry skipped");
                            warnedUnknown = true;
                        }
                        continue;
                    }
                    auto wideKey = lower(Utf8ToWide(std::string(keyView.str())));
                    if (wideKey.empty()) continue;
                    apps.emplace(std::move(wideKey), isVietnamese);
                }
                return apps;
            }

            // 2. Legacy fallback: [smart_switch].english_mode_apps array.
            //    One-time silent migration — first save will emit V2 schema
            //    and the legacy section is dropped on full-rewrite.
            if (auto arr = (*smart)["english_mode_apps"].as_array()) {
                for (auto& item : *arr) {
                    if (apps.size() >= kMaxAppListEntries) break;
                    if (auto str = item.value<std::string>()) {
                        auto wideKey = lower(Utf8ToWide(*str));
                        if (!wideKey.empty()) apps.emplace(std::move(wideKey), false);
                    }
                }
            }
        }
    } catch (...) {
        // 3. Parse error / missing file → empty map (never throw to caller).
    }
    return apps;
}

bool ConfigManager::SaveSmartSwitchApps(
    const std::wstring& path,
    const std::unordered_map<std::wstring, bool>& apps) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        // Sort keys alphabetically for deterministic output (clean diffs).
        std::vector<const std::wstring*> sorted;
        sorted.reserve(apps.size());
        for (const auto& [k, _] : apps) sorted.push_back(&k);
        std::sort(sorted.begin(), sorted.end(),
                  [](const std::wstring* a, const std::wstring* b) {
                      return *a < *b;
                  });

        toml::table appsTbl;
        for (const auto* keyPtr : sorted) {
            auto it = apps.find(*keyPtr);
            if (it == apps.end()) continue;
            appsTbl.insert(WideToUtf8(*keyPtr),
                           std::string(it->second ? "vietnamese" : "english"));
        }

        // Replace the entire [smart_switch] subtable. This drops the legacy
        // `english_mode_apps` array on first save after migration (clean
        // break per anh's 2026-05-28 decision).
        toml::table smartSection;
        smartSection.insert("apps", std::move(appsTbl));
        tbl.insert_or_assign("smart_switch", std::move(smartSection));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::vector<std::wstring> ConfigManager::LoadTsfApps(const std::wstring& path) {
    std::vector<std::wstring> apps;
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        if (auto section = table["tsf_apps"].as_table()) {
            if (auto arr = (*section)["list"].as_array()) {
                for (auto& item : *arr) {
                    if (apps.size() >= kMaxAppListEntries) break;
                    if (auto str = item.value<std::string>()) {
                        apps.push_back(Utf8ToWide(*str));
                    }
                }
            }
        }
    } catch (...) {}
    return apps;
}

bool ConfigManager::SaveTsfApps(const std::wstring& path, const std::vector<std::wstring>& apps) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::array arr;
        for (auto& app : apps) {
            arr.push_back(WideToUtf8(app));
        }
        toml::table section;
        section.insert_or_assign("list", std::move(arr));
        tbl.insert_or_assign("tsf_apps", std::move(section));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::unordered_map<std::wstring, AppOverrideEntry> ConfigManager::LoadAppOverrides(const std::wstring& path) {
    std::unordered_map<std::wstring, AppOverrideEntry> data;
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        size_t entryCount = 0;

        if (auto section = table["app_overrides"].as_table()) {
            for (auto& [key, val] : *section) {
                if (++entryCount > kMaxPerAppEntries) {
                    NEXTKEY_LOG(L"[ConfigManager] Per-app table exceeds limit (%zu), truncating",
                                kMaxPerAppEntries);
                    break;
                }
                if (auto* entry = val.as_table()) {
                    AppOverrideEntry e;
                    e.inputMethod = static_cast<int8_t>((*entry)["input_method"].value_or(-1));
                    e.encodingOverride = static_cast<int8_t>((*entry)["encoding"].value_or(-1));
                    e.sendMethod = static_cast<int8_t>((*entry)["send_method"].value_or(-1));
                    data[Utf8ToWide(std::string(key.str()))] = e;
                }
            }
        }
    } catch (...) {}
    return data;
}

bool ConfigManager::SaveAppOverrides(const std::wstring& path,
                                      const std::unordered_map<std::wstring, AppOverrideEntry>& entries) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table section;
        for (auto& [exe, e] : entries) {
            toml::table entry;
            entry.insert_or_assign("input_method", static_cast<int64_t>(e.inputMethod));
            entry.insert_or_assign("encoding", static_cast<int64_t>(e.encodingOverride));
            entry.insert_or_assign("send_method", static_cast<int64_t>(e.sendMethod));
            section.insert_or_assign(WideToUtf8(exe), std::move(entry));
        }
        tbl.insert_or_assign("app_overrides", std::move(section));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

std::optional<SystemConfig> ConfigManager::LoadSystemConfig(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        SystemConfig config;

        if (auto system = table["system"].as_table()) {
            config.runAtStartup = (*system)["run_at_startup"].value_or(false);
            config.runAsAdmin = (*system)["run_as_admin"].value_or(false);
            config.showOnStartup = (*system)["show_on_startup"].value_or(false);
            config.desktopShortcut = (*system)["desktop_shortcut"].value_or(false);
            config.language = static_cast<uint8_t>((*system)["language"].value_or(0));
            config.iconStyle = static_cast<uint8_t>((*system)["icon_style"].value_or(0));
            config.customColorV = static_cast<uint32_t>((*system)["custom_color_v"].value_or(int64_t(0)));
            config.customColorE = static_cast<uint32_t>((*system)["custom_color_e"].value_or(int64_t(0)));
            config.showTsfIndicator = (*system)["show_tsf_indicator"].value_or(false);
            config.showFloatingIcon = (*system)["show_floating_icon"].value_or(false);
            config.floatingIconX = static_cast<int32_t>((*system)["floating_icon_x"].value_or(int64_t(INT32_MIN)));
            config.floatingIconY = static_cast<int32_t>((*system)["floating_icon_y"].value_or(int64_t(INT32_MIN)));
            config.autoCheckUpdate = (*system)["auto_check_update"].value_or(true);
            config.startupMode = static_cast<uint8_t>((*system)["startup_mode"].value_or(0));
            config.forceLightTheme = (*system)["force_light_theme"].value_or(false);
            config.watchdogEnabled = (*system)["watchdog_enabled"].value_or(false);
            config.browserExtensionEnabled = (*system)["browser_extension_enabled"].value_or(false);
        }

        return config;
    } catch (...) {
        return std::nullopt;
    }
}

bool ConfigManager::SaveSystemConfig(const std::wstring& path, const SystemConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table system;
        system.insert_or_assign("run_at_startup", config.runAtStartup);
        system.insert_or_assign("run_as_admin", config.runAsAdmin);
        system.insert_or_assign("show_on_startup", config.showOnStartup);
        system.insert_or_assign("desktop_shortcut", config.desktopShortcut);
        system.insert_or_assign("language", static_cast<int64_t>(config.language));
        system.insert_or_assign("icon_style", static_cast<int64_t>(config.iconStyle));
        system.insert_or_assign("custom_color_v", static_cast<int64_t>(config.customColorV));
        system.insert_or_assign("custom_color_e", static_cast<int64_t>(config.customColorE));
        system.insert_or_assign("show_tsf_indicator", config.showTsfIndicator);
        system.insert_or_assign("show_floating_icon", config.showFloatingIcon);
        system.insert_or_assign("floating_icon_x", static_cast<int64_t>(config.floatingIconX));
        system.insert_or_assign("floating_icon_y", static_cast<int64_t>(config.floatingIconY));
        system.insert_or_assign("auto_check_update", config.autoCheckUpdate);
        system.insert_or_assign("startup_mode", static_cast<int64_t>(config.startupMode));
        system.insert_or_assign("force_light_theme", config.forceLightTheme);
        system.insert_or_assign("watchdog_enabled", config.watchdogEnabled);
        system.insert_or_assign("browser_extension_enabled", config.browserExtensionEnabled);
        tbl.insert_or_assign("system", std::move(system));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

SystemConfig ConfigManager::LoadSystemConfigOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadSystemConfig(configPath);
    if (config) {
        return *config;
    }
    return SystemConfig{};
}

std::optional<ConvertConfig> ConfigManager::LoadConvertConfig(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        ConvertConfig config;

        if (auto convert = table["convert"].as_table()) {
            config.allCaps = (*convert)["all_caps"].value_or(false);
            config.allLower = (*convert)["all_lower"].value_or(false);
            config.capsFirst = (*convert)["caps_first"].value_or(false);
            config.capsEach = (*convert)["caps_each"].value_or(false);
            config.removeMark = (*convert)["remove_mark"].value_or(false);
            config.alertDone = (*convert)["alert_done"].value_or(false);
            config.autoPaste = (*convert)["auto_paste"].value_or(false);
            config.sequential = (*convert)["sequential"].value_or(false);
            config.enableLog = (*convert)["enable_log"].value_or(false);
            config.sourceEncoding = static_cast<uint8_t>(
                (*convert)["source_encoding"].value_or(0));
            config.destEncoding = static_cast<uint8_t>(
                (*convert)["dest_encoding"].value_or(0));

            // Nested [convert.hotkey] table
            if (auto hk = (*convert)["hotkey"].as_table()) {
                config.hotkey.ctrl  = (*hk)["ctrl"].value_or(false);
                config.hotkey.shift = (*hk)["shift"].value_or(false);
                config.hotkey.alt   = (*hk)["alt"].value_or(false);
                config.hotkey.win   = (*hk)["win"].value_or(false);

                // New schema: `vk = <integer VK_*>`. Preferred.
                if (auto vkNode = (*hk)["vk"]; vkNode.is_integer()) {
                    config.hotkey.vk = static_cast<uint32_t>(vkNode.value_or<int64_t>(0));
                } else {
                    // Legacy schema (pre-2026-05): `key = "Z"` (single char).
                    // Clean migration — A-Z/0-9 only; OEM punctuation drops to
                    // vk=0 and user must rebind via new capture overlay.
                    auto keyStr = (*hk)["key"].value_or<std::string>("");
                    if (!keyStr.empty()) {
                        config.hotkey.vk = LegacyKeyCharToVk(Utf8ToWide(keyStr));
                    }
                }
            }
        }

        return config;
    } catch (...) {
        return std::nullopt;
    }
}

bool ConfigManager::SaveConvertConfig(const std::wstring& path, const ConvertConfig& config) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table convert;
        convert.insert_or_assign("all_caps", config.allCaps);
        convert.insert_or_assign("all_lower", config.allLower);
        convert.insert_or_assign("caps_first", config.capsFirst);
        convert.insert_or_assign("caps_each", config.capsEach);
        convert.insert_or_assign("remove_mark", config.removeMark);
        convert.insert_or_assign("alert_done", config.alertDone);
        convert.insert_or_assign("auto_paste", config.autoPaste);
        convert.insert_or_assign("sequential", config.sequential);
        convert.insert_or_assign("enable_log", config.enableLog);
        convert.insert_or_assign("source_encoding", static_cast<int64_t>(config.sourceEncoding));
        convert.insert_or_assign("dest_encoding", static_cast<int64_t>(config.destEncoding));

        toml::table hotkey;
        hotkey.insert_or_assign("ctrl", config.hotkey.ctrl);
        hotkey.insert_or_assign("shift", config.hotkey.shift);
        hotkey.insert_or_assign("alt", config.hotkey.alt);
        hotkey.insert_or_assign("win", config.hotkey.win);

        // New schema: write `vk` as integer. The legacy `key = "..."` field
        // (pre-2026-05 schema) is dropped automatically because we replace the
        // entire `hotkey` sub-table below — no need to explicitly erase it.
        hotkey.insert_or_assign("vk", static_cast<int64_t>(config.hotkey.vk));

        convert.insert_or_assign("hotkey", std::move(hotkey));
        tbl.insert_or_assign("convert", std::move(convert));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

ConvertConfig ConfigManager::LoadConvertConfigOrDefault() {
    std::wstring configPath = GetConfigPath();
    auto config = LoadConvertConfig(configPath);
    if (config) {
        return *config;
    }
    return ConvertConfig{};
}

std::unordered_map<std::wstring, std::wstring> ConfigManager::LoadMacros(const std::wstring& path) {
    std::unordered_map<std::wstring, std::wstring> data;
    // Guard: reject files larger than 1 MB — a large macro section triggers OOM when
    // HookEngine allocates SendInput arrays proportional to the expansion length.
    std::error_code sizeEc;
    auto fileSize = std::filesystem::file_size(path, sizeEc);
    if (sizeEc || fileSize > kMaxConfigFileSizeBytes) {
        NEXTKEY_LOG(L"[ConfigManager] Macro file too large or unreadable (%llu bytes), skipping macros",
                    sizeEc ? 0ULL : static_cast<unsigned long long>(fileSize));
        return data;
    }
    try {
        std::string utf8Path = WideToUtf8(path);
        auto table = ParseTomlCached(utf8Path);

        if (auto section = table["macros"].as_table()) {
            for (auto& [key, val] : *section) {
                auto str = val.value<std::string>();
                if (!str) continue;

                if (key.str().size() > kMaxMacroKeyLen || str->size() > kMaxMacroValueLen) {
                    NEXTKEY_LOG(L"[ConfigManager] Macro entry too long, skipping (key=%zu, value=%zu)",
                                key.str().size(), str->size());
                    continue;
                }
                data[Utf8ToWide(std::string(key.str()))] = Utf8ToWide(*str);
            }
        }
    } catch (...) {}
    return data;
}

bool ConfigManager::SaveMacros(const std::wstring& path,
                                const std::unordered_map<std::wstring, std::wstring>& macros) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);

        toml::table section;
        for (auto& [key, value] : macros) {
            section.insert_or_assign(WideToUtf8(key), WideToUtf8(value));
        }
        tbl.insert_or_assign("macros", std::move(section));

        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

void ConfigManager::LoadCustomKeyMap(const void* table_ptr, TypingConfig& config) {
    const auto* km = static_cast<const toml::table*>(table_ptr);
    if (!km) return;

    for (auto& [key, val] : *km) {
        if (key.str().empty()) continue;
        wchar_t k = static_cast<wchar_t>(key.str()[0]);
        if (k >= 128) continue;

        if (auto actionStr = val.value<std::string>()) {
            config.customKeyMap[static_cast<uint8_t>(k)] = StringToTypingAction(*actionStr);
        }
    }
}

void ConfigManager::SaveCustomKeyMap(void* table_ptr, const TypingConfig& config) {
    auto* km = static_cast<toml::table*>(table_ptr);
    if (!km) return;

    for (size_t i = 0; i < 128; ++i) {
        TypingAction action = config.customKeyMap[i];
        if (action != TypingAction::None) {
            char key[2] = { static_cast<char>(i), '\0' };
            km->insert_or_assign(key, std::string(TypingActionToString(action)));
        }
    }
}

bool ConfigManager::ImportCustomKeyMap(const std::wstring& path, TypingConfig& config) {
    try {
        auto tbl = ParseTomlCached(WideToUtf8(path));
        LoadCustomKeyMap(&tbl, config);
        return true;
    } catch (...) {
        return false;
    }
}

bool ConfigManager::ExportCustomKeyMap(const std::wstring& path, const TypingConfig& config) {
    try {
        toml::table tbl;
        SaveCustomKeyMap(&tbl, config);
#ifdef _WIN32
        std::ofstream file(path);
#else
        std::ofstream file(WideToUtf8(path));
#endif
        if (file.is_open()) {
            file << tbl;
            file.close();
            return true;
        }
    } catch (...) {}
    return false;
}

std::string ConfigManager::FormatConfigTomlForLexicon(
    const std::wstring& configPath,
    bool spellSuggestEnabled,
    const std::vector<std::wstring>& spellExclusions,
    uint64_t wireGeneration) {
    try {
        std::string utf8Path = WideToUtf8(configPath);
        auto tbl = LoadExistingToml(utf8Path);

        toml::array exclArr;
        for (const auto& excl : spellExclusions) {
            exclArr.push_back(WideToUtf8(excl));
        }

        if (auto* features = tbl["features"].as_table()) {
            features->insert_or_assign("spell_suggest", spellSuggestEnabled);
            features->insert_or_assign("spell_exclusions", std::move(exclArr));
        } else {
            toml::table newFeatures;
            newFeatures.insert_or_assign("spell_suggest", spellSuggestEnabled);
            newFeatures.insert_or_assign("spell_exclusions", std::move(exclArr));
            tbl.insert_or_assign("features", std::move(newFeatures));
        }

        if (wireGeneration > 0) {
            if (auto* internalTbl = tbl["internal"].as_table()) {
                internalTbl->insert_or_assign("wire_generation", static_cast<int64_t>(wireGeneration));
            } else {
                toml::table newInternal;
                newInternal.insert_or_assign("wire_generation", static_cast<int64_t>(wireGeneration));
                tbl.insert_or_assign("internal", std::move(newInternal));
            }
        }

        std::stringstream ss;
        ss << tbl;
        return ss.str();
    } catch (...) {
        return "";
    }
}

uint64_t ConfigManager::LoadWireGeneration(const std::wstring& path) {
    try {
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);
        if (auto* internalTbl = tbl["internal"].as_table()) {
            if (auto* genVal = (*internalTbl)["wire_generation"].as_integer()) {
                if (genVal->get() > 0) {
                    return static_cast<uint64_t>(genVal->get());
                }
            }
        }
    } catch (...) {}
    return 1;
}

bool ConfigManager::SaveWireGeneration(const std::wstring& path, uint64_t generation) {
    try {
        ConfigFileLock lock;
        std::string utf8Path = WideToUtf8(path);
        auto tbl = LoadExistingToml(utf8Path);
        if (auto* internalTbl = tbl["internal"].as_table()) {
            internalTbl->insert_or_assign("wire_generation", static_cast<int64_t>(generation));
        } else {
            toml::table newInternal;
            newInternal.insert_or_assign("wire_generation", static_cast<int64_t>(generation));
            tbl.insert_or_assign("internal", std::move(newInternal));
        }
        return WriteToml(utf8Path, tbl);
    } catch (...) {
        return false;
    }
}

}  // namespace NextKey
