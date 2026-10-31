#!/usr/bin/env bash
#
# Sprint 1 D7 — Phase B compliance gate.
#
# Verifies seven guarantees at the source level:
#   1. The hook-thread `lock_guard<recursive_mutex>` regression-trap lines
#      from D4 SPIKE are still commented — ≥2 in HookEngine.cpp
#      (LowLevelKeyboardProc + LowLevelMouseProc). Wave 3 PR 3.2 moved
#      WinEventProc into FocusOwner.cpp so the threshold dropped from 3 → 2.
#   2. No uncommented `stateMutex_` reference inside any hook-callback entry
#      function body (`LowLevelKeyboardProc`, `WinEventProc`,
#      `LowLevelMouseProc`, `RawInputWndProc`). WinEventProc now SKIPs in
#      HookEngine.cpp; FocusOwner has no stateMutex_, so the invariant
#      holds vacuously there.
#   3. All migrated atomic fields (D5 + D5.1 + D5.2 + D6) use `.load()` /
#      `.store()` — no plain assignment or read of these fields. The atomic
#      RCU `config_` field also obeys this rule.
#   4. The output injector RCU pointer is accessed only through atomic load /
#      store operations.
#   5. QuickSyncFromSharedState retains its lock-free hot path.
#   6. Exactly one production keyboard-hook API call exists under `src`, owned
#      by HookLifecycle. The exact-Dorion exception is one delayed, bounded,
#      quiet-gated replacement through that site, with one HookEngine requester.
#   7. Generic/burst/immediate recovery and Raw Input hook-healer machinery
#      stays removed.
#
# Run from repo root:
#   bash tools/audit/check_hook_thread_no_mutex.sh
#
# Exit code 0 = pass, non-zero = fail.
#
# Notes:
# * This is a source-level heuristic, not a full call-graph analysis. Check 6
#   uses a comment/literal-aware lexical scan; the remaining checks catch the
#   named regression patterns with some accepted over-approximation.
# * If a check produces a false positive, prefer tightening this script over
#   weakening the source-level invariant.
#
# Reference: docs/plans/sprint-1-single-owner-refactor.md §B D7.

set -e

# Emit one `path:line:call` record for every low-level keyboard-hook install
# found below the supplied source roots.  Check 6 and its fixture-based
# regression tests intentionally share this scanner so the tests exercise the
# exact production gate.
scan_keyboard_hook_installs() {
    python3 - "$@" <<'PY'
import re
import sys
from pathlib import Path


SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx",
    ".h", ".hh", ".hpp", ".hxx",
    ".inl", ".ipp",
}
CALL_PATTERN = re.compile(
    r"(?<![A-Za-z0-9_])"
    r"(?P<api>SetWindowsHookEx(?:A|W)?)"
    r"(?![A-Za-z0-9_])\s*\(\s*"
    r"WH_KEYBOARD_LL(?![A-Za-z0-9_])"
)
RAW_PREFIXES = ("u8R\"", "uR\"", "UR\"", "LR\"", "R\"")


def source_files(roots):
    for root_arg in roots:
        root = Path(root_arg)
        if root.is_file():
            if root.suffix.lower() in SOURCE_SUFFIXES:
                yield root
            continue
        if root.is_dir():
            yield from sorted(
                path for path in root.rglob("*")
                if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
            )


def blank_non_newlines(chars, start, end):
    for index in range(start, end):
        if chars[index] not in "\r\n":
            chars[index] = " "


def raw_literal_end(text, start):
    if start > 0 and (text[start - 1].isalnum() or text[start - 1] == "_"):
        return None
    prefix = next((item for item in RAW_PREFIXES if text.startswith(item, start)), None)
    if prefix is None:
        return None
    delimiter_start = start + len(prefix)
    open_paren = text.find("(", delimiter_start, delimiter_start + 17)
    if open_paren == -1:
        return None
    delimiter = text[delimiter_start:open_paren]
    if any(char.isspace() or char in "\\()" for char in delimiter):
        return None
    terminator = ")" + delimiter + '"'
    close = text.find(terminator, open_paren + 1)
    return len(text) if close == -1 else close + len(terminator)


def without_comments_and_literals(text):
    chars = list(text)
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            end = len(text) if end == -1 else end
            blank_non_newlines(chars, index, end)
            index = end
            continue
        if text.startswith("/*", index):
            close = text.find("*/", index + 2)
            end = len(text) if close == -1 else close + 2
            blank_non_newlines(chars, index, end)
            index = end
            continue

        raw_end = raw_literal_end(text, index)
        if raw_end is not None:
            blank_non_newlines(chars, index, raw_end)
            index = raw_end
            continue

        if text[index] in "\"'":
            quote = text[index]
            end = index + 1
            while end < len(text):
                if text[end] == "\\":
                    end = min(end + 2, len(text))
                    continue
                if text[end] == quote:
                    end += 1
                    break
                end += 1
            blank_non_newlines(chars, index, end)
            index = end
            continue
        index += 1
    return "".join(chars)


