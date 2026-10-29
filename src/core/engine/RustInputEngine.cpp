// VKey - Rust engine adapter
// Copyright (c) 2024-2026 PhatMT. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-only

#include "RustInputEngine.h"

#include "RustEngineLoader.h"
#include "core/config/SpellExclusionCanonicalizer.h"

#include "vkey_engine.h"  // vendored C ABI (extern/vkey_engine/include)

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE  // dladdr
#  endif
#  include <dlfcn.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace NextKey {
namespace {

// Resolved entry points of the prebuilt library. Loaded once, lazily.
struct EngineApi {
    VKeyEngine* (*create)(uint32_t, uint32_t) = nullptr;
    void (*destroy)(VKeyEngine*) = nullptr;
    void (*reset)(VKeyEngine*) = nullptr;
    void (*push_char)(VKeyEngine*, uint32_t) = nullptr;
#if VKEY_ENGINE_ABI_VERSION >= 9u
    // ABI v9: physical scalar and output-case intent are independent.
    void (*push_key)(VKeyEngine*, uint32_t, bool) = nullptr;
#endif
    void (*backspace)(VKeyEngine*) = nullptr;
    size_t (*peek_utf16)(const VKeyEngine*, uint16_t*, size_t) = nullptr;
    size_t (*commit_utf16)(VKeyEngine*, uint16_t*, size_t) = nullptr;
    size_t (*count)(const VKeyEngine*) = nullptr;
    uint32_t (*abi_version)(void) = nullptr;
    uint32_t (*runtime_status)(void) = nullptr;
    // ABI v2 host query surface.
    bool (*is_english_word)(const VKeyEngine*) = nullptr;
    bool (*is_tone_escaped)(const VKeyEngine*) = nullptr;
    bool (*has_active_quick_consonant)(const VKeyEngine*) = nullptr;
    size_t (*peek_raw_utf16)(const VKeyEngine*, uint16_t*, size_t) = nullptr;
    bool (*seed_text_utf16)(VKeyEngine*, const uint16_t*, size_t) = nullptr;
    // ABI v3 host query surface.
    bool (*last_commit_was_corrected)(const VKeyEngine*) = nullptr;
    // ABI v4: user-defined custom keymap.
    void (*set_custom_keymap)(VKeyEngine*, const uint8_t*, size_t) = nullptr;
    // ABI v5: spell-check exclusions. Process-global (no engine handle) --
    // must be called before create() to affect the engine being constructed.
    bool (*set_spell_exclusions_utf16)(const uint16_t*, size_t) = nullptr;
    // ABI v7: bounded committed-word replay eligibility.
    bool (*should_replay_key_after_raw_utf16)(
        const VKeyEngine*, const uint16_t*, size_t, uint32_t) = nullptr;
#if VKEY_ENGINE_ABI_VERSION >= 8u
    // ABI v8: immutable user exact-protection dictionary.
    VKeyUserDictionary* (*user_dictionary_create_utf16)(
        const uint16_t*, size_t, uint32_t*, size_t*) = nullptr;
    void (*user_dictionary_destroy)(VKeyUserDictionary*) = nullptr;
    bool (*set_user_dictionary)(VKeyEngine*, const VKeyUserDictionary*) = nullptr;
#endif
    bool ok = false;
    std::wstring reason;  // diagnostic when !ok; empty when ok
};

template <typename Fn>
Fn Resolve(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<Fn>(::GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return reinterpret_cast<Fn>(::dlsym(lib, name));
#endif
}

const EngineApi& Api() {
    // Trust verification and symbol resolution happen once per process, never per key.
    static const EngineApi api = [] {
        EngineApi a;
        RustEngineLibraryResult loaded = LoadRustEngineLibrary();
        if (!loaded.handle) {
            a.reason = std::move(loaded.reason);
            return a;
        }
        void* lib = loaded.handle;
        a.create = Resolve<decltype(a.create)>(lib, "vkey_engine_create");
        a.destroy = Resolve<decltype(a.destroy)>(lib, "vkey_engine_destroy");
        a.reset = Resolve<decltype(a.reset)>(lib, "vkey_engine_reset");
        a.push_char = Resolve<decltype(a.push_char)>(lib, "vkey_engine_push_char");
#if VKEY_ENGINE_ABI_VERSION >= 9u
        a.push_key = Resolve<decltype(a.push_key)>(lib, "vkey_engine_push_key");
#endif
        a.backspace = Resolve<decltype(a.backspace)>(lib, "vkey_engine_backspace");
        a.peek_utf16 = Resolve<decltype(a.peek_utf16)>(lib, "vkey_engine_peek_utf16");
        a.commit_utf16 = Resolve<decltype(a.commit_utf16)>(lib, "vkey_engine_commit_utf16");
        a.count = Resolve<decltype(a.count)>(lib, "vkey_engine_count");
        a.abi_version = Resolve<decltype(a.abi_version)>(lib, "vkey_engine_abi_version");
        a.runtime_status =
            Resolve<decltype(a.runtime_status)>(lib, "vkey_engine_runtime_status");
        a.is_english_word =
            Resolve<decltype(a.is_english_word)>(lib, "vkey_engine_is_english_word");
        a.is_tone_escaped =
            Resolve<decltype(a.is_tone_escaped)>(lib, "vkey_engine_is_tone_escaped");
        a.has_active_quick_consonant = Resolve<decltype(a.has_active_quick_consonant)>(
            lib, "vkey_engine_has_active_quick_consonant");
        a.peek_raw_utf16 =
            Resolve<decltype(a.peek_raw_utf16)>(lib, "vkey_engine_peek_raw_utf16");
        a.seed_text_utf16 =
            Resolve<decltype(a.seed_text_utf16)>(lib, "vkey_engine_seed_text_utf16");
        a.last_commit_was_corrected = Resolve<decltype(a.last_commit_was_corrected)>(
            lib, "vkey_engine_last_commit_was_corrected");
        a.set_custom_keymap = Resolve<decltype(a.set_custom_keymap)>(
            lib, "vkey_engine_set_custom_keymap");
        a.set_spell_exclusions_utf16 = Resolve<decltype(a.set_spell_exclusions_utf16)>(
            lib, "vkey_engine_set_spell_exclusions_utf16");
        a.should_replay_key_after_raw_utf16 =
            Resolve<decltype(a.should_replay_key_after_raw_utf16)>(
                lib, "vkey_engine_should_replay_key_after_raw_utf16");
#if VKEY_ENGINE_ABI_VERSION >= 8u
        a.user_dictionary_create_utf16 =
            Resolve<decltype(a.user_dictionary_create_utf16)>(
                lib, "vkey_user_dictionary_create_utf16");
        a.user_dictionary_destroy = Resolve<decltype(a.user_dictionary_destroy)>(
            lib, "vkey_user_dictionary_destroy");
        a.set_user_dictionary = Resolve<decltype(a.set_user_dictionary)>(
            lib, "vkey_engine_set_user_dictionary");
#endif
        const bool symbolsResolved =
            a.create && a.destroy && a.reset && a.push_char && a.backspace &&
#if VKEY_ENGINE_ABI_VERSION >= 9u
            a.push_key &&
#endif
            a.peek_utf16 && a.commit_utf16 && a.count && a.abi_version && a.runtime_status &&
            a.is_english_word && a.is_tone_escaped && a.has_active_quick_consonant &&
            a.peek_raw_utf16 && a.seed_text_utf16 && a.last_commit_was_corrected &&
            a.set_custom_keymap && a.set_spell_exclusions_utf16 &&
            (VKEY_ENGINE_ABI_VERSION < 7u || a.should_replay_key_after_raw_utf16)
#if VKEY_ENGINE_ABI_VERSION >= 8u
            && a.user_dictionary_create_utf16 && a.user_dictionary_destroy &&
            a.set_user_dictionary
#endif
            ;
        if (!symbolsResolved) {
            CloseRustEngineLibrary(lib);
            a.reason = L"vkey_engine library is missing a required exported symbol";
            return a;
        }
        // A floor, not an equality: this binary must be able to load an engine
        // newer than the header it was built against, otherwise a VKeyTSF.dll left
        // behind by a deferred update refuses the engine installed beside it and
        // drops to the C++ engine with no user-visible sign. Safe because the ABI
        // only ever gains symbols, and every symbol this build needs was already
        // resolved by name above.
        const uint32_t libVersion = a.abi_version();
        if (libVersion < VKEY_ENGINE_ABI_VERSION) {
            CloseRustEngineLibrary(lib);
            a.reason = L"vkey_engine ABI is older than this build (lib=" + std::to_wstring(libVersion) +
                        L", needs>=" + std::to_wstring(VKEY_ENGINE_ABI_VERSION) + L")";
            return a;
        }
        const uint32_t runtimeStatus = a.runtime_status();
        if (runtimeStatus != VKEY_ENGINE_RUNTIME_OK) {
            CloseRustEngineLibrary(lib);
            a.reason = L"vkey_engine runtime identity check failed (status=" +
                       std::to_wstring(runtimeStatus) + L")";
            return a;
        }
        a.ok = true;
        return a;
    }();
    return api;
}

uint32_t MapMethod(InputMethod method) {
    switch (method) {
        case InputMethod::VNI:
            return VKEY_METHOD_VNI;
        case InputMethod::SimpleTelex:
            return VKEY_METHOD_SIMPLE_TELEX;
        case InputMethod::Combined:
            return VKEY_METHOD_COMBINED;
        case InputMethod::UserDefined:
            return VKEY_METHOD_USER_DEFINED;
        case InputMethod::Telex:
        default:
            return VKEY_METHOD_TELEX;
    }
}

uint32_t MapFeatures(const TypingConfig& c) {
    uint32_t f = 0;
    if (c.modernOrtho) f |= VKEY_FEAT_MODERN_ORTHOGRAPHY;
    if (c.quickStartConsonant) f |= VKEY_FEAT_QUICK_START_CONSONANT;
    if (c.quickConsonant) f |= VKEY_FEAT_QUICK_CONSONANT;
    if (c.quickEndConsonant) f |= VKEY_FEAT_QUICK_END_CONSONANT;
    if (c.spellCheckEnabled) f |= VKEY_FEAT_SPELL_CHECK;
    if (!c.autoRestoreEnabled) f |= VKEY_FEAT_DISABLE_AUTO_RESTORE;
    if (c.spellSuggestEnabled && c.autoRestoreEnabled) f |= VKEY_FEAT_SPELL_SUGGEST;
    if (c.allowEnglishBypass) f |= VKEY_FEAT_ALLOW_ENGLISH_BYPASS;
    if (c.allowZwjf) f |= VKEY_FEAT_ALLOW_ZWJF;
    return f;
}

// Bounded engine: the active composition never exceeds this many UTF-16 units.
constexpr size_t kTextCap = 256;

struct Utf16Text {
    std::array<uint16_t, kTextCap> units{};
    size_t length = 0;
    bool valid = true;
};

// Stack-only encoding keeps hook-path UTF-16 conversion allocation-free.
Utf16Text ToUtf16(std::wstring_view text) {
    Utf16Text out;
    for (const wchar_t wc : text) {
        const uint32_t c = static_cast<uint32_t>(wc);
        if (c <= 0xFFFF) {
            if (out.length == out.units.size()) {
                out.valid = false;
                break;
            }
            out.units[out.length++] = static_cast<uint16_t>(c);
        } else if (c <= 0x10FFFF) {
            if (out.length + 2 > out.units.size()) {
                out.valid = false;
                break;
            }
            const uint32_t v = c - 0x10000;
            out.units[out.length++] = static_cast<uint16_t>(0xD800 + (v >> 10));
            out.units[out.length++] = static_cast<uint16_t>(0xDC00 + (v & 0x3FF));
        } else {
            out.valid = false;
            break;
        }
    }
    return out;
}

constexpr size_t kUserDictionaryMaxUtf16Units = 65'536;
constexpr size_t kUserDictionaryMaxUtf8Bytes = kUserDictionaryMaxUtf16Units * 3 + 3;
#if VKEY_ENGINE_ABI_VERSION >= 8u
static_assert(kUserDictionaryMaxUtf16Units == VKEY_USER_DICTIONARY_MAX_UTF16_UNITS);
#endif

constexpr char kUserDictionaryTemplate[] =
    "# VKey user dictionary - one word per line.\n"
    "# Report wrong corrections first: https://github.com/phatMT97/VKey/issues\n"
    "# Then add the word below, save, and select Advanced again to reload.\n";

std::filesystem::path UserDictionaryPath(const std::wstring& configPath) {
    std::filesystem::path path(configPath);
    path.replace_filename(L"user_dictionary.txt");
    return path;
}

bool EnsureUserDictionaryTemplate(const std::filesystem::path& path, bool& created) {
    created = false;
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return false;
    }

#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_EXISTS;
    }
    created = true;
    DWORD written = 0;
    const DWORD length = static_cast<DWORD>(sizeof(kUserDictionaryTemplate) - 1);
    const bool ok = WriteFile(file, kUserDictionaryTemplate, length, &written, nullptr) != FALSE
                 && written == length;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(path.c_str());
        created = false;
    }
    return ok;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return errno == EEXIST;
    created = true;
    const char* cursor = kUserDictionaryTemplate;
    size_t remaining = sizeof(kUserDictionaryTemplate) - 1;
    bool ok = true;
    while (remaining != 0) {
        const ssize_t written = ::write(fd, cursor, remaining);
        if (written <= 0) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= static_cast<size_t>(written);
    }
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        ::unlink(path.c_str());
        created = false;
    }
    return ok;
