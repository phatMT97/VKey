// VKey - ConfigManager Tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyRegistry.h"
#include "core/UIConfig.h"
#include <algorithm>
#include <fstream>
#include <memory>
#include <filesystem>
#include <chrono>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace NextKey {
namespace {

class ConfigManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        testConfigPath_ = L"test_config.toml";
    }
    
    void TearDown() override {
        // Clean up test file
        std::filesystem::remove(std::filesystem::path(testConfigPath_));
    }
    
    void WriteTestConfig(const std::string& content) {
        std::ofstream file("test_config.toml");
        file << content;
        file.close();
    }
    
    std::wstring testConfigPath_;
};

// Use an isolated directory so .tmp/.corrupt files cannot leak between tests.
class ConfigDurabilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
            ("vkey-config-durability-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(dir_));
        path_ = (dir_ / "config.toml").wstring();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    void Write(const std::wstring& path, const std::string& content) {
        std::ofstream file(std::filesystem::path(path), std::ios::binary);
        file << content;
        file.close();
        ASSERT_FALSE(file.fail());
    }

    std::string Read(const std::wstring& path) {
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    void SaveVni() {
        TypingConfig config;
        config.inputMethod = InputMethod::VNI;
        config.spellCheckEnabled = false;
        ASSERT_TRUE(ConfigManager::SaveToFile(path_, config));
    }

    std::filesystem::path dir_;
    std::wstring path_;
};

TEST_F(ConfigDurabilityTest, SaveLeavesOnlyTheConfigFile) {
    SaveVni();
    SaveVni();
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir_),
                            std::filesystem::directory_iterator()), 1);
    auto loaded = ConfigManager::LoadFromFile(path_);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->inputMethod, InputMethod::VNI);
}

// The shape a power loss left behind before saves flushed data before rename.
TEST_F(ConfigDurabilityTest, DamagedConfigIsMovedAsideNotOverwritten) {
    const std::string damaged(128, '\0');
    Write(path_, damaged);
    EXPECT_FALSE(ConfigManager::LoadFromFile(path_));
    SaveVni();
    EXPECT_EQ(Read(path_ + L".corrupt"), damaged);
    auto loaded = ConfigManager::LoadFromFile(path_);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->inputMethod, InputMethod::VNI);
}

TEST_F(ConfigDurabilityTest, UnreadableConfigFailsSaveInsteadOfBeingReplaced) {
    ASSERT_TRUE(std::filesystem::create_directory(path_));
    EXPECT_FALSE(ConfigManager::SaveToFile(path_, TypingConfig{}));
    EXPECT_TRUE(std::filesystem::is_directory(path_));
    EXPECT_FALSE(std::filesystem::exists(path_ + L".corrupt"));
}

TEST_F(ConfigDurabilityTest, MergeKeepsOtherSections) {
    Write(path_, "[input]\nmethod = 'vni'\n[macros]\nbtw = 'by the way'\n[custom]\nvalue = 42\n");
    ASSERT_TRUE(ConfigManager::SaveUIConfig(path_, UIConfig{}));
    EXPECT_EQ(ConfigManager::LoadMacros(path_)[L"btw"], L"by the way");
    EXPECT_NE(Read(path_).find("value = 42"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(path_ + L".corrupt"));
}

TEST_F(ConfigDurabilityTest, CacheNoticesRewriteWithUnchangedTimestamp) {
    ASSERT_TRUE(ConfigManager::SaveToFile(path_, TypingConfig{}));
    ASSERT_TRUE(ConfigManager::LoadFromFile(path_)); // Prime cache.
    const auto mtime = std::filesystem::last_write_time(path_);
    Write(path_, "[input]\nmethod = 'vni'\n");
    std::filesystem::last_write_time(path_, mtime);
    auto loaded = ConfigManager::LoadFromFile(path_);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->inputMethod, InputMethod::VNI);
}

TEST_F(ConfigDurabilityTest, TempWriteFailureLeavesConfigIntact) {
    SaveVni();
    const auto previous = Read(path_);
    ASSERT_TRUE(std::filesystem::create_directory(path_ + L".tmp"));
    EXPECT_FALSE(ConfigManager::SaveToFile(path_, TypingConfig{}));
    EXPECT_EQ(Read(path_), previous);
}

#ifdef _WIN32
TEST_F(ConfigDurabilityTest, UnicodePathRoundTrips) {
    path_ = (dir_ / L"c\u1ea5u-h\u00ecnh.toml").wstring();
    SaveVni();
    auto loaded = ConfigManager::LoadFromFile(path_);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->inputMethod, InputMethod::VNI);
}

