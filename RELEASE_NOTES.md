# VKey v4.4.0 🚀

Bản cập nhật tập trung cải thiện **gõ tắt, tương thích ứng dụng và độ ổn định**.

### ✨ Tính năng mới
- **Khôi phục gõ tắt bằng Backspace:** Gõ `bbh` → `bún bò Huế`, nhấn Backspace để trở lại `bbh`. Cho phép thay đổi từ gõ tắt sau khi Backspace. ([#264](https://github.com/phatMT97/VKey/issues/264))
- **Cải thiện viết hoa macro:** `BBH` → `BÚN BÒ HUẾ`, `Bbh` → `Bún bò Huế`. Có thêm tùy chọn viết hoa đầu từng từ. ([#265](https://github.com/phatMT97/VKey/issues/265))
- **Tương thích WebView2 / Tauri:** Thêm cách gửi phím mới, khắc phục lỗi mất ký tự tiếng Việt trong Mooring và terminal nhúng.

### 🛠️ Sửa lỗi và cải thiện
- Khắc phục lỗi **Ctrl/Alt/Win kết hợp Backspace** gây xoá nhầm từ hoặc dòng. ([#267](https://github.com/phatMT97/VKey/issues/267))
- Sửa lỗi tự khởi động cùng Windows khi chạy bằng quyền Administrator. ([#261](https://github.com/phatMT97/VKey/issues/261))
- Cải thiện giao diện sáng, bố cục, cuộn trang và trạng thái E/V.
- Sửa lỗi cửa sổ phụ luôn nổi, mở trùng và đồng bộ trạng thái ghim.
- Giữ nguyên nội dung đang gõ khi chuyển chế độ hoặc tạm tắt bộ gõ.
- Cải thiện màu sắc và độ tương phản UI giúp tăng khả năng hiển thị.
- Sửa lỗi hướng dẫn tải thủ công engine chưa đầy đủ.
- **Cải thiện engine gõ tiếng Việt nâng cao:**
    - Sửa các lỗi gõ tiếng Việt.
    - Giảm size của file dll.
    - Tăng tốc độ nhận diện từ ngữ.
    - Thêm khả năng nhận diện theo ngữ cảnh **(hiện tại vẫn chỉ hạn chế một số từ)**, ví dụ nếu gõ riêng d-d-i-e-u (đieu) -> điều, nếu trước đó có từ "nói" -> nói điêu (không có dấu huyền).

### 🛡️ VirusTotal
Kết quả quét SHA-256 sẽ được cập nhật sau khi hoàn tất phát hành.

---

*Cảm ơn bạn đã tin tưởng và lựa chọn VKey! Mọi đóng góp của bạn đều là nguồn động lực lớn giúp bộ gõ ngày càng hoàn thiện hơn.* ❤️
