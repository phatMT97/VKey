// VKey - Lexicon & Spell Check Exclusions Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "SpellExclusionsDialog.h"
#include "helpers/AppHelpers.h"
#include "core/config/ConfigManager.h"
#include "core/config/LexiconTransaction.h"
#include "core/config/LexiconValidation.h"
#include "core/config/SpellExclusionCanonicalizer.h"
#include "core/WinStrings.h"
#include "sciter-x-dom.hpp"
#include "DialogUtils.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace sciter::dom;

namespace NextKey {

SpellExclusionsDialog::SpellExclusionsDialog(HWND parent)
    : SciterSubDialog({
        L"this://app/spellexclusions/spellexclusions.html",
        L"VKey - Spell Exclusions",
        420, 580, parent, false, 36, 40, true
    }) {
    loadData();
    populateUI();
}

LRESULT SpellExclusionsDialog::onCustomMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    return -1;
}

bool SpellExclusionsDialog::loadData() {
    dataLoaded_ = false;
    const auto configPath = ConfigManager::GetConfigPath();
    LexiconSyncLock lock;
    if (!lock.IsLocked() || !LexiconRecovery::RecoverIfNeeded(configPath)) return false;

    std::error_code ec;
    const bool configExists = std::filesystem::exists(configPath, ec);
    if (ec) return false;
    auto loadedConfig = ConfigManager::LoadFromFile(configPath);
    if (configExists && !loadedConfig) return false;

    std::vector<std::wstring> loadedWords;
    // Windows named mutexes are recursive for the owning thread; both reads
    // remain under the same outer lock while the reader acquires its own lock.
    if (!LexiconReader::LoadUserDictionaryWordsLocked(configPath, loadedWords)) return false;

    const TypingConfig config = loadedConfig.value_or(TypingConfig{});
    spellSuggestEnabled_ = config.spellSuggestEnabled;
    spellExclusions_ = config.spellExclusions;
    userDictWords_ = std::move(loadedWords);
    dataLoaded_ = true;
    modified_ = false;
    return true;
}

void SpellExclusionsDialog::populateUI() {
    sciter::value userWordsArr = sciter::value::make_array();
    for (const auto& w : userDictWords_) {
        userWordsArr.append(sciter::value(w.c_str()));
    }

    sciter::value exclArr = sciter::value::make_array();
    for (const auto& e : spellExclusions_) {
        exclArr.append(sciter::value(e.c_str()));
    }

    call_function("setInitialData", sciter::value(spellSuggestEnabled_), userWordsArr, exclArr);
    if (!dataLoaded_) {
        call_function("showFieldError", sciter::value(L"Không thể nạp từ điển và cấu hình. Hãy đóng và mở lại cửa sổ."));
    }
}

void SpellExclusionsDialog::populateList(int listIndex) {
    sciter::value arr = sciter::value::make_array();
    if (listIndex == 0) {
        for (const auto& w : userDictWords_) {
            arr.append(sciter::value(w.c_str()));
        }
    } else {
        for (const auto& e : spellExclusions_) {
            arr.append(sciter::value(e.c_str()));
        }
    }
    call_function("renderList", sciter::value(listIndex), arr);
}

bool SpellExclusionsDialog::handle_event(HELEMENT he, BEHAVIOR_EVENT_PARAMS& params) {
    if (params.cmd == BUTTON_CLICK) {
        element el(params.heTarget);
        std::wstring id = el.get_attribute("id");

        if (id == L"btn-close" || id == L"btn-cancel") {
            PostMessage(get_hwnd(), WM_CLOSE, 0, 0);
            return true;
        }
    }

    if (params.cmd == VALUE_CHANGED) {
        element el(params.heTarget);
        std::wstring id = el.get_attribute("id");

        if (id == L"val-action") {
            sciter::value val = el.get_value();
            std::wstring action = val.is_string() ? val.get<std::wstring>() : L"";
            if (!action.empty()) {
                element root = get_root();

                element targetEl = root.find_first("#val-target-list");
                int targetList = 0;
                if (targetEl.is_valid()) {
                    sciter::value tv = targetEl.get_value();
                    if (tv.is_string()) {
                        targetList = _wtoi(tv.get<std::wstring>().c_str());
                    }
                }

                element indexEl = root.find_first("#val-entry-index");
                int entryIndex = -1;
                if (indexEl.is_valid()) {
                    sciter::value iv = indexEl.get_value();
                    if (iv.is_string()) {
                        entryIndex = _wtoi(iv.get<std::wstring>().c_str());
                    }
                }

                element nameInput = root.find_first("#val-entry-name");
                std::wstring entryName;
                if (nameInput.is_valid()) {
                    sciter::value nv = nameInput.get_value();
                    entryName = nv.is_string() ? nv.get<std::wstring>() : L"";
                }

                if (action == L"add") {
                    addEntry(targetList, entryName);
                } else if (action == L"edit") {
                    editEntry(targetList, entryIndex, entryName);
                } else if (action == L"delete") {
                    deleteEntry(targetList, entryIndex);
                } else if (action == L"import") {
                    importEntries(targetList);
                } else if (action == L"export") {
                    exportEntries(targetList);
                } else if (action == L"reload") {
                    reloadData();
                } else if (action == L"save") {
                    (void)saveAndApply();
                } else if (action == L"toggle_suggest") {
                    element sugEl = root.find_first("#val-spell-suggest");
                    if (sugEl.is_valid()) {
                        sciter::value sv = sugEl.get_value();
                        if (sv.is_string()) {
                            spellSuggestEnabled_ = (sv.get<std::wstring>() == L"1");
                            (void)saveAndApply(false);
                        }
                    }
                } else if (action == L"close") {
                    PostMessage(get_hwnd(), WM_CLOSE, 0, 0);
                }

                el.set_value(sciter::value(L""));
            }
            return true;
        }
    }

    return sciter::window::handle_event(he, params);
}

