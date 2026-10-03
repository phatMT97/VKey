# VKey Browser extension

> [!WARNING]
> **Experimental feature:** the extension is not yet distributed through the
> Chrome Web Store or Firefox AMO. Enable **Hỗ trợ trình duyệt** in VKey's
> **Tiện ích mở rộng** menu; VKey can download the matching `VKeyBrowserHost.exe`.

VKey can restore a per-domain V/E mode for the current browser session and
accept persistent hard routes from the companion
[VKey Browser](https://github.com/phatMT97/VKey-Browser) extension.

The extension offers three routes:

- **Remember V/E from hotkey:** after an accepted V/E toggle, the native host
  returns only the resulting mode and active hostname. The extension remembers
  that pair for the current browser session and restores it on tab focus.
- **English (hard):** force English for the configured domain and block V/E
  toggles while the rule is active.
- **TSF compatibility (hard):** route the configured domain through VKey TSF.
  This is intended for
  browser editors that duplicate or attach replacement text after emoji, such
  as the XenForo cases reported in issue #92. VKey's TSF application support
  must be enabled; otherwise this route safely falls back to the normal hook.

Learned V/E state lives in extension `storage.session` and is cleared when the
browser closes/restarts. Hard rules live in extension `storage.local` and remain
until the user removes them. VKey does not write learned browser domains into
its TOML config. While browser context routing is active, the same hotkey is not
also persisted as a browser-wide Smart Switch entry such as `chrome.exe`; when
the extension is paused, normal app-level Smart Switch behavior resumes.

## Yêu cầu

- Windows 10/11 và bản VKey có mục **Hỗ trợ trình duyệt** trong menu
  **Tiện ích mở rộng**. VKey sẽ hỏi tải `VKeyBrowserHost.exe` nếu chưa có.
- Repo [VKey-Browser](https://github.com/phatMT97/VKey-Browser) đã được tải và
  giải nén; thư mục được chọn khi cài phải chứa `manifest.json`.
- Muốn dùng route **TSF tương thích**, bật hỗ trợ ứng dụng TSF trong VKey.

Khi bật Hỗ trợ trình duyệt, VKey tạo native-messaging manifest theo user tại
`%APPDATA%\VKey\native-messaging` và đăng ký cho Chrome, Edge, Brave, Vivaldi,
Opera và Firefox sau khi xác thực file. VKey đăng ký lại khi khởi động nếu tùy
chọn đang bật và file vẫn hợp lệ. Manifest trỏ tới ứng dụng VKey; mỗi lần trình
duyệt kết nối, VKey kiểm tra `VKeyBrowserHost.exe` rồi chuyển tiếp kết nối tới
file đã xác thực.

## Cài trên Chrome, Edge, Brave, Vivaldi hoặc Opera

1. Bật **Hỗ trợ trình duyệt** trong menu **Tiện ích mở rộng** của VKey; chấp
   nhận tải file nếu được hỏi.
2. Mở trang quản lý extension:
   - Chrome: `chrome://extensions`
   - Edge: `edge://extensions`
   - Brave: `brave://extensions`
   - Vivaldi: `vivaldi://extensions`
   - Opera: `opera://extensions`
3. Bật **Developer mode**.
4. Chọn **Load unpacked** và chọn thư mục VKey-Browser có `manifest.json`.
5. Ghim VKey Browser lên toolbar để đổi route của trang hiện tại.

## Cài tạm trên Firefox

1. Chạy VKey một lần như bước 1 phía trên.
2. Trong thư mục VKey-Browser, chạy `npm run build:firefox` để tạo manifest
   dùng `background.scripts` riêng cho Firefox.
3. Mở `about:debugging#/runtime/this-firefox`.
4. Chọn **Load Temporary Add-on** rồi chọn `dist/firefox/manifest.json`.

Không chọn `manifest.json` ở thư mục gốc cho Firefox; file đó là gói Chromium
Manifest V3 và chỉ khai báo `background.service_worker`.

Firefox sẽ gỡ temporary add-on khi khởi động lại; cần load lại cho đến khi có
bản XPI được ký qua AMO.

## Sử dụng

Extension có ba lựa chọn:

- **Tự nhớ V/E theo hotkey:** hostname chưa biết kế thừa trạng thái hiện tại.
  Sau khi người dùng đổi V/E, extension nhớ kết quả trong phiên và khôi phục khi
  quay lại hostname đó.
- **Luôn gõ English (hard):** khóa E và chặn hotkey đổi V/E trên hostname.
- **TSF tương thích (hard):** cố định TSF cho editor/forum bị lỗi với hook; V/E
  vẫn được nhớ theo hotkey độc lập với lựa chọn engine.

Công tắc **Bật điều hướng theo website** được bật mặc định và có ở cả popup lẫn
trang quản lý tên miền. Khi tắt, extension giữ nguyên cả session mode lẫn hard
rule nhưng xóa context hiệu lực khỏi NexusKey; bật lại sẽ áp dụng chúng ngay.

Rule được áp dụng cho hostname và các subdomain của nó. Extension theo dõi đổi
tab, navigation và focus cửa sổ, nên không cần mở popup lại sau mỗi lần chuyển.

Ví dụ:

1. Trên `google.com`, nhấn hotkey để chuyển sang V.
2. Trên `github.com`, nhấn hotkey để chuyển sang E.
3. Chuyển tab Google → GitHub → Google: VKey tự đổi V → E → V trong phiên.
4. Đặt `facebook.com` thành **Luôn gõ English (hard)** nếu muốn hostname đó luôn ở E
   và hotkey không thay đổi rule.

## Xử lý lỗi kết nối

Nếu popup lưu rule nhưng VKey không đổi chế độ:

1. Xem trạng thái trong popup. **Chưa kết nối VKeyBrowserHost** nghĩa là trình
   duyệt chưa mở được native host.
2. Kiểm tra `VKeyBrowserHost.exe` nằm cạnh đúng file VKey đang chạy. Các bản
   VKey cũ không kèm file này sẽ không dùng được extension.
3. Thoát hoàn toàn VKey rồi mở lại để tạo manifest trong
   `%APPDATA%\VKey\native-messaging` và đăng ký registry theo user.
4. Khởi động lại browser và reload extension.
5. Với TSF, xác nhận hỗ trợ ứng dụng TSF đang bật trong VKey; nếu chưa bật,
   route TSF sẽ an toàn quay về hook bình thường.

## Privacy and failure behavior

Only these values cross the extension/native boundary: protocol version,
browser executable name, focused state, hard route, session V/E mode, and
hostname. After an accepted toggle, VKey returns the resulting V/E mode and the
hostname captured with that toggle; it never returns the pressed key. Full URLs,
paths, query strings, page contents, and keystrokes are neither requested nor
transmitted. The native host rejects messages over 8 KiB, unknown fields,
unsupported executable names, and non-hostname characters.

The host publishes a fixed-size versioned seqlock mapping. HookEngine normally
checks one 32-bit generation value per key; JSON parsing and browser APIs stay
outside the keyboard hook. State expires after five seconds if a browser or
extension crashes. Blur/EOF clears only state owned by that native connection,
so an unfocused browser's heartbeat cannot erase the currently focused
browser's route/mode. An app-level hard lock still has higher precedence than a
domain preference.

## Native messaging protocol

Protocol 2 separates the persistent hard `route` from the session `mode`:

```json
{"protocol":2,"browser":"chrome.exe","hostname":"example.com","route":"default","mode":"vietnamese","focused":true}
```

`route` accepts `default`, `english`, or `tsf`. `mode` accepts `default`,
`vietnamese`, or `english`. A hard-English route wins over `mode`.

An accepted V/E toggle is returned to the owning browser connection on its next
context heartbeat:

```json
{"ok":true,"protocol":2,"event":"mode-changed","hostname":"example.com","mode":"english"}
```

The event stores the hostname captured at toggle time, so a quick tab switch
cannot attribute it to the next tab. Protocol 1 remains accepted for older
extensions and keeps the original route-only behavior.

## Address-bar limitation (#100)

Browser extension APIs do not expose text typed in the normal address bar.
Consequently the extension can apply a hostname route after navigation or tab
focus, but cannot detect `google.com` while the user is still typing it into the
omnibox. Implementations based on UI Automation were deliberately rejected:
they are browser-specific, fragile, and too expensive to place near the input
hook.

## TSF visual behavior

VKey registers its `ITfDisplayAttributeProvider` and applies its
`TF_LS_NONE` GUID atom to every live composition range. Supported hosts should
therefore not draw the usual composition underline/highlight. A host that
ignores TSF display attributes may still impose its own visual treatment.