cwd = Path.cwd()
for source_path in source_files(sys.argv[1:]):
    text = source_path.read_text(encoding="utf-8", errors="replace")
    clean = without_comments_and_literals(text)
    try:
        display_path = source_path.resolve().relative_to(cwd.resolve())
    except ValueError:
        display_path = source_path
    for match in CALL_PATTERN.finditer(clean):
        line = clean.count("\n", 0, match.start()) + 1
        print(f"{display_path}:{line}:{match.group('api')}(WH_KEYBOARD_LL")
PY
}

count_keyboard_hook_installs() {
    scan_keyboard_hook_installs "$@" | awk 'NF { count += 1 } END { print count + 0 }'
}

run_keyboard_hook_scan_self_tests() {
    local fixture_root fixture_source case_file label expected actual failures
    fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/vkey-hook-audit.XXXXXX")
    fixture_source="$fixture_root/src/app/system"
    case_file="$fixture_source/Adversarial.cpp"
    failures=0
    mkdir -p "$fixture_source"

    printf '%s\n' \
        'void InstallInitialHook() {' \
        '    SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, module, 0);' \
        '}' \
        > "$fixture_source/HookLifecycle.cpp"

    assert_keyboard_fixture_count() {
        label="$1"
        expected="$2"
        actual=$(count_keyboard_hook_installs "$fixture_root/src")
        if [ "$actual" -ne "$expected" ]; then
            echo "  SELF-TEST FAIL [$label]: expected $expected install(s), found $actual"
            scan_keyboard_hook_installs "$fixture_root/src" | sed 's/^/    /'
            failures=$((failures + 1))
        else
            echo "  SELF-TEST OK   [$label]: found $actual install(s)"
        fi
    }

    assert_keyboard_fixture_count "baseline W call" 1

    printf '%s\n' \
        '// SetWindowsHookExW(WH_KEYBOARD_LL, IgnoredProc, nullptr, 0);' \
        '/* SetWindowsHookExA(WH_KEYBOARD_LL, IgnoredProc, nullptr, 0); */' \
        'const char* text = "SetWindowsHookEx(WH_KEYBOARD_LL, ...)";' \
        'const char* raw = R"tag(SetWindowsHookExW(WH_KEYBOARD_LL, ...))tag";' \
        > "$case_file"
    assert_keyboard_fixture_count "comment and string decoys" 1

    printf '%s\n' \
        'void InstallAgain() { SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, nullptr, 0); }' \
        > "$case_file"
    assert_keyboard_fixture_count "second W call fails exact-one" 2

    printf '%s\n' \
        'void InstallAgain() { SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, nullptr, 0); }' \
        > "$case_file"
    assert_keyboard_fixture_count "second generic call fails exact-one" 2

    printf '%s\n' \
        'void InstallAgain() { SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardProc, nullptr, 0); }' \
        > "$case_file"
    assert_keyboard_fixture_count "second A call fails exact-one" 2

    printf '%s\n' \
        'void InstallAgain() {' \
        '    SetWindowsHookExW /* comment between API and call */ (WH_KEYBOARD_LL,' \
        '        KeyboardProc, nullptr, 0);' \
        '}' \
        > "$case_file"
    assert_keyboard_fixture_count "second comment-separated W call fails exact-one" 2

    printf '%s\n' \
        'void InstallAgain() {' \
        '    SetWindowsHookEx' \
        '    /* comment and newlines between call tokens */' \
        '    (' \
        '        /* hook kind */' \
        '        WH_KEYBOARD_LL, KeyboardProc, nullptr, 0);' \
        '}' \
        > "$case_file"
    assert_keyboard_fixture_count "second multiline generic call fails exact-one" 2

    printf '%s\n' '// no second app-local install in this case' > "$case_file"
    mkdir -p "$fixture_root/src/core"
    printf '%s\n' \
        'void InstallFromSiblingProductionTree() {' \
        '    SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, nullptr, 0);' \
        '}' \
        > "$fixture_root/src/core/SiblingHook.cpp"
    assert_keyboard_fixture_count "second call in sibling src subtree fails exact-one" 2

    rm -rf -- "$fixture_root"
    if [ "$failures" -gt 0 ]; then
        echo "  SELF-TEST FAIL: $failures keyboard-hook scanner case(s) failed"
        return 1
    fi
    echo "  SELF-TEST PASS: keyboard-hook scanner adversarial fixtures"
}

if [ "${1:-}" = "--self-test-keyboard-hook-scan" ]; then
    run_keyboard_hook_scan_self_tests
    exit $?
fi

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"

CPP="src/app/system/HookEngine.cpp"
LIFECYCLE_CPP="src/app/system/HookLifecycle.cpp"
DORION_POLICY_H="src/core/DorionHookReclaimPolicy.h"
errors=0

for required_source in "$CPP" "$LIFECYCLE_CPP" "$DORION_POLICY_H"; do
    if [ ! -f "$required_source" ]; then
        echo "ERROR: $required_source not found (run from repo root)"
        exit 2
    fi
done

echo "=== Sprint 1 D7 audit: $(basename "$CPP") ==="
echo