#endif
}

bool ReadBoundedFile(const std::filesystem::path& path, std::string& bytes,
                     bool& oversized) {
    oversized = false;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const std::streampos end = input.tellg();
    if (end < 0) return false;
    const auto length = static_cast<unsigned long long>(end);
    if (length > kUserDictionaryMaxUtf8Bytes) {
        oversized = true;
        return false;
    }
    bytes.resize(static_cast<size_t>(length));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return false;
    }
    return true;
}

bool DecodeUtf8(std::string_view bytes, std::vector<uint16_t>& units,
                bool& oversized) {
    oversized = false;
    units.clear();
    units.reserve((std::min)(bytes.size(), kUserDictionaryMaxUtf16Units + 1));
    size_t i = 0;
    while (i < bytes.size()) {
        const auto first = static_cast<uint8_t>(bytes[i]);
        uint32_t scalar = 0;
        size_t width = 0;
        if (first <= 0x7F) {
            scalar = first;
            width = 1;
        } else if (first >= 0xC2 && first <= 0xDF) {
            scalar = first & 0x1F;
            width = 2;
        } else if (first >= 0xE0 && first <= 0xEF) {
            scalar = first & 0x0F;
            width = 3;
        } else if (first >= 0xF0 && first <= 0xF4) {
            scalar = first & 0x07;
            width = 4;
        } else {
            return false;
        }
        if (i + width > bytes.size()) return false;
        for (size_t j = 1; j < width; ++j) {
            const auto continuation = static_cast<uint8_t>(bytes[i + j]);
            if ((continuation & 0xC0) != 0x80) return false;
            scalar = (scalar << 6) | (continuation & 0x3F);
        }
        if ((width == 3 && scalar < 0x800) || (width == 4 && scalar < 0x10000)
            || (scalar >= 0xD800 && scalar <= 0xDFFF) || scalar > 0x10FFFF) {
            return false;
        }
        if (scalar <= 0xFFFF) {
            units.push_back(static_cast<uint16_t>(scalar));
        } else {
            scalar -= 0x10000;
            units.push_back(static_cast<uint16_t>(0xD800 + (scalar >> 10)));
            units.push_back(static_cast<uint16_t>(0xDC00 + (scalar & 0x3FF)));
        }
        if (units.size() > kUserDictionaryMaxUtf16Units) {
            oversized = true;
            return false;
        }
        i += width;
    }
    return true;
}

}  // namespace