TEST_F(ConfigDurabilityTest, FailedReplacementPreservesConfig) {
    SaveVni();
    const auto previous = Read(path_);
    // Allow reads/writes but deny deletion/replacement, like a file scanner.
    HANDLE handle = CreateFileW(path_.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    std::unique_ptr<void, decltype(&CloseHandle)> heldFile(handle, &CloseHandle);
    EXPECT_FALSE(ConfigManager::SaveToFile(path_, TypingConfig{}));
    EXPECT_EQ(Read(path_), previous);
    EXPECT_FALSE(std::filesystem::exists(path_ + L".tmp"));
}
#endif

// ============================================================================
// Loading Tests
// ============================================================================

TEST_F(ConfigManagerTest, LoadFromFile_DefaultTelex) {
    WriteTestConfig(R"(
[input]
method = "telex"

[features]
spell_check = false
optimize_level = 0
)");
    
    auto config = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->inputMethod, InputMethod::Telex);
    EXPECT_FALSE(config->spellCheckEnabled);
    EXPECT_EQ(config->optimizeLevel, 0);
}

TEST_F(ConfigManagerTest, LoadFromFile_VNI) {
    WriteTestConfig(R"(
[input]
method = "vni"

[features]
spell_check = true
optimize_level = 2
)");
    
    auto config = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->inputMethod, InputMethod::VNI);
    EXPECT_TRUE(config->spellCheckEnabled);
    EXPECT_EQ(config->optimizeLevel, 2);
}

TEST_F(ConfigManagerTest, LoadFromFile_MissingFile) {
    auto config = ConfigManager::LoadFromFile(L"nonexistent_config.toml");
    EXPECT_FALSE(config.has_value());
}

TEST_F(ConfigManagerTest, LoadFromFile_InvalidToml) {
    WriteTestConfig("this is not valid toml {{{{");
    
    auto config = ConfigManager::LoadFromFile(testConfigPath_);
    EXPECT_FALSE(config.has_value());
}

TEST_F(ConfigManagerTest, LoadOrDefault_NoFile) {
    // LoadOrDefault should return compiled defaults when no file exists
    auto config = ConfigManager::LoadOrDefault();
    
    // Defaults are Telex, spell check ON, optimize 0
    EXPECT_EQ(config.inputMethod, InputMethod::Telex);
    EXPECT_TRUE(config.spellCheckEnabled);
    EXPECT_EQ(config.optimizeLevel, 0);
}

// ============================================================================
// Saving Tests
// ============================================================================

TEST_F(ConfigManagerTest, SaveToFile_Telex) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    config.spellCheckEnabled = false;
    config.optimizeLevel = 1;
    
    EXPECT_TRUE(ConfigManager::SaveToFile(testConfigPath_, config));
    
    // Reload and verify
    auto loaded = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->inputMethod, InputMethod::Telex);
    EXPECT_EQ(loaded->optimizeLevel, 1);
}

TEST_F(ConfigManagerTest, SaveToFile_VNI) {
    TypingConfig config;
    config.inputMethod = InputMethod::VNI;
    config.spellCheckEnabled = true;

    EXPECT_TRUE(ConfigManager::SaveToFile(testConfigPath_, config));

    auto loaded = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->inputMethod, InputMethod::VNI);
    EXPECT_TRUE(loaded->spellCheckEnabled);
}

TEST_F(ConfigManagerTest, HidePreeditUnderlineDefaultsOffAndRoundTrips) {
    WriteTestConfig(R"(
[features]
tsf_apps = true
)");

    auto defaults = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(defaults.has_value());
    EXPECT_FALSE(defaults->hidePreeditUnderline);

    defaults->hidePreeditUnderline = true;
    ASSERT_TRUE(ConfigManager::SaveToFile(testConfigPath_, *defaults));

    auto loaded = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->hidePreeditUnderline);
}

TEST_F(ConfigManagerTest, MacroUndoOnBackspaceDefaultsOffAndRoundTrips) {
    WriteTestConfig(R"(
[features]
macro_enabled = true
[macros]
bbh = 'bún bò Huế'
)");
    auto config = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->macroUndoOnBackspace);
    const auto macros = ConfigManager::LoadMacros(testConfigPath_);

    for (bool undoOnBs : {true, false}) {
        config->macroUndoOnBackspace = undoOnBs;
        ASSERT_TRUE(ConfigManager::SaveToFile(testConfigPath_, *config));
        const auto loaded = ConfigManager::LoadFromFile(testConfigPath_);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->macroUndoOnBackspace, undoOnBs);
        EXPECT_TRUE(loaded->macroEnabled);
        EXPECT_EQ(ConfigManager::LoadMacros(testConfigPath_), macros);
    }
}

