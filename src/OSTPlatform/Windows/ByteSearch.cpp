#include "include/ByteSearch.h"

#include "Windows/Handles.h"
#include "include/Log.h"
#include "include/Stopwatch.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <vector>

namespace OSTPlatform::ByteSearch {
namespace {

class FixedPatternScanner {
public:
    explicit FixedPatternScanner(std::span<const uint8_t> pattern)
        : pattern_(pattern.begin(), pattern.end()) {
        const size_t patternSize = pattern_.size();
        skip_.fill(patternSize == 0 ? 1 : patternSize);
        if (patternSize <= 1) return;

        for (size_t i = 0; i + 1 < patternSize; ++i) {
            skip_[pattern_[i]] = patternSize - 1 - i;
        }
    }

    std::optional<size_t> Find(std::span<const uint8_t> bytes) const {
        const size_t patternSize = pattern_.size();
        if (patternSize == 0 || bytes.size() < patternSize) return std::nullopt;

        size_t offset = 0;
        while (offset <= bytes.size() - patternSize) {
            const uint8_t tail = bytes[offset + patternSize - 1];
            if (tail == pattern_[patternSize - 1]) {
                size_t i = patternSize - 1;
                while (i > 0 && bytes[offset + i - 1] == pattern_[i - 1]) {
                    --i;
                }
                if (i == 0) return offset;
            }

            offset += skip_[tail];
        }

        return std::nullopt;
    }

    size_t PatternSize() const { return pattern_.size(); }

private:
    std::vector<uint8_t> pattern_;
    std::array<size_t, 256> skip_{};
};

// Open for overlapped (async) sequential reads. FILE_FLAG_SEQUENTIAL_SCAN biases
// the cache manager toward aggressive readahead and evict-behind; FILE_FLAG_OVERLAPPED
// lets us queue the next chunk's read before scanning the current one so disk and
// CPU work proceed concurrently. Sharing is permissive: the module is usually mapped.
Windows::UniqueFileHandle OpenForOverlappedSequentialRead(const std::filesystem::path& path) {
    return Windows::UniqueFileHandle(::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED,
        nullptr));
}

} // namespace

std::optional<size_t> Find(std::span<const uint8_t> bytes, std::span<const uint8_t> pattern) {
    const Stopwatch scanTimer;
    const auto result = FixedPatternScanner(pattern).Find(bytes);
    OSTP_LOG_DEBUG("ByteSearch::Find bytes={} pattern={} matched={} elapsed_ms={:.3f}",
                   bytes.size(), pattern.size(), result.has_value(), scanTimer.ElapsedMs());
    return result;
}

std::optional<uint64_t> FindInFile(
    const std::filesystem::path& path,
    std::span<const uint8_t> pattern,
    size_t chunkBytes) {
    // A whole-file scan is just a range scan over [0, EOF). FindInFileRange clamps
    // the requested size to the real file size, so a max sentinel means "to the end
    // of the file" — this routes the legacy path through the same overlapped
    // double-buffered engine (read of chunk i+1 overlaps the BMH scan of chunk i)
    // and keeps a single I/O implementation.
    return FindInFileRange(
        path, 0, (std::numeric_limits<uint64_t>::max)(), pattern, chunkBytes);
}