# ────────────────────────────────────────────────────────────────────────
# Check 1: D4 SPIKE comment integrity
# ────────────────────────────────────────────────────────────────────────
# REGRESSION TRAP — Phase B Sprint 1 D11 downgraded `stateMutex_` from
# `std::recursive_mutex` to `std::mutex`. The original 3 commented lock_guard
# lines in HookEngine.cpp (LowLevelKeyboardProc / WinEventProc /
# LowLevelMouseProc) referenced the old `recursive_mutex` type which no
# longer exists, so any "cleanup" attempt that uncomments them triggers
# a compile error. That is the intended trap.
#
# Wave 3 PR 3.2 (2026-05-24) moved WinEventProc out of HookEngine into
# FocusOwner.cpp; FocusOwner has no stateMutex_, so the WinEventProc trap
# is moot there. Remaining traps in HookEngine.cpp: LowLevelKeyboardProc +
# LowLevelMouseProc (≥2).
#
# This check enforces the lines stay commented — verifying both that
# someone hasn't uncommented (which would fail compile anyway) and that
# someone hasn't deleted the trap entirely (which would lose the
# regression marker for the eventual mutex removal). If a future
# reviewer flags these as "dangling references", point them here and
# at the in-source comments above each line.

echo "Check 1: D4 SPIKE comment integrity (≥2 commented lock_guard lines, post Wave 3 PR 3.2)"
spike_commented=$(grep -cE "^\s*//\s*std::lock_guard<std::recursive_mutex>" "$CPP")
if [ "$spike_commented" -lt 2 ]; then
    echo "  FAIL: expected ≥2 commented lock_guard<recursive_mutex> lines, found $spike_commented"
    grep -nE "^\s*//\s*std::lock_guard<std::recursive_mutex>" "$CPP" || true
    errors=$((errors + 1))
else
    echo "  OK: $spike_commented commented lock_guard line(s) preserved"
fi

# ────────────────────────────────────────────────────────────────────────
# Check 2: No uncommented stateMutex_ in hook callback entry function bodies
# ────────────────────────────────────────────────────────────────────────
# Plan §B D7 scope: LL keyboard / LL mouse / WinEvent — the callbacks that
# run on hookThread_ (LL keyboard / mouse / RawInput WM_INPUT). WinEventProc
# is included for plan continuity even though current architecture installs
# it from main thread (WINEVENT_OUTOFCONTEXT runs on installer thread); the
# audit catches the case where a future refactor moves it back onto the
# hook thread without removing the lock.
#
# Sprint 1 D10 retired `FocusPollTimerProc` (the 200 ms `SetTimer` poll for
# CJK layout + foreground PID). Its body now lives in `OnTickPoll`, driven
# from `MainThreadWorker`'s tick branch. Worker-thread access to stateMutex_
# is fine — Rule #11 only forbids it on the LL hook thread.

HOOK_ENTRIES=(
    "LowLevelKeyboardProc"
    "WinEventProc"
    "LowLevelMouseProc"
    "RawInputWndProc"
)

echo
echo "Check 2: stateMutex_ not reachable from hook callback entries"
for fn in "${HOOK_ENTRIES[@]}"; do
    # Extract function body using awk: from the line containing "::fn(" through
    # the matching closing brace at column 0. This is approximate (assumes
    # consistent indentation — closing brace at column 0 — which the codebase
    # follows). Grep -v filters comment lines.
    body=$(awk -v fn="$fn" '
        $0 ~ ("HookEngine::" fn "[[:space:]]*\\(") { in_fn = 1 }
        in_fn { print }
        in_fn && /^\}/ { in_fn = 0 }
    ' "$CPP")
    if [ -z "$body" ]; then
        # Function not found — could be a static helper or moved. Skip with note.
        echo "  SKIP: $fn (not found in $CPP)"
        continue
    fi
    # Count uncommented stateMutex_ references in the body.
    refs=$(echo "$body" | grep -vE "^\s*//" | grep -c "stateMutex_" || true)
    if [ "$refs" -gt 0 ]; then
        echo "  FAIL: $fn body contains $refs uncommented stateMutex_ reference(s)"
        echo "$body" | grep -vE "^\s*//" | grep -nE "stateMutex_" || true
        errors=$((errors + 1))
    else
        echo "  OK: $fn — 0 uncommented stateMutex_ references"
    fi
done

# ────────────────────────────────────────────────────────────────────────
# Check 3: Migrated atomic fields use .load() / .store() exclusively
# ────────────────────────────────────────────────────────────────────────
# Fields migrated across D5 / D5.1 / D5.2 / D6. Any plain access (assignment
# or read) on these fields outside of an explicit `.load(` / `.store(` call
# violates Rule #11.3 and is flagged.
#
# False-positive sources we explicitly tolerate:
#   * Lines starting with `//` (full-line C++ comments)
#   * Lines starting with `///` (Doxygen-style)
#   * Lines containing only field name in a printf format string would also
#     match — we accept that and recommend tightening this filter only if
#     it triggers in practice.
#   * Lines bearing an inline `// audit-allow: <reason>` annotation —
#     used for legitimate pass-by-reference patterns like
#     `make_unique<Gate>(atomic_field_)` where the receiver stores a
#     const ref and uses `.load()` inside. The reason after the colon is
#     mandatory so reviewers see the justification without leaving the
#     file. See `docs/CODING_RULES/12-worker-thread-doctrine.md` §12.6.