void SpellExclusionsDialog::addEntry(int targetList, const std::wstring& text) {
    if (targetList == 0) {
        // User Dictionary
        std::wstring normalized;
        auto valRes = LexiconValidator::ValidateUserDictWord(text, userDictWords_.size() + 1, &normalized);
        if (!valRes.Succeeded()) {
            call_function("showFieldError", sciter::value(valRes.errorMessage.c_str()));
            return;
        }
        if (userDictWords_.size() >= kMaxUserDictEntries) {
            call_function("showFieldError", sciter::value(L"Từ điển cá nhân đã đạt giới hạn 1.024 từ."));
            return;
        }
        if (std::find(userDictWords_.begin(), userDictWords_.end(), normalized) != userDictWords_.end()) {
            call_function("showFieldError", sciter::value(L"Từ này đã có trong từ điển cá nhân."));
            return;
        }
        userDictWords_.push_back(normalized);
        std::sort(userDictWords_.begin(), userDictWords_.end());
        (void)saveAndApply(false);
    } else {
        // Spell Exclusions
        std::wstring normalized;
        auto valRes = LexiconValidator::ValidateSpellExclusionWord(text, spellExclusions_.size() + 1, &normalized);
        if (!valRes.Succeeded()) {
            call_function("showFieldError", sciter::value(valRes.errorMessage.c_str()));
            return;
        }
        if (spellExclusions_.size() >= 8) {
            call_function("showFieldError", sciter::value(L"Danh sách ngoại lệ đã đạt tối đa 8 từ."));
            return;
        }
        if (std::find(spellExclusions_.begin(), spellExclusions_.end(), normalized) != spellExclusions_.end()) {
            call_function("showFieldError", sciter::value(L"Từ viết tắt này đã có trong danh sách ngoại lệ."));
            return;
        }
        spellExclusions_.push_back(normalized);
        // Canonicalize to guarantee order and dedup
        auto canon = SpellExclusionCanonicalizer::Canonicalize(spellExclusions_);
        if (canon.Succeeded()) {
            spellExclusions_ = std::move(canon.entries);
        }
        (void)saveAndApply(false);
    }
}

void SpellExclusionsDialog::editEntry(int targetList, int index, const std::wstring& text) {
    if (targetList == 0) {
        if (index < 0 || index >= static_cast<int>(userDictWords_.size())) return;
        std::wstring normalized;
        auto valRes = LexiconValidator::ValidateUserDictWord(text, index + 1, &normalized);
        if (!valRes.Succeeded()) {
            call_function("showFieldError", sciter::value(valRes.errorMessage.c_str()));
            return;
        }
        userDictWords_[index] = normalized;
        std::sort(userDictWords_.begin(), userDictWords_.end());
        userDictWords_.erase(std::unique(userDictWords_.begin(), userDictWords_.end()), userDictWords_.end());
        (void)saveAndApply(false);
    } else {
        if (index < 0 || index >= static_cast<int>(spellExclusions_.size())) return;
        std::wstring normalized;
        auto valRes = LexiconValidator::ValidateSpellExclusionWord(text, index + 1, &normalized);
        if (!valRes.Succeeded()) {
            call_function("showFieldError", sciter::value(valRes.errorMessage.c_str()));
            return;
        }
        spellExclusions_[index] = normalized;
        auto canon = SpellExclusionCanonicalizer::Canonicalize(spellExclusions_);
        if (canon.Succeeded()) {
            spellExclusions_ = std::move(canon.entries);
        }
        (void)saveAndApply(false);
    }
}

void SpellExclusionsDialog::deleteEntry(int targetList, int index) {
    if (targetList == 0) {
        if (index >= 0 && index < static_cast<int>(userDictWords_.size())) {
            userDictWords_.erase(userDictWords_.begin() + index);
            (void)saveAndApply(false);
        }
    } else {
        if (index >= 0 && index < static_cast<int>(spellExclusions_.size())) {
            spellExclusions_.erase(spellExclusions_.begin() + index);
            (void)saveAndApply(false);
        }
    }
}