bool RustInputEngine::LibraryAvailable() {
    return Api().ok;
}

std::wstring RustInputEngine::UnavailableReason() {
    return Api().reason;
}

RustUserDictionarySnapshot::~RustUserDictionarySnapshot() {
#if VKEY_ENGINE_ABI_VERSION >= 8u
    if (handle_ && Api().user_dictionary_destroy) {
        Api().user_dictionary_destroy(static_cast<VKeyUserDictionary*>(handle_));
    }
#endif
}

bool RustInputEngine::CreateUserDictionaryTemplate(
    const std::wstring& configPath, bool* created) {
    const std::filesystem::path path = UserDictionaryPath(configPath);
    bool createdLocal = false;
    const bool ok = EnsureUserDictionaryTemplate(path, createdLocal);
    if (created) *created = createdLocal;
    return ok;
}

RustUserDictionaryLoadResult RustInputEngine::LoadUserDictionary(
    const std::wstring& configPath) {
    RustUserDictionaryLoadResult result;
    const std::filesystem::path path = UserDictionaryPath(configPath);
    result.path = path.wstring();
    try {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            // Missing file produces a valid empty snapshot without creating the file on disk.
            // Succeeded() is true, snapshot contains an empty compiled dictionary.
            result.status = RustUserDictionaryLoadStatus::Loaded;
            result.created = false;
#if VKEY_ENGINE_ABI_VERSION >= 8u
            const EngineApi& api = Api();
            if (api.ok && api.user_dictionary_create_utf16) {
                uint32_t status = VKEY_USER_DICTIONARY_OK;
                size_t errorLine = 0;
                VKeyUserDictionary* dictionary = api.user_dictionary_create_utf16(
                    nullptr, 0, &status, &errorLine);
                result.engineStatus = status;
                result.errorLine = errorLine;
                if (!dictionary) {
                    result.status = RustUserDictionaryLoadStatus::EngineRejected;
                    return result;
                }
                result.snapshot = std::shared_ptr<const RustUserDictionarySnapshot>(
                    new RustUserDictionarySnapshot(dictionary));
            } else {
                result.status = RustUserDictionaryLoadStatus::EngineUnavailable;
            }
#else
            result.status = RustUserDictionaryLoadStatus::EngineUnavailable;
#endif
            return result;
        }

        std::string bytes;
        bool oversized = false;
        if (!ReadBoundedFile(path, bytes, oversized)) {
            result.status = oversized ? RustUserDictionaryLoadStatus::EngineRejected
                                      : RustUserDictionaryLoadStatus::IoError;
#if VKEY_ENGINE_ABI_VERSION >= 8u
            if (oversized) {
                result.engineStatus = VKEY_USER_DICTIONARY_PAYLOAD_TOO_LARGE;
            }
#endif
            return result;
        }
        std::vector<uint16_t> units;
        bool decodedOversized = false;
        if (!DecodeUtf8(bytes, units, decodedOversized)) {
            result.status = decodedOversized ? RustUserDictionaryLoadStatus::EngineRejected
                                             : RustUserDictionaryLoadStatus::InvalidUtf8;
#if VKEY_ENGINE_ABI_VERSION >= 8u
            if (decodedOversized) {
                result.engineStatus = VKEY_USER_DICTIONARY_PAYLOAD_TOO_LARGE;
            }
#endif
            return result;
        }
#if VKEY_ENGINE_ABI_VERSION >= 8u
        const EngineApi& api = Api();
        if (!api.ok || !api.user_dictionary_create_utf16) {
            result.status = RustUserDictionaryLoadStatus::EngineUnavailable;
            return result;
        }
        uint32_t status = VKEY_USER_DICTIONARY_INVALID_ARGUMENT;
        size_t errorLine = 0;
        VKeyUserDictionary* dictionary = api.user_dictionary_create_utf16(
            units.empty() ? nullptr : units.data(), units.size(), &status, &errorLine);
        result.engineStatus = status;
        result.errorLine = errorLine;
        if (!dictionary) {
            result.status = RustUserDictionaryLoadStatus::EngineRejected;
            return result;
        }
        result.snapshot = std::shared_ptr<const RustUserDictionarySnapshot>(
            new RustUserDictionarySnapshot(dictionary));
        result.status = RustUserDictionaryLoadStatus::Loaded;
#else
        result.status = RustUserDictionaryLoadStatus::EngineUnavailable;
#endif
    } catch (...) {
        result.snapshot.reset();
        result.status = RustUserDictionaryLoadStatus::IoError;
    }
    return result;
}

