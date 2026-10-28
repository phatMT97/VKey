// VKey - About Dialog Implementation
// SPDX-License-Identifier: GPL-3.0-only

#include "AboutDialog.h"
#include "core/Version.h"
#include "core/WinStrings.h"
#include "sciter-x-dom.hpp"

namespace NextKey {

AboutDialog::AboutDialog(HWND parent)
    : SciterSubDialog({
        L"this://app/about/about.html",
        L"VKey - About",
        330, 360, parent, false, 36, 40, true
    }) {
    sciter::dom::element root = get_root();
    if (root.is_valid()) {
        sciter::dom::element verSpan = root.find_first("#val-version");
        if (verSpan.is_valid()) {
            verSpan.set_text(VKEY_VERSION_WSTR);
        }
        sciter::dom::element buildTimeSpan = root.find_first("#val-build-time");
        if (buildTimeSpan.is_valid()) {
            buildTimeSpan.set_text(GetBuildVersion(__DATE__, __TIME__).c_str());
        }
    }
}

}  // namespace NextKey
