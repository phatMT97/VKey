# VKey v4.3.2

Sửa lỗi tương thích bộ gõ tiếng Việt trong các ứng dụng WebView2 / Tauri (như Mooring, terminal nhúng) và bổ sung hồ sơ gửi phím chuyên dụng cho WebView App.

### Điểm mới và sửa lỗi trong v4.3.2

#### Tương thích WebView2 & Terminal nhúng (Mooring)

- **Khắc phục lỗi nuốt ký tự:** Sửa lỗi khi gõ tiếng Việt có dấu (ví dụ gõ `nhâ` bị mất chữ `h` thành `nâ`) trong tab PowerShell của Mooring (ứng dụng Tauri/WebView2) khi chọn kiểu gửi "Tương thích WebView App" cho ứng dụng đó. Các ứng dụng WebView2 khác giữ nguyên cách gửi mặc định.
- **Thêm kiểu gửi "Tương thích WebView App" (`send_method = 6`):**
  - Bổ sung hồ sơ cấu hình mới trong danh sách **Cách gửi** (Cấu hình từng ứng dụng / App Overrides) dành riêng cho ứng dụng WebView2 / Tauri. Nếu bị mất chữ khi gõ, có thể thử cấu hình này

#### Cập nhật giao diện & sửa lỗi hiển thị

- Sửa lỗi bảng điều khiển bị khoá kích thước và không scroll được ([#259](https://github.com/phatMT97/VKey/issues/259))
- Sửa lỗi hiển thị khi mở lại từ cửa sổ thu nhỏ ([#258](https://github.com/phatMT97/VKey/issues/258))
- Hỗ trợ thu nhỏ (minimize) cửa sổ cài đặt và các hộp thoại khi click vào biểu tượng ứng dụng trên thanh tác vụ (Taskbar).

#### Sửa lỗi không tự khởi động khi bật "Chạy với quyền Administrator"

- Sửa lỗi VKey không tự khởi động cùng Windows khi bật "Chạy với quyền Administrator" trên tài khoản Microsoft, tài khoản domain hoặc khi đăng nhập bằng PIN / Windows Hello ([#261](https://github.com/phatMT97/VKey/issues/261)). Tác vụ khởi động giờ áp dụng cho mọi tài khoản thuộc nhóm Administrators trên máy; watchdog áp dụng cho mọi tài khoản thuộc nhóm Users. Cần tắt rồi bật lại tuỳ chọn để tác vụ cũ được đăng ký lại.

### Kết quả VirusTotal (v4.3.2)

Các gói đang được dựng; kết quả quét VirusTotal theo đúng SHA-256 sẽ được cập nhật sau khi hoàn tất bản phát hành trên GitHub Releases. Xem [hướng dẫn kiểm tra bản tải](https://github.com/phatMT97/VKey/blob/Main/docs/ANTIVIRUS.md).

---

*Cảm ơn bạn đã tin tưởng và lựa chọn VKey! Mọi đóng góp của bạn đều là nguồn động lực lớn giúp bộ gõ ngày càng hoàn thiện hơn.* ❤️