bool RustInputEngine::SetUserDictionary(
    const std::shared_ptr<const RustUserDictionarySnapshot>& snapshot) {
#if VKEY_ENGINE_ABI_VERSION >= 8u
    if (!handle_ || !Api().set_user_dictionary) return false;
    const auto* dictionary = snapshot
        ? static_cast<const VKeyUserDictionary*>(snapshot->handle_)
        : nullptr;
    return Api().set_user_dictionary(static_cast<VKeyEngine*>(handle_), dictionary);
#else
    (void)snapshot;
    return false;
#endif
}

std::shared_ptr<const RustUserDictionarySnapshot>
RustInputEngine::CreateUserDictionaryFromUtf16(const uint16_t* utf16Units, size_t count) {
#if VKEY_ENGINE_ABI_VERSION >= 8u
    const EngineApi& api = Api();
    if (!api.ok || !api.user_dictionary_create_utf16) {
        return nullptr;
    }
    uint32_t status = VKEY_USER_DICTIONARY_INVALID_ARGUMENT;
    size_t errorLine = 0;
    VKeyUserDictionary* dictionary = api.user_dictionary_create_utf16(
        utf16Units, count, &status, &errorLine);
    if (!dictionary) {
        return nullptr;
    }
    return std::shared_ptr<const RustUserDictionarySnapshot>(
        new RustUserDictionarySnapshot(dictionary));
#else
    (void)utf16Units;
    (void)count;
    return nullptr;
#endif
}

