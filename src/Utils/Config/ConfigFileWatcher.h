#pragma once

#include <string>

namespace ConfigFileWatcher {
    using LicenseChangedCallback = void (*)();
    void SetLicenseChangedCallback(LicenseChangedCallback cb);

    void Start(const std::string& configPath, const std::string& defaultLuaDir);
    void Stop();
}
