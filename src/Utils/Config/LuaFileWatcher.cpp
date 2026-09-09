#include "Hook/Hooks_Package.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/Logging/Log.h"
#include "OSTPlatform/include/DirectoryWatch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <thread>
#include <unordered_map>

namespace LuaFileWatcher {
namespace {

enum class ChangeAction {
    Added,
    Modified,
    Removed,
};

struct FileChange {
    std::string path;
    ChangeAction action = ChangeAction::Modified;
};

std::atomic<bool> g_running{false};
std::thread g_watcherThread;
std::vector<std::string> g_watchDirs;

constexpr uint32_t kDebounceMs = 500;

bool IsLuaFile(const std::string& path) {
    if (path.size() < 4) return false;
    return std::equal(path.end() - 4, path.end(), ".lua", [](char lhs, char rhs) {
        return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
    });
}

bool IsManifestFile(const std::string& path) {
    if (path.size() < 9) return false;
    return std::equal(path.end() - 9, path.end(), ".manifest", [](char lhs, char rhs) {
        return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
    });
}

ChangeAction FromPlatformAction(OSTPlatform::DirectoryWatch::ChangeAction action) {
    switch (action) {
    case OSTPlatform::DirectoryWatch::ChangeAction::Added:
    case OSTPlatform::DirectoryWatch::ChangeAction::RenamedNewName:
        return ChangeAction::Added;
    case OSTPlatform::DirectoryWatch::ChangeAction::Removed:
    case OSTPlatform::DirectoryWatch::ChangeAction::RenamedOldName:
        return ChangeAction::Removed;
    case OSTPlatform::DirectoryWatch::ChangeAction::Modified:
        return ChangeAction::Modified;
    }
    return ChangeAction::Modified;
}

const char* ToString(ChangeAction action) {
    switch (action) {
    case ChangeAction::Added: return "added";
    case ChangeAction::Modified: return "modified";
    case ChangeAction::Removed: return "removed";
    }
    return "modified";
}

void MergeChanges(
    std::unordered_map<std::string, ChangeAction>& accumulated,
    std::vector<std::string>& order,
    const std::vector<FileChange>& newChanges) {
    for (const auto& ch : newChanges) {
        if (!accumulated.contains(ch.path)) {
            order.push_back(ch.path);
        }
        accumulated[ch.path] = ch.action;
    }
}

std::vector<FileChange> FlattenChanges(
    const std::unordered_map<std::string, ChangeAction>& accumulated,
    const std::vector<std::string>& order) {
    std::vector<FileChange> changes;
    changes.reserve(order.size());
    for (const auto& path : order) {
        changes.push_back({path, accumulated.at(path)});
    }
    return changes;
}

std::vector<FileChange> ToFileChanges(
    const std::string& dir,
    const std::vector<OSTPlatform::DirectoryWatch::Change>& changes) {
    std::vector<FileChange> result;
    result.reserve(changes.size());
    for (const auto& change : changes) {
        if (change.relativePath.empty()) continue;
        std::string fullPath = (std::filesystem::path(dir) / change.relativePath).lexically_normal().string();
        result.push_back({std::move(fullPath), FromPlatformAction(change.action)});
    }
    return result;
}

void ProcessChanges(const std::vector<FileChange>& changes) {
    bool luaStateChanged = false;
    std::unordered_set<std::string> processedLua;

    for (const auto& change : changes) {
        if (IsManifestFile(change.path)) {
            if (change.action == ChangeAction::Added || change.action == ChangeAction::Modified) {
                LOG_PACKAGE_TRACE("Manifest file {}: {}", ToString(change.action), change.path);
                LuaConfig::CopyManifestToDepotcache(change.path);
            }
            continue;
        }

        if (IsLuaFile(change.path)) {
            LOG_PACKAGE_TRACE("Lua file {}: {}", ToString(change.action), change.path);
            std::string normPath = std::filesystem::path(change.path).lexically_normal().string();
            if (change.action == ChangeAction::Removed) {
                LuaConfig::UnloadFile(normPath);
                luaStateChanged = true;
            } else {
                if (processedLua.insert(normPath).second) {
                    LuaConfig::ParseFile(normPath);
                    luaStateChanged = true;
                }
            }
            continue;
        }

        // Handle subdirectories added or removed
        std::error_code ec;
        if (change.action == ChangeAction::Removed) {
            LOG_PACKAGE_TRACE("Resource removed: {}", change.path);
            if (LuaConfig::UnloadDirectory(change.path) > 0) {
                luaStateChanged = true;
            }
        } else if (change.action == ChangeAction::Added || change.action == ChangeAction::Modified) {
            if (std::filesystem::exists(change.path, ec) && std::filesystem::is_directory(change.path, ec)) {
                LOG_PACKAGE_TRACE("Directory added/modified: {}", change.path);
                LuaConfig::SyncManifests(change.path);
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         change.path, std::filesystem::directory_options::skip_permission_denied, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file(ec)) continue;
                    if (IsLuaFile(entry.path().string())) {
                        std::string normPath = entry.path().lexically_normal().string();
                        if (processedLua.insert(normPath).second) {
                            LuaConfig::ParseFile(normPath);
                            luaStateChanged = true;
                        }
                    }
                }
            }
        }
    }

