cmake_minimum_required(VERSION 3.20)
project(VKey VERSION 4.3.0 LANGUAGES CXX)

# Generate Version.h from Version.h.in — version is defined once in project() above.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Version.h
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows specific configurations
if(MSVC)
    add_compile_options(/W4 /WX /permissive- /utf-8)
    add_definitions(-DUNICODE -D_UNICODE)
    # Size shrink. Most flags here are already implicit in MSVC's /O2 Release
    # path (/Gy is implied by /O2; /OPT:REF and /OPT:ICF are on by default when
    # /INCREMENTAL:NO is set, which CMake Release defaults to). We restate them
    # explicitly so a future tweak to release flags doesn't silently turn them
    # off. The actual NEW flag here is:
    #   /Gw  — data-level linking: each global var in its own COMDAT so
    #          /OPT:REF can strip unreferenced const tables and /OPT:ICF can
    #          fold identical ones (template-emitted strings, vtables, etc.).
    #          NOT implied by /O2 and saves measurable .rdata.
    add_compile_options($<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw>)
    # Whole-program optimization (Release only — Debug/Linux test loop unaffected).
    # /GL makes each TU emit IL instead of machine code; the linker (/LTCG below)
    # then does cross-TU inlining + whole-program dead-code elimination. /GL on the
    # static libs (VKeyEngine/VKeyCore) is REQUIRED or there is no inlining across
    # the lib boundary into VKeyTSF/VKeyApp/VKeyLite. A /GL object forces LTCG at
    # link time regardless, so the two flags are always paired.
    add_compile_options($<$<CONFIG:Release>:/GL>)
    # /GR-: RTTI is provably unused (0 dynamic_cast/typeid in src/; Sciter headers
    # grep-clean). Strips type_info objects + RTTI COMDATs — most valuable here
    # because of the many small polymorphic classes (IEngineRule/IFeature/IGate/
    # injectors/proposals). If a future feature ever needs dynamic_cast, scope /GR
    # back onto that one target rather than dropping this global strip.
    add_compile_options($<$<CONFIG:Release>:/GR->)
    # Linker security flags: ASLR, DEP, High-Entropy ASLR
    add_link_options(/DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
    # PE legitimacy: checksum (reduces AV false positives)
    add_link_options(/RELEASE)
    add_link_options($<$<CONFIG:Release>:/INCREMENTAL:NO>
                     $<$<CONFIG:Release>:/LTCG>
                     $<$<CONFIG:Release>:/OPT:REF>
                     $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# Per-target include directories (no global include_directories)

# Engine Library (pure C++17, zero platform dependencies)
set(NEXTKEY_ENGINE_SOURCES
    src/core/config/TypingConfig.h
    src/core/config/ConfigSnapshot.h
    src/core/QuickConvertLogic.cpp
    src/core/QuickConvertLogic.h
    src/core/engine/TypingEngine.cpp
    src/core/engine/TypingEngine.h
    src/core/engine/EngineFactory.cpp
    src/core/engine/EngineFactory.h
    src/core/engine/IInputEngine.h
    src/core/engine/EngineHelpers.h
    src/core/engine/EnglishProtection.h
    src/core/engine/VietnameseTables.h
    src/core/engine/VietnamesePhonologyData.h
    src/core/engine/PhonotacticsValidator.cpp
    src/core/engine/PhonotacticsValidator.h
    src/core/engine/Phonotactics.cpp
    src/core/engine/Phonotactics.h
    src/core/engine/TypingAction.h
    src/core/engine/CodeTableConverter.cpp
    src/core/engine/CodeTableConverter.h
    src/core/engine/rule/EngineRulePhase.h
    src/core/engine/rule/EngineRuleResult.h
    src/core/engine/rule/EngineRuleContext.h
    src/core/engine/rule/IEngineRule.h
    src/core/engine/rule/EngineRuleRegistry.h
    src/core/engine/rule/EngineRuleRegistry.cpp
    src/core/engine/rule/IToneExecutor.h
    src/core/engine/rule/ToneRule.h
    src/core/engine/rule/ToneRule.cpp
    src/core/engine/rule/IModifierExecutor.h
    src/core/engine/rule/ModifierRule.h
    src/core/engine/rule/ModifierRule.cpp
    src/core/engine/rule/IModifierSubExecutor.h
    src/core/engine/rule/ModifierProposal.h
    src/core/engine/rule/AdjacentCircumflexProposal.h
    src/core/engine/rule/AdjacentCircumflexProposal.cpp
    src/core/engine/rule/HornModifierProposal.h
    src/core/engine/rule/HornModifierProposal.cpp
    src/core/engine/rule/StrokeDProposal.h
    src/core/engine/rule/StrokeDProposal.cpp
    src/core/engine/rule/BracketProposal.h
    src/core/engine/rule/BracketProposal.cpp
    src/core/engine/rule/VniCircumflexProposal.h
    src/core/engine/rule/VniCircumflexProposal.cpp
    src/core/engine/rule/VniHornProposal.h
    src/core/engine/rule/VniHornProposal.cpp
    src/core/engine/rule/VniBreveProposal.h
    src/core/engine/rule/VniBreveProposal.cpp
    src/core/engine/rule/IQuickConsonantExecutor.h
    src/core/engine/rule/QuickStartConsonantRule.h
    src/core/engine/rule/QuickStartConsonantRule.cpp
    src/core/engine/rule/QuickEndConsonantRule.h
    src/core/engine/rule/QuickEndConsonantRule.cpp
)

add_library(VKeyEngine STATIC ${NEXTKEY_ENGINE_SOURCES})
target_include_directories(VKeyEngine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Route EngineFactory through the prebuilt, closed-source Rust engine (consumed
# via its C ABI, loaded at runtime). The in-tree C++ TypingEngine stays compiled
# as a fallback (used when the library is missing, and by the parity tests).
# Defaults OFF on purpose. The prebuilt engine in extern/vkey_engine/ is NOT
# covered by this project's GPL-3.0 license and embeds CC BY-NC data, so a
# plain `cmake -B build` must not require it — see extern/vkey_engine/LICENSE.
# Official Sciter builds opt in explicitly with -DVKEY_USE_RUST_ENGINE=ON in CI.
# Classic declares its product boundary here too, before any engine artifact is
# inspected, so an accidental Classic + Rust release configuration fails early.
option(VKEY_LITE_MODE "Build Classic (Win32 native) UI instead of Sciter" OFF)
option(VKEY_USE_RUST_ENGINE "Use the prebuilt closed-source Rust engine (C ABI)" OFF)
option(VKEY_ALLOW_UNSIGNED_LOCAL_ENGINE
    "Allow a Release development build to trust the exact engine.lock hash" OFF)
option(VKEY_CLASSIC_ALLOW_RUST_ENGINE_FOR_DEV
    "Allow a manual, non-release Classic development build to use the Rust engine" OFF)
option(VKEY_BUILD_BROWSER_HOST "Build VKeyBrowserHost companion executable" ON)
option(VKEY_BUILD_WATCHDOG "Build VKeyWatchdog process supervisor" ON)
option(VKEY_ALLOW_UNSIGNED_EXTENSIONS
    "Allow running unsigned extensions in development builds" OFF)
option(VKEY_FOUNDATION_BUILD
    "Build only the GPL-3.0 VKeyClassic targets approved for Foundation signing" OFF)
if(VKEY_FOUNDATION_BUILD)
    if(NOT VKEY_LITE_MODE)
        message(FATAL_ERROR
            "VKEY_FOUNDATION_BUILD=ON requires VKEY_LITE_MODE=ON")
    endif()
    if(VKEY_USE_RUST_ENGINE OR VKEY_CLASSIC_ALLOW_RUST_ENGINE_FOR_DEV OR
       VKEY_ALLOW_UNSIGNED_LOCAL_ENGINE)
        message(FATAL_ERROR
            "VKEY_FOUNDATION_BUILD=ON forbids every Rust engine option. Use "
            "-DVKEY_USE_RUST_ENGINE=OFF "
            "-DVKEY_CLASSIC_ALLOW_RUST_ENGINE_FOR_DEV=OFF "
            "-DVKEY_ALLOW_UNSIGNED_LOCAL_ENGINE=OFF.")
    endif()
    message(STATUS
        "Foundation build: Rust engine, Sciter app, and browser host are excluded")
endif()
if(VKEY_CLASSIC_ALLOW_RUST_ENGINE_FOR_DEV AND
   (NOT VKEY_LITE_MODE OR NOT VKEY_USE_RUST_ENGINE))
    message(FATAL_ERROR
        "VKEY_CLASSIC_ALLOW_RUST_ENGINE_FOR_DEV=ON requires both "
        "VKEY_LITE_MODE=ON and VKEY_USE_RUST_ENGINE=ON")
endif()
if(VKEY_LITE_MODE AND VKEY_USE_RUST_ENGINE AND
   NOT VKEY_CLASSIC_ALLOW_RUST_ENGINE_FOR_DEV)
    message(FATAL_ERROR
        "Official VKey Classic is C++-only. Use -DVKEY_USE_RUST_ENGINE=OFF. "
        "Maintainers testing the non-release variant must also opt in with "
        "-DVKEY_CLASSIC_ALLOW_RUST_ENGINE_FOR_DEV=ON.")
endif()
if(VKEY_ALLOW_UNSIGNED_LOCAL_ENGINE AND NOT VKEY_USE_RUST_ENGINE)
    message(FATAL_ERROR
        "VKEY_ALLOW_UNSIGNED_LOCAL_ENGINE=ON requires VKEY_USE_RUST_ENGINE=ON")
endif()
if(VKEY_ALLOW_UNSIGNED_EXTENSIONS)
    add_compile_definitions(VKEY_ALLOW_UNSIGNED_EXTENSIONS)
endif()
if(VKEY_USE_RUST_ENGINE)
    # Overridable so the libraries need not be committed: tools/fetch_engine.py
    # populates a directory from a published engine release, and the build is
    # pointed at it with -DVKEY_ENGINE_ROOT=<dir>. engine.lock and the header stay
    # in the tree — they are text and carry no corpus data — while the libraries,
    # which embed CC BY-NC data, do not have to.
    set(VKEY_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/extern/vkey_engine" CACHE PATH
        "Directory holding the prebuilt engine: engine.lock, include/, lib/")
    set(VKEY_ENGINE_LOCK_PATH "${VKEY_ENGINE_ROOT}/engine.lock")
    set(VKEY_ENGINE_DLL "${VKEY_ENGINE_ROOT}/lib/win-x64/vkey_engine.dll")
    # The public key is committed here, never fetched: fetching the thing that
    # decides what to trust alongside the thing being trusted proves nothing.
    set(VKEY_ENGINE_PUBKEY_PATH "${CMAKE_CURRENT_SOURCE_DIR}/extern/vkey_engine/engine.pub")
    if(NOT EXISTS "${VKEY_ENGINE_LOCK_PATH}" OR NOT EXISTS "${VKEY_ENGINE_DLL}")
        message(FATAL_ERROR
            "No prebuilt engine at ${VKEY_ENGINE_ROOT}. Either run the VKey-rs "
            "engine sync, or fetch a published one:\n"
            "  python3 tools/fetch_engine.py --lock extern/vkey_engine/engine.lock "
            "--dest build/vkey_engine --base-url <engine release URL>\n"
            "then configure with -DVKEY_ENGINE_ROOT=build/vkey_engine.\n"
            "To build without it: -DVKEY_USE_RUST_ENGINE=OFF.")
    endif()

    # The lock is a configure-time input baked into VKeyEngineLock.h. Without
    # this, a plain `cmake --build` after an engine sync keeps the stale hash and
    # the freshly vendored DLL fails the trust check → silent C++ fallback.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${VKEY_ENGINE_LOCK_PATH}" "${VKEY_ENGINE_DLL}" "${VKEY_ENGINE_PUBKEY_PATH}")

    file(READ "${VKEY_ENGINE_LOCK_PATH}" _vkey_engine_lock LIMIT 1025)
    string(LENGTH "${_vkey_engine_lock}" _vkey_engine_lock_length)
    set(_vkey_engine_lock_pattern
        "^VKEY-ENGINE-LOCK-V2\nabi_version=([0-9]+)\ntarget=windows-x86_64\nasset_name=vkey_engine[.]dll\ninstalled_name=vkey_engine[.]dll\nbyte_len=([0-9]+)\nsha256=([0-9a-f]+)\ncounter=([0-9]+)\n$")
    string(REGEX MATCH "${_vkey_engine_lock_pattern}" _vkey_engine_lock_match
        "${_vkey_engine_lock}")
    if(_vkey_engine_lock_length GREATER 1024 OR
       NOT "${_vkey_engine_lock_match}" STREQUAL "${_vkey_engine_lock}")
        message(FATAL_ERROR "extern/vkey_engine/engine.lock is not canonical V2")
    endif()
    set(VKEY_ENGINE_LOCK_ABI "${CMAKE_MATCH_1}")
    set(VKEY_ENGINE_LOCK_BYTES "${CMAKE_MATCH_2}")
    set(VKEY_ENGINE_LOCK_SHA256 "${CMAKE_MATCH_3}")
    set(VKEY_ENGINE_LOCK_COUNTER "${CMAKE_MATCH_4}")
    string(LENGTH "${VKEY_ENGINE_LOCK_SHA256}" _vkey_engine_sha256_length)
    if(NOT _vkey_engine_sha256_length EQUAL 64)
        message(FATAL_ERROR "extern/vkey_engine/engine.lock has an invalid SHA-256")
    endif()

    # The counter in the lock becomes the downgrade floor baked into this build:
    # a signed but older engine is refused even though its signature is valid.
    if(NOT EXISTS "${VKEY_ENGINE_PUBKEY_PATH}")
        message(FATAL_ERROR
            "extern/vkey_engine/engine.pub is missing — it is the key Release builds "
            "verify the engine signature against.")
    endif()
    file(READ "${VKEY_ENGINE_PUBKEY_PATH}" _vkey_engine_pubkey)
    string(STRIP "${_vkey_engine_pubkey}" VKEY_ENGINE_PUBKEY)
    # CMake's regex flavour has no {n} repetition, so length is checked separately.
    string(LENGTH "${VKEY_ENGINE_PUBKEY}" _vkey_engine_pubkey_length)
    if(NOT _vkey_engine_pubkey_length EQUAL 128 OR
       NOT VKEY_ENGINE_PUBKEY MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR
            "extern/vkey_engine/engine.pub must be 128 hex characters (P-256 X||Y)")
    endif()

    file(SIZE "${VKEY_ENGINE_DLL}" _vkey_engine_dll_bytes)
    file(SHA256 "${VKEY_ENGINE_DLL}" _vkey_engine_dll_sha256)
    if(NOT "${_vkey_engine_dll_bytes}" STREQUAL "${VKEY_ENGINE_LOCK_BYTES}" OR
       NOT "${_vkey_engine_dll_sha256}" STREQUAL "${VKEY_ENGINE_LOCK_SHA256}")
        message(FATAL_ERROR "vendored vkey_engine.dll does not match engine.lock")
    endif()

    set(VKEY_ENGINE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/vkey_engine")
    file(MAKE_DIRECTORY "${VKEY_ENGINE_GENERATED_DIR}")
    configure_file(
        src/core/engine/VKeyEngineLock.h.in
        "${VKEY_ENGINE_GENERATED_DIR}/VKeyEngineLock.h"
        @ONLY
    )

    target_sources(VKeyEngine PRIVATE
        src/core/engine/EngineSignature.h
        src/core/engine/RustInputEngine.h
        src/core/engine/RustInputEngine.cpp
        src/core/engine/RustEngineLoader.h
        src/core/engine/RustEngineLoader.cpp
        src/core/engine/RustEngineTrust.h
        src/core/engine/RustEngineTrust.cpp
    )
    target_include_directories(VKeyEngine PRIVATE
        ${VKEY_ENGINE_ROOT}/include
        ${VKEY_ENGINE_GENERATED_DIR}
    )
    target_compile_definitions(VKeyEngine PRIVATE VKEY_USE_RUST_ENGINE)
    # Debug accepts an engine whose hash matches engine.lock, so a locally synced
    # build (which has no signature — the release workflow holds the key) stays
    # usable. A maintainer may explicitly enable the same exact-hash path in a
    # local Release build to test optimized engine code. The option defaults OFF
    # and official builds never set it, so shipped Release builds still accept
    # only signed engines.
    if(VKEY_ALLOW_UNSIGNED_LOCAL_ENGINE)
        message(WARNING
            "VKEY_ALLOW_UNSIGNED_LOCAL_ENGINE=ON: this local Release build accepts "
            "only the exact DLL hash in VKEY_ENGINE_ROOT/engine.lock. Do not ship it.")
        target_compile_definitions(VKeyEngine PRIVATE VKEY_ENGINE_TRUST_DEV)
    else()
        target_compile_definitions(VKeyEngine PRIVATE $<$<CONFIG:Debug>:VKEY_ENGINE_TRUST_DEV>)
    endif()
    target_link_libraries(VKeyEngine PRIVATE ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(VKeyEngine PRIVATE bcrypt)
    endif()
else()
    # Warn, do not stay silent. RustInputEngineTest.cpp and part of
    # EngineFactoryTest.cpp are inside `#ifdef VKEY_USE_RUST_ENGINE`, so with the
    # option off they compile away to nothing and ctest still reports success.
    # Anyone running the test suite as a gate needs to know coverage shrank.
    if(VKEY_LITE_MODE)
        message(STATUS
            "VKey Classic: enforcing the built-in C++ engine only; the Rust "
            "loader, installer and artifact are excluded from the official build.")
    else()
        message(WARNING
            "VKEY_USE_RUST_ENGINE=OFF: building against the in-tree C++ engine only. "
            "RustInputEngineTest and the Rust half of EngineFactoryTest are NOT "
            "compiled, so a green ctest does not cover the Rust engine. "
            "Maintainers: configure Sciter with -DVKEY_USE_RUST_ENGINE=ON. "
            "Forks: this is the correct default — see extern/vkey_engine/LICENSE, "
            "those artifacts are not GPL and are noncommercial-only.")
    endif()
endif()

# Core Library (platform layer: SharedState, Config, Debug)
set(NEXTKEY_CORE_SOURCES
    src/core/Debug.h
    src/core/Logger.h
    src/core/Logger.cpp
    src/core/CrashLog.h
    src/core/CrashLog.cpp
    src/core/FocusApplyDecision.h
    src/core/Strings.h
    src/core/Strings.cpp
    src/core/MacroCase.h
    src/core/MacroCase.cpp
    src/core/hotkey/HotkeyRegistry.h
    src/core/hotkey/HotkeyRegistry.cpp
    src/core/hotkey/HotkeyLabel.h
    src/core/hotkey/HotkeyLabel.cpp
    src/core/ipc/SharedState.h
    src/core/ipc/SharedConstants.h
    src/core/ipc/BrowserContextState.h
    src/core/pipeline/Stage.h
    src/core/pipeline/Result.h
    src/core/pipeline/GateMask.h
    src/core/pipeline/Intent.h
    src/core/pipeline/IntentSink.h
    src/core/pipeline/ICompositionSession.h
    src/core/pipeline/HookCompositionSession.h
    src/core/pipeline/KeyContext.h
    src/core/pipeline/IFeature.h
    src/core/pipeline/IGate.h
    src/core/pipeline/IBackwardEditExecutor.h
    src/core/pipeline/BackwardEditFeature.h
    src/core/pipeline/BackwardEditFeature.cpp
    src/core/pipeline/ICommitUndoExecutor.h
    src/core/pipeline/CommitUndoFeature.h
    src/core/pipeline/CommitUndoFeature.cpp
    src/core/pipeline/IEscRestoreRawExecutor.h
    src/core/pipeline/EscRestoreRawFeature.h
    src/core/pipeline/EscRestoreRawFeature.cpp
    src/core/pipeline/IMacroExecutor.h
    src/core/pipeline/MacroFeature.h
    src/core/pipeline/MacroFeature.cpp
    src/core/pipeline/gates/EnglishBiasGate.h
    src/core/pipeline/gates/SpellCheckGate.h
    src/core/pipeline/gates/ToneEscapeGate.h
    src/core/pipeline/Coordinator.h
    src/core/pipeline/Coordinator.cpp
    src/core/pipeline/OutputChannel.h
    src/core/pipeline/OutputChannel.cpp
    src/core/config/SpellExclusionCanonicalizer.h
    src/core/config/SpellExclusionCanonicalizer.cpp
    src/core/config/LexiconTransaction.h
    src/core/config/LexiconTransaction.cpp
    src/core/config/LexiconValidation.h
    src/core/config/LexiconValidation.cpp
    src/core/ipc/LexiconWireFormat.h
    src/core/ipc/LexiconWireFormat.cpp
    src/core/ipc/LexiconWireManager.h
    src/core/ipc/LexiconWireManager.cpp
    src/core/security/ExtensionTrust.h
    src/core/security/ExtensionTrust.cpp
)

# Windows-only sources (require Windows.h)
if(WIN32)
    list(APPEND NEXTKEY_CORE_SOURCES
        src/core/ipc/SharedStateManager.cpp
        src/core/ipc/SharedStateManager.h
        src/core/ipc/BrowserContextManager.cpp
        src/core/ipc/BrowserContextManager.h
        src/browser_host/Registration.cpp
        src/browser_host/Registration.h
        src/core/SmartSwitchManager.cpp
        src/core/SmartSwitchManager.h
        src/core/SmartSwitchState.h
        src/core/config/ConfigEvent.cpp
        src/core/config/ConfigEvent.h
        src/core/config/ConfigManager.cpp
        src/core/config/ConfigManager.h
        src/core/config/ConfigSnapshotBuilder.cpp
        src/core/config/ConfigSnapshotBuilder.h
    )
endif()

add_library(VKeyCore STATIC ${NEXTKEY_CORE_SOURCES})
target_include_directories(VKeyCore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/tomlplusplus
)
target_link_libraries(VKeyCore PUBLIC VKeyEngine)
if(VKEY_USE_RUST_ENGINE)
    # LexiconReader's compiled Rust dictionary snapshot is available only when
    # VKeyEngine includes RustInputEngine.cpp. Propagate this capability to
    # VKeyCore and its consumers; Classic deliberately builds without it.
    target_compile_definitions(VKeyCore PUBLIC VKEY_USE_RUST_ENGINE)
endif()
if(NOT WIN32)
    find_package(ICU COMPONENTS uc)
    if(ICU_FOUND)
        target_link_libraries(VKeyCore PUBLIC ICU::uc)
        target_compile_definitions(VKeyCore PUBLIC VKEY_HAS_ICU=1)
    endif()
else()
    target_link_libraries(VKeyCore PUBLIC normaliz)
endif()

# TSF DLL (TIP)
add_library(VKeyTSF SHARED
    src/tsf/stdafx.h
    src/tsf/Globals.h
    src/tsf/Globals.cpp
    src/tsf/TextService.h
    src/tsf/TextService.cpp
    src/tsf/KeyEventSink.h
    src/tsf/KeyEventSink.cpp
    src/tsf/CompositionManager.h
    src/tsf/CompositionManager.cpp
    src/tsf/EngineController.h
    src/tsf/EngineController.cpp
    src/tsf/ReadonlyContextProvider.h
    src/tsf/ReadonlyContextProvider.cpp
    src/tsf/Register.cpp
    src/tsf/dllmain.cpp
    src/tsf/EditSession.h
    src/tsf/CompositionEditSession.h
    src/tsf/EscRestoreLastCommitSession.h
    src/tsf/QuickConvertEditSession.h
    src/tsf/InputScopeChecker.h
    src/tsf/LanguageBarButton.h
    src/tsf/LanguageBarButton.cpp
    src/tsf/DisplayAttribute.h
    src/tsf/ComUtils.h
    src/tsf/Define.h
    src/tsf/resource.h
    src/tsf/VKeyTSF.rc
    src/tsf/VKeyTSF.def
)
target_link_libraries(VKeyTSF PRIVATE VKeyCore)
set_target_properties(VKeyTSF PROPERTIES 
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/src/tsf/VKeyTSF.def\""
)
if(VKEY_USE_RUST_ENGINE)
    target_compile_definitions(VKeyTSF PRIVATE VKEY_USE_RUST_ENGINE)
endif()

if(WIN32 AND NOT VKEY_FOUNDATION_BUILD AND VKEY_BUILD_BROWSER_HOST)
    add_executable(VKeyBrowserHost
        src/browser_host/main.cpp
        src/browser_host/NativeMessaging.cpp
        src/browser_host/NativeMessaging.h
        src/browser_host/VKeyBrowserHost.rc
    )
    target_link_libraries(VKeyBrowserHost PRIVATE VKeyCore advapi32)
endif()

# Core App (EXE) - Win32 GUI application with Sciter UI
if(NOT VKEY_FOUNDATION_BUILD)
add_executable(VKeyApp WIN32
    src/app/main.cpp
    src/app/resource.h
    src/app/VKey.rc
    # dialogs/
    src/app/dialogs/SciterSubDialog.h
    src/app/dialogs/SciterSubDialog.cpp
    src/app/dialogs/SubDialogConfig.h
    src/app/dialogs/SettingsDialog.h
    src/app/dialogs/SettingsDialog.cpp
    src/app/dialogs/ExcludedAppsDialog.h
    src/app/dialogs/ExcludedAppsDialog.cpp
    src/app/dialogs/TsfAppsDialog.h
    src/app/dialogs/TsfAppsDialog.cpp
    src/app/dialogs/MacroTableDialog.h
    src/app/dialogs/MacroTableDialog.cpp
    src/app/dialogs/ConvertToolDialog.h
    src/app/dialogs/ConvertToolDialog.cpp
    src/app/dialogs/AboutDialog.h
    src/app/dialogs/AboutDialog.cpp
    src/app/dialogs/WindowPickerDialog.h
    src/app/dialogs/WindowPickerDialog.cpp
    src/app/dialogs/AppOverridesDialog.h
    src/app/dialogs/AppOverridesDialog.cpp
    src/app/dialogs/SpellExclusionsDialog.h
    src/app/dialogs/SpellExclusionsDialog.cpp
    src/app/dialogs/UserDefinedDialog.h
    src/app/dialogs/UserDefinedDialog.cpp
    src/app/dialogs/HotkeysDialog.h
    src/app/dialogs/HotkeysDialog.cpp
    src/app/dialogs/IconSettingsDialog.h
    src/app/dialogs/IconSettingsDialog.cpp
    # system/
    src/app/system/TrayIcon.h
    src/app/system/TrayIcon.cpp
    src/app/system/TrayStatusText.h
    src/app/system/FloatingIcon.h
    src/app/system/FloatingIcon.cpp
    src/app/system/HookEngine.h
    src/app/system/HookEngine.cpp
    src/app/system/HookLifecycle.h
    src/app/system/HookLifecycle.cpp
    src/app/system/FocusOwner.h
    src/app/system/FocusOwner.cpp
    src/app/system/OutputDispatcher.h
    src/app/system/OutputDispatcher.cpp
    src/app/system/CommitState.h
    src/app/system/HookCommandMailbox.h
    src/app/system/HookCommandMailbox.cpp
    src/app/system/HeartbeatPublisher.h
    src/app/system/HeartbeatPublisher.cpp
    src/app/system/WatchdogController.h
    src/app/system/WatchdogController.cpp
    src/app/system/BrowserExtensionController.h
    src/app/system/BrowserExtensionController.cpp
    src/app/system/MainThreadWorker.h
    src/app/system/MainThreadWorker.cpp
    src/app/system/HotkeyManager.h
    src/app/system/HotkeyManager.cpp
    src/app/system/HotkeyWiring.h
    src/app/system/HotkeyWiring.cpp
    src/app/system/QuickConvert.h
    src/app/system/QuickConvert.cpp
    src/app/system/ToastPopup.h
    src/app/system/ToastPopup.cpp
    src/app/system/UpdateChecker.h
    src/app/system/UpdateChecker.cpp
    src/app/system/UpdateInstaller.h
    src/app/system/UpdateInstaller.cpp
    src/app/system/ExtensionInstaller.h
    src/app/system/ExtensionInstaller.cpp
    src/app/system/UpdateSecurity.h
    src/app/system/UpdateSecurity.cpp
    src/app/system/PendingDllApply.h
    src/app/system/PendingDllApply.cpp
    src/app/system/PerfHistogram.h
    src/app/system/PerfHistogram.cpp
    src/app/system/StartupHelper.h
    src/app/system/SubprocessHelper.h
    src/app/system/SubprocessRunners.h
    src/app/system/SubprocessRunners.cpp
    src/app/system/TsfRegistration.h
    src/app/system/TsfRegistration.cpp
    # output/ — IOutputInjector strategy (Sprint 2 T3)
    src/app/output/IOutputInjector.h
    src/app/output/Internal.h
    src/app/output/Internal.cpp
    src/app/output/OutputInjectorFactory.h
    src/app/output/OutputInjectorFactory.cpp
    src/app/output/Win32SendInputInjector.h
    src/app/output/Win32SendInputInjector.cpp
    src/app/output/ClipboardInjector.cpp
    src/app/output/RichEditEmReplaceSelInjector.h
    src/app/output/RichEditEmReplaceSelInjector.cpp
    src/app/output/SplitDispatchInjector.h
    src/app/output/SplitDispatchInjector.cpp
    # sciter/
    src/app/sciter/SciterArchive.h
    src/app/sciter/SciterArchive.cpp
    src/app/sciter/SciterHelper.h
    src/app/sciter/SciterHelper.cpp
    src/app/sciter/ScaleHelper.h
    # system/ (dark mode utils, shared with LITE_MODE)
    src/app/system/DarkModeHelper.h
    src/app/system/DarkModeHelper.cpp
    # other
    src/core/Version.h
    extern/sciter/include/sciter-main.cpp
    src/app/VKey.exe.manifest
)
target_include_directories(VKeyApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/sciter/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/tomlplusplus
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/dialogs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/system
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/sciter
)
target_compile_definitions(VKeyApp PRIVATE SKIP_MAIN VKEY_HOOK_ENGINE VKEY_PERF_HIST $<$<CONFIG:Debug>:NEXTKEY_DEBUG>)
if(VKEY_USE_RUST_ENGINE)
    target_sources(VKeyApp PRIVATE
        src/app/system/AdvancedEngineDownloadPolicy.h
        src/app/system/AdvancedEngineDownloadPolicy.cpp
        src/app/system/AdvancedEngineStorage.h
        src/app/system/AdvancedEngineStorage.cpp
        src/app/system/AdvancedEngineInstaller.h
        src/app/system/AdvancedEngineInstaller.cpp
    )
    target_compile_definitions(VKeyApp PRIVATE VKEY_USE_RUST_ENGINE)
endif()
target_link_libraries(VKeyApp PRIVATE VKeyCore)
set_target_properties(VKeyApp PROPERTIES OUTPUT_NAME "VKey")
if(MSVC)
    target_link_options(VKeyApp PRIVATE /SUBSYSTEM:WINDOWS,6.01)
endif()
endif()

# ============================================================================
# Classic UI (Lite build) — Win32 native, no Sciter dependency
# Official build: cmake -B build -DVKEY_LITE_MODE=ON -DVKEY_USE_RUST_ENGINE=OFF
# A Classic + Rust build requires VKEY_CLASSIC_ALLOW_RUST_ENGINE_FOR_DEV=ON and
# is explicitly a local development variant, never a release artifact.
# ============================================================================
if(VKEY_LITE_MODE AND WIN32)
    add_executable(VKeyLite WIN32
        src/app/main_lite.cpp
        src/app/classic/resource.h
        src/app/classic/VKeyLite.rc
        src/app/classic/VKeyLite.exe.manifest
        # system/ (shared with main app)
        src/app/system/TrayIcon.h
        src/app/system/TrayIcon.cpp
        src/app/system/TrayStatusText.h
        src/app/system/FloatingIcon.h
        src/app/system/FloatingIcon.cpp
        src/app/system/HookEngine.h
        src/app/system/HookEngine.cpp
        src/app/system/HookLifecycle.h
        src/app/system/HookLifecycle.cpp
        src/app/system/FocusOwner.h
        src/app/system/FocusOwner.cpp
        src/app/system/OutputDispatcher.h
        src/app/system/OutputDispatcher.cpp
        src/app/system/CommitState.h
        src/app/system/HeartbeatPublisher.h
        src/app/system/HeartbeatPublisher.cpp
        src/app/system/WatchdogController.h
        src/app/system/WatchdogController.cpp
        src/app/system/BrowserExtensionController.h
        src/app/system/BrowserExtensionController.cpp
        src/app/system/MainThreadWorker.h
        src/app/system/MainThreadWorker.cpp
        src/app/system/HotkeyManager.h
        src/app/system/HotkeyManager.cpp
        src/app/system/HotkeyWiring.h
        src/app/system/HotkeyWiring.cpp
        src/app/system/QuickConvert.h
        src/app/system/QuickConvert.cpp
        src/app/system/ToastPopup.h
        src/app/system/ToastPopup.cpp
        src/app/system/UpdateChecker.h
        src/app/system/UpdateChecker.cpp
        src/app/system/UpdateInstaller.h
        src/app/system/UpdateInstaller.cpp
        src/app/system/ExtensionInstaller.h
        src/app/system/ExtensionInstaller.cpp
        src/app/system/UpdateSecurity.h
        src/app/system/UpdateSecurity.cpp
        src/app/system/PendingDllApply.h
        src/app/system/PendingDllApply.cpp
        src/app/system/PerfHistogram.h
        src/app/system/PerfHistogram.cpp
        src/app/system/HookCommandMailbox.h
        src/app/system/HookCommandMailbox.cpp
        src/app/system/StartupHelper.h
        src/app/system/SubprocessHelper.h
        src/app/system/TsfRegistration.h
        src/app/system/TsfRegistration.cpp
        # output/ — IOutputInjector strategy (Sprint 2 T3)
        src/app/output/IOutputInjector.h
        src/app/output/Internal.h
        src/app/output/Internal.cpp
        src/app/output/OutputInjectorFactory.h
        src/app/output/OutputInjectorFactory.cpp
        src/app/output/Win32SendInputInjector.h
        src/app/output/Win32SendInputInjector.cpp
        src/app/output/ClipboardInjector.cpp
        src/app/output/RichEditEmReplaceSelInjector.h
        src/app/output/RichEditEmReplaceSelInjector.cpp
        src/app/output/SplitDispatchInjector.h
        src/app/output/SplitDispatchInjector.cpp
        # DarkModeHelper (pure Win32 dark mode utils, needed by TrayIcon/ToastPopup)
        src/app/system/DarkModeHelper.h
        src/app/system/DarkModeHelper.cpp
        # classic/ (new)
        src/app/classic/ClassicDialogUtils.h
        src/app/classic/ClassicTheme.h
        src/app/classic/ClassicTheme.cpp
        src/app/classic/ClassicSettingsDialog.h
        src/app/classic/ClassicSettingsDialog.cpp
        src/app/classic/ClassicExcludedAppsDialog.h
        src/app/classic/ClassicExcludedAppsDialog.cpp
        src/app/classic/ClassicTsfAppsDialog.h
        src/app/classic/ClassicTsfAppsDialog.cpp
        src/app/classic/ClassicMacroTableDialog.h
        src/app/classic/ClassicMacroTableDialog.cpp
        src/app/classic/ClassicHotkeysDialog.h
        src/app/classic/ClassicHotkeysDialog.cpp
        src/app/classic/ClassicHotkeyCapture.h
        src/app/classic/ClassicHotkeyCapture.cpp
        src/app/classic/ClassicAppOverridesDialog.h
        src/app/classic/ClassicAppOverridesDialog.cpp
        src/app/classic/ClassicConvertToolDialog.h
        src/app/classic/ClassicConvertToolDialog.cpp
        src/app/classic/ClassicIconColorDialog.h
        src/app/classic/ClassicIconColorDialog.cpp
        src/app/classic/ClassicSpellExclusionsDialog.h
        src/app/classic/ClassicSpellExclusionsDialog.cpp
        src/app/classic/ClassicUserDefinedDialog.h
        src/app/classic/ClassicUserDefinedDialog.cpp
        # version
        src/core/Version.h
    )
    target_include_directories(VKeyLite PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/tomlplusplus
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/classic
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/system
    )
    target_compile_definitions(VKeyLite PRIVATE
        VKEY_HOOK_ENGINE
        VKEY_LITE_MODE
        VKEY_PERF_HIST
        $<$<CONFIG:Debug>:NEXTKEY_DEBUG>
    )
    if(VKEY_USE_RUST_ENGINE)
        # Reachable only through VKEY_CLASSIC_ALLOW_RUST_ENGINE_FOR_DEV. Official
        # Classic CI configures Rust OFF and therefore omits these sources.
        target_sources(VKeyLite PRIVATE
            src/app/system/AdvancedEngineDownloadPolicy.h
            src/app/system/AdvancedEngineDownloadPolicy.cpp
            src/app/system/AdvancedEngineStorage.h
            src/app/system/AdvancedEngineStorage.cpp
            src/app/system/AdvancedEngineInstaller.h
            src/app/system/AdvancedEngineInstaller.cpp
        )
        target_compile_definitions(VKeyLite PRIVATE VKEY_USE_RUST_ENGINE)
    endif()
    target_link_libraries(VKeyLite PRIVATE
        VKeyCore
        advapi32 user32 ole32 shell32 dwmapi uuid urlmon version comctl32 winmm bcrypt gdiplus winhttp
    )
    set_target_properties(VKeyLite PROPERTIES OUTPUT_NAME "VKeyClassic")
    if(MSVC)
        target_link_options(VKeyLite PRIVATE /SUBSYSTEM:WINDOWS,6.01)
    endif()
endif()

# Link Windows libraries
if(WIN32)
    target_link_libraries(VKeyTSF PRIVATE advapi32 user32 ole32 oleaut32 uuid shell32)
    if(TARGET VKeyApp)
        target_link_libraries(VKeyApp PRIVATE advapi32 user32 ole32 shell32 dwmapi uuid urlmon version comctl32 winmm bcrypt winhttp)

        # Copy sciter.dll to output directory
        add_custom_command(TARGET VKeyApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/extern/sciter/bin/sciter.dll"
                "$<TARGET_FILE_DIR:VKeyApp>/sciter.dll"
            COMMENT "Copying sciter.dll to output directory"
        )
    endif()

    # Deploy the prebuilt Rust engine next to every binary that creates engines
    # (VKeyApp.exe and the in-process VKeyTSF.dll). RustInputEngine loads it
    # module-relative, so co-locating it here makes it resolvable in TSF hosts.
    # Keep this as an explicit dependency target: a TARGET POST_BUILD command is
    # skipped when only the vendored DLL changes and the executable is up to date.
    if(VKEY_USE_RUST_ENGINE)
        get_property(_vkey_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(_vkey_multi_config)
            set(_vkey_engine_output_dir "${CMAKE_BINARY_DIR}/$<CONFIG>")
        else()
            set(_vkey_engine_output_dir "${CMAKE_BINARY_DIR}")
        endif()
        # The .sig is not optional baggage: Release verifies it instead of the
        # lock hash, so an output directory with the DLL alone silently falls back
        # to the C++ engine. Copied through a conditional because a locally synced
        # engine has none — that case is exactly what the Debug hash path covers.
        add_custom_target(VKeyEngineRuntimeDeploy
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VKEY_ENGINE_DLL}"
                "${_vkey_engine_output_dir}/vkey_engine.dll"
            COMMAND ${CMAKE_COMMAND}
                -DSOURCE=${VKEY_ENGINE_DLL}.sig
                -DDESTINATION=${_vkey_engine_output_dir}/vkey_engine.dll.sig
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyIfPresent.cmake"
            DEPENDS "${VKEY_ENGINE_DLL}"
            COMMENT "Deploying vkey_engine.dll to the runtime output directory"
            VERBATIM
        )
        foreach(_tgt VKeyApp VKeyTSF VKeyLite)
            if(TARGET ${_tgt})
                add_dependencies(${_tgt} VKeyEngineRuntimeDeploy)
            endif()
        endforeach()
        unset(_vkey_engine_output_dir)
        unset(_vkey_multi_config)
    endif()
endif()

# Sciter UI resources do not exist in the Foundation build graph.
if(TARGET VKeyApp)
# ============================================================================
# Sciter UI Resources
# Release: Embed UI files into exe using packfolder
# Debug: Copy UI files to output directory for easy editing
# ============================================================================

# Packfolder is included in extern/sciter/bin/
set(PACKFOLDER_EXE "${CMAKE_CURRENT_SOURCE_DIR}/extern/sciter/bin/packfolder.exe")

# Check if packfolder exists
if(EXISTS "${PACKFOLDER_EXE}")
    set(PACKFOLDER_AVAILABLE TRUE)
    message(STATUS "Sciter: packfolder.exe found at ${PACKFOLDER_EXE}")
else()
    set(PACKFOLDER_AVAILABLE FALSE)
    message(STATUS "Sciter: packfolder.exe not found, using file copy mode")
endif()

# Generate resources.cpp from UI files (run before build if UI changed)
set(RESOURCES_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/app/resources.cpp")
set(UI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/app/ui")

if(PACKFOLDER_AVAILABLE)
    # Custom command to generate resources.cpp.
    # GLOB keeps the dependency list complete — the old hand-maintained list
    # had drifted (subdialog.css/toggle.css/base.css/hotkey-capture.js/logo.png
    # missing → edits didn't trigger a repack).
    file(GLOB_RECURSE UI_RESOURCE_FILES CONFIGURE_DEPENDS "${UI_SOURCE_DIR}/*")
    add_custom_command(
        OUTPUT "${RESOURCES_CPP}"
        COMMAND "${PACKFOLDER_EXE}" "${UI_SOURCE_DIR}" "${RESOURCES_CPP}" -v resources
        DEPENDS ${UI_RESOURCE_FILES}
        COMMENT "Packing UI resources into resources.cpp"
        VERBATIM
    )

    # Custom target to ensure resources are generated
    add_custom_target(PackUIResources DEPENDS "${RESOURCES_CPP}")
    add_dependencies(VKeyApp PackUIResources)

    # Enable packfolder for Release, file copy for Debug
    target_compile_definitions(VKeyApp PRIVATE
        $<$<CONFIG:Release>:SCITER_USE_PACKFOLDER>
        $<$<CONFIG:RelWithDebInfo>:SCITER_USE_PACKFOLDER>
    )
    message(STATUS "Sciter: Release=embedded resources, Debug=file copy")
endif()

# Always copy UI files for Debug builds (allows live editing)
add_custom_command(TARGET VKeyApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<CONFIG:Debug>,copy_directory,true>
        "${UI_SOURCE_DIR}"
        "$<TARGET_FILE_DIR:VKeyApp>/ui"
    COMMENT "Copying UI files (Debug only)"
)
endif()

# ============================================================================
# Testing with Google Test (vendored in extern/)
# ============================================================================
# Google Test must be manually placed in extern/googletest/
# Download from: https://github.com/google/googletest/releases/tag/v1.14.0
# Extract googletest-1.14.0 folder contents to extern/googletest/

set(GOOGLETEST_VERSION "1.14.0" CACHE STRING "Google Test version")
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(extern/googletest EXCLUDE_FROM_ALL)

enable_testing()

# Sciter's macro selection state is plain JavaScript. Register its regression
# suite with CTest when Node is available (including GitHub's Windows runners).
find_program(VKEY_NODE_EXECUTABLE NAMES node nodejs)
if(VKEY_NODE_EXECUTABLE)
    add_test(
        NAME MacroSelectionJsTest
        COMMAND "${VKEY_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/MacroSelectionTest.js"
    )
endif()

# Tests are excluded from ALL - build explicitly with --target VKeyTests
set(NEXTKEY_TEST_SOURCES
    tests/HotkeyManagerTest.cpp
    src/app/system/HotkeyManager.cpp
    tests/DorionHookReclaimPolicyTest.cpp
    tests/DorionHookReclaimSequenceTest.cpp
    tests/SingleHookReplacementTest.cpp
    tests/TelexEngineTest.cpp
    tests/CombinedEngineTest.cpp
    tests/VniParityTest.cpp
    tests/TelexDictionaryTest.cpp
    tests/EngineFactoryTest.cpp
    tests/EngineSignatureTest.cpp
    tests/TrayStatusTextTest.cpp
    tests/SharedStateTest.cpp
    tests/HookContextAnchorTest.cpp
    tests/FeatureOptionsTest.cpp
    tests/EnglishBypassTest.cpp
    tests/PhonotacticsValidatorTest.cpp
    tests/PhonotacticsTest.cpp
    tests/TypingActionTest.cpp
    tests/CustomKeyMapTest.cpp
    tests/CodeTableConverterTest.cpp
    tests/QuickConvertLogicTest.cpp
    tests/UpdateSecurityTest.cpp
    tests/AdvancedEngineDownloadPolicyTest.cpp
    tests/EngineBenchmarkTest.cpp
    tests/MacroPrefixTest.cpp
    tests/MacroCaseTest.cpp
    tests/MacroContextMatchTest.cpp
    tests/MacroTableDecisionTest.cpp
    tests/HookEngineAtomicTests.cpp
    tests/TypingConfigRCUTests.cpp
    tests/SpellExclusionCanonicalizerTest.cpp
    tests/LexiconTransactionTest.cpp
    tests/LexiconValidationTest.cpp
    tests/LexiconWireFormatTest.cpp
    tests/LexiconWireManagerTest.cpp
    tests/MainThreadWorkerTests.cpp
    tests/AdaptiveTickTest.cpp
    tests/AdaptiveTickBenchmark.cpp
    tests/AutoCapDecisionTest.cpp
    tests/TsfEditDecisionTest.cpp
    tests/TsfPromotionDecisionTest.cpp
    tests/AutoCapStateTransitionTest.cpp
    tests/CjkSwitchDecisionTest.cpp
    tests/LeakedKeyDuringSendDecisionTest.cpp
    tests/PerAppModeDecisionTest.cpp
    tests/WebView2CacheDecisionTest.cpp
    tests/CommitUndoExemptionTest.cpp
    tests/CommitUndoArmDecisionTest.cpp
    tests/PathUtilTest.cpp
    tests/DigitLedWordDecisionTest.cpp
    tests/LoggerTest.cpp
    tests/HotkeyRegistryTest.cpp
    tests/PerfHistogramTest.cpp
    tests/HookCommandMailboxTest.cpp
    tests/FocusApplyDecisionTest.cpp
    tests/FocusTargetDecisionTest.cpp
    tests/BrowserContextStateTest.cpp
    tests/NativeMessagingTest.cpp
    src/browser_host/NativeMessaging.cpp
    tests/FocusInterleavingTest.cpp
    tests/ThreadOwnershipTest.cpp
    tests/ConfigSnapshotTest.cpp
    tests/ConfigReloadBurstTest.cpp
    tests/InjectorSwitchTest.cpp
    tests/HotkeyLabelTest.cpp
    tests/HotkeyConfigMigrationTest.cpp
    tests/ParseConfigLinesTest.cpp
    tests/pipeline/EnumsTest.cpp
    tests/pipeline/IntentTest.cpp
    tests/pipeline/IntentSinkTest.cpp
    tests/pipeline/CoordinatorRegistryTest.cpp
    tests/pipeline/CoordinatorDispatchTest.cpp
    tests/pipeline/CoordinatorStageDispatchTest.cpp
    tests/pipeline/CoordinatorGateFilterTest.cpp
    tests/pipeline/OutputChannelTest.cpp
    tests/pipeline/HookCompositionSessionTest.cpp
    tests/pipeline/EnglishBiasGateTest.cpp
    tests/pipeline/SpellCheckGateTest.cpp
    tests/pipeline/ToneEscapeGateTest.cpp
    tests/pipeline/BackwardEditFeatureTest.cpp
    tests/pipeline/CommitUndoFeatureTest.cpp
    tests/pipeline/EscRestoreRawFeatureTest.cpp
    tests/pipeline/MacroFeatureTest.cpp
    tests/engine/EngineRuleRegistryTest.cpp
    tests/engine/ToneRuleTest.cpp
    tests/engine/ModifierRuleTest.cpp
    tests/engine/AdjacentCircumflexProposalTest.cpp
    tests/engine/HornWSpeculateParityProbeTest.cpp
    tests/engine/HornModifierProposalTest.cpp
    tests/engine/StrokeDProposalTest.cpp
    tests/engine/BracketProposalTest.cpp
    tests/engine/VniVowelModifierSpeculateParityProbeTest.cpp
    tests/engine/VniProposalsTest.cpp
    tests/engine/QuickStartConsonantRuleTest.cpp
    tests/engine/QuickEndConsonantRuleTest.cpp
    tests/RestartThrottleTest.cpp
    src/app/system/UpdateSecurity.cpp
    src/app/system/AdvancedEngineDownloadPolicy.cpp
    src/app/system/MainThreadWorker.cpp
    src/app/system/PerfHistogram.cpp
    src/app/system/HookCommandMailbox.cpp
)

# Windows-only tests (require ConfigManager, ConfigEvent, Win32 APIs)
if(WIN32)
    list(APPEND NEXTKEY_TEST_SOURCES
        tests/ConfigManagerTest.cpp
        tests/ConfigEventTest.cpp
        tests/SmartSwitchPersistenceTest.cpp
        tests/HeartbeatPublisherTest.cpp
        tests/core/config/ConfigSnapshotBuilderTest.cpp
        # IOutputInjector unit tests + OutputDispatcher retry integration.
        # VKeyTests links VKeyCore (which excludes app/output and app/system),
        # so the required implementations are pulled into the test binary.
        tests/output/Win32SendInputInjectorTest.cpp
        tests/output/RichEditEmReplaceSelInjectorTest.cpp
        tests/output/OutputDispatcherRetryBudgetTest.cpp
        tests/output/FocusOutputTest.cpp
        tests/output/SplitDispatchInjectorTest.cpp
        tests/output/OutputInjectorFactoryTest.cpp
        tests/output/InjectorTraitsTest.cpp
        src/app/output/Internal.cpp
        src/app/output/Win32SendInputInjector.cpp
        src/app/output/ClipboardInjector.cpp
        src/app/output/RichEditEmReplaceSelInjector.cpp
        src/app/output/SplitDispatchInjector.cpp
        src/app/output/OutputInjectorFactory.cpp
        src/app/system/FocusOwner.cpp
        src/app/system/OutputDispatcher.cpp
        src/app/system/HeartbeatPublisher.cpp
    )
endif()

if(VKEY_USE_RUST_ENGINE)
    list(APPEND NEXTKEY_TEST_SOURCES tests/RustInputEngineTest.cpp)
endif()

if(WIN32 AND VKEY_USE_RUST_ENGINE)
    list(APPEND NEXTKEY_TEST_SOURCES
        tests/AdvancedEngineStorageTest.cpp
        src/app/system/AdvancedEngineStorage.cpp
    )
endif()

add_executable(VKeyTests EXCLUDE_FROM_ALL ${NEXTKEY_TEST_SOURCES})
target_link_libraries(VKeyTests PRIVATE VKeyCore GTest::gtest_main)
target_include_directories(VKeyTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/tomlplusplus
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
)
if(VKEY_USE_RUST_ENGINE)
    target_compile_definitions(VKeyTests PRIVATE VKEY_USE_RUST_ENGINE)
    # RustInputEngineTest reads VKEY_ENGINE_ABI_VERSION to skip assertions the
    # vendored artifact cannot satisfy yet. Compile-time visible on purpose: an
    # unreachable header breaks the build instead of silently skipping.
    target_include_directories(VKeyTests PRIVATE ${VKEY_ENGINE_ROOT}/include)
    if(WIN32)
        set(_vkey_test_engine "${VKEY_ENGINE_DLL}")
        set(_vkey_test_engine_name "vkey_engine.dll")
    else()
        set(_vkey_test_engine
            "${VKEY_ENGINE_ROOT}/lib/linux-x64/libvkey_engine.so")
        set(_vkey_test_engine_name "libvkey_engine.so")
    endif()
    add_custom_command(TARGET VKeyTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_vkey_test_engine}"
            "$<TARGET_FILE_DIR:VKeyTests>/${_vkey_test_engine_name}"
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE=${_vkey_test_engine}.sig
            -DDESTINATION=$<TARGET_FILE_DIR:VKeyTests>/${_vkey_test_engine_name}.sig
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyIfPresent.cmake"
        COMMENT "Deploying trusted engine for VKeyTests"
        VERBATIM
    )
    unset(_vkey_test_engine)
    unset(_vkey_test_engine_name)
endif()
if(MSVC)
    # Test diagnostics narrow wide strings to char for gtest's ostream
    # (pure-ASCII filenames/timestamps, e.g. LoggerTest). Benign C4244 that the
    # MSVC 14.50 toolchain promotes to error via the global /WX — allow it for
    # the test target only; production targets keep /WX fully strict.
    target_compile_options(VKeyTests PRIVATE /wd4244)
    # VKeyTests keeps RTTI in Release. Test code (OutputInjectorFactoryTest,
    # InjectorSwitchTest) uses dynamic_cast to assert injector concrete types;
    # the global $<Release:/GR-> would make that C4541 "unpredictable behavior"
    # (and /WX-fatal). Production code has zero dynamic_cast, so /GR- still ships
    # on VKeyTSF/VKeyApp/VKeyLite — this override is test-only.
    target_compile_options(VKeyTests PRIVATE $<$<CONFIG:Release>:/GR>)
endif()

# Put tests in separate directory; override global /SUBSYSTEM:WINDOWS
set_target_properties(VKeyTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
)
if(MSVC)
    target_link_options(VKeyTests PRIVATE /SUBSYSTEM:CONSOLE)
endif()

include(GoogleTest)
gtest_discover_tests(VKeyTests)

# ============================================================================
# Tools — VKeyTestRunner: cross-platform tests + Windows-only runner exe
# (sub-CMakeLists gates platform-specific targets internally)
#
# Lives in the private `internal/` submodule after the public/private split
# (commit 58960e6). Public CI clones without submodule init → skip silently.
# ============================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/internal/tools/VKeyTestRunner/CMakeLists.txt")
    add_subdirectory(internal/tools/VKeyTestRunner)
endif()

# ============================================================================
# Watchdog — standalone Windows-only process supervisor (VKeyWatchdog.exe)
# (sub-CMakeLists return()s on non-WIN32)
# ============================================================================
if(VKEY_BUILD_WATCHDOG)
    add_subdirectory(src/watchdog)
endif()
