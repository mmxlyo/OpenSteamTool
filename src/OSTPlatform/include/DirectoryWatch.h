#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace OSTPlatform::DirectoryWatch {

    enum class ChangeAction {
        Added,
        Removed,
        Modified,
        RenamedOldName,
        RenamedNewName,
    };

    struct Change {
        std::string relativePath;
        ChangeAction action = ChangeAction::Modified;
    };

    enum class WaitStatus {
        Signaled,
        Timeout,
        Failed,
    };

    struct WaitResult {
        WaitStatus status = WaitStatus::Failed;
        size_t index = 0;
    };

    class Watch {
    public:
        Watch();
        ~Watch();

        Watch(Watch&&) noexcept;
        Watch& operator=(Watch&&) noexcept;

        Watch(const Watch&) = delete;
        Watch& operator=(const Watch&) = delete;

        bool Open(const std::string& directory, uint32_t bufferSize, bool watchSubtree = false);
        bool IssueRead();
        std::vector<Change> Drain();
        void Cancel();

        bool IsOpen() const;
        const std::string& Directory() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend WaitResult WaitAny(std::span<Watch*> watches, uint32_t timeoutMs);
    };

    WaitResult WaitAny(std::span<Watch*> watches, uint32_t timeoutMs);

} // namespace OSTPlatform::DirectoryWatch
