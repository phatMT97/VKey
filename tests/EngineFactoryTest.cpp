// VKey - EngineFactory Tests
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>
#include "core/engine/EngineFactory.h"
#include "core/engine/IInputEngine.h"
#include "core/engine/TypingEngine.h"
#ifdef VKEY_USE_RUST_ENGINE
#include "core/engine/RustInputEngine.h"
#endif

namespace NextKey {
namespace {

class EngineFactoryTest : public ::testing::Test {};

// ============================================================================
// Factory Creation Tests
// ============================================================================

TEST_F(EngineFactoryTest, Create_Telex_FromConfig) {
    TypingConfig config;
    config.inputMethod = InputMethod::Telex;
    
    auto engine = EngineFactory::Create(config);
    ASSERT_NE(engine, nullptr);
    
    // Verify it's Telex by checking behavior (aa -> â)
    engine->PushChar(L'a');
    engine->PushChar(L'a');
    EXPECT_EQ(engine->Peek(), L"â");
}

TEST_F(EngineFactoryTest, Create_VNI_FromConfig) {
    TypingConfig config;
    config.inputMethod = InputMethod::VNI;
    
    auto engine = EngineFactory::Create(config);
    ASSERT_NE(engine, nullptr);
    
    // Verify it's VNI by checking behavior (a6 -> â)
    engine->PushChar(L'a');
    engine->PushChar(L'6');
    EXPECT_EQ(engine->Peek(), L"â");
}

TEST_F(EngineFactoryTest, Create_Telex_FromMethod) {
    auto engine = EngineFactory::Create(InputMethod::Telex);
    ASSERT_NE(engine, nullptr);
    
    engine->PushChar(L'e');
    engine->PushChar(L'e');
    EXPECT_EQ(engine->Peek(), L"ê");
}

TEST_F(EngineFactoryTest, Create_VNI_FromMethod) {
    auto engine = EngineFactory::Create(InputMethod::VNI);
    ASSERT_NE(engine, nullptr);
    
    engine->PushChar(L'e');
    engine->PushChar(L'6');
    EXPECT_EQ(engine->Peek(), L"ê");
}

// ============================================================================
// Runtime Switching Simulation Tests
// ============================================================================

TEST_F(EngineFactoryTest, RuntimeSwitch_TelexToVNI) {
    auto telexEngine = EngineFactory::Create(InputMethod::Telex);
    telexEngine->PushChar(L'a');
    telexEngine->PushChar(L'a');
    EXPECT_EQ(telexEngine->Peek(), L"â");
    
    // Default spell checking restores this unfinished â prefix at the boundary.
    std::wstring committed = telexEngine->Commit();
    EXPECT_EQ(committed, L"aa");
    
    // Create VNI engine (like switching)
    auto vniEngine = EngineFactory::Create(InputMethod::VNI);
    vniEngine->PushChar(L'a');
    vniEngine->PushChar(L'6');
    EXPECT_EQ(vniEngine->Peek(), L"â");
}

TEST_F(EngineFactoryTest, RuntimeSwitch_VNIToTelex) {
    auto vniEngine = EngineFactory::Create(InputMethod::VNI);
    vniEngine->PushChar(L'd');
    vniEngine->PushChar(L'9');
    EXPECT_EQ(vniEngine->Peek(), L"đ");
    
    std::wstring committed = vniEngine->Commit();
    EXPECT_EQ(committed, L"đ");
    
    // Create Telex engine
    auto telexEngine = EngineFactory::Create(InputMethod::Telex);
    telexEngine->PushChar(L'd');
    telexEngine->PushChar(L'd');
    EXPECT_EQ(telexEngine->Peek(), L"đ");
}

// ============================================================================
// Simple Telex Factory Tests
// ============================================================================

TEST_F(EngineFactoryTest, Create_SimpleTelex_FromConfig) {
    TypingConfig config;
    config.inputMethod = InputMethod::SimpleTelex;

    auto engine = EngineFactory::Create(config);
    ASSERT_NE(engine, nullptr);

    // Verify Simple Telex: standalone 'w' should be literal
    engine->PushChar(L'w');
    EXPECT_EQ(engine->Peek(), L"w");
}

TEST_F(EngineFactoryTest, Create_SimpleTelex_W_AfterVowel_IsModifier) {
    TypingConfig config;
    config.inputMethod = InputMethod::SimpleTelex;

    auto engine = EngineFactory::Create(config);
    ASSERT_NE(engine, nullptr);

    // 'w' after 'u' should still produce ư
    engine->PushChar(L'u');
    engine->PushChar(L'w');
    EXPECT_EQ(engine->Peek(), L"ư");
}

TEST_F(EngineFactoryTest, Create_SimpleTelex_FromMethod) {
    auto engine = EngineFactory::Create(InputMethod::SimpleTelex);
    ASSERT_NE(engine, nullptr);

    // Should still support circumflex (aa → â)
    engine->PushChar(L'a');
    engine->PushChar(L'a');
    EXPECT_EQ(engine->Peek(), L"â");
}

// ============================================================================
// Default Behavior Tests
// ============================================================================

TEST_F(EngineFactoryTest, DefaultConfig_IsTelex) {
    TypingConfig config;  // Default constructor

    auto engine = EngineFactory::Create(config);

    // Default should be Telex (aa -> â)
    engine->PushChar(L'a');
    engine->PushChar(L'a');
    EXPECT_EQ(engine->Peek(), L"â");
}

TEST_F(EngineFactoryTest, RustSelectionIsDisabledWithStandardSpellCheck) {
    TypingConfig config;
    config.spellSuggestEnabled = false;

    EXPECT_FALSE(EngineFactory::WillUseRustEngine(config));
}

TEST_F(EngineFactoryTest, AdvancedSpellCheckReportsRuntimeAvailability) {
    TypingConfig config;
    config.spellSuggestEnabled = true;

#ifdef VKEY_USE_RUST_ENGINE
    EXPECT_EQ(EngineFactory::WillUseRustEngine(config),
              RustInputEngine::LibraryAvailable());
#else
    EXPECT_FALSE(EngineFactory::WillUseRustEngine(config));
#endif
}

TEST_F(EngineFactoryTest, AdvancedConfigKeepsTypingAvailable) {
    TypingConfig config;
    config.spellSuggestEnabled = true;

    auto engine = EngineFactory::Create(config);
    engine->PushChar(L'a');
    engine->PushChar(L'a');

    EXPECT_EQ(engine->Peek(), L"â");
}

// ============================================================================
// ApplySharedState Simulation Tests (Bug Regression)
// These tests simulate the EngineController::ApplySharedState logic
// to catch the bug where engine wasn't created when method matched default
// ============================================================================

// Simulates ApplySharedState logic - the FIXED version
class ApplySharedStateSimulator {
public:
    void Apply(const TypingConfig& config) {
        InputMethod newMethod = config.inputMethod;

        // FIXED: Create engine if null OR method changed
        if (!engine_ || newMethod != currentMethod_) {
            if (engine_ && engine_->Count() > 0) {
                (void)engine_->Commit();
            }
            currentMethod_ = newMethod;
            engine_ = EngineFactory::Create(config);
        }
    }

