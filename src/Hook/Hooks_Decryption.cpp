#include "Hooks_Decryption.h"
#include "HookMacros.h"
#include "dllmain.h"

#include <atomic>
#include <charconv>
#include <cstring>
#include <string>
#include <string_view>

namespace {

    std::atomic<void*> g_pConfigStoreLocal{nullptr};

    HOOK_FUNC(ConfigStoreGetBinary, int32, void* pObject, EConfigStore eConfigStore, const char* KeyName, char* Key, uint32 KeySize) {
        if (!KeyName) {
            return oConfigStoreGetBinary(pObject, eConfigStore, KeyName, Key, KeySize);
        }

        if (eConfigStore == k_EConfigStoreUserLocal && pObject && !g_pConfigStoreLocal.load(std::memory_order_relaxed)) {
            g_pConfigStoreLocal.store(pObject, std::memory_order_release);
            LOG_DECRYPTIONKEY_DEBUG("ConfigStoreGetBinary: captured local ConfigStore at {}", pObject);
        }

        std::string_view name(KeyName);
        LOG_DECRYPTIONKEY_DEBUG("ConfigStore::GetBinary called for pObject={}, eConfigStore={}, KeyName='{}'", 
                                    pObject, static_cast<uint32>(eConfigStore), name);

        // Expected shape: ".../<DepotId>\DecryptionKey" or ".../<DepotId>/DecryptionKey"
        if (size_t last = name.rfind("DecryptionKey"); last != std::string_view::npos && last > 0) {
            if (name[last - 1] == '\\' || name[last - 1] == '/') {
                size_t sep = last - 1;
                size_t start = name.find_last_of("\\/", sep > 0 ? sep - 1 : 0);
                size_t idStart = (start == std::string_view::npos) ? 0 : start + 1;
                std::string_view idStr = name.substr(idStart, sep - idStart);

                AppId_t depotId = 0;
                auto [ptr, ec] = std::from_chars(idStr.data(), idStr.data() + idStr.size(), depotId);
                if (ec == std::errc{} && ptr == idStr.data() + idStr.size() && depotId != 0) {
                    if (const auto& key = LuaConfig::GetDecryptionKey(depotId); !key.empty()) {
                        if (KeySize >= key.size()) {
                            LOG_DECRYPTIONKEY_INFO("Providing decryption key for depot {}: {}", depotId,
                                                   spdlog::to_hex(key.data(), key.data() + key.size()));
                            std::memcpy(Key, key.data(), key.size());
                            return static_cast<int32>(key.size());
                        }
                        LOG_DECRYPTIONKEY_WARN("Decryption key for depot {} is too large ({} bytes) for buffer ({} bytes)",
                                                depotId, key.size(), KeySize);
                    }
                }
            }
        }
        return oConfigStoreGetBinary(pObject, eConfigStore, KeyName, Key, KeySize);
    }

    std::vector<uint8_t> ReadConfigStoreLocalBinary(const std::string& keyName) {
        void* pLocal = g_pConfigStoreLocal.load(std::memory_order_acquire);
        if (!pLocal || !oConfigStoreGetBinary) {
            LOG_DECRYPTIONKEY_WARN("GetConfigStoreLocalBinary: ConfigStoreGetBinary not ready, cannot get binary value");
            return {};
        }

        std::vector<uint8_t> value(1024);
        int32 result = oConfigStoreGetBinary(pLocal, k_EConfigStoreUserLocal,
                                             keyName.c_str(),
                                             reinterpret_cast<char*>(value.data()),
                                             static_cast<uint32>(value.size()));
        if (result <= 0) {
            LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: failed to read KeyName='{}'", keyName);
            return {};
        }

        value.resize(result);
        LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: got value for KeyName='{}' ({} bytes)",
                                keyName, value.size());
        return value;
    }
}

namespace Hooks_Decryption {
    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(ConfigStoreGetBinary);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(ConfigStoreGetBinary);
        UNHOOK_END();
    }

    std::vector<uint8_t> GetCacheAppOwnershipTicket(AppId_t appId) {
        std::vector<uint8_t> ticket = ReadConfigStoreLocalBinary(std::format("apptickets\\{}", appId));
        if (ticket.empty()) {
            LOG_DECRYPTIONKEY_DEBUG("no cached ticket for AppId {}", appId);
            return ticket;
        }
        LOG_DECRYPTIONKEY_DEBUG("got cached ticket for AppId {} ({} bytes)", appId, ticket.size());
        return ticket;
    }
}
