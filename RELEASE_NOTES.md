# VKey v4.4.0

Bổ sung khôi phục gõ tắt bằng Backspace, cải thiện viết hoa macro và giao diện, khắc phục lỗi Ctrl xoá nhầm từ/dòng và lỗi tương thích WebView2 / Tauri (như Mooring, terminal nhúng).

### Điểm mới và sửa lỗi trong v4.4.0

#### Gõ tắt và viết hoa ([#264](https://github.com/phatMT97/VKey/issues/264), [#265](https://github.com/phatMT97/VKey/issues/265))

- **Khôi phục từ gõ tắt khi nhấn Backspace ([#264](https://github.com/phatMT97/VKey/issues/264)):** Sau khi bung nội dung đầy đủ (ví dụ gõ `bbh` + Space → `bún bò Huế `), nếu nhấn Backspace ngay sau đó thì VKey sẽ xoá toàn bộ cụm từ đã bung và khôi phục lại từ gõ tắt ban đầu (`bbh`) thay vì phải xoá lùi từng ký tự. Bật/tắt tuỳ chọn **Khôi phục gõ tắt bằng Backspace** trong tab **Gõ tắt**

- Sửa lỗi không thể viết hoa toàn bộ khi nội dung macro đã có chữ hoa: `bbh = bún bò Huế`, gõ `BBH` nay ra `BÚN BÒ HUẾ`.
- Khi bật **Tự động viết hoa theo phím tắt**, `Bbh` mặc định ra `Bún bò Huế`. Nếu muốn hành vi cũ `Bún Bò Huế`, bật tùy chọn phụ **Viết hoa đầu từng từ** trong tab **Gõ tắt**. Tùy chọn mới mặc định tắt, kể cả với cấu hình cũ; bảng macro đã lưu không thay đổi.
- Hoa đầu câu do VKey tự tạo chỉ nâng chữ đầu cụm, kể cả khi tắt viết hoa theo phím tắt. Macro có key hoa/thường riêng vẫn ưu tiên nội dung đã lưu. Xem [hướng dẫn ngắn](docs/MACRO_CASING.md).

#### Tương thích WebView2 & Terminal nhúng (Mooring)

- **Khắc phục lỗi nuốt ký tự:** Sửa lỗi khi gõ tiếng Việt có dấu (ví dụ gõ `nhâ` bị mất chữ `h` thành `nâ`) trong tab PowerShell của Mooring (ứng dụng Tauri/WebView2) khi chọn kiểu gửi "Tương thích WebView App" cho ứng dụng đó. Các ứng dụng WebView2 khác giữ nguyên cách gửi mặc định.
- **Thêm kiểu gửi "Tương thích WebView App" (`send_method = 6`):**
  - Bổ sung hồ sơ cấu hình mới trong danh sách **Cách gửi** (Cấu hình từng ứng dụng / App Overrides) dành riêng cho ứng dụng WebView2 / Tauri. Nếu bị mất chữ khi gõ, có thể thử cấu hình này

#### Cập nhật giao diện & sửa lỗi hiển thị ([#266](https://github.com/phatMT97/VKey/issues/266))

- **Sửa lỗi bố cục khi mở Cài đặt:** Khắc phục tình trạng nội dung bị dồn vào góc trái, khung cửa sổ thừa vùng nền mờ và tiêu đề còn hiển thị phiên bản mặc định. Giao diện được khởi tạo đầy đủ và căn kích thước trước khi hiển thị.
- **Đồng bộ chế độ V/E khi mở Cài đặt:** Sửa lỗi nút gạt E-V luôn bị chuyển sang tiếng Anh (E) khi double-click vào biểu tượng khay hệ thống (tray icon) để mở bảng điều khiển.
- **Tối ưu hiển thị giao diện sáng (Light Mode):** Đổi màu nền sang tone neutral dịu mắt, tăng độ tương phản của viền cửa sổ trên Windows 10, giúp giao diện không bị chìm hoặc lẫn vào nền trắng của các ứng dụng như Chrome, Word, Notepad.
- **Cải thiện độ tương phản chữ trên nền mờ:** Đổi màu chữ tiêu đề các cửa sổ và tiêu đề nhóm từ màu xanh sang màu chữ tiêu chuẩn để tăng độ rõ nét và dễ đọc khi sử dụng hiệu ứng nền mờ (translucent background).
- **Khắc phục lỗi cửa sổ phụ luôn nổi (Always on Top):** Gán quyền sở hữu cửa sổ chuẩn Win32 để các bảng con (Bảng gõ tắt, Từ điển cá nhân, v.v.) luôn nằm trên bảng Cài đặt nhưng không còn đè lên các ứng dụng khác của hệ điều hành; chỉ giữ chế độ luôn nổi khi bảng Cài đặt đang được ghim (Pin).
- **Chống mở lặp nhiều cửa sổ phụ:** Sửa lỗi tiêu đề cửa sổ Từ điển & Loại trừ chính tả khiến hệ thống không nhận diện được cửa sổ đang mở, đồng thời bổ sung cơ chế chống click đúp dồn dập khi mở các cửa sổ phụ.
- **Đồng bộ trạng thái ghim khi mở lại Cài đặt:** Giữ trạng thái luôn nổi đã lưu khi mở lại cửa sổ, tránh icon ghim màu vàng trong khi cửa sổ thực tế chưa được ghim. Khi tải lại cấu hình, cả trạng thái bật và tắt đều được áp dụng cho cửa sổ và icon.

- Sửa lỗi bảng điều khiển bị khoá kích thước và không scroll được ([#259](https://github.com/phatMT97/VKey/issues/259))
- Sửa lỗi hiển thị khi mở lại từ cửa sổ thu nhỏ ([#258](https://github.com/phatMT97/VKey/issues/258))
- Hỗ trợ thu nhỏ (minimize) cửa sổ cài đặt và các hộp thoại khi click vào biểu tượng ứng dụng trên thanh tác vụ (Taskbar).

#### Sửa lỗi không tự khởi động khi bật "Chạy với quyền Administrator"

- Sửa lỗi VKey không tự khởi động cùng Windows khi bật "Chạy với quyền Administrator" trên tài khoản Microsoft, tài khoản domain hoặc khi đăng nhập bằng PIN / Windows Hello ([#261](https://github.com/phatMT97/VKey/issues/261)). Tác vụ khởi động giờ áp dụng cho mọi tài khoản thuộc nhóm Administrators trên máy; watchdog áp dụng cho mọi tài khoản thuộc nhóm Users. Cần tắt rồi bật lại tuỳ chọn để tác vụ cũ được đăng ký lại.

#### Sửa lỗi tổ hợp phím và chuyển chế độ ([#267](https://github.com/phatMT97/VKey/issues/267))

- Khắc phục Backspace giả lập bị dính Ctrl/Alt/Win, gây xoá nhầm từ/dòng khi chuyển chế độ gõ. Các bộ gửi phím nhả modifier quanh thao tác thay thế và khôi phục khi gửi thiếu sự kiện.
- Chuyển chế độ gõ hoặc tạm tắt bộ gõ giữ nguyên chữ đang hiển thị, không tự sửa theo từ điển gợi ý chính tả.

### Kết quả VirusTotal (v4.4.0)

Các gói đang được dựng; kết quả quét VirusTotal theo đúng SHA-256 sẽ được cập nhật sau khi hoàn tất bản phát hành trên GitHub Releases. Xem [hướng dẫn kiểm tra bản tải](https://github.com/phatMT97/VKey/blob/Main/docs/ANTIVIRUS.md).

---

*Cảm ơn bạn đã tin tưởng và lựa chọn VKey! Mọi đóng góp của bạn đều là nguồn động lực lớn giúp bộ gõ ngày càng hoàn thiện hơn.* ❤️