namespace {

std::mutex s_engineCreationMutex;
std::vector<uint16_t> s_activeCanonicalExclusionsUnits;
bool s_activeCanonicalExclusionsInitialized = false;

} // namespace

bool RustInputEngine::SetSpellExclusionsFromUtf16(
    const uint16_t* utf16Units, size_t count, bool* outChanged) {
#if VKEY_ENGINE_ABI_VERSION >= 5u
    const EngineApi& api = Api();
    if (!api.ok || !api.set_spell_exclusions_utf16) {
        if (outChanged) *outChanged = false;
        return false;
    }
    std::lock_guard<std::mutex> lock(s_engineCreationMutex);
    std::vector<uint16_t> newUnits;
    if (utf16Units && count > 0) {
        newUnits.assign(utf16Units, utf16Units + count);
    }
    const bool changed = !s_activeCanonicalExclusionsInitialized ||
                         (newUnits != s_activeCanonicalExclusionsUnits);
    if (outChanged) *outChanged = changed;
    if (!changed) {
        return true;
    }
    const bool ok = api.set_spell_exclusions_utf16(utf16Units, count);
    if (ok) {
        s_activeCanonicalExclusionsUnits = std::move(newUnits);
        s_activeCanonicalExclusionsInitialized = true;
    }
    return ok;
#else
    (void)utf16Units;
    (void)count;
    if (outChanged) *outChanged = false;
    return false;
#endif
}