# Sprint 2 D3 deleted: isConsoleApp_ — Console selection now flows through
# WindowClassification.isConsole → SplitDispatchInjector(5ms) by the factory.
# Sprint 2 D4 deleted: useEditMsgPath_ — replaced by HookEngine::IsSync-
# ReplaceChannel() proxy on injector_->SettleBudget()==0.
# Post-T3 ChannelTraits cleanup deleted: isElectronApp_ + needBaitChar_ —
# traits moved onto IOutputInjector (HasMultiProcessRenderer() /
# RequiresSyntheticAlphaLockstep() / NeedsBaitCharPrefix()) so the dispatch
# channel owns its own behavior.
ATOMIC_BOOLS="vietnameseMode_|isTsfApp_|isExcludedApp_|skipEmptyChar_|useClipboardPaste_|isOutlookApp_|macroEnabled_|macroInEnglish_|autoCaps_|autoCapsMacro_|tempOffMacroByEsc_|tempOffByAlt_"
ATOMIC_DWORD="excludedPid_"
ATOMIC_ENUM="currentMethod_"
ATOMIC_RCU="config_"
# Sprint 2 D2: injector_ is std::atomic<std::shared_ptr<IOutputInjector>>.
# Same discipline as config_ — access only via std::atomic_load/store, never
# via plain assignment or read.
ATOMIC_INJECTOR="injector_"

echo
echo "Check 3: atomic fields use .load()/.store() (no plain assignment or read)"

# Helper: count plain accesses for a field pattern.
# A "plain access" is any reference NOT followed by `.load(` or `.store(`,
# excluding C++ comments and the field's own declaration line.
audit_field_group() {
    local label="$1"
    local pattern="$2"
    local extra_exclude="$3"  # optional extra grep -vE pattern

    # Build the exclusion regex. Lines we don't count as violations:
    #   - .load( or .store( (the migrated atomic call sites)
    #   - C++ // comment lines (whole-line comments)
    #   - field declaration ("std::atomic<...>")
    #   - inline `// audit-allow:` annotation (see header comment above)
    #   - extra exclude (e.g. configEvent_ for the config_ check)
    local exclude="\\.(load|store)\\s*\\(|^\\s*[0-9]+:\\s*//|std::atomic|//\\s*audit-allow:"
    if [ -n "$extra_exclude" ]; then
        exclude="$exclude|$extra_exclude"
    fi

    local matches
    matches=$(grep -nE "\b($pattern)\b" "$CPP" | grep -vE "$exclude" || true)
    local count
    count=$(echo -n "$matches" | grep -c '^' || true)
    if [ "$count" -gt 0 ]; then
        echo "  FAIL [$label]: $count plain access(es)"
        echo "$matches" | head -10 | sed 's/^/    /'
        errors=$((errors + 1))
    else
        echo "  OK   [$label]"
    fi
}

audit_field_group "atomic<bool> primitives"  "$ATOMIC_BOOLS"  ""
audit_field_group "atomic<DWORD>"            "$ATOMIC_DWORD"  ""
audit_field_group "atomic<InputMethod>"      "$ATOMIC_ENUM"   ""
# config_ check excludes configEvent_ / configReloadCallback_ / config_t typedefs
audit_field_group "atomic<shared_ptr<TypingConfig>>" "$ATOMIC_RCU" "configEvent_|configReloadCallback_|TypingConfig"

# ────────────────────────────────────────────────────────────────────────
# Check 4: injector_ accessed only via std::atomic_load / std::atomic_store
# ────────────────────────────────────────────────────────────────────────
# Sprint 2 D2 introduced injector_ (RCU on std::shared_ptr<IOutputInjector>).
# Hot path readers MUST use std::atomic_load(&injector_) — plain
# `injector_->Replace(...)` would be a torn read on the shared_ptr control
# block and could invoke Replace on a destructed impl. Same regression-
# trap intent as Check 3 for config_.
echo
echo "Check 4: injector_ accessed only via std::atomic_load / std::atomic_store"
inj_violations=$(grep -nE "\binjector_\b" "$CPP" | \
    grep -vE "\\.(load|store)\\s*\\(|^\\s*[0-9]+:\\s*//|std::atomic|//\\s*audit-allow:" || true)
inj_count=$(echo -n "$inj_violations" | grep -c '^' || true)
if [ "$inj_count" -gt 0 ]; then
    echo "  FAIL: $inj_count plain access(es) to injector_ outside .load()/.store()"
    echo "$inj_violations" | head -10 | sed 's/^/    /'
    errors=$((errors + 1))
else
    echo "  OK"
fi
# Suppress unused-variable warning when ATOMIC_INJECTOR is reserved for future
# decomposition (e.g. dynamic_cast traits via the same regex helper).
: "${ATOMIC_INJECTOR:?}" >/dev/null

