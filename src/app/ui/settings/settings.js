// VKey Settings JavaScript
// Handles div-based toggles with hidden inputs for VALUE_CHANGED events

document.on("ready", function () {
    // Enable blur-behind effect (Sciter built-in)
    // Tint must match the current theme: "dark" or "light", plus "source-auto"
    requestAnimationFrame(function() {
        if (!document.body.classList.contains("win10")) {
            Window.this.blurBehind = (document.body.classList.contains("dark") ? "dark" : "light") + " source-auto";
        } else {
            Window.this.blurBehind = "none";
        }
    });

    // Apply i18n translations
    if (typeof applyTranslations === "function") applyTranslations();

    initializeToggles();
    initializeAdvancedPanel();
    initializeOpacitySlider();
    initializeScrollbarResize(".tab-body");
    initializeSwitchKeyDisplay();
    initializeTabPanels();
    initializeDropdownTooltips();
    initializeVersionCopy();
});

// Initialize click-to-copy for version info
function initializeVersionCopy() {
    var container = document.getElementById("version-copy-container");
    if (!container) return;
    container.onclick = function () {
        var verSpan = document.getElementById("app-version-number");
        var verText = verSpan ? verSpan.textContent.trim() : "";
        if (copyToClipboard("VKey v" + verText)) {
            showToastI18n("Đã sao chép thông tin phiên bản vào bộ nhớ tạm", "Copied version info to clipboard");
        } else {
            showToastI18n("Không sao chép được thông tin phiên bản", "Could not copy version info");
        }
    };
}

// Set tooltip on all dropdowns to show only the selected item text
function initializeDropdownTooltips() {
    document.querySelectorAll("select").forEach(function (select) {
        var selected = select.querySelector("option:checked");
        if (selected) select.setAttribute("title", selected.textContent);
    });
}

// Initialize switch key input - convert space char to "Space" display
function initializeSwitchKeyDisplay() {
    var input = document.getElementById("switch-key-char");
    if (input && input.value === " ") {
        input.value = "Space";
    }
}

// ============================================
// RESTART BANNER - Called from C++
// ============================================
// showUpdateBanner(state) — state: 0=hide, 1=pending copy, 2=mismatch copy.
// Triggered by SettingsDialog::initializeUI() after reading SharedState flags.
function showUpdateBanner(state) {
    var banner = document.getElementById("update-banner");
    if (!banner) return;

    if (state === 0) {
        banner.style.display = "none";
        return;
    }

    var msgKey = (state === 1) ? "update.banner.pending" : "update.banner.mismatch";
    var text = document.getElementById("update-banner-text");
    var btnRestart = document.getElementById("update-banner-restart");
    var btnLater = document.getElementById("update-banner-later");

    var isVi = (typeof getLang === "function" ? getLang() : "vi") !== "en";
    var defaultMsg = (state === 1)
        ? (isVi ? "Cập nhật chưa hoàn tất. Khởi động lại Windows để áp dụng phiên bản TSF mới."
                : "Update not finished. Restart Windows to apply the new TSF version.")
        : (isVi ? "Một vài ứng dụng đang chạy phiên bản cũ. Khởi động lại Windows để đồng bộ."
                : "Some apps still run the old version. Restart Windows to sync.");
    var defaultRestart = isVi ? "Khởi động lại ngay" : "Restart now";
    var defaultLater = isVi ? "Để sau" : "Later";

    if (text) {
        text.setAttribute("data-i18n", msgKey);
        var translated = (typeof t === "function") ? t(msgKey) : null;
        text.textContent = translated || defaultMsg;
    }
    if (btnRestart) {
        btnRestart.setAttribute("data-i18n", "update.banner.restartNow");
        var translated = (typeof t === "function") ? t("update.banner.restartNow") : null;
        btnRestart.textContent = translated || defaultRestart;
    }
    if (btnLater) {
        btnLater.setAttribute("data-i18n", "update.banner.later");
        var translated = (typeof t === "function") ? t("update.banner.later") : null;
        btnLater.textContent = translated || defaultLater;
    }

    banner.style.display = "";

    if (btnRestart) btnRestart.onclick = function () {
        Window.this.requestRestartWindows();
    };
    if (btnLater) btnLater.onclick = function () {
        banner.style.display = "none";
        // Do not persist — banner re-appears on next dialog open (design §4).
    };
}

