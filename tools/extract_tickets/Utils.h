#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

// String & numeric validation
[[nodiscard]] std::string_view TrimWhitespace(std::string_view str) noexcept;
[[nodiscard]] bool IsDecimal(std::string_view value) noexcept;
[[nodiscard]] bool IsValidManifestId(std::string_view value) noexcept;
[[nodiscard]] bool IsHex64(std::string_view value) noexcept;
[[nodiscard]] std::optional<uint32_t> ParseAppId(std::string_view value) noexcept;
[[nodiscard]] std::optional<uint32_t> ReadAppIdFromConsole();
[[nodiscard]] std::string SanitizeComment(std::string_view text);
[[nodiscard]] std::vector<std::string> TokenizeQuoted(std::string_view line);

// Hex formatting
[[nodiscard]] std::string ToHexString(std::span<const uint8_t> data);
void PrintHex(const char* label, std::span<const uint8_t> data);

// File I/O
bool WriteBinaryFile(const std::filesystem::path& path, std::span<const uint8_t> data);

// Path & Registry
[[nodiscard]] std::string JoinPath(std::string_view base, std::string_view name);
[[nodiscard]] std::string NormalizeDir(std::string dir);
[[nodiscard]] std::optional<std::string> QueryRegistryString(HKEY root, const char* subKey, const char* valueName);
[[nodiscard]] std::optional<std::string> FindSteamInstallPath();

} // namespace OST::ExtractTickets
