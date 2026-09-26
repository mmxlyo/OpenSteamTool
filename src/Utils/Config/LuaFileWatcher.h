#pragma once

#include <string>
#include <vector>

namespace LuaFileWatcher {
    using LicenseChangedCallback = void (*)();
    void SetLicenseChangedCallback(LicenseChangedCallback cb);

    void Start(const std::vector<std::string>& directories);
    void Stop();
}