// ============================================
// THEME MANAGEMENT - Called from C++
// ============================================
// setTheme(isDark) - Called from C++ to apply dark/light theme
// This avoids using eval() for security and clean code
function setTheme(isDark) {
    if (isDark) {
        document.body.classList.add("dark");
    } else {
        document.body.classList.remove("dark");
    }

    // Update background color based on theme and current opacity
    updateBackgroundForTheme(isDark);

    // Fix update-card toggle thumb color in dark mode
    fixUpdateToggleThumb();
}

// Force update-card toggle thumb to be white (workaround for Sciter CSS quirk)
function fixUpdateToggleThumb() {
    var thumb = document.getElementById("update-toggle-thumb");
    if (thumb) {
        thumb.style.backgroundColor = "#FFFFFF";
        thumb.style.background = "#FFFFFF";
    }
}

// Helper to update background rgba based on theme
function updateBackgroundForTheme(isDark) {
    var container = document.getElementById("main-container");
    var hiddenInput = document.getElementById("val-bg-opacity");
    if (container && hiddenInput) {
        var opacity = parseInt(hiddenInput.value || "80") / 100;
        if (isDark) {
            // Semi-transparent blue-gray for frosted glass
            container.style.backgroundColor = "rgba(18, 20, 28, " + opacity + ")";
        } else {
            container.style.backgroundColor = "rgba(255, 255, 255, " + opacity + ")";
        }
    }
}


function initializeToggles() {
    // Get all toggle elements and attach handlers
    const allToggles = document.querySelectorAll(".toggle-switch, .toggle-switch-small");

    allToggles.forEach(function (toggle) {
        toggle.onclick = function (evt) {
            // Block clicks on disabled toggles (child of unchecked parent)
            if (this.classList.contains("disabled")) return false;

            const isChecked = this.classList.contains("checked");

            if (isChecked) {
                this.classList.remove("checked");
            } else {
                this.classList.add("checked");
            }

            const id = this.id;
            const newState = !isChecked;

            // English UI toggle: switch language immediately in JS
            if (id === "english-ui") {
                document.documentElement.setAttribute("lang", newState ? "en" : "vi");
                applyTranslations();
            }

            // Show-advanced toggle: update container class
            if (id === "show-advanced") {
                const container = document.getElementById("main-container");
                if (container) {
                    if (newState) {
                        container.classList.add("expanded");
                        // Force sync layout before C++ measures DOM
                        Window.this.update();
                        // Tab indicator couldn't be positioned during init
                        // (section was display:none → offsetLeft/Width = 0).
                        // Position it now that the section is visible.
                        requestAnimationFrame(function() {
                            var activeTab = document.querySelector(".tab-item.active");
                            if (activeTab) updateTabIndicator(activeTab);
                        });
                    } else {
                        container.classList.remove("expanded");
                        Window.this.update();
                    }
                }
            }

            // Toggling the "show toast" switch itself: apply the new value to the
            // body attribute synchronously so the toast decision below reflects it
            // immediately (turning ON shows a confirmation, turning OFF stays silent).
            if (id === "enable-toast") {
                document.body.setAttribute("data-enable-toast", newState ? "true" : "false");
            }

            if (id === "tsf-apps") {
                updateTsfChildren(newState);
            }
            if (id === "use-macro") {
                updateMacroChildren(newState);
            }
            if (id === "auto-caps-macro") {
                updateMacroCapsChildren(newState);
            }

            // Show toast message (except show-advanced)
            if (id !== "show-advanced" && typeof showToastI18n === "function") {
                if (id === "toggle-language") {
                    showToastI18n(
                        "Chế độ gõ: " + (newState ? "Tiếng Anh" : "Tiếng Việt"),
                        "Typing mode: " + (newState ? "English" : "Vietnamese"));
                } else {
                    var row = this.closest(".setting-row");
                    var labelEl = row ? row.querySelector(".setting-label") : null;
                    var labelText = labelEl ? labelEl.textContent.trim() : "";
                    if (labelText) {
                        showToastI18n(
                            (newState ? "Đã bật: " : "Đã tắt: ") + labelText,
                            (newState ? "Enabled: " : "Disabled: ") + labelText);
                    }
                }
            }

            // Defer C++ notification to next frame so toggle animation starts instantly.
            requestAnimationFrame(function() {
                var hiddenInput = document.getElementById("val-" + id);
                if (hiddenInput) {
                    hiddenInput.value = newState ? "1" : "0";
                    hiddenInput.dispatchEvent(new Event("change", { bubbles: true }));
                }
            });

            return true;
        };
    });

    // Initial state: sync child toggles with spell-check-level parent
    var spellLevel = document.getElementById("spell-check-level");
    if (spellLevel) {
        updateSpellCheckChildren(parseInt(spellLevel.value) !== 0);
    }

    // Initial state: sync userdefined button
    updateUserDefinedButton();
    var useMacro = document.getElementById("use-macro");
    updateMacroChildren(useMacro && useMacro.classList.contains("checked"));
    var macroCaps = document.getElementById("auto-caps-macro");
    updateMacroCapsChildren(macroCaps && macroCaps.classList.contains("checked"));
}