TEST_F(ConfigManagerTest, MacroTitleCaseDefaultsOffAndRoundTripsWithoutChangingMacros) {
    WriteTestConfig(R"(
[features]
auto_caps_macro = true
[macros]
nMa = 'on My way'
)");
    auto config = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->macroTitleCase);
    const auto macros = ConfigManager::LoadMacros(testConfigPath_);

    for (bool titleCase : {true, false}) {
        config->macroTitleCase = titleCase;
        ASSERT_TRUE(ConfigManager::SaveToFile(testConfigPath_, *config));
        const auto loaded = ConfigManager::LoadFromFile(testConfigPath_);
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->macroTitleCase, titleCase);
        EXPECT_TRUE(loaded->autoCapsMacro);
        EXPECT_EQ(ConfigManager::LoadMacros(testConfigPath_), macros);
    }
}

TEST_F(ConfigManagerTest, SaveToFile_PreservesUISection) {
    // First save UI config
    UIConfig uiConfig;
    uiConfig.showAdvanced = true;
    uiConfig.backgroundOpacity = 65;
    EXPECT_TRUE(ConfigManager::SaveUIConfig(testConfigPath_, uiConfig));

    // Now save typing config - should preserve [ui] section
    TypingConfig typingConfig;
    typingConfig.inputMethod = InputMethod::VNI;
    typingConfig.spellCheckEnabled = true;
    EXPECT_TRUE(ConfigManager::SaveToFile(testConfigPath_, typingConfig));

    // Verify UI config is preserved
    auto loadedUI = ConfigManager::LoadUIConfig(testConfigPath_);
    ASSERT_TRUE(loadedUI.has_value());
    EXPECT_TRUE(loadedUI->showAdvanced);
    EXPECT_EQ(loadedUI->backgroundOpacity, 65);

    // Verify typing config was saved
    auto loadedTyping = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(loadedTyping.has_value());
    EXPECT_EQ(loadedTyping->inputMethod, InputMethod::VNI);
}

// ============================================================================
// Path Resolution Tests
// ============================================================================

TEST_F(ConfigManagerTest, GetConfigPath_NotEmpty) {
    auto path = ConfigManager::GetConfigPath();
    EXPECT_FALSE(path.empty());
    EXPECT_TRUE(path.find(L"config.toml") != std::wstring::npos);
}

// ============================================================================
// UIConfig Loading Tests
// ============================================================================

TEST_F(ConfigManagerTest, LoadUIConfig_DefaultValues) {
    WriteTestConfig(R"(
[ui]
show_advanced = false
background_opacity = 80
dark_mode = true
pinned = false
)");

    auto config = ConfigManager::LoadUIConfig(testConfigPath_);
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->showAdvanced);
    EXPECT_EQ(config->backgroundOpacity, 80);
    EXPECT_FALSE(config->pinned);
}

TEST_F(ConfigManagerTest, LoadUIConfig_CustomValues) {
    WriteTestConfig(R"(
[ui]
show_advanced = true
background_opacity = 50
pinned = true
)");

    auto config = ConfigManager::LoadUIConfig(testConfigPath_);
    ASSERT_TRUE(config.has_value());
    EXPECT_TRUE(config->showAdvanced);
    EXPECT_EQ(config->backgroundOpacity, 50);
    EXPECT_TRUE(config->pinned);
}

TEST_F(ConfigManagerTest, LoadUIConfig_MissingFile) {
    auto config = ConfigManager::LoadUIConfig(L"nonexistent_config.toml");
    EXPECT_FALSE(config.has_value());
}

TEST_F(ConfigManagerTest, LoadUIConfig_MissingUISection) {
    WriteTestConfig(R"(
[input]
method = "telex"
)");

    auto config = ConfigManager::LoadUIConfig(testConfigPath_);
    // Should return config with default values when [ui] section is missing
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->showAdvanced);
    EXPECT_EQ(config->backgroundOpacity, 80);
}

