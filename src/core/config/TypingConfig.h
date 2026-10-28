// VKey - Typing Configuration
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/engine/TypingAction.h"

namespace NextKey {

/// Input method types
enum class InputMethod : uint8_t {
    Telex = 0,
    VNI = 1,
    SimpleTelex = 2,  // w/[/] as literal when standalone (no vowel context)
    Combined = 3,     // Telex + VNI triggers simultaneously
    UserDefined = 4   // User-defined custom keymap
};

/// Output encoding (bảng mã)
enum class CodeTable : uint8_t {
    Unicode = 0,
    TCVN3 = 1,
    VNIWindows = 2,
    UnicodeCompound = 3,
    VietnameseLocale = 4
};

/// User-facing spell-check strength. Off/Standard preserve the original
/// on/off `spellCheckEnabled` behavior; Advanced additionally turns on
/// lexicon-based auto-correct on commit (`spellSuggestEnabled`).
enum class SpellCheckLevel : uint8_t {
    Off = 0,
    Standard = 1,
    Advanced = 2
};

/// Hotkey configuration for V/E toggle and quick-convert (internal,
/// separate from Windows KL switching). `vk` is a Win32 VK_* code captured
/// by the dialog overlay — supports printable keys, F1-F24, OEM punctuation,
/// Numpad, etc. 0 = unassigned.
struct HotkeyConfig {
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
    bool win   = false;
    uint32_t vk = 0;  // VK_*; was `wchar_t key` pre-2026-05 (ConfigManager migrates)

    [[nodiscard]] bool HasAny() const noexcept {
        return ctrl || shift || alt || win || vk != 0;
    }

    [[nodiscard]] bool ModifiersMatch(bool c, bool s, bool a, bool w) const noexcept {
        return ctrl == c && shift == s && alt == a && win == w;
    }

    /// Pack the 4 modifier flags into a HotkeyRegistry-compatible bitmask
    /// (`kModCtrl | kModShift | kModAlt | kModWin`). Helper for callers
    /// that need to format/compare with `Trigger`-style data.
    ///
    /// The bit values are hardcoded here rather than including
    /// `core/hotkey/HotkeyRegistry.h` to avoid pulling `<unordered_map>`
    /// into every TypingConfig consumer. Drift guarded by the static_assert
    /// in HotkeyLabel.cpp where both headers do see each other.
    [[nodiscard]] uint32_t ToMods() const noexcept {
        uint32_t m = 0;
        if (ctrl)  m |= 0x01u;  // kModCtrl
        if (shift) m |= 0x02u;  // kModShift
        if (alt)   m |= 0x04u;  // kModAlt
        if (win)   m |= 0x08u;  // kModWin
        return m;
    }

    /// Inverse of ToMods — unpack a bitmask into the 4 boolean fields.
    /// `vk` is untouched. Single source of truth for the kMod* ↔ flag map.
    void SetModsFromMask(uint32_t mods) noexcept {
        ctrl  = (mods & 0x01u) != 0;
        shift = (mods & 0x02u) != 0;
        alt   = (mods & 0x04u) != 0;
        win   = (mods & 0x08u) != 0;
    }