    if (luaStateChanged) {
        Hooks_Package::NotifyLicenseChanged();
        CloudRedirectHost::SyncAppSet();
        LOG_PACKAGE_DEBUG("Lua refresh completed");
    }
}

void WatcherThread() {
    const size_t numDirs = g_watchDirs.size();
    std::vector<OSTPlatform::DirectoryWatch::Watch> watches(numDirs);
    std::vector<OSTPlatform::DirectoryWatch::Watch*> watchPtrs(numDirs, nullptr);

    for (size_t i = 0; i < numDirs; ++i) {
        if (!watches[i].Open(g_watchDirs[i], 65536, true)) {
            LOG_PACKAGE_WARN("Failed to open Lua watch directory: {}", g_watchDirs[i]);
            continue;
        }
        if (!watches[i].IssueRead()) {
            watches[i].Cancel();
            continue;
        }

        watchPtrs[i] = &watches[i];
        LOG_PACKAGE_DEBUG("Watching Lua directory: {}", g_watchDirs[i]);
    }

    bool allFailed = true;
    for (auto* watch : watchPtrs) {
        if (watch && watch->IsOpen()) {
            allFailed = false;
            break;
        }
    }
    if (allFailed) {
        LOG_PACKAGE_WARN("No Lua watch directories could be opened");
        return;
    }

    auto drainEvent = [&](size_t idx,
                          std::unordered_map<std::string, ChangeAction>& accumulated,
                          std::vector<std::string>& order) {
        auto* watch = idx < watchPtrs.size() ? watchPtrs[idx] : nullptr;
        if (!watch || !watch->IsOpen()) return;

        MergeChanges(accumulated, order, ToFileChanges(g_watchDirs[idx], watch->Drain()));
        watch->IssueRead();
    };

    while (g_running) {
        auto waitResult = OSTPlatform::DirectoryWatch::WaitAny(watchPtrs, 1000);

        if (!g_running) break;
        if (waitResult.status == OSTPlatform::DirectoryWatch::WaitStatus::Timeout) continue;
        if (waitResult.status != OSTPlatform::DirectoryWatch::WaitStatus::Signaled ||
            waitResult.index >= numDirs) {
            continue;
        }

        std::unordered_map<std::string, ChangeAction> accumulated;
        std::vector<std::string> order;

        drainEvent(waitResult.index, accumulated, order);

        while (g_running) {
            auto debounceResult = OSTPlatform::DirectoryWatch::WaitAny(watchPtrs, kDebounceMs);
            if (!g_running) break;
            if (debounceResult.status == OSTPlatform::DirectoryWatch::WaitStatus::Timeout) break;
            if (debounceResult.status != OSTPlatform::DirectoryWatch::WaitStatus::Signaled ||
                debounceResult.index >= numDirs) {
                break;
            }
            drainEvent(debounceResult.index, accumulated, order);
        }

        if (!order.empty()) {
            ProcessChanges(FlattenChanges(accumulated, order));
        }
    }

    for (auto& watch : watches) {
        watch.Cancel();
    }
    LOG_PACKAGE_DEBUG("Lua watcher stopped");
}

} // namespace

void Start(const std::vector<std::string>& directories) {
    if (g_running.exchange(true)) {
        LOG_PACKAGE_WARN("Lua watcher already running");
        return;
    }

    std::vector<std::string> uniqueDirs;
    std::unordered_set<std::string> seen;
    for (const auto& d : directories) {
        std::string norm = std::filesystem::path(d).lexically_normal().string();
        if (seen.insert(norm).second) {
            uniqueDirs.push_back(norm);
        }
    }

    g_watchDirs = std::move(uniqueDirs);
    g_watcherThread = std::thread(WatcherThread);
}

void Stop() {
    if (!g_running) return;
    g_running = false;
    if (g_watcherThread.joinable()) {
        g_watcherThread.join();
    }
}

} // namespace LuaFileWatcher
