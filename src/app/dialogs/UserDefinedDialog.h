// VKey - User Defined Input Dialog Header
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "SciterSubDialog.h"
#include "core/engine/TypingAction.h"
#include <string>
#include <array>

namespace NextKey {

/// Dialog for managing user-defined input method keymap (Sciter subdialog)
class UserDefinedDialog : public SciterSubDialog {
public:
    static constexpr int kBaseWidth  = 420;
    static constexpr int kBaseHeight = 480;

    UserDefinedDialog(HWND parent);

    // Override event handler for VALUE_CHANGED on #val-action
    bool handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) override;

private:
    void populateList();
    void persistAndSignal();
    void importKeyMap();
    void exportKeyMap();
    void loadTemplate(bool telex);

    /// Reassign action to newKey, clearing any other key currently holding
    /// the same action (enforces 1 action = 1 key invariant).
    void applyAction(TypingAction action, wchar_t newKey) noexcept;

    /// Clear all keys mapped to the given action.
    void clearAction(TypingAction action) noexcept;

    std::array<TypingAction, 128> keyMap_;
};

}  // namespace NextKey