RustInputEngine::RustInputEngine(const TypingConfig& config) {
    const EngineApi& api = Api();
    if (api.ok) {
        // Serialized engine creation: vkey_engine_set_spell_exclusions_utf16 is
        // process-global. We must synchronize setter + create() so concurrent
        // threads never interleave exclusions, and roll back if create() fails.
        std::lock_guard<std::mutex> lock(s_engineCreationMutex);

        std::vector<uint16_t> newExclusionsUnits;
        if (!config.spellExclusions.empty()) {
            auto canonical = SpellExclusionCanonicalizer::Canonicalize(config.spellExclusions);
            const auto& srcList = canonical.Succeeded() ? canonical.entries : config.spellExclusions;
            for (size_t i = 0; i < srcList.size(); ++i) {
                if (i > 0) newExclusionsUnits.push_back(static_cast<uint16_t>('\n'));
                const auto& w = srcList[i];
#if defined(_WIN32)
                newExclusionsUnits.insert(
                    newExclusionsUnits.end(),
                    reinterpret_cast<const uint16_t*>(w.data()),
                    reinterpret_cast<const uint16_t*>(w.data() + w.size()));
#else
                std::u32string u32;
                if (SpellExclusionCanonicalizer::Utf16ToUtf32(w, u32)) {
                    for (uint32_t cp : u32) {
                        if (cp <= 0xFFFF) {
                            newExclusionsUnits.push_back(static_cast<uint16_t>(cp));
                        } else if (cp <= 0x10FFFF) {
                            uint32_t v = cp - 0x10000;
                            newExclusionsUnits.push_back(static_cast<uint16_t>(0xD800 + (v >> 10)));
                            newExclusionsUnits.push_back(static_cast<uint16_t>(0xDC00 + (v & 0x3FF)));
                        }
                    }
                }
#endif
            }
        }

        const bool exclusionsChanged = !s_activeCanonicalExclusionsInitialized ||
                                       (newExclusionsUnits != s_activeCanonicalExclusionsUnits);

        if (exclusionsChanged) {
            bool setOk = false;
            if (newExclusionsUnits.empty()) {
                setOk = api.set_spell_exclusions_utf16(nullptr, 0);
            } else {
                setOk = api.set_spell_exclusions_utf16(
                    newExclusionsUnits.data(),
                    newExclusionsUnits.size());
            }
            if (!setOk) {
                // Setter failed: fail-stale, do not proceed with create()
                return;
            }
        }

        handle_ = api.create(MapMethod(config.inputMethod), MapFeatures(config));
        if (!handle_) {
            // Creation failed: roll back process-global Rust state if changed
            if (exclusionsChanged && s_activeCanonicalExclusionsInitialized) {
                if (s_activeCanonicalExclusionsUnits.empty()) {
                    api.set_spell_exclusions_utf16(nullptr, 0);
                } else {
                    api.set_spell_exclusions_utf16(
                        s_activeCanonicalExclusionsUnits.data(),
                        s_activeCanonicalExclusionsUnits.size());
                }
            }
            return;
        }

        if (exclusionsChanged) {
            s_activeCanonicalExclusionsUnits = std::move(newExclusionsUnits);
            s_activeCanonicalExclusionsInitialized = true;
        }

        if (config.inputMethod == InputMethod::UserDefined) {
            api.set_custom_keymap(
                static_cast<VKeyEngine*>(handle_),
                reinterpret_cast<const uint8_t*>(config.customKeyMap.data()),
                config.customKeyMap.size());
        }
    }
    // Rule 11 (hook hot path): pre-size the buffers so the per-keystroke Refresh()
    // assigns reuse capacity and never allocate (composition is bounded to
    // kTextCap UTF-16 units).
    peek_.reserve(kTextCap);
    raw_.reserve(kTextCap);
}