TEST_F(ConfigManagerTest, LoadUIConfig_FallbackToDefaults) {
    // When LoadUIConfig returns nullopt, caller should use UIConfig defaults
    auto config = ConfigManager::LoadUIConfig(L"nonexistent_file.toml");

    // Simulate what LoadUIConfigOrDefault does
    UIConfig result = config.value_or(UIConfig{});

    EXPECT_FALSE(result.showAdvanced);
    EXPECT_EQ(result.backgroundOpacity, 80);
    EXPECT_FALSE(result.pinned);
}

// ============================================================================
// UIConfig Saving Tests
// ============================================================================

TEST_F(ConfigManagerTest, SaveUIConfig_Basic) {
    UIConfig config;
    config.showAdvanced = true;
    config.backgroundOpacity = 60;
    config.pinned = true;

    EXPECT_TRUE(ConfigManager::SaveUIConfig(testConfigPath_, config));

    // Reload and verify
    auto loaded = ConfigManager::LoadUIConfig(testConfigPath_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->showAdvanced);
    EXPECT_EQ(loaded->backgroundOpacity, 60);
    EXPECT_TRUE(loaded->pinned);
}

TEST_F(ConfigManagerTest, SaveUIConfig_PreservesOtherSections) {
    // Write initial config with [input] section
    WriteTestConfig(R"(
[input]
method = "vni"

[features]
spell_check = true
optimize_level = 2
)");

    // Save UI config
    UIConfig uiConfig;
    uiConfig.showAdvanced = true;
    uiConfig.backgroundOpacity = 75;
    EXPECT_TRUE(ConfigManager::SaveUIConfig(testConfigPath_, uiConfig));

    // Verify [input] section is preserved
    auto typingConfig = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(typingConfig.has_value());
    EXPECT_EQ(typingConfig->inputMethod, InputMethod::VNI);
    EXPECT_TRUE(typingConfig->spellCheckEnabled);

    // Verify UI config was saved
    auto loadedUI = ConfigManager::LoadUIConfig(testConfigPath_);
    ASSERT_TRUE(loadedUI.has_value());
    EXPECT_TRUE(loadedUI->showAdvanced);
    EXPECT_EQ(loadedUI->backgroundOpacity, 75);
}

TEST_F(ConfigManagerTest, SaveUIConfig_OpacityBoundaries) {
    // Test minimum opacity
    UIConfig configMin;
    configMin.backgroundOpacity = 0;
    EXPECT_TRUE(ConfigManager::SaveUIConfig(testConfigPath_, configMin));
    auto loadedMin = ConfigManager::LoadUIConfig(testConfigPath_);
    ASSERT_TRUE(loadedMin.has_value());
    EXPECT_EQ(loadedMin->backgroundOpacity, 0);

    // Test maximum opacity
    UIConfig configMax;
    configMax.backgroundOpacity = 100;
    EXPECT_TRUE(ConfigManager::SaveUIConfig(testConfigPath_, configMax));
    auto loadedMax = ConfigManager::LoadUIConfig(testConfigPath_);
    ASSERT_TRUE(loadedMax.has_value());
    EXPECT_EQ(loadedMax->backgroundOpacity, 100);
}

// ============================================================================
// UIConfig Default Values Test
// ============================================================================

TEST_F(ConfigManagerTest, UIConfig_DefaultConstructor) {
    UIConfig config;

    EXPECT_FALSE(config.showAdvanced);
    EXPECT_EQ(config.backgroundOpacity, 80);
    EXPECT_FALSE(config.pinned);
}

// ============================================================================
// SaveHotkeyRegistry — TSF mirror of [features].esc_restore_raw
//
// SaveHotkeyRegistry MUST keep [features].esc_restore_raw in sync with
// "CancelComposition intent is enabled AND has bare-Esc trigger" so the TSF
// EngineController gate stays consistent with the v3 Hotkeys UI. Without
// this, disabling cancel-composition in the dialog wouldn't reach TSF hosts
// (Word, Edge in TSF mode) — they'd keep restoring raw keys on Esc.
// Phase 2 will route TSF through HotkeyRegistry directly and drop the mirror.
// ============================================================================

namespace {
constexpr uint32_t kVkEsc = 0x1B;
[[nodiscard]] bool ReadEscRestoreRaw(const std::wstring& path) {
    auto cfg = ConfigManager::LoadFromFile(path);
    return cfg && cfg->escRestoreRawEnabled;
}
}  // namespace

