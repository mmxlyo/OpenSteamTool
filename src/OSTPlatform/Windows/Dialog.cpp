#include "include/Dialog.h"

#include "include/Encoding.h"

#include <windows.h>

namespace OSTPlatform::Dialog {

void ShowWarning(std::string title, std::string message) {
    const std::wstring wideTitle = Encoding::Utf8ToWide(title);
    const std::wstring wideMessage = Encoding::Utf8ToWide(message);
    MessageBoxW(nullptr, wideMessage.c_str(), wideTitle.c_str(), MB_OK | MB_ICONWARNING | MB_TOPMOST);
}

} // namespace OSTPlatform::Dialog
