// VKey - Localized String Dictionary Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "Strings.h"
#include <atomic>

namespace NextKey {

static std::atomic<Language> g_language{Language::Vietnamese};

// Vietnamese string table (readable UTF-8 source literals)
static const wchar_t* const kVietnamese[] = {
    L"Bật Tiếng Việt",                               // MENU_TOGGLE_VIET
    L"Bật kiểm tra chính tả",                        // MENU_SPELL_CHECK
    L"Lưu chế độ gõ theo app",                       // MENU_SMART_SWITCH
    L"Bật gõ tắt",                                   // MENU_MACRO_TOGGLE
    L"Cấu hình gõ tắt...",                           // MENU_MACRO_CONFIG
    L"Công cụ chuyển mã...",                          // MENU_CONVERT_TOOL
    L"Chuyển mã nhanh",                               // MENU_QUICK_CONVERT
    L"Kiểu gõ",                                      // MENU_INPUT_METHOD
    L"Bảng mã",                                      // MENU_CODE_TABLE
    L"Unicode tổ hợp",                               // MENU_UNICODE_COMPOUND
    L"Bảng điều khiển...",                            // MENU_SETTINGS
    L"Giới thiệu VKey",                          // MENU_ABOUT
    L"Thoát",                                        // MENU_EXIT
    L"Tắt tự khởi động lại",                         // MENU_STOP_WATCHDOG
    L"Bật tự khởi động lại",                         // MENU_ENABLE_WATCHDOG
    L"Tiện ích mở rộng",                             // MENU_EXTENSIONS
    L"Hỗ trợ trình duyệt (Browser Host)",            // MENU_EXT_BROWSER
    L"Tự khởi động lại khi crash (Watchdog)",        // MENU_EXT_WATCHDOG
    L"VKey - Tiếng Việt",                        // TIP_VIETNAMESE
    L"VKey - English",                            // TIP_ENGLISH
    L"Giới thiệu VKey",                          // ABOUT_TITLE
    L"VKey - Bộ gõ Tiếng Việt\n"                 // ABOUT_BODY
    L"Giải pháp gõ Tiếng Việt hiện đại cho Windows.\n\n"
    L"SPDX-License-Identifier: GPL-3.0-only",
    L"Cập nhật",                                     // UPDATE_TITLE
    L"Có phiên bản mới!",                            // UPDATE_AVAILABLE_TITLE
    L"Phiên bản mới %s đã sẵn sàng.\n\nỨng dụng sẽ tự động đóng, cập nhật và khởi động lại.", // UPDATE_AVAILABLE_BODY
    L"Cập nhật ngay",                                // UPDATE_NOW
    L"Bỏ qua",                                      // UPDATE_SKIP
    L"Bạn đang dùng phiên bản mới nhất!",           // UPDATE_LATEST
    L"Không thể kiểm tra cập nhật.",                 // UPDATE_FAILED
    L"Đang tải cập nhật...",                         // UPDATE_DOWNLOADING
    L"Xem thay đổi",                                // UPDATE_CHANGELOG
    L"Cập nhật thành công!",                         // UPDATE_SUCCESS
    L"Cập nhật thất bại.",                           // UPDATE_INSTALL_FAILED
    L"Kiểm tra ngay",                                // UPDATE_CHECK_NOW
    L"Đang kiểm tra...",                             // UPDATE_CHECKING
    L"Tải cập nhật thất bại.",                       // UPDATE_DOWNLOAD_FAILED
    L"Cập nhật chưa hoàn tất. Khởi động lại Windows để áp dụng phiên bản TSF mới.", // UPDATE_BANNER_PENDING
    L"Một vài ứng dụng đang chạy phiên bản cũ. Khởi động lại Windows để đồng bộ.",  // UPDATE_BANNER_MISMATCH
    L"Khởi động lại ngay",                           // UPDATE_BANNER_RESTART_NOW
    L"Để sau",                                       // UPDATE_BANNER_LATER
    L"Khởi động lại Windows ngay để hoàn tất cập nhật VKey?", // UPDATE_BANNER_CONFIRM
    L"Chưa chọn file nguồn.",                       // CONVERT_NO_SOURCE_FILE
    L"Không thể đọc file nguồn.",                    // CONVERT_READ_ERROR
    L"Clipboard trống.",                             // CONVERT_CLIPBOARD_EMPTY
    L"Chưa chọn file đích.",                         // CONVERT_NO_DEST_FILE
    L"Không thể ghi file đích.",                     // CONVERT_WRITE_ERROR
    L"Không thể ghi vào clipboard.",                 // CONVERT_CLIPBOARD_WRITE_ERROR
    L"Chuyển mã thành công!",                        // CONVERT_SUCCESS
    L"Đã đăng ký TSF thành công.\n"                  // TSF_REGISTER_SUCCESS
    L"Cần khởi động lại các ứng dụng đang mở để thay đổi có hiệu lực.",
    L"Đã gỡ đăng ký TSF thành công.\n"               // TSF_UNREGISTER_SUCCESS
    L"Cần khởi động lại các ứng dụng đang mở để thay đổi có hiệu lực.",
    L"Không thể gỡ đăng ký TSF.\n"                   // TSF_UNREGISTER_FAILED
    L"Vui lòng chạy với quyền Administrator.",
    L"Không thể thêm VKey vào danh sách loại trừ.",  // EXCLUDED_CANNOT_SELF
    L"Bạn có muốn giữ lại danh sách hiện tại không?",    // IMPORT_KEEP_EXISTING
    L"Không thể mở file để nạp dữ liệu.",                // IMPORT_FILE_OPEN_FAILED
    L"Không thể ghi file để xuất dữ liệu.",              // EXPORT_FILE_WRITE_FAILED
    L"Không thể tự khởi động lại để bỏ quyền Admin.\n"   // ADMIN_DEELEVATION_FAILED
    L"Vui lòng thoát và khởi động lại VKey thủ công.",
    L"Đã tắt tự khởi động lại. VKey sẽ không tự bật lại nếu thoát đột ngột.", // WATCHDOG_STOPPED_BODY
    L"Đã bật tự khởi động lại. VKey sẽ tự bật lại nếu thoát đột ngột.", // WATCHDOG_ENABLED_BODY
    L"Đã tắt tiện ích Hỗ trợ trình duyệt (Browser Host).", // BROWSER_EXT_STOPPED_BODY
    L"Đã bật tiện ích Hỗ trợ trình duyệt (Browser Host).\nCác trình duyệt được hỗ trợ sẽ tự động kết nối với VKey.", // BROWSER_EXT_ENABLED_BODY
    L"Cấu hình hiện tại đã được sao lưu thành công tại:\n%s\n\nỨng dụng sẽ tự động đóng để tiến hành cập nhật.", // UPDATE_BACKUP_SUCCESS
    L"Cần VKey Engine",                              // SPELL_ADVANCED_REQUIRED_TITLE
    L"Chế độ Nâng cao cần vkey_engine.dll đúng phiên bản. VKey luôn kiểm tra tính toàn vẹn của file trước khi nạp.", // SPELL_ADVANCED_DOWNLOAD_PROMPT
    L"Tải và cài đặt tự động\nTải đúng engine từ GitHub và xác minh trước khi sử dụng.", // SPELL_ADVANCED_DOWNLOAD_AUTO
    L"Cài đặt thủ công\nMở file bằng trình duyệt; phù hợp khi firewall chặn truy cập Internet của VKey.", // SPELL_ADVANCED_INSTALL_MANUAL
    L"Tiếp tục dùng chế độ Cơ bản\nKhông tải và không hỏi lại khi VKey khởi động.", // SPELL_ADVANCED_USE_STANDARD
    L"Không thể bật chế độ Nâng cao",                // SPELL_ADVANCED_NOT_ENABLED_TITLE
    L"Cài đặt VKey Engine thủ công",                 // SPELL_ADVANCED_MANUAL_TITLE
    L"1. VKey vừa mở trình duyệt để tải vkey_engine.dll đúng với phiên bản hiện tại. Nếu trình duyệt cảnh báo file .dll, hãy chọn Giữ lại (Keep).\n\n2. Đặt file vừa tải vào thư mục chứa VKey.exe:\n", // SPELL_ADVANCED_MANUAL_BODY
    L"3. Quay lại VKey và chọn Kiểm tra chính tả: Nâng cao lần nữa. VKey sẽ xác minh file trước khi nạp.", // SPELL_ADVANCED_MANUAL_FINISH
    L"Tải lại vkey_engine.dll bằng trình duyệt",     // SPELL_ADVANCED_MANUAL_LINK
    L"Mở thư mục VKey",                              // SPELL_ADVANCED_OPEN_FOLDER
    L"Windows không thể mở mục này. Hãy sao chép liên kết hoặc đường dẫn bên dưới và mở thủ công:", // SPELL_ADVANCED_OPEN_TARGET_FAILED
    L"Chọn Yes để tải tự động, No để cài thủ công hoặc Cancel để tiếp tục dùng chế độ Cơ bản.", // SPELL_ADVANCED_FALLBACK_CHOICE
    L"Chọn Yes để cài thủ công hoặc No để tiếp tục dùng chế độ Cơ bản.", // SPELL_ADVANCED_FAILURE_FALLBACK_CHOICE
    L"Đang tải và xác minh VKey Engine...",          // SPELL_ADVANCED_DOWNLOADING
    L"Không thể tải VKey Engine. Hãy kiểm tra kết nối mạng rồi thử lại.", // SPELL_ADVANCED_NETWORK_FAILED
    L"File VKey Engine tải về không khớp bản tin cậy. Chế độ Nâng cao chưa được bật.", // SPELL_ADVANCED_VERIFY_FAILED
    L"Không thể cài vkey_engine.dll cạnh VKey. Hãy đóng các phiên bản VKey khác và kiểm tra quyền ghi của thư mục rồi thử lại.", // SPELL_ADVANCED_INSTALL_FAILED
    L"Bạn cần khởi động lại VKey để giải phóng VKey Engine khỏi bộ nhớ.\nBạn có muốn khởi động lại VKey ngay?",  // SPELL_ADVANCED_CLOSE_APP
    L"Chế độ Nâng cao sử dụng VKey Engine (Rust).\nKhởi động lại VKey để engine được nạp cho cả những ứng dụng đang mở.\nBạn có muốn khởi động lại VKey ngay?",  // SPELL_ADVANCED_LOAD_APP
    L"Bạn có muốn xóa gõ tắt này không?",             // MACRO_CONFIRM_DELETE
    L"Bạn có chắc chắn muốn xóa %d từ gõ tắt đã chọn?", // MACRO_CONFIRM_DELETE_MULTI
};

// English string table
static const wchar_t* const kEnglish[] = {
    L"Enable Vietnamese",                             // MENU_TOGGLE_VIET
    L"Enable spell check",                            // MENU_SPELL_CHECK
    L"Smart app exclusion",                           // MENU_SMART_SWITCH
    L"Enable macro typing",                           // MENU_MACRO_TOGGLE
    L"Macro settings...",                             // MENU_MACRO_CONFIG
    L"Convert tool...",                               // MENU_CONVERT_TOOL
    L"Quick convert",                                 // MENU_QUICK_CONVERT
    L"Input method",                                  // MENU_INPUT_METHOD
    L"Code table",                                    // MENU_CODE_TABLE
    L"Unicode Compound",                              // MENU_UNICODE_COMPOUND
    L"Settings...",                                   // MENU_SETTINGS
    L"About VKey",                                // MENU_ABOUT
    L"Exit",                                          // MENU_EXIT
    L"Stop auto-restart",                             // MENU_STOP_WATCHDOG
    L"Enable auto-restart",                           // MENU_ENABLE_WATCHDOG
    L"Extensions",                                    // MENU_EXTENSIONS
    L"Browser Integration (Browser Host)",            // MENU_EXT_BROWSER
    L"Auto-restart on crash (Watchdog)",              // MENU_EXT_WATCHDOG
    L"VKey - Vietnamese",                         // TIP_VIETNAMESE
    L"VKey - English",                            // TIP_ENGLISH
    L"About VKey",                                // ABOUT_TITLE
    L"VKey Vietnamese Input\n"                    // ABOUT_BODY
    L"A modern Vietnamese typing solution for Windows.\n\n"
    L"SPDX-License-Identifier: GPL-3.0-only",
    L"Update",                                       // UPDATE_TITLE
    L"Update available!",                            // UPDATE_AVAILABLE_TITLE
    L"Version %s is available.\n\nThe application will automatically close, update, and restart.", // UPDATE_AVAILABLE_BODY
    L"Update now",                                   // UPDATE_NOW
    L"Skip",                                         // UPDATE_SKIP
    L"You're on the latest version!",                // UPDATE_LATEST
    L"Unable to check for updates.",                 // UPDATE_FAILED
    L"Downloading update...",                        // UPDATE_DOWNLOADING
    L"View changelog",                               // UPDATE_CHANGELOG
    L"Update successful!",                           // UPDATE_SUCCESS
    L"Update failed.",                               // UPDATE_INSTALL_FAILED
    L"Check now",                                    // UPDATE_CHECK_NOW
    L"Checking...",                                  // UPDATE_CHECKING
    L"Download failed.",                             // UPDATE_DOWNLOAD_FAILED
    L"Update not finished. Restart Windows to apply the new TSF version.", // UPDATE_BANNER_PENDING
    L"Some apps still run the old version. Restart Windows to sync.",      // UPDATE_BANNER_MISMATCH
    L"Restart now",                                  // UPDATE_BANNER_RESTART_NOW
    L"Later",                                        // UPDATE_BANNER_LATER
    L"Restart Windows now to finish the VKey update?", // UPDATE_BANNER_CONFIRM
    L"No source file selected.",                     // CONVERT_NO_SOURCE_FILE
    L"Cannot read source file.",                     // CONVERT_READ_ERROR
    L"Clipboard is empty.",                          // CONVERT_CLIPBOARD_EMPTY
    L"No destination file selected.",                // CONVERT_NO_DEST_FILE
    L"Cannot write destination file.",               // CONVERT_WRITE_ERROR
    L"Cannot write to clipboard.",                   // CONVERT_CLIPBOARD_WRITE_ERROR
    L"Conversion successful!",                       // CONVERT_SUCCESS
    L"TSF registered successfully.\n"                // TSF_REGISTER_SUCCESS
    L"Please restart open applications for changes to take effect.",
    L"TSF unregistered successfully.\n"              // TSF_UNREGISTER_SUCCESS
    L"Please restart open applications for changes to take effect.",
    L"Unable to unregister TSF.\n"                   // TSF_UNREGISTER_FAILED
    L"Please run as Administrator.",
    L"Cannot add VKey to the exclusion list.",   // EXCLUDED_CANNOT_SELF
    L"Keep the existing list?",                      // IMPORT_KEEP_EXISTING
    L"Could not open file for import.",              // IMPORT_FILE_OPEN_FAILED
    L"Could not write file for export.",             // EXPORT_FILE_WRITE_FAILED
    L"Could not auto-restart to drop admin rights.\n"  // ADMIN_DEELEVATION_FAILED
    L"Please exit and relaunch VKey manually.",
    L"Auto-restart stopped. VKey will not relaunch on crash.", // WATCHDOG_STOPPED_BODY
    L"Auto-restart enabled. VKey will relaunch on crash.", // WATCHDOG_ENABLED_BODY
    L"Browser Integration (Browser Host) disabled.", // BROWSER_EXT_STOPPED_BODY
    L"Browser Integration (Browser Host) enabled.\nSupported browsers will automatically connect to VKey.", // BROWSER_EXT_ENABLED_BODY
    L"Your current configuration has been backed up at:\n%s\n\nThe application will automatically close to perform the update.", // UPDATE_BACKUP_SUCCESS
    L"VKey Engine required",                         // SPELL_ADVANCED_REQUIRED_TITLE
    L"Advanced mode requires the correct version of vkey_engine.dll. VKey always verifies the file's integrity before loading it.", // SPELL_ADVANCED_DOWNLOAD_PROMPT
    L"Download and install automatically\nDownload the exact engine from GitHub and verify it before use.", // SPELL_ADVANCED_DOWNLOAD_AUTO
    L"Install manually\nOpen the file in your browser; suitable when a firewall blocks VKey's Internet access.", // SPELL_ADVANCED_INSTALL_MANUAL
    L"Continue with Standard mode\nDo not download or ask again when VKey starts.", // SPELL_ADVANCED_USE_STANDARD
    L"Unable to enable Advanced mode",               // SPELL_ADVANCED_NOT_ENABLED_TITLE
    L"Install VKey Engine manually",                 // SPELL_ADVANCED_MANUAL_TITLE
    L"1. VKey just opened your browser to download the vkey_engine.dll matching this version. If your browser warns about the .dll, choose Keep.\n\n2. Place the downloaded file in the folder containing VKey.exe:\n", // SPELL_ADVANCED_MANUAL_BODY
    L"3. Return to VKey and select Spell check: Advanced again. VKey will verify the file before loading it.", // SPELL_ADVANCED_MANUAL_FINISH
    L"Download vkey_engine.dll again in your browser", // SPELL_ADVANCED_MANUAL_LINK
    L"Open VKey folder",                             // SPELL_ADVANCED_OPEN_FOLDER
    L"Windows could not open this item. Copy the link or path below and open it manually:", // SPELL_ADVANCED_OPEN_TARGET_FAILED
    L"Choose Yes to download automatically, No to install manually, or Cancel to continue with Standard mode.", // SPELL_ADVANCED_FALLBACK_CHOICE
    L"Choose Yes to install manually or No to continue with Standard mode.", // SPELL_ADVANCED_FAILURE_FALLBACK_CHOICE
    L"Downloading and verifying VKey Engine...",     // SPELL_ADVANCED_DOWNLOADING
    L"Unable to download VKey Engine. Check your network connection and try again.", // SPELL_ADVANCED_NETWORK_FAILED
    L"The downloaded VKey Engine does not match the trusted build. Advanced mode was not enabled.", // SPELL_ADVANCED_VERIFY_FAILED
    L"Unable to install vkey_engine.dll next to VKey. Close other VKey instances, check the folder's write permission, and try again.", // SPELL_ADVANCED_INSTALL_FAILED
    L"VKey needs to restart to release VKey Engine from memory.\nDo you want to restart VKey now?",  // SPELL_ADVANCED_CLOSE_APP
    L"Advanced mode uses VKey Engine (Rust).\nRestart VKey so the engine also loads for apps that are already open.\nDo you want to restart VKey now?",  // SPELL_ADVANCED_LOAD_APP
    L"Delete this shortcut?",                        // MACRO_CONFIRM_DELETE
    L"Delete the %d selected shortcuts?",            // MACRO_CONFIRM_DELETE_MULTI
};

static_assert(sizeof(kVietnamese) / sizeof(kVietnamese[0]) == static_cast<size_t>(StringId::_COUNT),
              "Vietnamese string table size mismatch");
static_assert(sizeof(kEnglish) / sizeof(kEnglish[0]) == static_cast<size_t>(StringId::_COUNT),
              "English string table size mismatch");

void SetLanguage(Language lang) noexcept {
    g_language.store(lang, std::memory_order_relaxed);
}

Language GetLanguage() noexcept {
    return g_language.load(std::memory_order_relaxed);
}

const wchar_t* S(StringId id) noexcept {
    auto index = static_cast<uint16_t>(id);
    if (index >= static_cast<uint16_t>(StringId::_COUNT)) return L"";

    return g_language.load(std::memory_order_relaxed) == Language::English
        ? kEnglish[index]
        : kVietnamese[index];
}

}  // namespace NextKey