TEST_F(ConfigManagerTest, SaveHotkeyRegistry_EscBound_EnabledSetsTrue) {
    HotkeyRegistry reg = HotkeyRegistry::Defaults();
    reg.SetEnabled(Intent::CancelComposition, true);
    ASSERT_TRUE(ConfigManager::SaveHotkeyRegistry(testConfigPath_, reg));
    EXPECT_TRUE(ReadEscRestoreRaw(testConfigPath_));
}

TEST_F(ConfigManagerTest, SaveHotkeyRegistry_DefaultEscBound_DisabledSetsFalse) {
    HotkeyRegistry reg = HotkeyRegistry::Defaults();
    ASSERT_TRUE(ConfigManager::SaveHotkeyRegistry(testConfigPath_, reg));
    EXPECT_FALSE(ReadEscRestoreRaw(testConfigPath_))
        << "Fresh defaults must leave TSF ESC restoration off even though Esc remains stored";
}

TEST_F(ConfigManagerTest, SaveHotkeyRegistry_NoBareEscTrigger_SetsFalse) {
    // Only Ctrl+Esc bound (not bare Esc). TSF gate must be off because the
    // legacy `esc_restore_raw` semantic was "bare Esc restores raw".
    HotkeyRegistry reg;
    reg.SetEnabled(Intent::CancelComposition, true);
    reg.AddTrigger(Intent::CancelComposition, Trigger{kVkEsc, kModCtrl, false});
    ASSERT_TRUE(ConfigManager::SaveHotkeyRegistry(testConfigPath_, reg));
    EXPECT_FALSE(ReadEscRestoreRaw(testConfigPath_))
        << "Bare-Esc binding required for TSF gate; chord doesn't count";
}

TEST_F(ConfigManagerTest, SaveHotkeyRegistry_EscPlusChord_BareEscWins) {
    HotkeyRegistry reg;
    reg.SetEnabled(Intent::CancelComposition, true);
    reg.AddTrigger(Intent::CancelComposition, Trigger{kVkEsc, 0,        false});
    reg.AddTrigger(Intent::CancelComposition, Trigger{kVkEsc, kModCtrl, false});
    ASSERT_TRUE(ConfigManager::SaveHotkeyRegistry(testConfigPath_, reg));
    EXPECT_TRUE(ReadEscRestoreRaw(testConfigPath_));
}

// ============================================================================
// HotkeyConfig (V/E toggle + convert) schema migration: `key` (wchar_t) →
// `vk` (uint32_t). Both top-level [hotkey] and nested [convert.hotkey].
// ============================================================================

TEST_F(ConfigManagerTest, LoadHotkeyConfig_LegacyKeyChar_MigratesToVk) {
    WriteTestConfig(R"(
[hotkey]
ctrl = false
shift = false
alt = true
win = false
key = "Z"
)");
    auto cfg = ConfigManager::LoadHotkeyConfig(testConfigPath_);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->vk, 0x5Au);   // VK_Z
    EXPECT_TRUE(cfg->alt);
    EXPECT_FALSE(cfg->ctrl);
}

TEST_F(ConfigManagerTest, LoadHotkeyConfig_NewVkSchema_LoadedDirect) {
    WriteTestConfig(R"(
[hotkey]
ctrl = true
shift = true
alt = false
win = false
vk = 112
)");
    auto cfg = ConfigManager::LoadHotkeyConfig(testConfigPath_);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->vk, 0x70u);   // VK_F1
    EXPECT_TRUE(cfg->ctrl);
    EXPECT_TRUE(cfg->shift);
}

TEST_F(ConfigManagerTest, LoadHotkeyConfig_VkWinsOverLegacyKey) {
    // If both `vk` and `key` are present, `vk` wins — legacy is fallback only.
    WriteTestConfig(R"(
[hotkey]
ctrl = false
shift = false
alt = false
win = false
vk = 90
key = "A"
)");
    auto cfg = ConfigManager::LoadHotkeyConfig(testConfigPath_);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->vk, 0x5Au);   // VK_Z from `vk`, NOT 0x41 from `key`
}

TEST_F(ConfigManagerTest, LoadHotkeyConfig_LegacyOemDropsToZero) {
    // ~/`/etc are OEM keys — clean rule rejects, vk=0 (user reassigns).
    WriteTestConfig(R"(
[hotkey]
ctrl = false
shift = false
alt = true
win = false
key = "~"
)");
    auto cfg = ConfigManager::LoadHotkeyConfig(testConfigPath_);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->vk, 0u);
    EXPECT_TRUE(cfg->alt);       // Modifiers preserved even when vk drops.
}