    bool operator==(const HotkeyConfig&) const noexcept = default;
};

/// Typing configuration loaded from TOML, used by engine
struct TypingConfig {
    InputMethod inputMethod = InputMethod::Telex;
    CodeTable codeTable = CodeTable::Unicode;
    bool spellCheckEnabled = true;
    bool spellSuggestEnabled = false;  // Advanced: lexicon auto-correct on commit
    bool beepOnSwitch = false;
    bool smartSwitch = false;
    bool excludeApps = false;  // Exclude apps feature toggle
    bool tsfApps = false;      // Use TSF engine for listed apps (skip hook)
    bool hidePreeditUnderline = false;  // Best-effort TSF preedit underline suppression
    uint8_t optimizeLevel = 0;  // 0 = off, 1 = basic, 2 = aggressive
    bool modernOrtho = true;   // Modern tone placement (oà, uý)
    bool autoCaps = false;      // Auto-capitalize first letter of sentence
    bool allowZwjf = false;     // z/w/j/f act as tone/modifier keys (normal Vietnamese)
    bool autoRestoreEnabled = true;   // Restore raw keys when word is invalid
    bool cjkAutoSwitch = false;       // Auto-suppress V mode while a CJK keyboard layout is active (opt-in)
    bool macroEnabled = false;         // Allow macro/shorthand expansion
    bool macroInEnglish = false;       // Allow macros even when Vietnamese mode is off
    bool quickConsonant = false;       // Quick typing: cc→ch, gg→gi, nn→ng
    bool quickStartConsonant = false;  // Quick start consonant: f→ph, j→gi, w→qu
    bool quickEndConsonant = false;    // Quick end consonant: g→ng, h→nh, k→ch
    // v3 cleanup: tempOffMethod / tempOffMacroByEsc removed (HotkeyRegistry
    // ToggleEnabled / SkipMacro intents replace them).
    // escRestoreRawEnabled kept temporarily — still consumed by TSF
    // EngineController. Phase 2: move TSF to read HotkeyRegistry directly,
    // then drop this field + SharedState ESC_RESTORE_RAW flag.
    bool escRestoreRawEnabled = false; // Esc restores raw keys (e.g., víu → virus) and ends composition
    bool autoCapsMacro = false;        // Auto-capitalize expansion to match typed case
    bool macroUndoOnBackspace = false; // Restore macro shortcut when Backspace is pressed right after expansion
    bool macroTitleCase = false;       // Abc: uppercase each word instead of only the first letter
    bool allowEnglishBypass = false;   // Cho phép gõ dấu tự do / Bypass English blocking (e.g. yes -> ýe)
    // BS keeps typed chars when Chromium suggestion popup is showing.
    // OFF (default): bait U+202F + extra BS on every Chromium replace, so BS
    // forces a delete even if the popup tries to swallow it (engine stays in
    // sync; trade-off: "face" + BS visually flickers to "fac"). ON: skip bait
    // when text is empty, so BS only dismisses the popup ("face" preserved);
    // engine/screen can desync after a popup-dismiss-BS, surfacing as wrong
    // tone placement on the next key (e.g., "nex" + BS + 'x' → "neẽ").
    bool suggestKeepChars = false;
    bool debugLogEnabled = false;      // System → "Bật debug log" — runtime-enable NextKey::Logger
    bool enableToast = true;           // System → "Hiển thị thông báo (toast)"
    bool perfHistogramEnabled = false; // Hidden TOML `[debug] perf_histogram` — Phase 1 per-stage histogram gate
                                       // (docs/plans/2026-05-19-architecture-review-design.md). Off by default;
                                       // overhead is ~one atomic load + branch when off, ≤1% at p99 when on.
    bool macroTriggerSpace = true;     // Kích hoạt bằng phím Space
    bool macroTriggerEnter = true;     // Kích hoạt bằng phím Enter
    bool macroTriggerTab = true;       // Kích hoạt bằng phím Tab
    bool macroTriggerDir = true;       // Kích hoạt bằng phím Mũi tên (Arrows)
    std::vector<std::wstring> spellExclusions;  // Spell check exclusion prefixes (e.g. "hđ", "đp")

    /// Per-key user override. Index by ASCII code; callers MUST guard
    /// with `lower < 128` before indexing. `TypingAction::None` (the
    /// default) means "no override — fall through to ClassifyKey".
    std::array<TypingAction, 128> customKeyMap{};

    // Default constructor for compiled defaults (FR8 - engine autonomy)
    TypingConfig() = default;

    /// Presentation-layer view of spellCheckEnabled/spellSuggestEnabled as a
    /// single 3-way level, for the Settings/Tray UI. Internal Vietnamese
    /// composition logic keeps reading the two bools directly — see
    /// SetSpellCheckLevel for the inverse.
    [[nodiscard]] SpellCheckLevel GetSpellCheckLevel() const noexcept {
        if (!spellCheckEnabled) return SpellCheckLevel::Off;
        return spellSuggestEnabled ? SpellCheckLevel::Advanced : SpellCheckLevel::Standard;
    }

    /// Inverse of GetSpellCheckLevel — unpack a UI-chosen level into the two
    /// underlying bools.
    void SetSpellCheckLevel(SpellCheckLevel level) noexcept {
        spellCheckEnabled = level != SpellCheckLevel::Off;
        spellSuggestEnabled = level == SpellCheckLevel::Advanced;
    }
};

/// Configuration for quick-convert hotkey feature
struct ConvertConfig {
    // Toggle options (which conversions are enabled)
    bool allCaps = false;         // Chuyển sang chữ HOA
    bool allLower = false;        // Chuyển sang chữ thường
    bool capsFirst = false;       // Đặt chữ Hoa đầu câu
    bool capsEach = false;        // Đặt chữ Hoa Sau Mỗi Từ
    bool removeMark = false;      // Loại bỏ dấu câu
    bool alertDone = false;       // Thông báo khi chuyển xong
    bool autoPaste = false;       // Tự động dán + bôi đen
    bool sequential = false;      // Convert tuần tự
    bool enableLog = false;       // Bật/tắt log debug

    // Encoding conversion
    uint8_t sourceEncoding = 0;   // CodeTable enum value
    uint8_t destEncoding = 0;     // CodeTable enum value

    // Hotkey for quick-convert
    HotkeyConfig hotkey{};
};

}  // namespace NextKey