// Grey out a child toggle + its row when the parent option is off.
// The .disabled class is also what blocks the click in the toggle handler.
function setToggleRowEnabled(id, enabled) {
    var toggle = document.getElementById(id);
    if (!toggle) return;
    var row = toggle.closest(".setting-row");

    if (enabled) {
        toggle.classList.remove("disabled");
        toggle.setAttribute("aria-disabled", "false");
        if (row) row.classList.remove("disabled");
    } else {
        toggle.classList.add("disabled");
        toggle.setAttribute("aria-disabled", "true");
        if (row) row.classList.add("disabled");
    }
}

function updateTsfChildren(tsfEnabled) {
    setToggleRowEnabled("hide-preedit-underline", tsfEnabled);
}

function updateMacroChildren(enabled) {
    setToggleRowEnabled("macro-undo-backspace", enabled);
}

function updateMacroCapsChildren(enabled) {
    setToggleRowEnabled("macro-title-case", enabled);
}

function updateUserDefinedButton() {
    var select = document.getElementById("input-type");
    var btn = document.getElementById("btn-userdefined");
    if (select && btn) {
        btn.style.display = (select.value == "4") ? "inline-block" : "none";
    }
}

// Enable/disable spell check child options based on parent state
function updateSpellCheckChildren(spellEnabled) {
    ["allow-zwjf", "restore-key"].forEach(function(id) {
        setToggleRowEnabled(id, spellEnabled);
    });

    // Exclusions button
    var btnExcl = document.getElementById("btn-spell-exclusions");
    if (btnExcl) {
        btnExcl.state.disabled = !spellEnabled;
    }
    var rowExcl = document.getElementById("row-spell-exclusions");
    if (rowExcl) {
        if (spellEnabled) {
            rowExcl.classList.remove("disabled");
        } else {
            rowExcl.classList.add("disabled");
        }
    }
}

// Initialize advanced panel toggle and tabs
function initializeAdvancedPanel() {
    // Tab switching
    const tabItems = document.querySelectorAll(".tab-item");
    tabItems.forEach(function (tab) {
        tab.onclick = function () {
            switchTab(this.getAttribute("data-tab"));
            return true;
        };
    });

    // Initialize indicator position once layout is parsed
    requestAnimationFrame(function() {
        var activeTab = document.querySelector(".tab-item.active");
        if (activeTab) updateTabIndicator(activeTab);
    });
}

function updateTabIndicator(activeTab) {
    var indicator = document.getElementById("tab-indicator");
    if (!indicator || !activeTab) return;
    
    var left = activeTab.offsetLeft;
    var width = activeTab.offsetWidth;
    
    if (left >= 0 && width > 0) {
        indicator.style.left = left + "px";
        indicator.style.width = width + "px";
    }
}

// Initialize tab panels with Sciter native state (first tab expanded)
function initializeTabPanels() {
    const tabPanels = document.querySelectorAll(".tab-panel");
    tabPanels.forEach(function (panel, index) {
        if (index === 0) {
            panel.state.expanded = true;
            panel.state.collapsed = false;
        } else {
            panel.state.expanded = false;
            panel.state.collapsed = true;
        }
    });
}

// Switch between tabs - using Sciter native state pattern
function switchTab(tabIndex) {
    // Update active tab header (visual only, use classList)
    const tabItems = document.querySelectorAll(".tab-item");
    var activeTab = null;
    tabItems.forEach(function (tab) {
        if (tab.getAttribute("data-tab") === tabIndex) {
            tab.classList.add("active");
            activeTab = tab;
        } else {
            tab.classList.remove("active");
        }
    });

    if (activeTab) updateTabIndicator(activeTab);

    // Update tab panels using Sciter native state (no flicker)
    // Window size is fixed to fit the tallest tab (see SettingsDialog::recalcWindowSize),
    // so switching tabs no longer needs to notify C++ to resize (#226).
    const tabPanels = document.querySelectorAll(".tab-panel");
    tabPanels.forEach(function (panel) {
        const panelIndex = panel.id.replace("tab-panel-", "");
        if (panelIndex === tabIndex) {
            panel.state.expanded = true;
            panel.state.collapsed = false;
        } else {
            panel.state.expanded = false;
            panel.state.collapsed = true;
        }
    });
}