TEST_F(ConfigManagerTest, SaveHotkeyConfig_RoundTripPreservesFRowVk) {
    HotkeyConfig original{};
    original.ctrl = true;
    original.shift = true;
    original.vk = 0x74;  // VK_F5

    ASSERT_TRUE(ConfigManager::SaveHotkeyConfig(testConfigPath_, original));
    auto loaded = ConfigManager::LoadHotkeyConfig(testConfigPath_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->vk, 0x74u);
    EXPECT_TRUE(loaded->ctrl);
    EXPECT_TRUE(loaded->shift);
}

TEST_F(ConfigManagerTest, SaveHotkeyConfig_DropsLegacyKeyField) {
    // After save, the TOML file must contain `vk = N` but NOT a legacy
    // `key = "..."` field — even if the previous file had one (we replace
    // the [hotkey] sub-table entirely on save).
    WriteTestConfig(R"(
[hotkey]
key = "Z"
ctrl = true
alt = false
shift = false
win = false
)");
    HotkeyConfig cfg{};
    cfg.alt = true;
    cfg.vk = 0x70;  // VK_F1
    ASSERT_TRUE(ConfigManager::SaveHotkeyConfig(testConfigPath_, cfg));

    std::ifstream in("test_config.toml");
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("vk ="), std::string::npos);
    EXPECT_EQ(content.find("key ="), std::string::npos)
        << "Legacy `key` field must be dropped after save";
}

TEST_F(ConfigManagerTest, LoadConvertConfig_LegacyKeyChar_MigratesToVk) {
    WriteTestConfig(R"(
[convert.hotkey]
ctrl = true
shift = false
alt = false
win = false
key = "1"
)");
    auto cfg = ConfigManager::LoadConvertConfig(testConfigPath_);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->hotkey.vk, 0x31u);   // VK_1 (digit-1 ASCII == VK_1)
    EXPECT_TRUE(cfg->hotkey.ctrl);
}

TEST_F(ConfigManagerTest, LoadConvertConfig_NewVkSchemaFRow) {
    WriteTestConfig(R"(
[convert.hotkey]
ctrl = false
shift = true
alt = false
win = false
vk = 123
)");
    auto cfg = ConfigManager::LoadConvertConfig(testConfigPath_);
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->hotkey.vk, 0x7Bu);   // VK_F12
    EXPECT_TRUE(cfg->hotkey.shift);
}

TEST_F(ConfigManagerTest, SaveHotkeyRegistry_DoesNotClobberExistingFeatures) {
    // Pre-write [features] with unrelated keys — SaveHotkeyRegistry must
    // merge in place, only touching esc_restore_raw.
    WriteTestConfig(R"(
[features]
spell_check = true
modern_ortho = true
)");

    HotkeyRegistry reg = HotkeyRegistry::Defaults();
    reg.SetEnabled(Intent::CancelComposition, true);
    ASSERT_TRUE(ConfigManager::SaveHotkeyRegistry(testConfigPath_, reg));

    auto loaded = ConfigManager::LoadFromFile(testConfigPath_);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->spellCheckEnabled)    << "Existing spell_check must survive merge";
    EXPECT_TRUE(loaded->modernOrtho)          << "Existing modern_ortho must survive merge";
    EXPECT_TRUE(loaded->escRestoreRawEnabled) << "esc_restore_raw added by mirror";
}

// ============================================================================
// Per-app mode lock — [excluded_apps].list (E) + .force_vn (V)
// ============================================================================

TEST_F(ConfigManagerTest, LoadForcedVnApps_ParsesForceVnArray) {
    WriteTestConfig(R"(
[excluded_apps]
list = ["game.exe"]
force_vn = ["zalo.exe", "messenger.exe"]
)");
    auto excluded = ConfigManager::LoadAllExcludedApps(testConfigPath_);
    auto forcedVn = ConfigManager::LoadForcedVnApps(testConfigPath_);
    ASSERT_EQ(excluded.size(), 1u);
    EXPECT_EQ(excluded[0], L"game.exe");
    ASSERT_EQ(forcedVn.size(), 2u);
    EXPECT_NE(std::find(forcedVn.begin(), forcedVn.end(), L"zalo.exe"), forcedVn.end());
    EXPECT_NE(std::find(forcedVn.begin(), forcedVn.end(), L"messenger.exe"), forcedVn.end());
}