RustInputEngine::~RustInputEngine() {
    if (handle_) {
        Api().destroy(static_cast<VKeyEngine*>(handle_));
    }
}

void RustInputEngine::Refresh() {
    peek_.clear();
    raw_.clear();
    count_ = 0;
    if (!handle_) {
        return;
    }
    const EngineApi& api = Api();
    auto* engine = static_cast<VKeyEngine*>(handle_);
    uint16_t buf[kTextCap];
    // Vietnamese output is entirely in the BMP, so one UTF-16 unit == one scalar:
    // the peek length already is the scalar count, so skip the extra count() FFI
    // round-trip on the per-keystroke hot path. Widening to wchar_t is correct on
    // both 16-bit (Windows) and 32-bit (Linux) wchar_t.
    const size_t needed = api.peek_utf16(engine, buf, kTextCap);
    count_ = needed;
    peek_.assign(buf, buf + (needed > kTextCap ? kTextCap : needed));
    // Cache the raw keystrokes so the noexcept PeekRawView() is a cheap view; the
    // hook hot path reads it every keystroke.
    const size_t rawNeeded = api.peek_raw_utf16(engine, buf, kTextCap);
    raw_.assign(buf, buf + (rawNeeded > kTextCap ? kTextCap : rawNeeded));
}

void RustInputEngine::PushChar(wchar_t c) {
    if (handle_) {
        Api().push_char(static_cast<VKeyEngine*>(handle_), static_cast<uint32_t>(c));
    }
    Refresh();
}