void SpellExclusionsDialog::importEntries(int targetList) {
    std::wstring path = ShowOpenFileDialogW(
        get_hwnd(),
        L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0",
        L"txt"
    );
    if (path.empty()) return;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        MessageBoxW(get_hwnd(), L"Không thể mở file được chọn.", L"Lỗi mở file", MB_OK | MB_ICONERROR);
        return;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    int choice = MessageBoxW(
        get_hwnd(),
        L"Bạn có muốn thêm tiếp vào danh sách hiện tại (Chọn YES) hay thay thế toàn bộ danh sách (Chọn NO)?",
        L"Chế độ nhập dữ liệu",
        MB_YESNOCANCEL | MB_ICONQUESTION
    );
    if (choice == IDCANCEL) return;
    bool append = (choice == IDYES);

    if (targetList == 0) {
        auto res = LexiconValidator::ParseAndValidateUserDictText(content, append ? &userDictWords_ : nullptr, append);
        if (!res.validation.Succeeded()) {
            MessageBoxW(get_hwnd(), res.validation.errorMessage.c_str(), L"Lỗi dữ liệu", MB_OK | MB_ICONERROR);
            return;
        }
        userDictWords_ = std::move(res.entries);
        (void)saveAndApply(false);
        call_function("showSuccessToast", sciter::value(L"Nhập từ điển thành công!"));
    } else {
        auto res = LexiconValidator::ParseAndValidateSpellExclusionsText(content, append ? &spellExclusions_ : nullptr, append);
        if (!res.validation.Succeeded()) {
            MessageBoxW(get_hwnd(), res.validation.errorMessage.c_str(), L"Lỗi dữ liệu", MB_OK | MB_ICONERROR);
            return;
        }
        spellExclusions_ = std::move(res.entries);
        (void)saveAndApply(false);
        call_function("showSuccessToast", sciter::value(L"Nhập ngoại lệ thành công!"));
    }
}

void SpellExclusionsDialog::exportEntries(int targetList) {
    std::wstring defaultName = (targetList == 0) ? L"user_dictionary" : L"spell_exclusions";
    std::wstring path = ShowSaveFileDialogW(
        get_hwnd(),
        L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0",
        L"txt",
        defaultName.c_str()
    );
    if (path.empty()) return;

    std::string content;
    if (targetList == 0) {
        content = LexiconValidator::FormatUserDictText(userDictWords_);
    } else {
        content = LexiconValidator::FormatSpellExclusionsText(spellExclusions_);
    }

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        MessageBoxW(get_hwnd(), L"Không thể tạo file xuất dữ liệu.", L"Lỗi", MB_OK | MB_ICONERROR);
        return;
    }
    file.write(content.data(), content.size());
    file.close();

    MessageBoxW(get_hwnd(), L"Xuất danh sách thành công.", L"Thành công", MB_OK | MB_ICONINFORMATION);
}

void SpellExclusionsDialog::reloadData() {
    if (modified_) {
        int choice = MessageBoxW(
            get_hwnd(),
            L"Các thay đổi chưa lưu sẽ bị hủy bỏ. Bạn có chắc chắn muốn nạp lại từ đĩa?",
            L"Nạp lại",
            MB_YESNO | MB_ICONQUESTION
        );
        if (choice != IDYES) return;
    }
    if (loadData()) {
        populateUI();
        call_function("showSuccessToast", sciter::value(L"Đã nạp lại dữ liệu từ đĩa."));
    } else {
        MessageBoxW(get_hwnd(), L"Không thể nạp từ điển và cấu hình.", L"Lỗi đọc dữ liệu", MB_OK | MB_ICONERROR);
    }
}

bool SpellExclusionsDialog::saveAndApply(bool showToast) {
    if (!dataLoaded_) {
        MessageBoxW(get_hwnd(), L"Chưa nạp được từ điển và cấu hình. Hãy đóng và mở lại cửa sổ.", L"Lỗi đọc dữ liệu", MB_OK | MB_ICONERROR);
        return false;
    }
    auto configPath = ConfigManager::GetConfigPath();

    auto canon = SpellExclusionCanonicalizer::Canonicalize(spellExclusions_);
    if (!canon.Succeeded()) {
        std::wstring err = L"Ngoại lệ viết tắt không hợp lệ: " + canon.error.message;
        MessageBoxW(get_hwnd(), err.c_str(), L"Lỗi", MB_OK | MB_ICONERROR);
        return false;
    }
    spellExclusions_ = std::move(canon.entries);

    const bool ok = LexiconWriter::CommitLexiconUpdate(
        configPath, spellSuggestEnabled_, spellExclusions_, userDictWords_);
    if (!ok) {
        MessageBoxW(get_hwnd(), L"Lỗi khi thực hiện giao dịch lưu từ điển và cấu hình.", L"Lỗi giao dịch", MB_OK | MB_ICONERROR);
        return false;
    }

    modified_ = false;

    if (!ApplyCommittedLexicon()) {
        MessageBoxW(
            get_hwnd(),
            L"Đã lưu dữ liệu nhưng chưa thể áp dụng ngay. Vui lòng khởi động lại VKey.",
            L"Chưa thể áp dụng",
            MB_OK | MB_ICONWARNING);
        return false;
    }

    populateList(0);
    populateList(1);
    if (showToast) {
        call_function("showSuccessToast", sciter::value(L"Đã lưu & áp dụng thành công!"));
    }
    return true;
}

}  // namespace NextKey
