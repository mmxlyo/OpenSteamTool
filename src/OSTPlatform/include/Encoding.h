#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace OSTPlatform::Encoding {

    std::string WideToUtf8(std::wstring_view value);
    std::wstring Utf8ToWide(std::string_view value);

    inline std::filesystem::path PathFromUtf8(std::string_view u8str) {
#if defined(_WIN32)
        return std::filesystem::path(Utf8ToWide(u8str));
#else
        return std::filesystem::path(u8str);
#endif
    }

    inline std::string PathToUtf8(const std::filesystem::path& p) {
#if defined(_WIN32)
        return WideToUtf8(p.wstring());
#else
        return p.string();
#endif
    }

} // namespace OSTPlatform::Encoding
