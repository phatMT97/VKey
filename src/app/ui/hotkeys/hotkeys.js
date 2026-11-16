// Unified Hotkey Rebind Dialog JS — drives the [+] / [×] / toggle UI and
// delegates capture-overlay state to the shared NextKeyHotkeyCapture
// module (../shared/hotkey-capture.js). HotkeysDialog allows double-tap
// gestures and bare-modifier triggers (e.g. "Ctrl alone", "2×Alt").

var capture = null;  // Lazy — created on first openCapture() after DOM ready.

document.ready = function () {
    initSubDialog();
    initHotkeysDialog();
};

function initHotkeysDialog() {
    document.getElementById("btn-close").addEventListener("click", function () {
        triggerAction("close");
    });
    document.getElementById("btn-reset").addEventListener("click", function () {
        triggerAction("reset");
    });
    document.on("click", ".btn-add-trigger", function (evt, btn) {
        var intent = btn.getAttribute("data-intent");
        openCapture(intent);
        evt.stopPropagation();
    });
    document.on("click", ".chip-delete", function (evt, btn) {
        var chip = btn.closest(".hotkey-chip");
        if (!chip) return;
        var intent = chip.getAttribute("data-intent");
        var vk     = parseInt(chip.getAttribute("data-vk"), 10) || 0;
        var mods   = parseInt(chip.getAttribute("data-mods"), 10) || 0;
        var dt     = chip.getAttribute("data-double-tap") === "true";
        if (!vk) return;
        document.getElementById("val-intent").value      = intent;
        document.getElementById("val-vk").value          = String(vk);
        document.getElementById("val-mods").value        = String(mods);
        document.getElementById("val-double-tap").value  = dt ? "true" : "false";
        triggerAction("delete");
        evt.stopPropagation();
    });

    // Per-intent enable toggle — single delegated handler. data-intent on each
    // `.toggle-switch-small` carries the matching Intent string so the C++ side
    // can wire VALUE_CHANGED back to HotkeyRegistry::SetEnabled.
    document.on("click", ".hotkey-section .toggle-switch-small", function (evt, toggle) {
        var intent = toggle.getAttribute("data-intent");
        if (!intent) return;
        var newState = !toggle.classList.contains("checked");
        if (newState) toggle.classList.add("checked");
        else          toggle.classList.remove("checked");
        document.getElementById("val-intent").value  = intent;
        document.getElementById("val-enabled").value = newState ? "true" : "false";
        triggerAction("set-enabled");

        // Toast feedback for toggle switch
        var labels = getIntentLabel(intent);
        if (typeof showToastI18n === "function") {
            if (newState) {
                showToastI18n("Đã bật: " + labels.vi, "Enabled: " + labels.en);
            } else {
                showToastI18n("Đã tắt: " + labels.vi, "Disabled: " + labels.en);
            }
        }

        evt.stopPropagation();
    });

    capture = NextKeyHotkeyCapture.create({
        // HotkeysDialog accepts both gestures the registry supports.
        allowDoubleTap:    true,
        allowBareModifier: true,
        onCommit: function (vk, mods, doubleTap, label) {
            document.getElementById("val-vk").value         = String(vk);
            document.getElementById("val-mods").value       = String(mods);
            document.getElementById("val-double-tap").value = doubleTap ? "true" : "false";
            triggerAction("add");

            // Toast feedback for adding a hotkey
            var intent = document.getElementById("val-intent").value;
            var labels = getIntentLabel(intent);
            if (typeof showToastI18n === "function") {
                showToastI18n("Đã thêm phím: " + label + " cho " + labels.vi,
                              "Added key: " + label + " for " + labels.en);
            }
        }
    });
}

function openCapture(intent) {
    document.getElementById("val-intent").value = intent;
    capture.open();
}

function triggerAction(action) {
    var actionInput = document.getElementById("val-action");
    if (!actionInput) return;
    actionInput.value = action;
    var event = new Event("change", { bubbles: true });
    actionInput.dispatchEvent(event);
}

// ────────────────────── Called from C++ side ────────────────────────────

// Receive the canonical VK→name table from HotkeysDialog::sendVkNames.
// Forwards to the shared module so ConvertToolDialog and any future
// dialogs share the same label dictionary.
function setVkNames(pairs) {
    NextKeyHotkeyCapture.setVkNames(pairs);
}

function clearAll() {
    ["cancel-composition", "skip-macro", "toggle-enabled", "toggle-game-mode"].forEach(function (intent) {
        var list = document.getElementById("chips-" + intent);
        if (list) list.innerHTML = "";
    });
}

// Receive per-intent enabled state from HotkeysDialog::sendEnabledStates.
// Payload: [[intent:string, enabled:bool], ...].
function setEnabledStates(pairs) {
    if (!pairs || typeof pairs.length !== "number") return;
    for (var i = 0; i < pairs.length; ++i) {
        var p = pairs[i];
        if (!p || p.length < 2) continue;
        var toggle = document.getElementById("enable-" + p[0]);
        if (!toggle) continue;
        if (p[1]) toggle.classList.add("checked");
        else      toggle.classList.remove("checked");
    }
}

// C++ packs the 5 fields into a single sciter::value array:
//   [intent, label, vk, mods, doubleTap]
// because sciter::host::call_function tops out below the 6 args we'd need
// to pass them individually. We accept both shapes here so future refactors
// (or alternative callers) can use either.
function addTrigger(intent, label, vk, mods, doubleTap) {
    if (typeof label === "undefined" && intent && typeof intent === "object"
        && typeof intent.length === "number") {
        var arr = intent;
        intent    = arr[0];
        label     = arr[1];
        vk        = arr[2];
        mods      = arr[3];
        doubleTap = arr[4];
    }
    var list = document.getElementById("chips-" + intent);
    if (!list) return;

    var chip = document.createElement("div");
    chip.className = "hotkey-chip";
    chip.setAttribute("data-intent", intent);
    chip.setAttribute("data-vk",   String(vk));
    chip.setAttribute("data-mods", String(mods));
    chip.setAttribute("data-double-tap", doubleTap ? "true" : "false");

    var span = document.createElement("span");
    span.className = "chip-label";
    span.textContent = label;
    chip.appendChild(span);

    var del = document.createElement("button");
    del.className = "chip-delete";
    del.textContent = "×";
    chip.appendChild(del);

    list.appendChild(chip);
}

function forceRefresh() {
    // No-op for now — chips are static after populate.
}

// Resolve an Intent string to its localized label for toast messages.
// VI is sourced from the matching .hotkey-section-title in the DOM (single
// source of truth — mirrors getActionLabel() in userdefined.js) so toasts
// never drift from the section headings. EN has no DOM home (HTML is VI-only),
// so it falls back to a small map, then to the VI label.
function getIntentLabel(intent) {
    var titleEl = document.querySelector(
        '.hotkey-section[data-intent="' + intent + '"] .hotkey-section-title');
    var vi = titleEl ? (titleEl.textContent || "").trim() : intent;
    var en = {
        "cancel-composition": "Cancel composition",
        "skip-macro":         "Skip macro",
        "toggle-enabled":     "Temporarily disable IME",
        "toggle-game-mode":   "Toggle game mode for current app"
    }[intent] || vi;
    return { vi: vi, en: en };
}
