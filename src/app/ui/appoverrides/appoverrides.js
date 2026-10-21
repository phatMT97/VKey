// App Overrides Dialog JavaScript
// Handles dropdown selection and C++ communication

var isFullyLoaded = false;

document.ready = function () {
    initSubDialog(".app-list");  // blur + dark + scrollbar + i18n
    initRunningAppsDropdown();
    initButtons();
    initEventDelegation();
    requestAnimationFrame(function() {
        requestAnimationFrame(function() {
            isFullyLoaded = true;
        });
    });
};

// ===== Input Method Labels =====
var inputMethodLabels = {
    "-1": "Mặc định",
    0: "Telex",
    1: "VNI",
    2: "Simple Telex",
    3: "Telex + VNI",
    4: "Tự định nghĩa"
};

var encodingLabels = {
    "-1": "Mặc định",
    0: "Unicode",
    1: "TCVN3",
    2: "VNI Win",
    3: "UNI Cmp",
    4: "CP 1258"
};

var sendMethodLabels = {
    "-1": "Mặc định",
    1: "Clipboard",
    2: "Firefox",
    3: "Cloud/Remote",
    4: "Thay thế trực tiếp",
    5: "Chế độ game",
    6: "WebView App"
};

var sendMethodLabelsEn = {
    "-1": "Default",
    1: "Clipboard",
    2: "Firefox",
    3: "Cloud/Remote",
    4: "Direct replace",
    5: "Game mode",
    6: "WebView App"
};

function getSendMethodLabel(sndKey) {
    var isEn = (typeof getLang === "function" && getLang() === "en");
    var map = isEn ? sendMethodLabelsEn : sendMethodLabels;
    return map[sndKey] || "-";
}

// ===== Running Apps Dropdown =====
function initRunningAppsDropdown() {
    var input = document.getElementById("app-name");
    var dropdown = document.getElementById("running-apps-dropdown");
    var refreshBtn = document.getElementById("btn-refresh");

    if (!input || !dropdown) return;

    // Show dropdown on focus
    input.addEventListener("focus", function () {
        triggerAction("get-running-apps");
        setTimeout(function () {
            dropdown.classList.add("visible");
        }, 100);
    });

    // Hide dropdown when clicking outside
    document.addEventListener("click", function (e) {
        if (!input.contains(e.target) && !dropdown.contains(e.target) && !refreshBtn.contains(e.target)) {
            dropdown.classList.remove("visible");
        }
    });

    // Filter on typing
    input.addEventListener("input", function () {
        filterDropdown(input.value.toLowerCase());
    });

    // Refresh button
    if (refreshBtn) {
        refreshBtn.addEventListener("click", function (e) {
            e.stopPropagation();
            triggerAction("get-running-apps");
            input.focus();
        });
    }
}

function filterDropdown(query) {
    var dropdown = document.getElementById("running-apps-dropdown");
    var items = dropdown.querySelectorAll(".dropdown-item");

    items.forEach(function (item) {
        var text = item.textContent.toLowerCase();
        if (text.indexOf(query) >= 0) {
            item.style.display = "block";
        } else {
            item.style.display = "none";
        }
    });
}

// Called from C++ to populate dropdown
function setRunningApps(apps) {
    var dropdown = document.getElementById("running-apps-dropdown");
    if (!dropdown) return;

    dropdown.innerHTML = "";

    if (!apps || apps.length === 0) {
        var empty = document.createElement("div");
        empty.className = "dropdown-empty";
        empty.textContent = (typeof t === "function" && t("no_running_apps")) || "Không có ứng dụng nào đang chạy";
        dropdown.appendChild(empty);
        return;
    }

    apps.forEach(function (app) {
        var item = document.createElement("div");
        item.className = "dropdown-item";
        item.textContent = app;
        item.addEventListener("click", function () {
            document.getElementById("app-name").value = app;
            dropdown.classList.remove("visible");
        });
        dropdown.appendChild(item);
    });

    dropdown.classList.add("visible");
}

// ===== Buttons =====
function initButtons() {
    var addBtn = document.getElementById("btn-add");
    var pickBtn = document.getElementById("btn-pick-window");

    if (addBtn) {
        addBtn.onclick = function () {
            onAddApp();
        };
    }

    if (pickBtn) {
        pickBtn.onclick = function () {
            triggerAction("pick-window");
        };
    }
}