TEST_F(ConfigManagerTest, LoadForcedVnApps_AbsentKeyReturnsEmpty_BackwardCompat) {
    // A config written before this feature has only [excluded_apps].list.
    WriteTestConfig(R"(
[excluded_apps]
list = ["legacy.exe"]
)");
    EXPECT_EQ(ConfigManager::LoadAllExcludedApps(testConfigPath_).size(), 1u);
    EXPECT_TRUE(ConfigManager::LoadForcedVnApps(testConfigPath_).empty());
}

TEST_F(ConfigManagerTest, SaveForcedVnApps_PreservesExcludedList) {
    WriteTestConfig(R"(
[excluded_apps]
list = ["game.exe", "mstsc.exe"]
)");
    ASSERT_TRUE(ConfigManager::SaveForcedVnApps(testConfigPath_, {L"zalo.exe"}));
    // The E list must survive a V-only save (read-modify-write, not clobber).
    auto excluded = ConfigManager::LoadAllExcludedApps(testConfigPath_);
    auto forcedVn = ConfigManager::LoadForcedVnApps(testConfigPath_);
    EXPECT_EQ(excluded.size(), 2u);
    ASSERT_EQ(forcedVn.size(), 1u);
    EXPECT_EQ(forcedVn[0], L"zalo.exe");
}

TEST_F(ConfigManagerTest, SaveExcludedApps_PreservesForceVnList) {
    WriteTestConfig(R"(
[excluded_apps]
list = ["old.exe"]
force_vn = ["zalo.exe"]
)");
    ASSERT_TRUE(ConfigManager::SaveExcludedApps(testConfigPath_, {L"game.exe"}));
    auto excluded = ConfigManager::LoadAllExcludedApps(testConfigPath_);
    auto forcedVn = ConfigManager::LoadForcedVnApps(testConfigPath_);
    ASSERT_EQ(excluded.size(), 1u);
    EXPECT_EQ(excluded[0], L"game.exe");
    ASSERT_EQ(forcedVn.size(), 1u)  << "force_vn must survive an E-list save";
    EXPECT_EQ(forcedVn[0], L"zalo.exe");
}

// ============================================================================
// #224 — pre-v3 `[excludedApps]` (camelCase) list must still be honored.
// The 713f862f rename to `[excluded_apps]` never carried old entries over
// (unlike hotkeys' MigrateLegacyHotkeysIfNeeded), so any app excluded before
// that rename silently fell out of enforcement on upgrade.
// ============================================================================

TEST_F(ConfigManagerTest, LoadAllExcludedApps_MergesLegacyCamelCaseSection) {
    WriteTestConfig(R"(
[excludedApps]
enabled = true
list = ["Palworld-Win64-Shipping.exe", "D2R.exe"]

[excluded_apps]
list = ["cs2.exe"]
)");
    auto excluded = ConfigManager::LoadAllExcludedApps(testConfigPath_);
    ASSERT_EQ(excluded.size(), 3u);
    EXPECT_NE(std::find(excluded.begin(), excluded.end(), L"cs2.exe"), excluded.end());
    // Legacy entries merge in lowercased — runtime exe-name matching is
    // always-lowercase (FocusOwner::GetExeNameForHwnd).
    EXPECT_NE(std::find(excluded.begin(), excluded.end(), L"palworld-win64-shipping.exe"),
              excluded.end());
    EXPECT_NE(std::find(excluded.begin(), excluded.end(), L"d2r.exe"), excluded.end());
}

TEST_F(ConfigManagerTest, LoadAllExcludedApps_LegacySectionDisabled_NotMerged) {
    WriteTestConfig(R"(
[excludedApps]
enabled = false
list = ["oldgame.exe"]

[excluded_apps]
list = ["cs2.exe"]
)");
    auto excluded = ConfigManager::LoadAllExcludedApps(testConfigPath_);
    ASSERT_EQ(excluded.size(), 1u);
    EXPECT_EQ(excluded[0], L"cs2.exe");
}

TEST_F(ConfigManagerTest, LoadAllExcludedApps_LegacyDuplicateNotDoubled) {
    WriteTestConfig(R"(
[excludedApps]
enabled = true
list = ["CS2.exe"]

[excluded_apps]
list = ["cs2.exe"]
)");
    auto excluded = ConfigManager::LoadAllExcludedApps(testConfigPath_);
    ASSERT_EQ(excluded.size(), 1u);
    EXPECT_EQ(excluded[0], L"cs2.exe");
}

