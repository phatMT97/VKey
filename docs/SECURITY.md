# Security Hardening

VKey là bộ gõ chạy với quyền truy cập bàn phím — bảo mật được đặt ưu tiên cao trong thiết kế.

## Threat Model

IME cục bộ trên Windows cá nhân. Kẻ tấn công chính: phần mềm độc hại cùng user session (không cần admin). MITM mạng là ưu tiên thấp.

---

## 1. Update System

| Biện pháp | Mô tả |
|-----------|-------|
| **Authenticode signature + publisher pin** | Trước khi cài, mỗi file `.exe`/`.dll` trong gói ứng dụng đã ký được xác minh qua `WinVerifyTrust` (chuỗi tin cậy tới root + kiểm tra thu hồi) và ghim đúng publisher `SignPath Foundation`. Đây là lớp chống giả mạo thật sự — chứng minh *ai* đã ký mã sắp chạy.<br>**Hiện trạng v4.3:** chỉ ba binary Classic C++-only (`VKeyClassic.exe`, `VKeyTSF.dll`, Watchdog phát hành riêng dưới tên `VKeyClassicWatchdog.exe`) đi qua SignPath. Gói tiêu chuẩn Sciter và optional `vkey_engine.dll` phát hành ngoài ZIP không ký Authenticode; engine được xác thực bằng pin đã compile như mô tả ở mục 4. Vì bản Sciter không qua publisher pin, cập nhật tự động lên bản đó **không hoạt động** và người dùng phải cài thủ công. Đây là fail-closed đúng thiết kế |
| **SHA-256 hash verification** | Mỗi bản cập nhật đi kèm file `.sha256`. Sau khi tải, VKey tính hash thực tế bằng Windows CNG (bcrypt) và so khớp trước khi giải nén. *Lưu ý:* sidecar `.sha256` tải cùng nguồn với ZIP → chỉ chống **hỏng file/CDN**, không chống giả mạo (nguồn bị chiếm là chiếm cả hai). Chống giả mạo do lớp Authenticode ở trên đảm nhận |
| **URL domain whitelist** | Chỉ chấp nhận tải từ `https://github.com/`, `https://objects.githubusercontent.com/`, `https://codeload.github.com/`. Từ chối HTTP và domain lạ |
| **PowerShell command escaping** | Escape ký tự `'` trong đường dẫn trước khi truyền vào `Expand-Archive`, chống command injection |
| **ZIP path validation** | Tham số `--install-update` chỉ chấp nhận file trong `%TEMP%`. Dùng `GetFullPathNameW()` để resolve path traversal (`..\..\evil.zip`) |

## 2. IPC & Shared Memory

