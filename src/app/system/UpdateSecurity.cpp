// VKey - Update Security Helpers Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "UpdateSecurity.h"
#include "CancelableBindStatusCallback.h"

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#include <urlmon.h>
#include <wintrust.h>
#include <Softpub.h>
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#endif

namespace NextKey {

// ── SEC-002: PowerShell single-quote escaping ──────────────────────────────

std::wstring EscapePowerShellSingleQuote(const std::wstring& input) noexcept {
    try {
        std::wstring result;
        result.reserve(input.size() + 8);
        for (wchar_t ch : input) {
            result += ch;
            if (ch == L'\'') {
                result += L'\'';  // Double the single quote: ' -> ''
            }
        }
        return result;
    } catch (...) {
        return {};  // Allocation failure — return empty rather than unescaped
    }
}

// ── SEC-003: URL domain validation ─────────────────────────────────────────

namespace {

/// ASCII-only case-insensitive prefix check (URLs are always ASCII in scheme+host).
bool StartsWithIgnoreCase(const std::wstring& str, const wchar_t* prefix) noexcept {
    for (size_t i = 0; prefix[i] != L'\0'; ++i) {
        if (i >= str.size()) return false;
        wchar_t sc = str[i];
        wchar_t pc = prefix[i];
        // ASCII A-Z fold only (safe for URL scheme+host)
        if (sc >= L'A' && sc <= L'Z') sc = sc - L'A' + L'a';
        if (pc >= L'A' && pc <= L'Z') pc = pc - L'A' + L'a';
        if (sc != pc) return false;
    }
    return true;
}

bool StartsWithIgnoreCase(const std::string& str, const char* prefix) noexcept {
    for (size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= str.size()) return false;
        char sc = str[i];
        char pc = prefix[i];
        if (sc >= 'A' && sc <= 'Z') sc = sc - 'A' + 'a';
        if (pc >= 'A' && pc <= 'Z') pc = pc - 'A' + 'a';
        if (sc != pc) return false;
    }
    return true;
}

}  // namespace

bool IsAllowedDownloadUrl(const std::wstring& url) noexcept {
    static constexpr const wchar_t* allowedPrefixes[] = {
        L"https://github.com/",
        L"https://objects.githubusercontent.com/",
        L"https://codeload.github.com/",
    };
    for (const auto* prefix : allowedPrefixes) {
        if (StartsWithIgnoreCase(url, prefix)) return true;
    }
    return false;
}

bool IsAllowedDownloadUrl(const std::string& url) noexcept {
    static constexpr const char* allowedPrefixes[] = {
        "https://github.com/",
        "https://objects.githubusercontent.com/",
        "https://codeload.github.com/",
    };
    for (const auto* prefix : allowedPrefixes) {
        if (StartsWithIgnoreCase(url, prefix)) return true;
    }
    return false;
}

// ── SEC-001: ParseSha256File (cross-platform — pure string parsing) ──────────

std::string ParseSha256File(const std::string& content) noexcept {
    try {
        if (content.empty()) return {};

        std::string line = content;

        // Strip trailing whitespace / newlines
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }

        // Extract first token (the hash) — split on space
        std::string hash;
        auto spacePos = line.find(' ');
        if (spacePos != std::string::npos) {
            hash = line.substr(0, spacePos);
        } else {
            hash = line;
        }

        // SHA-256 hash must be exactly 64 hex characters
        if (hash.size() != 64) return {};

        // Validate all chars are hex and lowercase the result
        std::string result;
        result.reserve(64);
        for (char ch : hash) {
            if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) {
                result += ch;
            } else if (ch >= 'A' && ch <= 'F') {
                result += static_cast<char>(ch + ('a' - 'A'));
            } else {
                return {};  // Not a valid hex character
            }
        }

        return result;
    } catch (...) {
        return {};
    }
}

// ── SEC-001: Windows CNG — SHA-256 compute + ZIP verify ──────────────────────

#ifdef _WIN32

std::string ComputeFileSha256(const std::wstring& filePath) noexcept {
    try {
        struct AlgHandle {
            BCRYPT_ALG_HANDLE h = nullptr;
            ~AlgHandle() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
        };
        struct HashHandle {
            BCRYPT_HASH_HANDLE h = nullptr;
            ~HashHandle() { if (h) BCryptDestroyHash(h); }
        };
        struct FileHandle {
            HANDLE h = INVALID_HANDLE_VALUE;
            ~FileHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        };

        AlgHandle alg;
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(status) || !alg.h) return {};

        DWORD hashObjectSize = 0, cbData = 0;
        status = BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PBYTE>(&hashObjectSize), sizeof(DWORD), &cbData, 0);
        if (!BCRYPT_SUCCESS(status)) return {};

        DWORD hashSize = 0;
        status = BCryptGetProperty(alg.h, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PBYTE>(&hashSize), sizeof(DWORD), &cbData, 0);
        if (!BCRYPT_SUCCESS(status) || hashSize != 32) return {};

        std::vector<BYTE> hashObjectBuf(hashObjectSize);

        HashHandle hash;
        status = BCryptCreateHash(alg.h, &hash.h, hashObjectBuf.data(),
            hashObjectSize, nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(status) || !hash.h) return {};

        FileHandle file;
        file.h = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.h == INVALID_HANDLE_VALUE) return {};

        constexpr DWORD kBufSize = 65536;
        std::vector<BYTE> readBuf(kBufSize);
        DWORD bytesRead = 0;

        for (;;) {
            if (!ReadFile(file.h, readBuf.data(), kBufSize, &bytesRead, nullptr)) return {};
            if (bytesRead == 0) break;
            status = BCryptHashData(hash.h, readBuf.data(), bytesRead, 0);
            if (!BCRYPT_SUCCESS(status)) return {};
        }

        std::vector<BYTE> hashValue(hashSize);
        status = BCryptFinishHash(hash.h, hashValue.data(), hashSize, 0);
        if (!BCRYPT_SUCCESS(status)) return {};

        std::string hexStr;
        hexStr.reserve(64);
        static constexpr char hexChars[] = "0123456789abcdef";
        for (DWORD i = 0; i < hashSize; ++i) {
            hexStr += hexChars[(hashValue[i] >> 4) & 0x0F];
            hexStr += hexChars[hashValue[i] & 0x0F];
        }

        return hexStr;
    } catch (...) {
        return {};
    }
}