    IInputEngine* GetEngine() const { return engine_.get(); }
    InputMethod GetCurrentMethod() const { return currentMethod_; }

private:
    std::unique_ptr<IInputEngine> engine_;
    InputMethod currentMethod_ = InputMethod::Telex;  // Default matches SharedState default
};

TEST_F(EngineFactoryTest, ApplySharedState_MethodMatchesDefault_EngineCreated) {
    // This was the bug: SharedState has Telex (0), currentMethod_ is Telex
    // Old code skipped engine creation because newMethod == currentMethod_
    ApplySharedStateSimulator sim;

    TypingConfig config;
    config.inputMethod = InputMethod::Telex;  // Same as default

    sim.Apply(config);

    // Engine MUST be created even when method matches default
    ASSERT_NE(sim.GetEngine(), nullptr) << "Engine must be created even when method matches default";

    // Verify it works
    sim.GetEngine()->PushChar(L'a');
    sim.GetEngine()->PushChar(L'a');
    EXPECT_EQ(sim.GetEngine()->Peek(), L"â");
}

TEST_F(EngineFactoryTest, ApplySharedState_MethodDifferent_EngineCreated) {
    ApplySharedStateSimulator sim;

    TypingConfig config;
    config.inputMethod = InputMethod::VNI;  // Different from default

    sim.Apply(config);

    ASSERT_NE(sim.GetEngine(), nullptr);
    EXPECT_EQ(sim.GetCurrentMethod(), InputMethod::VNI);

    // Verify VNI behavior
    sim.GetEngine()->PushChar(L'a');
    sim.GetEngine()->PushChar(L'6');
    EXPECT_EQ(sim.GetEngine()->Peek(), L"â");
}

TEST_F(EngineFactoryTest, ApplySharedState_MethodSwitch_CommitsPending) {
    ApplySharedStateSimulator sim;

    // First apply with Telex
    TypingConfig telexConfig;
    telexConfig.inputMethod = InputMethod::Telex;
    sim.Apply(telexConfig);

    // Type something
    sim.GetEngine()->PushChar(L'v');
    sim.GetEngine()->PushChar(L'i');
    sim.GetEngine()->PushChar(L'e');
    sim.GetEngine()->PushChar(L'e');  // "viê"
    EXPECT_GT(sim.GetEngine()->Count(), 0);

    // Now switch to VNI - should commit pending first
    TypingConfig vniConfig;
    vniConfig.inputMethod = InputMethod::VNI;
    sim.Apply(vniConfig);

    // Engine should be fresh (no pending composition)
    EXPECT_EQ(sim.GetEngine()->Count(), 0);
    EXPECT_EQ(sim.GetCurrentMethod(), InputMethod::VNI);
}

TEST_F(EngineFactoryTest, ApplySharedState_MultipleApplies_SameMethod_NoRecreate) {
    ApplySharedStateSimulator sim;

    TypingConfig config;
    config.inputMethod = InputMethod::Telex;

    // First apply - creates engine
    sim.Apply(config);
    IInputEngine* firstEngine = sim.GetEngine();
    ASSERT_NE(firstEngine, nullptr);

    // Type something
    sim.GetEngine()->PushChar(L'a');

    // Second apply with same method - should NOT recreate
    sim.Apply(config);
    IInputEngine* secondEngine = sim.GetEngine();

    // Same engine pointer (no recreation)
    EXPECT_EQ(firstEngine, secondEngine);

    // State preserved
    EXPECT_EQ(sim.GetEngine()->Count(), 1);
}

// ============================================================================
// Restart-banner condition (TSF_ENGINE_UNTRUSTED)
// ============================================================================

// The banner must fire exactly when Advanced spell-check is on and Create()
// silently fell back to the C++ engine — never in a build that ships no Rust
// engine, where that fallback is the intended backend.
TEST_F(EngineFactoryTest, RustEngineExpectedButUnavailable_MatchesFallback) {
    TypingConfig config;
    config.spellSuggestEnabled = true;
#ifdef VKEY_USE_RUST_ENGINE
    EXPECT_EQ(EngineFactory::RustEngineExpectedButUnavailable(config),
              !EngineFactory::WillUseRustEngine(config));
#else
    EXPECT_FALSE(EngineFactory::RustEngineExpectedButUnavailable(config));
#endif
}

TEST_F(EngineFactoryTest, RustEngineExpectedButUnavailable_SilentWhenSuggestOff) {
    TypingConfig config;
    config.spellSuggestEnabled = false;
    EXPECT_FALSE(EngineFactory::RustEngineExpectedButUnavailable(config));
}

}  // namespace
}  // namespace NextKey