| Biện pháp | Mô tả |
|-----------|-------|
| **DACL & session scope** | Shared memory, config event, mutex dùng **default DACL** (creator user SID + SYSTEM + Admins) + prefix `Local\` (session scope) → chặn truy cập **chéo-user** và **chéo-session**. *Lưu ý (2026-07-02):* KHÔNG dùng SDDL giới hạn `CreatorOwner` (`MakeCreatorOnlySecurityAttributes` là dead code) — SID `CO` chặn cả subprocess **cùng user**, mà TSF DLL chạy trong process host khác lại cần đọc. Hệ quả: malware **cùng user session** vẫn mở được các named object này. Đây là giới hạn chấp nhận được: attacker cùng user đã có năng lực tương đương (đọc config, inject vào VKey) bằng cách khác |
| **Session-scoped names** | Tất cả named objects dùng prefix `Local\` — không expose ra session khác |
| **Seqlock protocol** | Reader/writer dùng epoch + `std::atomic_thread_fence(acquire)` đảm bảo đọc ghi không bị race condition, tương thích cả x86 và ARM |
| **Magic number validation** | SharedState kiểm tra magic `0x4E4B4559` ('NKEY'), version và size trước khi sử dụng |
| **Volatile pointers** | Memory-mapped view dùng `volatile` ngăn compiler cache giá trị cũ |

## 3. Config Loading

| Biện pháp | Mô tả |
|-----------|-------|
| **File size limit** | Config > 1 MB → bỏ qua, dùng default. Chống OOM từ config.toml khổng lồ |
| **Macro bounds** | Key tối đa 32 ký tự, value tối đa 512 ký tự. Entry vượt giới hạn bị skip |
| **Per-app limit** | Tối đa 256 per-app entries. Vượt quá → truncate |
| **Enum range check** | Validate CodeTable (0–4), InputMethod (0–2) trước khi cast. Giá trị lạ → fallback default |

## 4. DLL & TSF Security

| Biện pháp | Mô tả |
|-----------|-------|
| **HKCU CLSID cleanup** | Xóa `HKCU\Software\Classes\CLSID\{GUID}` mỗi lần khởi động + đăng ký DLL. Chặn DLL hijack qua HKCU override — kẻ tấn công không thể chèn DLL vào Word, Chrome qua registry |
| **Safe DLL loading** | `LoadLibraryW()` dùng full path, không dựa vào search path |
| **Ghim mã `VKeyTSF.dll`** | Ứng dụng nhúng SHA-256 của DLL được build cùng bản đó và kiểm tra trước khi tự `LoadLibrary`, đăng ký, bỏ đăng ký, kích hoạt TSF hoặc áp dụng DLL cập nhật đang chờ. Bản Classic chấp nhận DLL được Foundation ký sau khi build khi chữ ký và phiên bản khớp. DLL vẫn ở trong ZIP của từng bản; Classic và Sciter dùng DLL riêng từ build riêng |
| **Trusted Advanced engine** | Khi người dùng bật Advanced, VKey chỉ tải asset `vkey_engine.dll` từ tag release trùng phiên bản đang chạy. WinHTTP tắt redirect tự động, giới hạn redirect ở HTTPS GitHub/CDN đã duyệt, giới hạn đúng byte length đã compile, ghi vào staging cùng thư mục và chỉ atomic-rename sau khi SHA-256 khớp `engine.lock`. Loader chỉ nạp file cạnh module, bỏ qua env/PATH/CWD, giữ handle chống write/delete qua bước `LoadLibraryExW`, rồi đối chiếu file identity và ABI/runtime status. Sai bất kỳ bước nào thì Advanced không được bật hoặc engine C++ được dùng làm fallback |
| **DisableThreadLibraryCalls** | Tắt thông báo DLL_THREAD_ATTACH/DETACH không cần thiết |

## 5. Memory Safety

| Biện pháp | Mô tả |
|-----------|-------|
| **SecureZeroMemory** | Xóa `inputHistory_` và `rawMacroBuffer_` khi chuyển focus app. Chống đọc lịch sử phím từ memory dump |
| **Smart pointers** | `unique_ptr` cho handle management, `CComPtr` cho COM — tự động cleanup, không leak |
| **No unsafe functions** | Không dùng `wcscpy`, `strcpy`, `sprintf`, `gets`. Chỉ dùng `std::wstring`, `StringCchPrintfW`, `swprintf_s` |
| **Zero-initialization** | SharedState `InitDefaults()` khởi tạo tường minh tất cả fields |

## 6. Process Security

| Biện pháp | Mô tả |
|-----------|-------|
| **Single instance mutex** | Mutex với restricted DACL chống chạy trùng instance |
| **UAC elevation** | Đăng ký TSF yêu cầu admin với consent dialog. Không tự nâng quyền ngầm |
| **Subprocess isolation** | Settings/Macro dialog chạy process riêng biệt |

## 7. Compiler & Linker Hardening

| Flag | Mục đích |
|------|----------|
| `/W4 /WX` | Warning level cao nhất + coi warning là error |
| `/permissive-` | C++ standards-conforming nghiêm ngặt |
| `/GS` | Stack buffer overrun detection (mặc định MSVC) |
| `/DYNAMICBASE` | ASLR — random hóa địa chỉ load |
| `/NXCOMPAT` | DEP — ngăn thực thi code trên stack/heap |
| `/HIGHENTROPYVA` | High-entropy ASLR cho 64-bit |

## 8. Hook Engine

| Biện pháp | Mô tả |
|-----------|-------|
| **Magic marker** | SendInput event gắn `VKEY_EXTRA_INFO = 0x4E4B` — ngăn hook xử lý lại phím do chính VKey tạo ra |
| **Focus isolation** | Chuyển app → xóa buffer, commit/discard composition. Không leak keystroke qua ranh giới ứng dụng |
| **Bounded buffers** | Input buffer có giới hạn, macro expansion bounded bởi config limits |

## 9. Extensions & Companion Binaries (Tiện ích mở rộng)

| Biện pháp | Mô tả |
|-----------|-------|
| **Tách rời khỏi các gói Release** | Hai tiện ích không nằm trong ZIP tiêu chuẩn hoặc Classic. Bản tiêu chuẩn phát hành `VKeyWatchdog.exe` và `VKeyBrowserHost.exe`; Classic phát hành `VKeyClassicWatchdog.exe` và `VKeyClassicBrowserHost.exe`. Khi bật tiện ích chưa có, VKey hỏi trước khi tải đúng asset của bản đang chạy, rồi lưu cạnh ứng dụng dưới tên `VKeyWatchdog.exe` hoặc `VKeyBrowserHost.exe` |
| **Mã gắn với phiên bản VKey** | Trong lúc build, SHA-256 của từng file tiện ích được nhúng vào ứng dụng chính. Bản Classic còn nhúng mã của Browser Host đi kèm và các asset bản tiêu chuẩn cùng phiên bản. VKey so khớp toàn bộ byte trước khi cài hoặc chạy: tác vụ Watchdog và native-messaging manifest mới đều gọi VKey để kiểm tra ngay trước khi mở tiện ích. File từ bản build khác hoặc bị sửa sẽ không khớp; chỉ so số phiên bản trong PE là không đủ vì giá trị đó có thể bị giả mạo |
| **Không dùng file `.sig` rời** | Hai tiện ích bản tiêu chuẩn không cần chứng chỉ Authenticode hoặc file `.sig` bên cạnh: ứng dụng chính giữ mã xác thực của chúng. Riêng watchdog trong gói Classic đã được SignPath ký sau khi build, nên VKey cho phép chữ ký Foundation hợp lệ khi phiên bản PE khớp chính xác phiên bản ứng dụng |
| **Tải về an toàn & Thay thế nguyên tử** | Trình cài đặt tiện ích tải binary từ GitHub Releases qua HTTPS, lưu tạm dưới dạng file `.tmp`, xác thực mã đã nhúng hoặc watchdog Classic đã ký trước khi di chuyển/thay thế nguyên tử (atomic rename/replace) vào thư mục VKey |
| **Phát triển & Debug cục bộ** | Trong chế độ Debug hoặc khi bật tùy chọn biên dịch `VKEY_ALLOW_UNSIGNED_EXTENSIONS`, cổng kiểm tra cho phép bỏ qua xác thực tiện ích và TSF DLL để lập trình viên thử nghiệm binary tự biên dịch cục bộ |

Mã SHA-256 trong VKey bảo vệ các tiện ích khi bản ứng dụng chính được tin cậy; nó
không xác thực chính `VKey.exe` nếu kẻ tấn công thay được cả ứng dụng và tiện ích.
Manifest native-messaging mới trỏ tới VKey, để VKey xác thực Browser Host khi
trình duyệt mở kết nối. Manifest cũ trỏ trực tiếp tới Browser Host cho đến khi
VKey chạy và đăng ký lại. Tác vụ Watchdog được tạo bởi bản cũ vẫn có thể trỏ
thẳng tới file Watchdog; với bản nâng cấp từ cơ chế cũ, hãy tắt rồi bật lại
Watchdog để tạo tác vụ mới đi qua VKey.

Windows nạp `VKeyTSF.dll` trực tiếp qua đăng ký COM trong ứng dụng đích. Kiểm tra
SHA-256 trong `VKey.exe`/`VKeyClassic.exe` chỉ bảo vệ những đường nạp và kích hoạt
do VKey điều khiển; nó không thể chạy trước mọi lần Windows nạp DLL. Bản Classic
có thêm chữ ký Authenticode của Foundation. Với bản tiêu chuẩn chưa ký, cần bảo vệ
thư mục cài đặt hoặc dùng chính sách kiểm soát mã của Windows nếu cần bảo đảm ở
cấp hệ điều hành.

---

## Testing

Các test bảo mật update và Advanced engine bao phủ:
- PowerShell escaping (6 tests)
- URL whitelist validation (14 tests)
- SHA-256 hash parsing (7 tests)
- Edge cases: empty string, invalid hex, oversized hash, case normalization
- Exact-tag engine URL và redirect-origin policy
- Same-name engine tampering và C++ fallback
- Windows storage/activation: tạo staging `CREATE_NEW`, thay nguyên tử một DLL giả, xác minh lại hash/size, và bảo đảm file cuối không có `FILE_ATTRIBUTE_TEMPORARY`

Trust path dùng CNG, `LoadLibraryExW` và `FILE_RENAME_INFO` là Windows-only. Windows CI chạy regression test storage/activation và tamper gate; một suite Linux xanh không phải bằng chứng cho các nhánh này.

`vkey_engine.dll` hiện được phát hành nguyên trạng như asset riêng và không đi qua SignPath. SHA-256 pin đã compile ngăn VKey thực thi một DLL bị thay thế, nhưng không tạo Authenticode reputation. Workflow v4.3 giữ engine ngoài cả hai ZIP; bản Classic còn được build trong cây Rust-OFF riêng và có gate từ chối mọi engine artifact. Windows/AV release smoke còn là release gate, không phải thuộc tính đã được chứng minh bởi unit test.

---

*Chi tiết kỹ thuật đầy đủ: xem `docs/SECURITY_FIXES-distillate.md`*