// CancelableBindStatusCallback lives in CancelableBindStatusCallback.h
// (shared with UpdateChecker.cpp).

bool VerifyDownloadedZip(
    const std::wstring& zipUrl,
    const std::wstring& localZipPath,
    std::atomic<bool>& cancelFlag) noexcept
{
    try {
        if (cancelFlag.load(std::memory_order_relaxed)) return false;

        // Defense-in-depth: reject non-GitHub URLs even if caller forgot to validate
        if (!IsAllowedDownloadUrl(zipUrl)) return false;

        // 1. Build checksum URL: append ".sha256" to the ZIP URL
        std::wstring checksumUrl = zipUrl + L".sha256";

        // 2. Download checksum file to temp location
        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempDir);
        std::wstring checksumPath = std::wstring(tempDir) + L"vkey_checksum.sha256";

        CancelableBindStatusCallback callback(cancelFlag);
        HRESULT hr = URLDownloadToFileW(nullptr, checksumUrl.c_str(),
            checksumPath.c_str(), 0, &callback);
        if (FAILED(hr)) {
            DeleteFileW(checksumPath.c_str());
            return false;
        }

        // 3. Read checksum file content
        std::ifstream checksumFile(std::filesystem::path(checksumPath), std::ios::binary);
        if (!checksumFile.is_open()) {
            DeleteFileW(checksumPath.c_str());
            return false;
        }
        std::ostringstream ss;
        ss << checksumFile.rdbuf();
        checksumFile.close();
        DeleteFileW(checksumPath.c_str());

        std::string checksumContent = ss.str();
        if (checksumContent.empty()) return false;

        // 4. Parse expected hash
        std::string expectedHash = ParseSha256File(checksumContent);
        if (expectedHash.empty()) return false;

        // 5. Compute actual hash of downloaded ZIP
        std::string actualHash = ComputeFileSha256(localZipPath);
        if (actualHash.empty()) return false;

        // 6. Compare (both are lowercase)
        return expectedHash == actualHash;
    } catch (...) {
        return false;
    }
}

// ── Authenticode signature + publisher pin ──────────────────────────────────

namespace {

bool ContainsIgnoreCase(const std::wstring& haystack, const std::wstring& needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    auto lower = [](wchar_t c) -> wchar_t {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    };
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j) {
            if (lower(haystack[i + j]) != lower(needle[j])) break;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

// Extract the signing certificate's subject display name and test the pin.
bool SignerSubjectContains(const std::wstring& filePath, const std::wstring& needle) noexcept {
    HCERTSTORE hStore = nullptr;
    HCRYPTMSG hMsg = nullptr;
    bool ok = false;

    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, filePath.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr,
            &hStore, &hMsg, nullptr)) {
        return false;
    }

    DWORD cbSignerInfo = 0;
    if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &cbSignerInfo)
        && cbSignerInfo > 0) {
        std::vector<BYTE> buf(cbSignerInfo);
        if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &cbSignerInfo)) {
            auto* signer = reinterpret_cast<CMSG_SIGNER_INFO*>(buf.data());
            CERT_INFO ci{};
            ci.Issuer = signer->Issuer;
            ci.SerialNumber = signer->SerialNumber;
            PCCERT_CONTEXT cert = CertFindCertificateInStore(hStore,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                CERT_FIND_SUBJECT_CERT, &ci, nullptr);
            if (cert) {
                DWORD len = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                    0, nullptr, nullptr, 0);
                if (len > 1) {
                    std::vector<wchar_t> name(len);
                    CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                        nullptr, name.data(), len);
                    ok = ContainsIgnoreCase(std::wstring(name.data()), needle);
                }
                CertFreeCertificateContext(cert);
            }
        }
    }

    if (hMsg) CryptMsgClose(hMsg);
    if (hStore) CertCloseStore(hStore, 0);
    return ok;
}

}  // namespace

bool VerifyAuthenticodeSignature(const std::wstring& filePath,
                                 const std::wstring& expectedSubjectSubstring) noexcept {
    try {
        // 1. WinVerifyTrust: signature intact + chains to a trusted root + not revoked.
        WINTRUST_FILE_INFO fileInfo{};
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = filePath.c_str();

        GUID actionGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        WINTRUST_DATA wtd{};
        wtd.cbStruct = sizeof(wtd);
        wtd.dwUIChoice = WTD_UI_NONE;
        wtd.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        wtd.dwUnionChoice = WTD_CHOICE_FILE;
        wtd.pFile = &fileInfo;
        wtd.dwStateAction = WTD_STATEACTION_VERIFY;
        wtd.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN | WTD_SAFER_FLAG;

        LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),
                                     &actionGuid, &wtd);
        // Release the WinVerifyTrust state regardless of the verify result.
        wtd.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &actionGuid, &wtd);

        if (status != ERROR_SUCCESS) return false;  // unsigned / untrusted / revoked

        // 2. Optional publisher pin.
        if (expectedSubjectSubstring.empty()) return true;
        return SignerSubjectContains(filePath, expectedSubjectSubstring);
    } catch (...) {
        return false;
    }
}

#endif  // _WIN32

}  // namespace NextKey