std::optional<uint64_t> FindInFileRange(
    const std::filesystem::path& path,
    uint64_t offset,
    uint64_t size,
    std::span<const uint8_t> pattern,
    size_t chunkBytes) {
    const Stopwatch totalTimer;
    FixedPatternScanner scanner(pattern);
    const size_t patternSize = scanner.PatternSize();
    if (patternSize == 0 || size == 0) return std::nullopt;

    Windows::UniqueFileHandle file = OpenForOverlappedSequentialRead(path);
    if (!file) {
        OSTP_LOG_DEBUG("ByteSearch::FindInFileRange open failed path={} offset=0x{:X} size={} err={}",
                       path.string(), offset, size, ::GetLastError());
        return std::nullopt;
    }

    LARGE_INTEGER sizeLi{};
    if (!::GetFileSizeEx(file.get(), &sizeLi) || sizeLi.QuadPart <= 0) {
        OSTP_LOG_DEBUG("ByteSearch::FindInFileRange empty/invalid file path={} offset=0x{:X} size={}",
                       path.string(), offset, size);
        return std::nullopt;
    }

    const uint64_t fileSize = static_cast<uint64_t>(sizeLi.QuadPart);
    if (offset >= fileSize) {
        OSTP_LOG_DEBUG("ByteSearch::FindInFileRange offset past EOF path={} offset=0x{:X} file_size={}",
                       path.string(), offset, fileSize);
        return std::nullopt;
    }

    const uint64_t rangeEnd = offset + (std::min)(size, fileSize - offset);

    chunkBytes = (std::max)(chunkBytes, patternSize);
    // Consecutive chunks overlap by (patternSize - 1) bytes: a match straddling a
    // chunk boundary runs off chunk i's tail (BMH won't see it there) but is fully
    // contained at the head of chunk i+1. Because the overlap is re-read from disk
    // rather than carry-copied from the previous buffer, the read of chunk i+1 has
    // no data dependency on the scan of chunk i — so the two can truly overlap.
    const uint64_t stride = static_cast<uint64_t>(chunkBytes) - (patternSize - 1);

    // Double buffering: while BMH scans chunk i in one buffer, the kernel streams
    // chunk i+1 into the other. make_unique_for_overwrite skips the zero-fill the
    // buffers are about to be overwritten anyway.
    std::unique_ptr<uint8_t[]> buffers[2] = {
        std::make_unique_for_overwrite<uint8_t[]>(chunkBytes),
        std::make_unique_for_overwrite<uint8_t[]>(chunkBytes),
    };
    Windows::UniqueHandle events[2] = {
        Windows::UniqueHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        Windows::UniqueHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr)),
    };
    if (!events[0] || !events[1]) {
        OSTP_LOG_DEBUG("ByteSearch::FindInFileRange event create failed path={} err={}",
                       path.string(), ::GetLastError());
        return std::nullopt;
    }
    OVERLAPPED ov[2]{};

    int pendingSlot = -1;  // slot whose overlapped read is currently in flight
    const auto issueRead = [&](int slot, uint64_t chunkStart, size_t len) -> bool {
        ov[slot] = OVERLAPPED{};
        ov[slot].Offset = static_cast<DWORD>(chunkStart & 0xFFFFFFFFull);
        ov[slot].OffsetHigh = static_cast<DWORD>(chunkStart >> 32);
        ov[slot].hEvent = events[slot].get();
        ::ResetEvent(events[slot].get());
        DWORD got = 0;
        const BOOL ok = ::ReadFile(file.get(), buffers[slot].get(),
                                   static_cast<DWORD>(len), &got, &ov[slot]);
        if (!ok && ::GetLastError() != ERROR_IO_PENDING) return false;
        pendingSlot = slot;
        return true;
    };
    // Drain the in-flight read before tearing buffers down: the kernel may still be
    // writing into a buffer, so cancel and wait for the op to settle first.
    struct PendingDrainGuard {
        HANDLE file = nullptr;
        OVERLAPPED* ov = nullptr;
        int* pendingSlot = nullptr;

        void Drain() {
            if (pendingSlot && *pendingSlot >= 0 && file && ov) {
                ::CancelIoEx(file, &ov[*pendingSlot]);
                DWORD discarded = 0;
                ::GetOverlappedResult(file, &ov[*pendingSlot], &discarded, TRUE);
                *pendingSlot = -1;
            }
        }

        ~PendingDrainGuard() {
            Drain();
        }
    } drainGuard{file.get(), ov, &pendingSlot};

    uint64_t bytesReadTotal = 0;
    size_t chunks = 0;
    double readMs = 0.0;
    double scanMs = 0.0;

    int slot = 0;
    uint64_t curStart = offset;
    {
        const size_t len = static_cast<size_t>(
            (std::min)(static_cast<uint64_t>(chunkBytes), rangeEnd - curStart));
        if (!issueRead(slot, curStart, len)) {
            OSTP_LOG_DEBUG("ByteSearch::FindInFileRange read start failed path={} offset=0x{:X} err={}",
                           path.string(), curStart, ::GetLastError());
            return std::nullopt;
        }
    }

    while (true) {
        const Stopwatch readTimer;
        DWORD got = 0;
        const BOOL ok = ::GetOverlappedResult(file.get(), &ov[slot], &got, TRUE);
        readMs += readTimer.ElapsedMs();
        pendingSlot = -1;  // this slot's read has completed (or errored)
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_HANDLE_EOF) {
                OSTP_LOG_DEBUG("ByteSearch::FindInFileRange read failed path={} offset=0x{:X} err={}",
                               path.string(), curStart, err);
                return std::nullopt;
            }
            got = 0;
        }
        if (got == 0) break;
        bytesReadTotal += got;
        ++chunks;

        const uint64_t curEnd = curStart + got;
        const bool moreChunks = curEnd < rangeEnd;

        // Queue chunk i+1's read BEFORE scanning chunk i so disk and CPU overlap.
        uint64_t nextStart = 0;
        const int nextSlot = slot ^ 1;
        if (moreChunks) {
            nextStart = curStart + stride;
            const size_t nextLen = static_cast<size_t>(
                (std::min)(static_cast<uint64_t>(chunkBytes), rangeEnd - nextStart));
            if (!issueRead(nextSlot, nextStart, nextLen)) {
                OSTP_LOG_DEBUG("ByteSearch::FindInFileRange read start failed path={} offset=0x{:X} err={}",
                               path.string(), nextStart, ::GetLastError());
                return std::nullopt;
            }
        }

        const Stopwatch scanTimer;
        const auto match = scanner.Find(std::span<const uint8_t>(buffers[slot].get(), got));
        scanMs += scanTimer.ElapsedMs();
        if (match) {
            drainGuard.Drain();
            const uint64_t absoluteMatch = curStart + *match;
            OSTP_LOG_DEBUG("ByteSearch::FindInFileRange matched path={} range=[0x{:X},0x{:X}) match=0x{:X} bytes_read={} chunks={} read_ms={:.3f} bmh_ms={:.3f} total_ms={:.3f}",
                           path.string(),
                           offset,
                           rangeEnd,
                           absoluteMatch,
                           bytesReadTotal,
                           chunks,
                           readMs,
                           scanMs,
                           totalTimer.ElapsedMs());
            return absoluteMatch;
        }

        if (!moreChunks) break;
        slot = nextSlot;
        curStart = nextStart;
    }

    OSTP_LOG_DEBUG("ByteSearch::FindInFileRange no match path={} range=[0x{:X},0x{:X}) bytes_read={} chunks={} read_ms={:.3f} bmh_ms={:.3f} total_ms={:.3f}",
                   path.string(),
                   offset,
                   rangeEnd,
                   bytesReadTotal,
                   chunks,
                   readMs,
                   scanMs,
                   totalTimer.ElapsedMs());
    return std::nullopt;
}

} // namespace OSTPlatform::ByteSearch
