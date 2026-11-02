// VKey - Advanced engine download policy tests
// SPDX-License-Identifier: GPL-3.0-only

#include "app/system/AdvancedEngineDownloadPolicy.h"
#include "core/Version.h"

#include <gtest/gtest.h>

using namespace NextKey;

TEST(AdvancedEngineDownloadPolicyTest, SignatureUses72ByteBudgetInsteadOfDllSize) {
    AdvancedEngineDownloadBudget signature(AdvancedEngineDownloadAsset::Signature, 1012224);
    EXPECT_TRUE(signature.MatchesContentLength(72));
    EXPECT_FALSE(signature.MatchesContentLength(1012224));
    EXPECT_TRUE(signature.AcceptChunk(32));
    EXPECT_FALSE(signature.Complete());
    EXPECT_TRUE(signature.AcceptChunk(40));
    EXPECT_TRUE(signature.Complete());
    EXPECT_FALSE(signature.AcceptChunk(1));
}

TEST(AdvancedEngineDownloadPolicyTest, DllKeepsItsOwnExactLengthAndRejectsTruncationOrOverflow) {
    AdvancedEngineDownloadBudget engine(AdvancedEngineDownloadAsset::Engine, 1012224);
    EXPECT_FALSE(engine.MatchesContentLength(72));
    EXPECT_TRUE(engine.MatchesContentLength(1012224));
    EXPECT_TRUE(engine.AcceptChunk(1012223));
    EXPECT_FALSE(engine.Complete());
    EXPECT_FALSE(engine.AcceptChunk(2));
    EXPECT_TRUE(engine.AcceptChunk(1));
    EXPECT_TRUE(engine.Complete());
}

TEST(AdvancedEngineDownloadPolicyTest, UsesCurrentReleaseTagAndExactAssetName) {
    EXPECT_EQ(BuildAdvancedEngineReleaseUrl(), std::wstring(L"https://github.com/phatMT97/VKey/releases/download/v") +
                                                   VKEY_VERSION_WSTR + L"/vkey_engine.dll");
    EXPECT_EQ(BuildAdvancedEngineReleasePageUrl(), std::wstring(L"https://github.com/phatMT97/VKey/releases/tag/v") +
                                                       VKEY_VERSION_WSTR);
}

TEST(AdvancedEngineDownloadPolicyTest, AllowsOnlyRequiredHttpsOrigins) {
    EXPECT_TRUE(IsAllowedAdvancedEngineUrl(BuildAdvancedEngineReleaseUrl()));
    EXPECT_TRUE(IsAllowedAdvancedEngineUrl(BuildAdvancedEngineReleasePageUrl()));
    EXPECT_TRUE(IsAllowedAdvancedEngineUrl(BuildAdvancedEngineSignatureUrl()));
    EXPECT_TRUE(IsAllowedAdvancedEngineUrl(L"https://github.com/phatMT97/VKey/releases/"
                                           L"download/v4.3.0/vkey_engine.dll"));
    EXPECT_TRUE(IsAllowedAdvancedEngineUrl(L"https://objects.githubusercontent.com/"
                                           L"github-production-release-asset/test"));
    EXPECT_TRUE(IsAllowedAdvancedEngineUrl(L"HTTPS://RELEASE-ASSETS.GITHUBUSERCONTENT.COM/"
                                           L"github-production-release-asset/test"));

    EXPECT_FALSE(IsAllowedAdvancedEngineUrl(L"http://github.com/phatMT97/VKey/releases/"
                                            L"download/v4.3.0/vkey_engine.dll"));
    EXPECT_FALSE(IsAllowedAdvancedEngineUrl(L"https://github.com.evil.example/vkey_engine.dll"));
    EXPECT_FALSE(IsAllowedAdvancedEngineUrl(L"https://github.com@evil.example/vkey_engine.dll"));
    EXPECT_FALSE(IsAllowedAdvancedEngineUrl(L"https://release-assets.githubusercontent.com.evil.example/"
                                            L"vkey_engine.dll"));
    EXPECT_FALSE(IsAllowedAdvancedEngineUrl(L"https://github.com:443/phatMT97/VKey/vkey_engine.dll"));
}