# ────────────────────────────────────────────────────────────────────────
# Check 5: QuickSyncFromSharedState hot path is lock-free
# ────────────────────────────────────────────────────────────────────────
# ProcessKeyDown calls QuickSyncFromSharedState on every keystroke from the
# LL hook thread. Rule #11.3 forbids the hook thread from waiting on a
# mutex contended with the main thread. The function MUST early-return
# before acquiring stateMutex_ on the common case (epoch unchanged).
#
# Heuristic: in the body of QuickSyncFromSharedState, an uncommented
# `return` statement must appear BEFORE the first uncommented
# `lock_guard<std::mutex> _lock(stateMutex_)` line. That return is the
# lock-free fast path; the lock guards only the slow path that handles
# an actual SharedState change.
#
# This check would have caught the pre-fix shape where the lock was
# acquired unconditionally at the top of the function (Pre-T3 review
# Minor 2, see docs/TODO.md).
echo
echo "Check 5: QuickSyncFromSharedState hot path returns before stateMutex_ lock"
qs_body=$(awk '
    /HookEngine::QuickSyncFromSharedState[[:space:]]*\(/ { in_fn = 1; next }
    in_fn { print }
    in_fn && /^\}/ { in_fn = 0 }
' "$CPP")
if [ -z "$qs_body" ]; then
    echo "  SKIP: QuickSyncFromSharedState body not found"
else
    # Strip whole-line C++ comments so commented patterns don't fool the heuristic.
    qs_clean=$(echo "$qs_body" | grep -vE "^\s*//")
    # Line numbers within the cleaned body for the lock and the first return.
    lock_line=$(echo "$qs_clean" | grep -nE "lock_guard<std::mutex>.*stateMutex_" | head -1 | cut -d: -f1)
    return_line=$(echo "$qs_clean" | grep -nE "\breturn[[:space:]]*;" | head -1 | cut -d: -f1)
    if [ -z "$lock_line" ]; then
        echo "  OK: QuickSyncFromSharedState no longer locks stateMutex_"
    elif [ -z "$return_line" ]; then
        echo "  FAIL: QuickSyncFromSharedState locks stateMutex_ with no early return"
        errors=$((errors + 1))
    elif [ "$return_line" -ge "$lock_line" ]; then
        echo "  FAIL: QuickSyncFromSharedState lock acquired before any early return"
        echo "        (lock at body line $lock_line, first return at body line $return_line)"
        echo "        Hook hot path must early-return on unchanged epoch BEFORE locking"
        errors=$((errors + 1))
    else
        echo "  OK: lock-free early return at body line $return_line precedes lock at $lock_line"
    fi
fi

# ────────────────────────────────────────────────────────────────────────
# Check 6: One API site and one bounded, quiet-gated Dorion requester
# ────────────────────────────────────────────────────────────────────────
# HookLifecycle owns the sole WH_KEYBOARD_LL installation API site. Startup and
# the exact Dorion foreground compatibility transaction share that helper, so a
# runtime replacement cannot add another SetWindowsHookEx call site. The narrow
# exception must wait 1800 ms, retry only its readiness gate at 100 ms for at
# most 30 checks, and call ReplaceSingleHook once only after the delayed ticket
# is due and the quiet predicate accepts it. HookEngine may request it from
# exactly one explicitly named Dorion policy branch.
echo
echo "Check 6: one WH_KEYBOARD_LL API site and bounded quiet-gated Dorion reclaim"
if ! run_keyboard_hook_scan_self_tests; then
    errors=$((errors + 1))
fi
keyboard_install_sites=$(scan_keyboard_hook_installs src)
keyboard_install_count=$(count_keyboard_hook_installs src)
if [ "$keyboard_install_count" -ne 1 ]; then
    echo "  FAIL: expected exactly 1 keyboard-hook installation site, found $keyboard_install_count"
    echo "$keyboard_install_sites" | sed '/^$/d; s/^/    /'
    errors=$((errors + 1))
elif ! echo "$keyboard_install_sites" | grep -q "^${LIFECYCLE_CPP}:"; then
    echo "  FAIL: sole keyboard-hook installation is not owned by HookLifecycle"
    echo "$keyboard_install_sites" | sed 's/^/    /'
    errors=$((errors + 1))
else
    install_line=$(echo "$keyboard_install_sites" | head -1 | cut -d: -f2)
    pump_line=$(grep -n 'while (GetMessageW' "$LIFECYCLE_CPP" \
        | head -1 | cut -d: -f1)
    if [ -z "$install_line" ] || [ -z "$pump_line" ] || [ "$install_line" -ge "$pump_line" ]; then
        echo "  FAIL: HookLifecycle keyboard-hook install must occur before the message pump"
        echo "$keyboard_install_sites" | sed 's/^/    /'
        errors=$((errors + 1))
    else
        echo "  OK: sole install API site is owned by HookLifecycle before its pump"
    fi
fi

dorion_request_sites=$(grep -RInE --include='*.cpp' --include='*.h' \
    'lifecycle_[[:space:]]*\.[[:space:]]*RequestDorionKeyboardReclaim[[:space:]]*\(' \
    src || true)
dorion_request_count=$(echo -n "$dorion_request_sites" | grep -c '^' || true)
if [ "$dorion_request_count" -ne 1 ]; then
    echo "  FAIL: expected exactly 1 Dorion keyboard-reclaim request, found $dorion_request_count"
    echo "$dorion_request_sites" | sed '/^$/d; s/^/    /'
    errors=$((errors + 1))
elif ! echo "$dorion_request_sites" | grep -qE '^src/app/system/HookEngine(Focus)?\.cpp:'; then
    echo "  FAIL: Dorion keyboard-reclaim request must be owned by HookEngine"
    echo "$dorion_request_sites" | sed 's/^/    /'
    errors=$((errors + 1))
else
    echo "  OK: one explicitly named Dorion requester in HookEngine"
fi

# The policy must reject every executable spelling except the canonical exact
# lower-case basename produced by FocusOwner. Do not broaden this to Chromium,
# Electron, Discord, or a configurable application list: that would turn the
# compatibility exception back into generic hook recovery.
exact_dorion_sites=$(grep -nE \
    'exeName[[:space:]]*!=[[:space:]]*L"dorion\.exe"' \
    "$DORION_POLICY_H" || true)
exact_dorion_count=$(echo -n "$exact_dorion_sites" | grep -c '^' || true)
if [ "$exact_dorion_count" -ne 1 ]; then
    echo "  FAIL: Dorion policy must have exactly 1 exact exeName rejection, found $exact_dorion_count"
    echo "$exact_dorion_sites" | sed '/^$/d; s/^/    /'
    errors=$((errors + 1))
else
    echo "  OK: reclaim policy is restricted to exact dorion.exe"
fi

# Keep the compatibility window explicit and reviewable. Retrying this timer
# retries only the quiet/identity gate; the hook replacement itself remains a
# one-shot transaction.
for dorion_bound in \
    'kDorionInitialDelayMs[[:space:]]*=[[:space:]]*1800' \
    'kDorionGateRetryMs[[:space:]]*=[[:space:]]*100' \
    'kDorionMaxGateChecks[[:space:]]*=[[:space:]]*30'; do
    bound_sites=$(grep -nE "$dorion_bound" "$LIFECYCLE_CPP" || true)
    bound_count=$(echo -n "$bound_sites" | grep -c '^' || true)
    if [ "$bound_count" -ne 1 ]; then
        echo "  FAIL: expected exactly 1 bounded Dorion constant matching '$dorion_bound', found $bound_count"
        echo "$bound_sites" | sed '/^$/d; s/^/    /'
        errors=$((errors + 1))
    fi
done

if grep -q 'kDorionInitialDelayMs[[:space:]]*=[[:space:]]*1800' "$LIFECYCLE_CPP" \
        && grep -q 'kDorionGateRetryMs[[:space:]]*=[[:space:]]*100' "$LIFECYCLE_CPP" \
        && grep -q 'kDorionMaxGateChecks[[:space:]]*=[[:space:]]*30' "$LIFECYCLE_CPP"; then
    echo "  OK: Dorion delay/retry budget is explicit (1800 ms + 30 x 100 ms gate checks)"
fi

slot_sites=$(grep -nE '\bDorionDelayedReclaimSlots\b' "$LIFECYCLE_CPP" || true)
slot_count=$(echo -n "$slot_sites" | grep -c '^' || true)
quiet_policy_sites=$(grep -nE '\bIsDorionReclaimQuiet[[:space:]]*\(' "$CPP" || true)
quiet_policy_count=$(echo -n "$quiet_policy_sites" | grep -c '^' || true)
replacement_sites=$(grep -nE '\bReplaceSingleHook[[:space:]]*\(' "$LIFECYCLE_CPP" || true)
replacement_count=$(echo -n "$replacement_sites" | grep -c '^' || true)
take_due_line=$(grep -nE '\.TakeIfDue[[:space:]]*\(' "$LIFECYCLE_CPP" \
    | head -1 | cut -d: -f1)
quiet_gate_sites=$(grep -nE '\breclaimReady[[:space:]]*\(' "$LIFECYCLE_CPP" || true)
quiet_gate_count=$(echo -n "$quiet_gate_sites" | grep -c '^' || true)
quiet_gate_line=$(echo "$quiet_gate_sites" | head -1 | cut -d: -f1)
replacement_run_sites=$(grep -nE '\brunReplacement[[:space:]]*\(' "$LIFECYCLE_CPP" || true)
replacement_run_count=$(echo -n "$replacement_run_sites" | grep -c '^' || true)
replacement_run_line=$(echo "$replacement_run_sites" | head -1 | cut -d: -f1)

if [ "$slot_count" -ne 1 ]; then
    echo "  FAIL: expected exactly 1 bounded DorionDelayedReclaimSlots owner, found $slot_count"
    echo "$slot_sites" | sed '/^$/d; s/^/    /'
    errors=$((errors + 1))
elif [ "$quiet_policy_count" -ne 1 ]; then
    echo "  FAIL: HookEngine must invoke IsDorionReclaimQuiet exactly once, found $quiet_policy_count"
    echo "$quiet_policy_sites" | sed '/^$/d; s/^/    /'
    errors=$((errors + 1))
elif [ "$replacement_count" -ne 1 ]; then
    echo "  FAIL: delayed Dorion path must have exactly 1 ReplaceSingleHook call, found $replacement_count"
    echo "$replacement_sites" | sed '/^$/d; s/^/    /'
    errors=$((errors + 1))
elif [ "$quiet_gate_count" -ne 1 ] || [ "$replacement_run_count" -ne 1 ]; then
    echo "  FAIL: expected exactly 1 quiet-gate call and 1 replacement-helper call"
    echo "        (quiet=$quiet_gate_count, replacement=$replacement_run_count)"
    errors=$((errors + 1))
elif [ -z "$take_due_line" ] || [ -z "$quiet_gate_line" ] \
        || [ "$take_due_line" -ge "$quiet_gate_line" ] \
        || [ "$quiet_gate_line" -ge "$replacement_run_line" ]; then
    echo "  FAIL: replacement helper must run after TakeIfDue and the quiet gate"
    echo "        (due=${take_due_line:-missing}, quiet=${quiet_gate_line:-missing}, replace=${replacement_run_line:-missing})"
    errors=$((errors + 1))
else
    echo "  OK: one replacement appears only after a due delayed ticket and quiet gate"
fi

# A thread timer shares the pump with unrelated TIMERPROC users. Consume only
# timer IDs owned by the Dorion slot table; every foreign WM_TIMER must fall
# through to the normal TranslateMessage/DispatchMessage path.
owned_timer_guard_sites=$(grep -nE \
    'dorionSlots\.HasTimer[[:space:]]*\([[:space:]]*timerId[[:space:]]*\)' \
    "$LIFECYCLE_CPP" || true)
owned_timer_guard_count=$(echo -n "$owned_timer_guard_sites" | grep -c '^' || true)
owned_timer_guard_line=$(echo "$owned_timer_guard_sites" | head -1 | cut -d: -f1)
dispatch_sites=$(grep -nE '\bDispatchMessageW[[:space:]]*\([[:space:]]*&msg[[:space:]]*\)' \
    "$LIFECYCLE_CPP" || true)
dispatch_count=$(echo -n "$dispatch_sites" | grep -c '^' || true)
dispatch_line=$(echo "$dispatch_sites" | head -1 | cut -d: -f1)
timer_pending_catchall=$(grep -nE \
    'msg\.message[[:space:]]*==[[:space:]]*WM_TIMER[^;{]*HasPending' \
    "$LIFECYCLE_CPP" || true)
if [ "$owned_timer_guard_count" -ne 1 ] || [ "$dispatch_count" -ne 1 ] \
        || [ -z "$owned_timer_guard_line" ] || [ -z "$take_due_line" ] \
        || [ -z "$dispatch_line" ] \
        || [ "$owned_timer_guard_line" -ge "$take_due_line" ] \
        || [ "$take_due_line" -ge "$dispatch_line" ] \
        || [ -n "$timer_pending_catchall" ]; then
    echo "  FAIL: WM_TIMER must consume only dorionSlots.HasTimer(timerId); foreign timers must dispatch"
    echo "        (guard=${owned_timer_guard_line:-missing}, due=${take_due_line:-missing}, dispatch=${dispatch_line:-missing})"
    echo "$timer_pending_catchall" | sed '/^$/d; s/^/    /'
    errors=$((errors + 1))
else
    echo "  OK: foreign WM_TIMER messages fall through to DispatchMessageW"
fi

gate_budget_refs=$(grep -cE '\bremainingGateChecks\b' "$LIFECYCLE_CPP" || true)
gate_budget_refs=${gate_budget_refs:-0}
gate_decrement=$(grep -nE \
    '(--[[:space:]]*[^;]*remainingGateChecks|remainingGateChecks[[:space:]]*--|remainingGateChecks[[:space:]]*-[[:space:]]*1)' \
    "$LIFECYCLE_CPP" || true)
if [ "$gate_budget_refs" -lt 2 ] || [ -z "$gate_decrement" ]; then
    echo "  FAIL: delayed readiness retries must consume a finite remainingGateChecks budget"
    errors=$((errors + 1))
else
    echo "  OK: readiness retries consume a finite gate-check budget"
fi

# A failed runtime replacement leaves the pump thread joinable even when the
# keyboard handle is null. HookEngine must reject a second Start before it
# rebuilds/finalizes any state; IsRunning() alone cannot express ownership.
owner_guard_sites=$(grep -nE \
    'if[[:space:]]*\([[:space:]]*lifecycle_\.HasOwnerThread\(\)[[:space:]]*\)[[:space:]]*return false' \
    src/app/system/HookEngine.cpp || true)
owner_guard_count=$(echo -n "$owner_guard_sites" | grep -c '^' || true)
if [ "$owner_guard_count" -ne 1 ]; then
    echo "  FAIL: HookEngine::Start must have exactly 1 lifecycle owner-thread guard, found $owner_guard_count"
    echo "$owner_guard_sites" | sed '/^$/d; s/^/    /'
    errors=$((errors + 1))
else
    echo "  OK: HookEngine startup is guarded by lifecycle pump ownership"
fi

# ────────────────────────────────────────────────────────────────────────
# Check 7: Generic, burst, immediate, and Raw Input recovery stays removed
# ────────────────────────────────────────────────────────────────────────
# These names cover the lifecycle entry points and every former producer or
# state owner.  The scan includes CMake so obsolete recovery components cannot
# be silently linked back into either application target.
echo
echo "Check 7: no generic/burst/immediate/Raw Input hook recovery"
recovery_pattern='PostReinstallHooks|WM_APP_REINSTALL_HOOKS|REINSTALL_REASON_|ReinstallFn|onReinstall_|requestReinstall|lastReinstallTime|HookHijackDetector|KeyboardHookHealthPolicy|KeyboardHookHealthEvidence|ReinstallBurstScheduler|SetChromiumClassActive|PostGhostKey|SetGhostKeyHandler|GhostKeyFn|ghostKeyFn_|HandleGhostChar|injectGhostChar|WM_APP_GHOSTKEY|RegisterDynamicHijacker|IsDynamicHijacker|IsKnownHijackerExe|ApplyFocusOperationalProtectionOnHookThread|isChromiumClassApp_|isKnownHijackerApp_|isKnownHijacker|isJavaApp|hookFireCount_|hijackDetector_|reinstallBurstScheduler_|burstTimerQueue_|DorionReclaimSequenceCompletion|ReplaceSingleHookWhenIdentityMatches|kDorionDelayedReclaimMs|DorionDelayedReclaimSlot([^sA-Za-z0-9_]|$)|Dorion[^[:space:]]*[Bb]urst|[Bb]urst[^[:space:]]*Dorion'
recovery_source_matches=$(grep -RInE --include='*.cpp' --include='*.h' \
    "$recovery_pattern" src || true)
recovery_cmake_matches=$(grep -nE "$recovery_pattern" CMakeLists.txt \
    | sed 's|^|CMakeLists.txt:|' || true)
recovery_matches=$(printf '%s\n%s\n' "$recovery_source_matches" "$recovery_cmake_matches" \
    | sed '/^$/d')
recovery_count=$(echo -n "$recovery_matches" | grep -c '^' || true)
if [ "$recovery_count" -gt 0 ]; then
    echo "  FAIL: found $recovery_count automatic rehook/recovery reference(s)"
    echo "$recovery_matches" | head -40 | sed 's/^/    /'
    if [ "$recovery_count" -gt 40 ]; then
        echo "    ... $((recovery_count - 40)) more"
    fi
    errors=$((errors + 1))
else
    echo "  OK: recovery-only symbols absent"
fi

# A same-process Raw Input watchdog previously caused WH_KEYBOARD_LL to stop
# reaching VKey's own windows. Keep that entire registration path out of the
# production tree; Dorion recovery is deliberately timer/identity based.
raw_input_pattern='RegisterRawInputDevices|RAWINPUTDEVICE|RIDEV_(INPUTSINK|NOLEGACY|REMOVE)|GetRawInputData'
raw_input_matches=$(grep -RInE --include='*.cpp' --include='*.h' \
    "$raw_input_pattern" src || true)
raw_input_count=$(echo -n "$raw_input_matches" | grep -c '^' || true)
if [ "$raw_input_count" -gt 0 ]; then
    echo "  FAIL: found $raw_input_count Raw Input hook-healer reference(s)"
    echo "$raw_input_matches" | head -40 | sed 's/^/    /'
    errors=$((errors + 1))
else
    echo "  OK: Raw Input registration/reader path absent"
fi

# The replacement call's source-order fence above rejects a second immediate
# call. Also reject explicitly named immediate Dorion paths so they cannot hide
# behind a wrapper while leaving the delayed call count unchanged.
immediate_dorion_matches=$(grep -InE \
    '(Dorion[^[:cntrl:]]*[Ii]mmediate|[Ii]mmediate[^[:cntrl:]]*Dorion)' \
    "$LIFECYCLE_CPP" src/app/system/HookLifecycle.h || true)
immediate_dorion_count=$(echo -n "$immediate_dorion_matches" | grep -c '^' || true)
if [ "$immediate_dorion_count" -gt 0 ]; then
    echo "  FAIL: found $immediate_dorion_count immediate Dorion reclaim reference(s)"
    echo "$immediate_dorion_matches" | sed 's/^/    /'
    errors=$((errors + 1))
else
    echo "  OK: no immediate Dorion replacement path"
fi

# ────────────────────────────────────────────────────────────────────────
# Result
# ────────────────────────────────────────────────────────────────────────
echo
if [ "$errors" -gt 0 ]; then
    echo "FAIL: $errors check(s) failed — Phase B compliance regressed"
    exit 1
fi
echo "PASS: all D7 audit checks passed — Phase B compliance maintained"
exit 0
