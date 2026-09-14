#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace OSTPlatform::DynamicLibrary {

    using ModuleHandle = void*;

    ModuleHandle Load(const std::filesystem::path& path);
    bool Unload(ModuleHandle module);
    ModuleHandle GetLoaded(std::string_view moduleName);
    void* GetSymbol(ModuleHandle module, const char* symbolName);
    void* GetSymbol(ModuleHandle module, uint16_t ordinal);
    uint32_t GetLastErrorCode();

    std::string GetCurrentDirectoryPath();
    std::string GetSystemDirectoryPath();
    std::filesystem::path GetModuleDirectory(ModuleHandle module);
    std::filesystem::path GetMainExecutablePath();

} // namespace OSTPlatform::DynamicLibrary