// Handle dropdown changes (already works via C++ VALUE_CHANGED handler)
document.on("change", "select", function (evt, select) {
    const id = select.id || select.getAttribute("id");
    const value = parseInt(select.value);

    // Update tooltip to show only the selected item text
    var selected = select.querySelector("option:checked");
    if (selected) select.setAttribute("title", selected.textContent);

    if (id === "input-type") {
        updateUserDefinedButton();
    }

    if (id === "spell-check-level") {
        updateSpellCheckChildren(value !== 0);
    }
});

// Handle text input changes - display "Space" for space character
document.on("change", "#switch-key-char", function (evt, input) {
    let keyChar = input.value;

    if (keyChar === " ") {
        // Space character typed - display "Space"
        input.value = "Space";
    } else if (keyChar === "Space") {
        // Full "Space" text - keep it (already displayed correctly)
    } else if (keyChar.length === 0) {
        // Empty - user deleted everything
    } else if (keyChar.length === 1) {
        // Single character input - uppercase it
        input.value = keyChar.toUpperCase();
    } else {
        // Partial text (like "Spac", "Sp" from deletion) - clear it
        input.value = "";
    }
});

// Real-time conversion while typing space
document.on("input", "#switch-key-char", function (evt, input) {
    if (input.value === " ") {
        input.value = "Space";
    }
});

// ============================================
// CUSTOM OPACITY SLIDER - Background Transparency
// ============================================
var sliderDragging = false;

function initializeOpacitySlider() {
    var slider = document.getElementById("bg-opacity-slider");
    var thumb = document.getElementById("bg-opacity-thumb");
    var fill = document.getElementById("bg-opacity-fill");
    var valueLabel = document.getElementById("bg-opacity-value");
    var hiddenInput = document.getElementById("val-bg-opacity");

    if (!slider || !thumb) return;

    // Click on track to set value
    slider.onmousedown = function (evt) {
        sliderDragging = true;
        slider.state.capture(true); // Capture mouse even outside window
        updateSliderFromMouse(evt, slider, thumb, fill, valueLabel, hiddenInput);
    };

    // Drag thumb — use slider (capture target) instead of document
    slider.onmousemove = function (evt) {
        if (sliderDragging) {
            updateSliderFromMouse(evt, slider, thumb, fill, valueLabel, hiddenInput);
        }
    };

    slider.onmouseup = function (evt) {
        if (sliderDragging) {
            sliderDragging = false;
            slider.state.capture(false);
            // Save to C++ on release
            hiddenInput.dispatchEvent(new Event("change", { bubbles: true }));
        }
    };
}

function updateSliderFromMouse(evt, slider, thumb, fill, valueLabel, hiddenInput) {
    var rect = slider.getBoundingClientRect();
    var x = evt.clientX - rect.left;
    var width = rect.width;

    // Clamp to 0-100%
    var percent = Math.max(0, Math.min(100, (x / width) * 100));
    var value = Math.round(percent);

    // Update UI
    thumb.style.left = percent + "%";
    fill.style.width = percent + "%";
    valueLabel.textContent = value + "%";
    hiddenInput.value = value.toString();

    // Apply background immediately (respect dark/light mode)
    var opacity = value / 100;
    var container = document.getElementById("main-container");
    if (container) {
        var isDark = document.body.classList.contains("dark");
        if (isDark) {
            // Semi-transparent blue-gray for frosted glass effect
            container.style.backgroundColor = "rgba(18, 20, 28, " + opacity + ")";
        } else {
            container.style.backgroundColor = "rgba(255, 255, 255, " + opacity + ")";
        }
    }
}

// Called from C++ to set initial opacity value.
// Overrides utils.js setBackgroundOpacity to also update slider UI elements.
function setBackgroundOpacity(value) {
    var thumb = document.getElementById("bg-opacity-thumb");
    var fill = document.getElementById("bg-opacity-fill");
    var valueLabel = document.getElementById("bg-opacity-value");
    var hiddenInput = document.getElementById("val-bg-opacity");

    if (thumb && fill) {
        thumb.style.left = value + "%";
        fill.style.width = value + "%";
        valueLabel.textContent = value + "%";
        hiddenInput.value = value.toString();

        // Apply background directly on container (respect dark/light mode)
        var opacity = value / 100;
        var container = document.getElementById("main-container");
        if (container) {
            var isDark = document.body.classList.contains("dark");
            if (isDark) {
                container.style.backgroundColor = "rgba(18, 20, 28, " + opacity + ")";
            } else {
                container.style.backgroundColor = "rgba(255, 255, 255, " + opacity + ")";
            }
        }
    }
}
