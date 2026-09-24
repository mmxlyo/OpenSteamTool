#pragma once

#include <cstdint>
#include <string>

namespace OSTPlatform::Http {

    struct Result {
        std::string body;
        uint32_t status = 0;
        // True once WinHTTP produced a response; callers must still check status for 2xx.
        bool ok = false;
    };

    constexpr size_t kDefaultMaxResponseBodyBytes = 16 * 1024 * 1024; // 16 MiB

    Result Execute(const wchar_t* method,
                   const char* url,
                   const void* reqBody = nullptr,
                   uint32_t reqBodyLen = 0,
                   const wchar_t* headers = nullptr,
                   uint32_t timeoutResolve = 5000,
                   uint32_t timeoutConnect = 5000,
                   uint32_t timeoutSend = 10000,
                   uint32_t timeoutRecv = 10000,
                   size_t maxResponseBytes = kDefaultMaxResponseBodyBytes);

} // namespace OSTPlatform::Http