TEST_F(ConfigManagerTest, SaveExcludedApps_DropsLegacyCamelCaseSection) {
    WriteTestConfig(R"(
[excludedApps]
enabled = true
list = ["oldgame.exe"]

[excluded_apps]
list = ["cs2.exe"]
)");
    ASSERT_TRUE(ConfigManager::SaveExcludedApps(testConfigPath_, {L"cs2.exe", L"oldgame.exe"}));

    std::ifstream in("test_config.toml");
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("excludedApps"), std::string::npos)
        << "Legacy [excludedApps] section must be dropped after save";
}

TEST_F(ConfigManagerTest, SaveAndLoad_UnicodePath) {
    std::wstring unicodePath = L"cấu_hình_tiếng_việt.toml";
    testConfigPath_ = unicodePath; // Fixture also cleans the recovery copy.
    std::filesystem::remove(std::filesystem::path(unicodePath));

    TypingConfig config;
    config.inputMethod = InputMethod::VNI;
    config.spellCheckEnabled = true;
    config.optimizeLevel = 2;

    EXPECT_TRUE(ConfigManager::SaveToFile(unicodePath, config));
    
    // Reload and verify
    auto loaded = ConfigManager::LoadFromFile(unicodePath);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->inputMethod, InputMethod::VNI);
    EXPECT_TRUE(loaded->spellCheckEnabled);
    EXPECT_EQ(loaded->optimizeLevel, 2);

    // Test UIConfig save/load with Unicode path
    UIConfig uiConfig;
    uiConfig.showAdvanced = true;
    uiConfig.backgroundOpacity = 65;
    EXPECT_TRUE(ConfigManager::SaveUIConfig(unicodePath, uiConfig));

    auto loadedUI = ConfigManager::LoadUIConfig(unicodePath);
    ASSERT_TRUE(loadedUI.has_value());
    EXPECT_TRUE(loadedUI->showAdvanced);
    EXPECT_EQ(loadedUI->backgroundOpacity, 65);

    // Cleanup
    std::filesystem::remove(std::filesystem::path(unicodePath));
}

TEST_F(ConfigManagerTest, FormatConfigTomlForLexicon) {
    WriteTestConfig(R"(
[input]
method = "telex"

[features]
macro_enabled = true
spell_check = true
spell_suggest = false
spell_exclusions = [ "old" ]
)");

    std::vector<std::wstring> newExclusions = {L"h\u0111", L"\u0111c\u0111t"};
    std::string formatted = ConfigManager::FormatConfigTomlForLexicon(testConfigPath_, true, newExclusions);
    EXPECT_FALSE(formatted.empty());

    // Verify spell_suggest was updated to true
    EXPECT_NE(formatted.find("spell_suggest = true"), std::string::npos);
    // Verify macro_enabled was preserved
    EXPECT_NE(formatted.find("macro_enabled = true"), std::string::npos);
    // Verify exclusions array updated
    EXPECT_NE(formatted.find("h\xc4\x91"), std::string::npos);
    EXPECT_NE(formatted.find("\xc4\x91" "c" "\xc4\x91" "t"), std::string::npos);
}

TEST_F(ConfigManagerTest, FormatConfigTomlForLexicon_WithWireGeneration) {
    WriteTestConfig(R"(
[input]
method = "telex"

[features]
spell_suggest = false
)");

    std::vector<std::wstring> exclusions = { L"msword" };
    std::string formatted = ConfigManager::FormatConfigTomlForLexicon(testConfigPath_, true, exclusions, 12345ULL);
    EXPECT_FALSE(formatted.empty());

    EXPECT_NE(formatted.find("spell_suggest = true"), std::string::npos);
    EXPECT_NE(formatted.find("[internal]"), std::string::npos);
    EXPECT_NE(formatted.find("wire_generation = 12345"), std::string::npos);
}

TEST_F(ConfigManagerTest, LoadAndSaveWireGeneration) {
    // Missing file or missing [internal] returns default generation 1
    EXPECT_EQ(ConfigManager::LoadWireGeneration(testConfigPath_), 1ULL);

    WriteTestConfig(R"(
[input]
method = "telex"

[internal]
wire_generation = 9876543210
)");

    EXPECT_EQ(ConfigManager::LoadWireGeneration(testConfigPath_), 9876543210ULL);

    // Save updated generation
    ASSERT_TRUE(ConfigManager::SaveWireGeneration(testConfigPath_, 9876543211ULL));
    EXPECT_EQ(ConfigManager::LoadWireGeneration(testConfigPath_), 9876543211ULL);
}

}  // namespace
}  // namespace NextKey