void RustInputEngine::PushKey(wchar_t physicalChar, bool uppercase) {
#if VKEY_ENGINE_ABI_VERSION >= 9u
    if (handle_) {
        Api().push_key(static_cast<VKeyEngine*>(handle_),
                       static_cast<uint32_t>(physicalChar), uppercase);
    }
    Refresh();
#else
    (void)uppercase;
    PushChar(physicalChar);
#endif
}

void RustInputEngine::Backspace() {
    if (handle_) {
        Api().backspace(static_cast<VKeyEngine*>(handle_));
    }
    Refresh();
}

std::wstring RustInputEngine::Commit() {
    std::wstring out;
    if (handle_) {
        uint16_t buf[kTextCap];
        size_t n = Api().commit_utf16(static_cast<VKeyEngine*>(handle_), buf, kTextCap);
        if (n > kTextCap) {
            n = kTextCap;
        }
        out.assign(buf, buf + n);
    }
    peek_.clear();
    raw_.clear();
    count_ = 0;
    return out;
}

void RustInputEngine::Reset() {
    if (handle_) {
        Api().reset(static_cast<VKeyEngine*>(handle_));
    }
    peek_.clear();
    raw_.clear();
    count_ = 0;
}

bool RustInputEngine::HasActiveQuickConsonant() const {
    return handle_ && Api().has_active_quick_consonant(static_cast<VKeyEngine*>(handle_));
}

bool RustInputEngine::IsEnglishWord() const {
    return handle_ && Api().is_english_word(static_cast<VKeyEngine*>(handle_));
}

bool RustInputEngine::IsToneEscaped() const {
    return handle_ && Api().is_tone_escaped(static_cast<VKeyEngine*>(handle_));
}

bool RustInputEngine::LastCommitWasCorrected() const {
    return handle_ && Api().last_commit_was_corrected(static_cast<VKeyEngine*>(handle_));
}

bool RustInputEngine::ShouldReplayCommittedKey(
    std::wstring_view rawInput, wchar_t key) const {
    const EngineApi& api = Api();
    if (!handle_ || !api.should_replay_key_after_raw_utf16) {
        return false;
    }
    const Utf16Text raw = ToUtf16(rawInput);
    return raw.valid && api.should_replay_key_after_raw_utf16(
        static_cast<const VKeyEngine*>(handle_), raw.units.data(), raw.length,
        static_cast<uint32_t>(key));
}

bool RustInputEngine::SeedFromText(const std::wstring& text) {
    if (!handle_) {
        Reset();
        return false;
    }
    // Literal restore: the engine reproduces the text so the English-word check
    // and continued typing/backspace work. Re-toning the restored glyphs is not
    // faithfully supported (needs a raw snapshot). On failure the engine is left
    // reset, per the IInputEngine contract.
    const Utf16Text units = ToUtf16(text);
    if (!units.valid) {
        Reset();
        return false;
    }
    const bool ok = Api().seed_text_utf16(
        static_cast<VKeyEngine*>(handle_), units.units.data(), units.length);
    Refresh();
    return ok;
}

}  // namespace NextKey
