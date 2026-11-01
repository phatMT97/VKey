// VKey - Internationalization Helper
// Vietnamese is the default (hardcoded in HTML). English strings are in strings.js.
// On first call, harvests Vietnamese text from DOM into STRINGS.vi so we can
// switch freely between languages without reloading.

function getLang() {
    return document.documentElement.getAttribute("lang") || "vi";
}

function t(key) {
    var lang = getLang();
    var dict = window.STRINGS && window.STRINGS[lang];
    if (dict && dict[key] !== undefined) return dict[key];
    if (lang === "vi") {
        var vi = window.STRINGS && window.STRINGS.vi;
        if (vi && vi[key] !== undefined) return vi[key];
    }
    return null;
}

// Build STRINGS.vi from current DOM (call once before any EN override)
function buildViDict() {
    if (!window.STRINGS) window.STRINGS = {};
    if (!window.STRINGS.vi) window.STRINGS.vi = {};
    if (window.STRINGS._viHarvested) return;
    window.STRINGS._viHarvested = true;

    var vi = window.STRINGS.vi;

    document.querySelectorAll("[data-i18n]").forEach(function (el) {
        var key = el.getAttribute("data-i18n");
        if (key && !vi[key]) {
            vi[key] = el.textContent;
        }
    });
    document.querySelectorAll("[data-i18n-ph]").forEach(function (el) {
        var key = el.getAttribute("data-i18n-ph");
        var val = el.getAttribute("placeholder") || "";
        if (key && !vi["_ph:" + key]) {
            vi["_ph:" + key] = val;
        }
    });
    document.querySelectorAll("[data-i18n-title]").forEach(function (el) {
        var key = el.getAttribute("data-i18n-title");
        var val = el.getAttribute("title") || "";
        if (key && !vi["_title:" + key]) {
            vi["_title:" + key] = val;
        }
    });
}

function applyTranslations() {
    buildViDict();

    var lang = getLang();
    var dict = window.STRINGS && window.STRINGS[lang];
    var vi = window.STRINGS.vi;
    if (!dict && lang !== "vi") return;

    document.querySelectorAll("[data-i18n]").forEach(function (el) {
        var key = el.getAttribute("data-i18n");
        var text = (dict && dict[key]) || (vi && vi[key]);
        if (text) el.textContent = text;
    });

    document.querySelectorAll("[data-i18n-ph]").forEach(function (el) {
        var key = el.getAttribute("data-i18n-ph");
        var text = (dict && dict[key]) || (vi && vi["_ph:" + key]);
        if (text) el.setAttribute("placeholder", text);
    });

    document.querySelectorAll("[data-i18n-title]").forEach(function (el) {
        var key = el.getAttribute("data-i18n-title");
        var text = (dict && dict[key]) || (vi && vi["_title:" + key]);
        if (text) el.setAttribute("title", text);
    });

    var titleEl = document.querySelector("title[data-i18n]");
    if (titleEl) {
        var key = titleEl.getAttribute("data-i18n");
        var text = (dict && dict[key]) || (vi && vi[key]);
        if (text) titleEl.textContent = text;
    }
}