function onAddApp() {
    var appName = document.getElementById("app-name").value.trim();
    if (!appName) {
        return;
    }

    var inputMethod = document.getElementById("input-method").value;
    var encodingOverride = document.getElementById("encoding-override").value;
    var sendMethod = document.getElementById("send-method").value;

    // Set hidden inputs and trigger action
    document.getElementById("val-app-name").value = appName;
    document.getElementById("val-input-method").value = inputMethod;
    document.getElementById("val-encoding-override").value = encodingOverride;
    document.getElementById("val-send-method").value = sendMethod;
    triggerAction("add-app");
}

// ===== Event Delegation for Dynamic Elements =====
function initEventDelegation() {
    // Handle delete button click using event delegation
    document.on("click", ".app-item-delete", function (evt, btn) {
        var item = btn.closest(".app-item");
        if (item) {
            var appName = item.getAttribute("data-app");
            if (appName) {
                document.getElementById("val-app-name").value = appName;
                triggerAction("delete-app");
            }
        }
        evt.stopPropagation();
    });
}

// ===== Action Trigger =====
function triggerAction(action) {
    var el = document.getElementById("val-action");
    if (el) {
        el.value = action;
        el.dispatchEvent(new Event("change", { bubbles: true }));
    }
}

// ===== App List Functions (called from C++) =====
function clearAppList() {
    var list = document.getElementById("app-list");
    if (list) list.innerHTML = "";
}

function addAppToList(appName, inputMethod, encodingOverride, sendMethod) {
    var list = document.getElementById("app-list");
    if (!list) return;

    var item = document.createElement("div");
    item.className = "app-item";
    item.setAttribute("data-app", appName);

    // App name column
    var nameSpan = document.createElement("span");
    nameSpan.className = "app-item-name";
    nameSpan.textContent = appName;
    nameSpan.title = appName;
    item.appendChild(nameSpan);

    // Input method column
    var methodSpan = document.createElement("span");
    methodSpan.className = "app-item-inputmethod";
    var mthKey = (inputMethod === undefined || inputMethod === null) ? "-1" : String(inputMethod);
    methodSpan.textContent = inputMethodLabels[mthKey] || "-";
    item.appendChild(methodSpan);

    // Encoding column
    var encodingSpan = document.createElement("span");
    encodingSpan.className = "app-item-encoding";
    var encKey = (encodingOverride === undefined || encodingOverride === null) ? "-1" : String(encodingOverride);
    encodingSpan.textContent = encodingLabels[encKey] || "-";
    item.appendChild(encodingSpan);

    // SendMethod column
    var sendSpan = document.createElement("span");
    sendSpan.className = "app-item-sendmethod";
    var sndKey = (sendMethod === undefined || sendMethod === null) ? "-1" : String(sendMethod);
    sendSpan.textContent = getSendMethodLabel(sndKey);
    item.appendChild(sendSpan);

    // Delete button (uses event delegation, no inline handler)
    var deleteBtn = document.createElement("button");
    deleteBtn.className = "app-item-delete";
    deleteBtn.textContent = "×";
    item.appendChild(deleteBtn);

    list.appendChild(item);

    if (isFullyLoaded && typeof showToastI18n === "function") {
        showToastI18n("Đã thêm cấu hình cho: " + appName, "Added override for: " + appName);
    }
}

function removeAppFromList(appName) {
    var list = document.getElementById("app-list");
    if (!list) return;

    var items = list.querySelectorAll(".app-item");
    items.forEach(function (item) {
        if (item.getAttribute("data-app") === appName) {
            item.remove();
        }
    });

    if (isFullyLoaded && typeof showToastI18n === "function") {
        showToastI18n("Đã xóa cấu hình của: " + appName, "Removed override for: " + appName);
    }
}

function clearInput() {
    var input = document.getElementById("app-name");
    if (input) input.value = "";

    var methodSelect = document.getElementById("input-method");
    if (methodSelect) methodSelect.value = "-1";

    var encodingSelect = document.getElementById("encoding-override");
    if (encodingSelect) encodingSelect.value = "-1";

    var sendSelect = document.getElementById("send-method");
    if (sendSelect) sendSelect.value = "-1";
}

function forceRefresh(scrollToBottom) {
    var list = document.getElementById("app-list");
    if (list && scrollToBottom) {
        list.scrollTop = list.scrollHeight;
    }
}
