# 📖 Hướng dẫn sử dụng VKey

> [!TIP]
> **💡 Quick Tips:** Bạn có thể copy toàn bộ nội dung trang này và gửi cho [ChatGPT](https://chatgpt.com) / [Gemini](https://gemini.google.com) kèm câu hỏi của bạn để được trả lời nhanh về cách sử dụng VKey.

---

## 📌 Mục lục
1. [❓ Câu hỏi thường gặp (FAQ)](#faq)
2. [⚙️ Các chức năng chính](#features)
   - [⌨️ 1. Kiểu gõ](#typing-method)
   - [📱 2. Cấu hình từng ứng dụng](#app-config)
   - [🔄 3. Lưu chế độ gõ theo app](#remember-mode)
   - [🚫 4. Khoá chế độ theo ứng dụng (E / V)](#exclude-app)
   - [🌏 5. Tự tắt khi bàn phím CJK](#cjk-layout)
   - [🔠 6. Viết hoa chữ cái đầu câu](#auto-capitalize)
   - [✍️ 7. Gõ tự do](#free-typing)
   - [⬅️ 8. BS giữ chữ khi có gợi ý](#backspace-suggest)
   - [📝 9. Kiểm tra chính tả](#spellcheck)
   - [⚡ 10. Quản lý phím tắt](#hotkey-mgmt)
   - [🔗 11. Sử dụng TSF](#tsf-mode)
   - [🚀 12. Gõ tắt (Macro)](#macro)
   - [🔁 13. Tự khởi động lại (Watchdog)](#watchdog)
   - [🔄 14. Công cụ chuyển mã nhanh](#convert-tool)
3. [🛡️ Cảnh báo bảo mật & Debug Log](#security-warning)

---

## <span id="faq">❓ Câu hỏi thường gặp (FAQ)</span>

### <span id="eset-browser">🛡️ Q. VKey gõ được trong Notepad/Word nhưng không gõ được tiếng Việt trên Edge/Chrome khi có ESET?</span>
**A.** Hãy kiểm tra tiện ích và tính năng bảo vệ trình duyệt của ESET. Trong [issue #229](https://github.com/phatMT97/VKey/issues/229), người dùng đã khắc phục bằng cách thêm VKey vào **Browser protection allowlist** rồi khởi động lại trình duyệt.

1. Mở `edge://extensions` (Edge), `chrome://extensions` (Chrome) hoặc `about:addons` → **Extensions** (Firefox); tìm tiện ích có tên **ESET**, chẳng hạn **ESET Browser Privacy & Security**. Dù không thấy tiện ích, vẫn kiểm tra **Browser protection / Safe Banking & Browsing** trong ứng dụng ESET trên máy.
2. Mở ESET → **Advanced setup** (`F5`) → **Protections → Browser protection → Browser protection allowlist → Edit → Add**. Thêm đúng đường dẫn `VKey.exe` đang chạy, hoặc `VKeyClassic.exe` nếu dùng Classic. Nếu dùng [TSF](#tsf-mode), thêm cả `VKeyTSF.dll`; chỉ thêm DLL này chưa đủ cho chế độ Hook mặc định.
3. Lưu bằng **OK / Apply**, thoát hoàn toàn trình duyệt rồi mở lại và thử gõ tiếng Việt. Cửa sổ ẩn danh hoặc chạy VKey bằng quyền Administrator có thể vẫn bị ESET chặn.

👉 Xem [hướng dẫn ESET chi tiết, cách tìm đường dẫn file và xử lý nếu vẫn lỗi](ANTIVIRUS.md#eset-browser).

### 🎮 Q. Tôi có thể vừa gõ tiếng Việt vừa chơi game mà không sợ bị dính phím không?
**A.** Được, nhưng cần bật **Chế độ game** cho game đó trước.

Bình thường khi bạn gõ một phím tạo dấu (ví dụ `w` trong `tuw` → `tư`), VKey phải nuốt phím đó để thay bằng chữ có dấu — nên game không nhìn thấy cú nhấn `w` và nhân vật không di chuyển. Chế độ game gửi lại phím gốc cho game ngay trong cùng lượt, nên `W` `A` `S` `D` vẫn chạy bình thường trong khi bạn vẫn gõ được tiếng Việt.

Có hai cách bật, chọn cách nào cũng được vì cả hai ghi vào **cùng một danh sách**:

1. **Nhấn phím tắt ngay trong game** (tiện nhất, không cần thoát ra): vào [⚡ Quản lý phím tắt](#hotkey-mgmt) gán một tổ hợp cho `Bật / tắt chế độ game cho app đang mở`. Sau đó cứ ở trong game nhấn tổ hợp đó — game hiện tại được thêm vào danh sách. Nhấn lần nữa để gỡ ra.
2. **Chọn thủ công**: vào [📱 Cấu hình từng ứng dụng](#app-config), thêm file `.exe` của game và đặt mục "Cách gửi" thành `Chế độ game`.

Bạn xem lại và xoá bất cứ lúc nào trong bảng Cấu hình từng ứng dụng.

> ⚠️ Phím tắt này **không có tổ hợp mặc định** — game nào cũng đã dùng gần hết bàn phím, nên VKey để bạn tự chọn tổ hợp không đụng hàng.
>
> ⚠️ Nếu game chạy bằng quyền Administrator (thường gặp ở game có anti-cheat), Windows chặn VKey đọc phím của game — cả phím tắt lẫn việc gõ tiếng Việt trong game đều không hoạt động. Trường hợp này cần chạy VKey bằng quyền Administrator.
>
> 💡 Nếu bạn không gõ tiếng Việt trong game, đơn giản hơn là cứ chuyển sang chế độ `E` trước khi chơi.

👉 Chi tiết xem tại phần [⌨️ Kiểu gõ](#typing-method).

### 📱 Q. Tôi muốn tùy chỉnh mỗi app một kiểu gõ và bảng mã khác nhau có được không?
**A.** Được chứ! VKey cho phép thiết lập profile riêng (bao gồm kiểu gõ và bảng mã) cho từng phần mềm. Khi bạn chuyển cửa sổ làm việc, VKey sẽ tự động nhận diện và áp dụng cấu hình tương ứng mà không cần bạn phải thao tác phím tắt thủ công. 👉 Chi tiết xem tại phần [📱 Cấu hình từng ứng dụng](#app-config).

### 🚀 Q. Tôi muốn tạo các từ gõ tắt (macro) dài, phức tạp và xuống dòng thì sao?
**A.** VKey hỗ trợ bạn tạo các macro cực dài lên tới **20.000 ký tự** và hỗ trợ xuống dòng đầy đủ, giúp bạn dễ dàng chèn nhanh các đoạn văn bản mẫu hoặc biểu mẫu phức tạp. 👉 Chi tiết xem tại phần [🚀 Gõ tắt (Macro)](#macro).

### 🔄 Q. Tôi muốn khi đang gõ tiếng Việt ở app này, chuyển sang app khác thì nó tự động chuyển sang gõ tiếng Anh thì làm thế nào?
**A.** Bạn có hai lựa chọn cực kỳ tiện lợi:
1. Sử dụng tính năng **Lưu chế độ gõ theo app** để VKey tự động nhớ trạng thái `V` (tiếng Việt) hoặc `E` (tiếng Anh) của từng ứng dụng. 👉 Xem [🔄 Lưu chế độ gõ theo app](#remember-mode).
2. Sử dụng tính năng **Khoá chế độ theo ứng dụng** để khoá cứng ứng dụng đó vào chế độ `E` (luôn tiếng Anh) hoặc `V` (luôn tiếng Việt), ngăn chặn hoàn toàn việc vô tình chuyển chế độ bằng phím tắt. 👉 Xem [🚫 Khoá chế độ theo ứng dụng (E / V)](#exclude-app).

### 🌏 Q. Tôi dùng nhiều layout bàn phím khác nhau, VKey có thể tự động chuyển sang E mode khi tôi đổi sang bàn phím ngôn ngữ khác không?
**A.** Có! Khi bạn bật tùy chọn **Tự tắt khi bàn phím CJK**, VKey sẽ tự động phát hiện khi bạn chuyển đổi sang các layout bàn phím Trung - Nhật - Hàn để tắt gõ tiếng Việt. Khi bạn đổi về layout US, VKey sẽ tự động kích hoạt lại chế độ tiếng Việt. 👉 Chi tiết xem tại phần [🌏 Tự tắt khi bàn phím CJK](#cjk-layout).

### 🎨 Q. Tôi muốn tùy chỉnh màu sắc icon ở khay hệ thống (traybar) thì có được không?
**A.** Hoàn toàn được. VKey cho phép bạn chọn các chế độ màu sắc icon: **Nhiều màu (Mặc định)**, **Trắng**, hoặc **Đen** và các tùy biến khác để hiển thị đẹp mắt, hài hòa nhất với theme hệ điều hành của bạn.

### 🖥️ Q. Tôi dùng màn hình rời làm màn hình chính, làm sao biết được VKey đang ở chế độ gõ nào?
**A.** VKey hỗ trợ tính năng **Icon nổi (Floating Icon)** 💬 hiển thị trực quan trạng thái gõ `V`/`E` ngay trên màn hình chính, giúp bạn dễ dàng theo dõi dù đang mở ứng dụng toàn màn hình hoặc dùng nhiều màn hình.

### 💤 Q. Tại sao sau một thời gian dài không sử dụng, khi bắt đầu gõ lại tôi cảm thấy có một chút độ trễ (delay)?
**A.** Để tối ưu và tiết kiệm tài nguyên hệ thống, VKey sẽ tự động đi vào **chế độ ngủ đông sâu (deep hibernation)** nếu không có hoạt động nào trong một thời gian dài. Khi bạn gõ phím trở lại, ứng dụng sẽ cần một khoảng thời gian rất ngắn để kích hoạt lại toàn bộ các dịch vụ và tính năng, điều này có thể gây ra một chút độ trễ (delay) nhỏ đối với một số tính năng trong vài giây đầu tiên. Sau đó, ứng dụng sẽ hoạt động mượt mà bình thường.

### 💾 Q. Tôi mở Task Manager thấy VKey chiếm khoảng 1.6 ~ 2 MB RAM. Mức này có cao không, sao không thấy giảm thêm?
**A.** Không cao đâu — đây đã là mức **cực kỳ nhẹ** và gần **giới hạn kỹ thuật** của một ứng dụng nền trên Windows rồi. Vài điều nên biết để khỏi hiểu nhầm:
- 📊 **Con số trong Task Manager không cố định.** Cột "Bộ nhớ" (*Memory / Working Set*) là phần bộ nhớ **đang hoạt động** — nó tự **lên xuống liên tục** theo lúc bạn dùng máy, chứ không phải một con số đứng yên. Thấy nó nhích lên khi bạn vừa thao tác là hoàn toàn bình thường.
- 💤 **RAM tự giảm khi máy rảnh.** Khi bạn không gõ trong một lúc, VKey vào [chế độ ngủ đông sâu](#faq) và Windows sẽ tự "dọn" bộ nhớ xuống — có thể còn **khoảng 0.3 MB**. Đây mới là lúc con số tụt rõ, và nó **diễn ra tự động**, bạn không cần làm gì cả. Cứ để máy rảnh vài phút rồi xem lại.
- 🧩 **Phần lớn còn lại là code dùng chung của Windows** (đã được chia sẻ với các ứng dụng khác, không phải VKey "ăn" riêng), nên không thể — và không nên — ép nó nhỏ hơn nữa: ép giảm cứng sẽ làm chậm phản hồi phím, đi ngược lại tiêu chí *nhanh & mượt* của bộ gõ.
- ✅ **Tóm lại:** thấy ~1.7 MB lúc đang dùng là **đúng và khỏe mạnh**, không phải lỗi rò rỉ bộ nhớ. Muốn thấy mức "nghỉ" thấp nhất, để máy rảnh vài phút rồi mở lại Task Manager.

### 🔄 Q. Tôi mới cập nhật phiên bản mới của VKey nhưng tính năng "Khởi động cùng Windows" (Auto-start) không hoạt động. Làm cách nào để khắc phục?
**A.** Nếu bạn vừa cập nhật lên phiên bản mới và tính năng tự khởi động không hoạt động, hãy thử **tắt tùy chọn này đi, sau đó bật lại** trong giao diện Settings. Hành động này sẽ cập nhật lại Registry hoặc Task Scheduler với đường dẫn đến file chạy mới.

Nếu vẫn không khắc phục được (ví dụ do file cũ bị kẹt hoặc xung đột quyền Admin), bạn có thể gỡ bỏ hoàn toàn cấu hình khởi động cũ bằng cách thực hiện các lệnh Command Line thủ công theo hướng dẫn chi tiết tại tài liệu: [Gỡ Auto-Start VKey bằng Command Line](uninstall-autostart.md).

---

## <span id="features">⚙️ Các chức năng chính</span>

### <span id="typing-method">⌨️ 1. Kiểu gõ</span>
VKey cung cấp nhiều phương thức gõ linh hoạt để đáp ứng mọi nhu cầu:
- 🔹 **Telex:** Kiểu gõ dấu bằng chữ phổ biến. Phím `w` được dùng làm chữ `ư` hoặc dấu móc cho `ư`, `ơ` và dấu mũ cho `ă`. 🎮 Muốn vừa chat tiếng Việt vừa dùng `W` `A` `S` `D` trong game thì bật thêm [Chế độ game](#app-config) cho game đó — xem [câu hỏi về chơi game](#faq).
- 🔹 **Telex + Vni:** Cho phép bạn gõ đồng thời cả 2 kiểu gõ và thoải mái kết hợp các phím đặt dấu của Telex và VNI trong cùng một từ.
- 🔹 **Simple Telex:** Giữ nguyên giá trị gốc (raw keys) của phím `w` và cặp phím ngoặc vuông `[` `]` thay vì tự động chuyển thành `ư`, `ơ`.
- 🔹 **Tự định nghĩa:** Cho phép bạn tùy ý gán các phím đặt dấu theo thói quen cá nhân 🛠️.

### <span id="app-config">📱 2. Cấu hình từng ứng dụng</span>
- 🔸 Hỗ trợ thiết lập profile độc lập (kiểu gõ, bảng mã) cho từng ứng dụng cụ thể.
- 🔸 Cung cấp tùy chọn chuyển sang **"Dùng Clipboard"** 📋 cho các ứng dụng không tương thích với cơ chế phím Hook. VKey sẽ tự động khôi phục và giữ nguyên nội dung Clipboard nếu đó là văn bản thuần túy (plain text). Tuy nhiên, các dữ liệu phức tạp khác như hình ảnh trong clipboard sẽ bị xóa sạch khi bạn thực hiện gõ/xóa.
- 🔸 Mục **"Cách gửi"** còn có 🎮 **Chế độ game** — gửi lại phím gốc cho ứng dụng để `W` `A` `S` `D` vẫn hoạt động khi bạn gõ tiếng Việt trong game. Chỉ nên bật cho game: với ứng dụng thường nó gây nháy chữ mà không đem lại lợi ích gì. Xem thêm ở [câu hỏi về chơi game](#faq).
- 🔸 Mỗi ứng dụng chỉ chọn được **một** giá trị "Cách gửi" — bật Chế độ game sẽ thay thế lựa chọn Clipboard / Firefox / Cloud-Remote / Thay thế trực tiếp trước đó của ứng dụng đó.

### <span id="remember-mode">🔄 3. Lưu chế độ gõ theo app</span>
- Tự động ghi nhớ trạng thái gõ tiếng Việt (`V`) hoặc tiếng Anh (`E`) cho từng ứng dụng. Khi bạn chuyển đổi qua lại giữa các ứng dụng, VKey sẽ tự động kích hoạt lại trạng thái gõ tương ứng của ứng dụng đó ✨.

### <span id="exclude-app">🚫 4. Khoá chế độ theo ứng dụng (E / V)</span>
- Danh sách các ứng dụng được **khoá cứng** vào một chế độ gõ cố định khi bạn chuyển focus vào chúng. Mỗi ứng dụng trong danh sách chọn được một trong hai chế độ:
  - **`E` — Loại trừ (tiếng Anh):** VKey trở nên hoàn toàn trong suốt, khóa chế độ tiếng Việt và bỏ qua mọi phím tắt chuyển đổi (rất thích hợp cho các tựa game 🎮 hoặc IDE viết code 💻).
  - **`V` — Khoá tiếng Việt:** VKey luôn **ép bật tiếng Việt** khi bạn vào ứng dụng đó, và cũng khóa phím tắt chuyển đổi (rất thích hợp cho ứng dụng chat 💬 luôn gõ tiếng Việt như Zalo, Messenger).
- Trong cả hai chế độ, phím tắt chuyển V/E bị **chặn** khi đang ở trong ứng dụng đã khoá — đúng nghĩa "khoá cứng". Khi rời ứng dụng, chế độ trở lại theo quy tắc bình thường (lưu chế độ theo app, hoặc cờ chung).
- Với bản Sciter, bạn click vào icon `E`/`V` của ứng dụng đã thêm để đổi nhanh chế độ; với bản Classic, nhấp đúp (double click) vào dòng ứng dụng để đổi chế độ. Một ứng dụng chỉ thuộc đúng một chế độ.
- *📌 Lưu ý:* Tính năng này áp dụng cho **hook engine**. Các ứng dụng dùng TSF (một số trình duyệt/Office) không bị ép chế độ V bởi tính năng này.

### <span id="cjk-layout">🌏 5. Tự tắt khi bàn phím CJK</span>
- Tự động tắt chế độ tiếng Việt (chuyển sang `E` mode) khi hệ điều hành chuyển sang layout bàn phím Trung 🇨🇳 - Nhật 🇯🇵 - Hàn 🇰🇷.
- Tự động bật lại chế độ tiếng Việt (`V` mode) ngay khi bạn quay trở lại layout US.

### <span id="auto-capitalize">🔠 6. Viết hoa chữ cái đầu câu</span>
- Tự động viết hoa chữ cái đầu tiên khi bạn bắt đầu câu mới, hỗ trợ trên cả chế độ tiếng Việt (`V` mode) lẫn tiếng Anh (`E` mode).
- *📌 Lưu ý:* Do giới hạn kỹ thuật của hook engine, đôi lúc tính năng này có thể hoạt động chưa chính xác. Bạn có thể bật tính năng [🔗 TSF](#tsf-mode) để tăng cường khả năng nhận diện ngữ cảnh của câu.

### <span id="free-typing">✍️ 7. Gõ tự do</span>
- Vô hiệu hóa toàn bộ cơ chế kiểm tra chính tả tiếng Việt và tiếng Anh. Giúp bạn thoải mái gõ code, viết tắt, hoặc nhập các ký tự đặc biệt mà không lo bị tự động sửa hay khôi phục từ 🆓.

### <span id="backspace-suggest">⬅️ 8. Phím Backspace (BS) giữ chữ khi có gợi ý</span>
- *Mặc định: Tắt.*
- Khi bạn nhập liệu trên thanh tìm kiếm trình duyệt 🔍 hoặc ô nhập liệu có gợi ý tự động: thông thường phím `Backspace` sẽ xóa ký tự cuối cùng và làm mất ô gợi ý.
- Nếu bật tính năng này, nhấn `Backspace` sẽ chỉ ẩn/tắt hộp gợi ý đi mà vẫn giữ nguyên chữ bạn đã gõ. *📌 Lưu ý:* Tính năng này đôi khi có thể gây lỗi hiển thị ký tự gõ tiếp theo trên một số trình duyệt.

### <span id="spellcheck">📝 9. Các tính năng kiểm tra chính tả</span>
- ✅ **Khôi phục từ với phím sai:** *(Mặc định: Bật)* Khi bạn gõ sai chính tả tiếng Việt và nhấn phím `Space`, VKey sẽ tự động hoàn trả lại đúng các ký tự gốc đã nhập. Ví dụ: gõ nhầm `pềct` + `Space` → tự động khôi phục lại thành `perfect`.
- ✅ **Quy tắc `đ` ở ký tự đầu (dd ↔ đ):** Khi chữ `đ` đứng ở **ký tự đầu**, gõ thêm `d` sẽ **chuyển đổi qua lại** một cách nhất quán: `d` → `đ`, rồi `đ` → `d`. Ví dụ: gõ `dm` rồi thêm `d` → `đm`; thêm `d` lần nữa → `dmd`. Nhờ vậy chữ `đ` đầu từ không bao giờ bị "kẹt" — bạn luôn quay lại được `d` gốc. *(Lưu ý: quy tắc này chỉ áp dụng cho `đ` ở ký tự đầu; các chữ `đ` ở giữa từ như trong `hđlđ` không bị ảnh hưởng.)*
- ✅ **Loại trừ chính tả:** Cho phép bạn thêm các từ đặc biệt, viết tắt hoặc thuật ngữ chuyên ngành vào danh sách loại trừ để bộ gõ không nhận nhầm là lỗi chính tả, giúp quá trình gõ chữ trơn tru hơn. 👉 Đây cũng là cách giữ nguyên các từ viết tắt **bắt đầu bằng `đ` và có nhiều `đ` gõ liền nhau** (ví dụ `đcđt`): chỉ cần thêm từ đó vào danh sách loại trừ, VKey sẽ giữ chữ `đ` đầu thay vì tự chuyển về `d` theo quy tắc ở trên.
- ✅ **Từ điển cá nhân (Luôn giữ từ riêng — chỉ chế độ Nâng cao):** Cho phép bảo vệ các từ lóng, từ riêng hoặc từ ít gặp (như `soà`, `khưm`, thuật ngữ...) không bị engine kiểm tra chính tả nâng cao tự động sửa thành từ khác.

#### Cách 1: Sử dụng giao diện trực quan (Khuyên dùng)
1. Nhấp chuột phải vào biểu tượng VKey ở khay hệ thống (System Tray) và chọn **Từ điển & Loại trừ chính tả** (hoặc mở từ cửa sổ Cài đặt).
2. Chọn tab **Từ điển cá nhân**:
   - **Thêm từ:** Nhập từ cần giữ nguyên vào ô văn bản (ví dụ `soà`, `khưm`, `alo`, tên riêng, thuật ngữ...), sau đó nhấn nút **Thêm** (hoặc nhấn phím `Enter`).
   - **Sửa / Xóa:** Nhấp chọn từ trong danh sách để chỉnh sửa hoặc nhấn **Xóa** để bỏ từ đó khỏi danh sách bảo vệ.
   - **Nạp / Xuất file:** Nhấn **Nạp từ file...** để nhập hàng loạt từ file văn bản `.txt` hoặc **Xuất ra file...** để sao lưu từ điển của bạn.

   Mỗi thay đổi (Thêm, Sửa, Xóa, Nạp file, bật/tắt Kiểm tra chính tả nâng cao) được **áp dụng ngay lập tức** — không cần nhấn thêm nút "Lưu". VKey đồng bộ tức thì cho cả Hook Engine lẫn TSF mà không cần khởi động lại. Bản VKey Classic thuần C++ chỉ hiển thị giao diện **Loại trừ chính tả**; phần Từ điển cá nhân và chế độ Nâng cao chỉ có trong bản dùng engine nâng cao.

#### Cách 2: Chỉnh sửa thủ công file `user_dictionary.txt`
File `user_dictionary.txt` nằm cùng thư mục với `config.toml` (thường là thư mục cài đặt VKey). Nếu thư mục đó không có quyền ghi, hãy đặt file trong `%APPDATA%\VKey`. File **không được tạo tự động** — bạn phải tự tạo bằng tay hoặc dùng giao diện **Từ điển cá nhân** ở Cách 1 để VKey tạo và quản lý giúp bạn.

1. Báo lỗi tự sửa tại [GitHub Issues](https://github.com/phatMT97/VKey/issues) trước khi thêm từ.
2. Mở file bằng Notepad hoặc trình soạn thảo văn bản, thêm mỗi từ vào một dòng riêng rồi lưu dưới dạng mã hóa **UTF-8**.
3. Khởi động lại VKey hoặc chuyển **Nâng cao → Cơ bản → Nâng cao** để nạp file mới.

Dòng trống và dòng bắt đầu bằng `#` được bỏ qua. (Lưu ý: Không dùng dấu `;` làm chú thích vì engine sẽ coi là lỗi). File rỗng hoặc chỉ có comment là hợp lệ và xóa danh sách bảo vệ. File sai UTF-8, có dòng không hợp lệ, quá lớn hoặc tạm thời không đọc được sẽ không làm mất danh sách hợp lệ gần nhất. Danh sách chỉ được lưu và xử lý cục bộ trên máy tính của bạn.

### <span id="hotkey-mgmt">⚡ 10. Quản lý phím tắt</span>
- 🔸 **Khôi phục từ gốc chủ động:** Cho phép thiết lập phím tắt để ngay lập tức trả lại các ký tự gốc (raw keys) mà không cần nhấn phím cách (`Space`) hoặc đợi gõ hết từ. Ví dụ: gõ `asus` → hiển thị thành `aus` → nhấn `ESC` → lập tức khôi phục lại thành `asus`.
- 🔸 **Hotkeys hệ thống:** Tự do tùy chỉnh các tổ hợp phím để bật/tắt nhanh bộ gõ tiếng Việt hoặc bỏ qua gõ tắt tạm thời.
- 🔸 🎮 **Bật / tắt chế độ game cho app đang mở:** Nhấn ngay khi đang ở trong game để thêm game đó vào danh sách [Cấu hình từng ứng dụng](#app-config) với "Cách gửi" là Chế độ game; nhấn lần nữa để gỡ. Dành cho lúc đang chơi fullscreen, khỏi phải thoát ra vào Cài đặt. Mục này **không có tổ hợp mặc định** — bạn tự chọn tổ hợp không trùng với phím đã gán trong game.

### <span id="tsf-mode">🔗 11. Sử dụng TSF (Text Services Framework)</span>
VKey tích hợp sâu công nghệ TSF của Windows mang lại độ tương thích tối đa:
- 🟢 **Tăng cường ngữ cảnh (chỉ bật TSF):** Giúp VKey đọc hiểu đoạn văn xung quanh tốt hơn để sửa dấu và tự động viết hoa cực kỳ chính xác. Bộ gõ vẫn dùng Hook làm phương thức nhập chính nên bạn không lo chữ bị gạch chân khi gõ. Bạn chỉ cần bật lên mà không cần cấu hình thêm ứng dụng vào danh sách.
- 🟡 **TSF làm phương thức nhập chính (khi thêm app vào danh sách):** Khi bạn thêm ứng dụng vào danh sách TSF, VKey sẽ chuyển hẳn sang sử dụng cơ chế nhập TSF cho ứng dụng đó.
  - ✅ *Ưu điểm:* Gõ tiếng Việt tốt trong mọi phần mềm, kể cả ứng dụng Windows Store (UWP) hay game chống cheat nghiêm ngặt.
  - ⚠️ *Nhược điểm:* Chữ đang gõ sẽ có đường gạch chân tạm thời của Windows trước khi được xác nhận (commit).

### <span id="macro">🚀 12. Gõ tắt (Macro)</span>
- 💬 Hỗ trợ lưu trữ các đoạn gõ tắt dài tới **20.000 ký tự**, hỗ trợ xuống dòng thoải mái.
- 🎯 Linh hoạt tùy chọn các phím để kích hoạt (trigger) gõ tắt như: phím Cách (`Space`), `Enter`, `Tab` hoặc các phím điều hướng.
- 🔠 **Tự động viết hoa theo phím tắt:** Với `bbh = bún bò Huế`, gõ `bbh` giữ nguyên nội dung, `Bbh` ra `Bún bò Huế`, `BBH` ra `BÚN BÒ HUẾ`. Bật thêm **Viết hoa đầu từng từ** nếu muốn `Bbh` ra `Bún Bò Huế`. Chữ hoa do tùy chọn viết hoa đầu câu tự tạo chỉ nâng đầu cụm. Xem [quy tắc gõ tắt và viết hoa](MACRO_CASING.md).
- ↩️ **Khôi phục từ gõ tắt khi nhấn Backspace:** Ngay sau khi bung nội dung đầy đủ (ví dụ `bbh` + Space ra `bún bò Huế `), nếu bạn nhấn Backspace thì VKey sẽ xoá toàn bộ cụm từ đã bung và khôi phục lại từ gõ tắt ban đầu (`bbh`) thay vì phải xoá lùi từng chữ. Bật/tắt trong tab **Gõ tắt** (mặc định tắt).
  - Nhấn trong vòng 4 giây, trước khi gõ phím khác hoặc đổi vị trí nhập. Undo chỉ được giữ với trigger chèn chữ như Space/dấu câu, không áp dụng sau Enter/Tab/phím điều hướng. Với Hook, undo hỗ trợ macro Unicode một dòng gửi trực tiếp; macro gửi qua clipboard, bảng mã cũ hoặc ký tự tổ hợp/emoji vẫn dùng Backspace thông thường.

### <span id="watchdog">🔁 13. Tự khởi động lại (Watchdog)</span>
- *Mặc định: Tắt.*
- Khi bật tính năng này, VKey sẽ tự động khởi động lại nếu bị thoát đột ngột do crash hoặc lỗi hệ thống 🛡️.
- VKey sử dụng một chương trình giám sát riêng (`VKeyWatchdog.exe`) chạy ngầm bên cạnh. Chương trình này liên tục theo dõi "nhịp tim" (heartbeat) của VKey — nếu VKey không phản hồi trong **90 giây** (tương đương bỏ lỡ 3 lần gửi tín hiệu), watchdog sẽ tự động khởi động lại VKey.
- Watchdog phân biệt được giữa crash thật (tự động khởi động lại) và thoát chủ ý bởi người dùng (không khởi động lại) ✅.
- Bạn có thể bật/tắt tính năng này từ **menu chuột phải** ở khay hệ thống (traybar) → `Bật tự khởi động lại` / `Tắt tự khởi động lại`.
- *📌 Lưu ý:* Khi bật, VKey sẽ tạo một tác vụ trong **Task Scheduler** của Windows để watchdog tự khởi động cùng hệ thống khi bạn đăng nhập.

### <span id="convert-tool">🔄 14. Công cụ chuyển mã nhanh</span>
VKey cung cấp công cụ chuyển đổi bảng mã và biến đổi chữ cái mạnh mẽ, hỗ trợ cả giao diện đồ hoạ lẫn phím tắt nhanh:
- **Chuyển đổi bảng mã:** Hỗ trợ các bảng mã Unicode, TCVN3 (ABC), VNI Windows, Unicode Compound và Vietnamese Locale.
- **Biến đổi chữ cái:** Chuyển HOA, chuyển thường, Hoa đầu câu, Hoa Từng Chữ, hoặc loại bỏ dấu. Chỉ áp dụng cho tuỳ chọn lớn nhất, ví dụ bạn chọn in HOA và in Hoa Chữ Cái Đầu thì kết quả sẽ chỉ là in HOA.
- **Phím tắt chuyển nhanh:** Đặt phím tắt tuỳ ý (VD: `Ctrl+Shift+C`) để chuyển mã mà không cần mở cửa sổ. 
- **Tự động dán + bôi đen:** Khi bật tuỳ chọn này, sau khi chuyển mã xong, VKey sẽ tự động dán kết quả đè lên đoạn đã chọn **và bôi đen lại** đoạn chữ vừa dán. Điều này giúp bạn dễ dàng kiểm tra kết quả hoặc tiếp tục chuyển đổi mà không cần bôi đen lại thủ công.
- **Convert tuần tự (Sequential):** Khi bật cả "Tự động dán + bôi đen" và "Convert tuần tự", mỗi lần nhấn phím tắt sẽ **lần lượt áp dụng từng phép chuyển đổi** đã bật thay vì áp dụng tất cả cùng lúc 🔄.
  - Ví dụ: Bạn bật cả "chữ HOA" và "Loại bỏ dấu câu", bôi đen "Xin chào":
    - Nhấn lần 1 → `XIN CHÀO` (chữ HOA)
    - Nhấn lần 2 → `Xin chao` (loại bỏ dấu)
    - Nhấn lần 3 → `Xin chào` (quay về bản gốc)
  - Chu kỳ tự động hết hạn sau **5 giây** không thao tác hoặc khi bạn chọn đoạn văn bản khác.
- *📌 Lưu ý:* Tính năng "Convert tuần tự" chỉ khả dụng khi "Tự động dán + bôi đen" đang bật.

---

## <span id="security-warning">🛡️ Cảnh báo bảo mật & Debug Log</span>

> [!CAUTION]
> ### 🚨 Cảnh báo bảo mật — Debug Log
>
> Bật Debug Log sẽ **ghi lại toàn bộ thao tác bàn phím** của bạn, bao gồm cả raw key.
> File log có thể chứa **🔑 mật khẩu** hoặc **thông tin nhạy cảm** nếu bật trong thời gian dài.
>
> ---
>
> 🔐 **Trước khi gửi file log cho ai khác:**
>
> | | Bước |
> |---|---|
> | 👀 | Mở file log và **xem lại nội dung** |
> | 🗑️ | Xóa các dòng **không liên quan** dựa theo thời gian |
> | ✂️ | **Chỉ gửi đoạn cần thiết** cho việc debug |

📚 Để biết thêm về các biện pháp tăng cường bảo mật của dự án, vui lòng đọc tài liệu chi tiết tại [🔒 SECURITY.md](SECURITY.md).
