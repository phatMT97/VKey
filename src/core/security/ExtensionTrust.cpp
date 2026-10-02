// VKey - Extension Trust & Binary Integrity Verification
// SPDX-License-Identifier: GPL-3.0-only

#include "core/security/ExtensionTrust.h"
#include "core/Debug.h"

#include <vector>
#include <filesystem>

#ifdef _WIN32
#include <Windows.h>
#include <wintrust.h>
#include <Softpub.h>
#include <wincrypt.h>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace {

constexpr DWORD kMaxExtensionBytes = 32 * 1024 * 1024; // 32 MiB ceiling

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

bool SignerSubjectMatches(const std::wstring& filePath, const std::vector<std::wstring>& allowedSigners) noexcept {
    HCERTSTORE hStore = nullptr;
    HCRYPTMSG hMsg = nullptr;
    bool matched = false;

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
                    std::wstring subjectName(name.data());
                    for (const auto& allowed : allowedSigners) {
                        if (ContainsIgnoreCase(subjectName, allowed)) {
                            matched = true;
                            break;
                        }
                    }
                }
                CertFreeCertificateContext(cert);
            }
        }
    }

    if (hMsg) CryptMsgClose(hMsg);
    if (hStore) CertCloseStore(hStore, 0);
    return matched;
}

} // namespace
#endif // _WIN32

namespace NextKey::Security {

ExtensionTrustResult VerifyExtensionBinary(
    const std::wstring& filePath,
    std::wstring* reasonOut) noexcept {
    try {
#ifdef _WIN32
        // 1. Accessibility & Existence
        WIN32_FILE_ATTRIBUTE_DATA fileAttr{};
        if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &fileAttr)) {
            if (reasonOut) *reasonOut = L"Không tìm thấy tệp tiện ích mở rộng.";
            return ExtensionTrustResult::FileNotFound;
        }

        // 2. Size sanity check
        ULARGE_INTEGER fileSize{};
        fileSize.LowPart = fileAttr.nFileSizeLow;
        fileSize.HighPart = fileAttr.nFileSizeHigh;
        if (fileSize.QuadPart == 0 || fileSize.QuadPart > kMaxExtensionBytes) {
            if (reasonOut) *reasonOut = L"Kích thước tệp không hợp lệ.";
            return ExtensionTrustResult::SizeOutOfRange;
        }

        // 3. Authenticode signature check via WinVerifyTrust
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
        wtd.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &actionGuid, &wtd);

        if (status == ERROR_SUCCESS) {
            // Verify publisher pin
            static const std::vector<std::wstring> kAllowedSigners = {
                L"SignPath Foundation",
                L"NexusKey",
                L"VKey"
            };
            if (SignerSubjectMatches(filePath, kAllowedSigners)) {
                return ExtensionTrustResult::Trusted;
            }
        }

        // Development bypass escape hatch
#if !defined(NDEBUG) || defined(VKEY_ALLOW_UNSIGNED_EXTENSIONS)
        NEXTKEY_LOG(L"[DEV] Extension %ls bypasses signature check (allowed in dev/debug build)",
                    filePath.c_str());
        if (reasonOut) *reasonOut = L"Chế độ phát triển (Development): Bỏ qua kiểm tra chữ ký số.";
        return ExtensionTrustResult::DevBypass;
#else
        NEXTKEY_LOG(L"ExtensionTrust: verification failed for %ls (status=0x%lx)",
                    filePath.c_str(), status);
        if (reasonOut) {
            *reasonOut = L"Tệp tiện ích mở rộng không có chữ ký số chính thức hoặc đã bị thay đổi.";
        }
        return ExtensionTrustResult::SignatureInvalid;
#endif

#else
        // Non-Windows stub (e.g. Linux unit tests)
        std::error_code ec;
        if (!std::filesystem::exists(filePath, ec)) {
            if (reasonOut) *reasonOut = L"File not found";
            return ExtensionTrustResult::FileNotFound;
        }
        return ExtensionTrustResult::Trusted;
#endif
    } catch (...) {
        if (reasonOut) *reasonOut = L"Lỗi ngoại lệ trong quá trình xác thực.";
        return ExtensionTrustResult::CannotAccess;
    }
}

bool IsExtensionTrusted(const std::wstring& filePath) noexcept {
    const auto result = VerifyExtensionBinary(filePath, nullptr);
    return result == ExtensionTrustResult::Trusted || result == ExtensionTrustResult::DevBypass;
}

}  // namespace NextKey::Security
