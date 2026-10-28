// VKey - Unified Hotkey Rebind Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "HotkeysDialog.h"

#include <string>

#include "core/config/ConfigManager.h"
#include "core/Debug.h"
#include "core/hotkey/HotkeyLabel.h"
#include "core/WinStrings.h"
#include "helpers/AppHelpers.h"
#include "sciter-x-dom.hpp"

using namespace sciter::dom;

namespace NextKey {

namespace {

constexpr const wchar_t* kIntentCancel = L"cancel-composition";
constexpr const wchar_t* kIntentSkip   = L"skip-macro";
constexpr const wchar_t* kIntentToggle = L"toggle-enabled";
constexpr const wchar_t* kIntentGame   = L"toggle-game-mode";

[[nodiscard]] const wchar_t* IntentLabel(Intent intent) noexcept {
    switch (intent) {
    case Intent::CancelComposition: return kIntentCancel;
    case Intent::SkipMacro:         return kIntentSkip;
    case Intent::ToggleEnabled:     return kIntentToggle;
    case Intent::ToggleGameMode:    return kIntentGame;
    }
    return L"";
}

[[nodiscard]] bool ParseIntent(const std::wstring& s, Intent& out) noexcept {
    if (s == kIntentCancel) { out = Intent::CancelComposition; return true; }
    if (s == kIntentSkip)   { out = Intent::SkipMacro;         return true; }
    if (s == kIntentToggle) { out = Intent::ToggleEnabled;     return true; }
    if (s == kIntentGame)   { out = Intent::ToggleGameMode;    return true; }
    return false;
}

/// Render a Trigger as a localized chip label. Examples:
///   {vk=0x1B}                     → "Esc"
///   {vk=0x11, mods=0}             → "Ctrl (giữ-thả)" (implicit modifier-alone)
///   {vk=0x12, mods=0, doubleTap}  → "2×Alt"
///   {vk=0x56, mods=Ctrl|Shift}    → "Ctrl+Shift+V"
[[nodiscard]] std::wstring FormatTriggerLabel(const Trigger& t) {
    std::wstring s;
    if (t.mods & kModCtrl)  s += L"Ctrl+";
    if (t.mods & kModShift) s += L"Shift+";
    if (t.mods & kModAlt)   s += L"Alt+";
    if (t.mods & kModWin)   s += L"Win+";

    // Modifier-alone (no other mods + vk is itself a modifier):
    if (t.mods == 0 && !t.doubleTap && IsModifierKey(t.vk)) {
        switch (t.vk) {
        case 0x11: return L"Ctrl (giữ-thả)";
        case 0x12: return L"Alt (giữ-thả)";
        case 0x10: return L"Shift (giữ-thả)";
        case 0x5B: case 0x5C: return L"Win (giữ-thả)";
        }
    }

    std::wstring keyName;
    if (t.vk >= 0x60 && t.vk <= 0x69) {
        keyName = L"Num" + std::to_wstring(t.vk - 0x60);                  // VK_NUMPAD0..9
    } else if (t.vk >= 0x70 && t.vk <= 0x87) {
        keyName = L"F" + std::to_wstring(t.vk - 0x6F);                    // F1..F24
    } else if ((t.vk >= 'A' && t.vk <= 'Z') || (t.vk >= '0' && t.vk <= '9')) {
        keyName.push_back(static_cast<wchar_t>(t.vk));
    } else {
        for (const auto& [vk, name] : GetVkDisplayNames(VkNameStyle::CompactArrows)) {
            if (vk == t.vk) { keyName = name; break; }
        }
        if (keyName.empty()) keyName = L"VK_" + std::to_wstring(t.vk);
    }

    if (t.doubleTap) {
        s = L"2×" + keyName;  // strip any modifiers from prefix — doubleTap implies mods=0
        return s;
    }

    s += keyName;
    return s;
}

}  // namespace

HotkeysDialog::HotkeysDialog(HWND parent)
    : SciterSubDialog({
        L"this://app/hotkeys/hotkeys.html",
        L"VKey - Phím tắt",
        680, 340, parent, false, 36, 40, true
    }) {
    registry_ = ConfigManager::LoadHotkeyRegistryOrDefault();
    sendVkNames();  // upload before populate so capture preview has the table
    populate();
}

void HotkeysDialog::sendVkNames() {
    sciter::value pairs;
    int i = 0;
    for (const auto& [vk, name] : GetVkDisplayNames(VkNameStyle::CompactArrows)) {
        sciter::value pair;
        pair.set_item(0, sciter::value(static_cast<int>(vk)));
        pair.set_item(1, sciter::value(name));
        pairs.set_item(i++, pair);
    }
    call_function("setVkNames", pairs);
}

void HotkeysDialog::sendEnabledStates() {
    sciter::value pairs;
    int i = 0;
    for (Intent intent : kAllIntents) {
        sciter::value pair;
        pair.set_item(0, sciter::value(IntentLabel(intent)));
        pair.set_item(1, sciter::value(registry_.IsEnabled(intent)));
        pairs.set_item(i++, pair);
    }
    call_function("setEnabledStates", pairs);
}

void HotkeysDialog::populate() {
    call_function("clearAll");
    sendEnabledStates();
    for (Intent intent : kAllIntents) {
        for (const Trigger& t : registry_.TriggersFor(intent)) {
            const std::wstring label = FormatTriggerLabel(t);
            // Sciter's call_function caps at a few overloads — bundle the 5
            // payload fields into a single array (intent, label, vk, mods,
            // doubleTap). JS side unpacks via positional indexing.
            sciter::value arr;
            arr.set_item(0, sciter::value(IntentLabel(intent)));
            arr.set_item(1, sciter::value(label.c_str()));
            arr.set_item(2, sciter::value(static_cast<int>(t.vk)));
            arr.set_item(3, sciter::value(static_cast<int>(t.mods)));
            arr.set_item(4, sciter::value(t.doubleTap));
            call_function("addTrigger", arr);
        }
    }
    call_function("forceRefresh");
}

void HotkeysDialog::persistAndSignal() {
    auto path = ConfigManager::GetConfigPath();
    if (!ConfigManager::SaveHotkeyRegistry(path, registry_)) {
        // Save failure leaves the in-memory registry ahead of disk — user sees
        // the chip but next launch will lose it. Log + still signal so the
        // running hook engine picks up the in-memory state until next reload.
        NEXTKEY_LOG(L"HotkeysDialog: SaveHotkeyRegistry failed for %s", path.c_str());
    }
    SignalConfigChange();
}

Trigger HotkeysDialog::readPendingTrigger(Intent& outIntent, bool& outValid) {
    outValid = false;
    Trigger t;
    element root = get_root();

    auto readWString = [&](const char* id) -> std::wstring {
        element el = root.find_first(("#" + std::string(id)).c_str());
        if (!el.is_valid()) return L"";
        sciter::value v = el.get_value();
        return v.is_string() ? v.get<std::wstring>() : L"";
    };
    auto readInt = [&](const char* id, int fallback) -> int {
        element el = root.find_first(("#" + std::string(id)).c_str());
        if (!el.is_valid()) return fallback;
        sciter::value v = el.get_value();
        if (v.is_int())    return v.get<int>();
        if (v.is_string()) {
            try { return std::stoi(v.get<std::wstring>()); } catch (...) { return fallback; }
        }
        return fallback;
    };
    auto readBool = [&](const char* id) -> bool {
        element el = root.find_first(("#" + std::string(id)).c_str());
        if (!el.is_valid()) return false;
        sciter::value v = el.get_value();
        if (v.is_bool())   return v.get<bool>();
        if (v.is_int())    return v.get<int>() != 0;
        if (v.is_string()) {
            auto s = v.get<std::wstring>();
            return s == L"true" || s == L"1";
        }
        return false;
    };

    const std::wstring intentStr = readWString("val-intent");
    if (!ParseIntent(intentStr, outIntent)) return t;

    const int vk   = readInt("val-vk",   0);
    const int mods = readInt("val-mods", 0);
    if (vk <= 0 || vk > 0xFF) return t;  // sanity
    t.vk        = static_cast<uint32_t>(vk);
    t.mods      = static_cast<uint32_t>(mods);
    t.doubleTap = readBool("val-double-tap");
    outValid    = true;
    return t;
}

void HotkeysDialog::handleAction(const std::wstring& action) {
    if (action == L"reset") {
        registry_ = HotkeyRegistry::Defaults();
        populate();
        persistAndSignal();
        return;
    }
    if (action == L"close") {
        PostMessage(get_hwnd(), WM_CLOSE, 0, 0);
        return;
    }
    if (action == L"set-enabled") {
        // val-intent + val-enabled identify which intent toggle was flipped.
        element root = get_root();
        element intentEl  = root.find_first("#val-intent");
        element enabledEl = root.find_first("#val-enabled");
        if (!intentEl.is_valid() || !enabledEl.is_valid()) return;
        sciter::value iv = intentEl.get_value();
        sciter::value ev = enabledEl.get_value();
        if (!iv.is_string() || !ev.is_string()) return;
        Intent intent;
        if (!ParseIntent(iv.get<std::wstring>(), intent)) return;
        const auto evs = ev.get<std::wstring>();
        const bool enabled = (evs == L"true" || evs == L"1");
        registry_.SetEnabled(intent, enabled);
        NEXTKEY_LOG(L"HotkeysDialog: SetEnabled intent=%s enabled=%d",
                    iv.get<std::wstring>().c_str(), enabled);
        persistAndSignal();   // visual class already updated by JS; no need to repopulate
        return;
    }

    Intent intent;
    bool valid = false;
    Trigger t = readPendingTrigger(intent, valid);
    if (!valid) return;

    if (action == L"add") {
        // Skip if (intent, exact-trigger) already present.
        for (const Trigger& existing : registry_.TriggersFor(intent)) {
            if (existing == t) return;
        }
        registry_.AddTrigger(intent, t);
        populate();
        persistAndSignal();
        return;
    }
    if (action == L"delete") {
        // Rebuild registry without the matching trigger. Per-intent enabled
        // state must be carried over — a fresh HotkeyRegistry defaults
        // IsEnabled() to true, which would silently flip the user's toggles
        // back on whenever they deleted a chip.
        HotkeyRegistry rebuilt;
        for (Intent i : kAllIntents) {
            rebuilt.SetEnabled(i, registry_.IsEnabled(i));
            for (const Trigger& existing : registry_.TriggersFor(i)) {
                if (i == intent && existing == t) continue;  // drop
                rebuilt.AddTrigger(i, existing);
            }
        }
        registry_ = std::move(rebuilt);
        populate();
        persistAndSignal();
        return;
    }
}

bool HotkeysDialog::handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) {
    // btn-close is wired in hotkeys.js via triggerAction("close") which routes
    // through VALUE_CHANGED below — no need for a duplicate BUTTON_CLICK branch.
    if (params.cmd == VALUE_CHANGED) {
        element el(params.heTarget);
        std::wstring id = el.get_attribute("id");
        if (id == L"val-action") {
            sciter::value val = el.get_value();
            std::wstring action = val.is_string() ? val.get<std::wstring>() : L"";
            if (!action.empty()) {
                handleAction(action);
                el.set_value(sciter::value(L""));  // reset to avoid double-fire
            }
            return true;
        }
    }

    return sciter::window::handle_event(he, params);
}

}  // namespace NextKey
