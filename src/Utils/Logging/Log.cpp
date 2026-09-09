#include "Log.h"

#ifdef OPENSTEAMTOOL_LOGGING_ENABLED

#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Log.h"
#include "Utils/Config/Config.h"
#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>

namespace {
    std::atomic_bool g_mainReady{false};

    spdlog::level::level_enum ToSpdlog(Config::LogLevel lv) {
        switch (lv) {
        case Config::LogLevel::Trace: return spdlog::level::trace;
        case Config::LogLevel::Debug: return spdlog::level::debug;
        case Config::LogLevel::Info:  return spdlog::level::info;
        case Config::LogLevel::Warn:  return spdlog::level::warn;
        case Config::LogLevel::Error: return spdlog::level::err;
        default: return spdlog::level::info;
        }
    }

    std::shared_ptr<spdlog::logger> MakeLogger(const std::string& dir,
                                                const std::string& name) {
        auto path = OSTPlatform::Encoding::PathFromUtf8(dir) / (name + ".log");
        std::string filename;
#if defined(_WIN32)
        try {
            filename = path.string();
        } catch (...) {
            filename = OSTPlatform::Encoding::PathToUtf8(path);
        }
#else
        filename = OSTPlatform::Encoding::PathToUtf8(path);
#endif
        auto logger = spdlog::basic_logger_mt(name, filename, /*truncate=*/true);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [tid=%t] [%s:%# %!()] %v");
        logger->flush_on(spdlog::level::trace);
        return logger;
    }

    spdlog::level::level_enum ToSpdlog(OSTPlatform::Log::Level lv) {
        switch (lv) {
        case OSTPlatform::Log::Level::Trace: return spdlog::level::trace;
        case OSTPlatform::Log::Level::Debug: return spdlog::level::debug;
        case OSTPlatform::Log::Level::Info:  return spdlog::level::info;
        case OSTPlatform::Log::Level::Warn:  return spdlog::level::warn;
        case OSTPlatform::Log::Level::Error: return spdlog::level::err;
        }
        return spdlog::level::info;
    }

    void SetLoggerLevel(const std::shared_ptr<spdlog::logger>& logger,
                        spdlog::level::level_enum level) {
        if (logger) logger->set_level(level);
    }

    void SetAllLoggerLevels(spdlog::level::level_enum level) {
        SetLoggerLevel(Log::Main, level);
        #define OST_MOD(v, n) SetLoggerLevel(Log::v, level);
        #include "LogModules.def"
        #undef OST_MOD
    }

    // Forwards every OSTPlatform log line into the host's single "platform"
    // logger. The message is already formatted; the originating file/line/
    // function are reconstructed into spdlog's source_loc so the log pattern
    // still shows where in the platform library the line came from.
    void PlatformLogSink(OSTPlatform::Log::Level level,
                         const OSTPlatform::Log::Source& src,
                         std::string_view msg) {
        if (!Log::Platform) return;
        Log::Platform->log(
            spdlog::source_loc{src.file ? src.file : "", src.line,
                               src.function ? src.function : ""},
            ToSpdlog(level), "{}", msg);
    }
}

namespace Log {

    void Init(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
        bool expected = false;
        if (!g_mainReady.compare_exchange_strong(expected, true)) return;

        try {
            auto dir = OSTPlatform::DynamicLibrary::GetModuleDirectory(selfModule);
            if (dir.empty()) dir = ".";
            auto logDirPath = dir / "opensteamtool";
            std::error_code ec;
            std::filesystem::create_directories(logDirPath, ec);
            auto logDir = OSTPlatform::Encoding::PathToUtf8(logDirPath);
            Main = MakeLogger(logDir, "main");
            Main->set_level(spdlog::level::trace);  // early boot: log everything
            LOG_INFO("Log initialised at {}", logDir);
        } catch (const std::exception&) {
            g_mainReady.store(false);
        }
    }

    void InitModules() {
        if (!g_mainReady) return;

        try {
            const std::string logDir = Config::GetLogDir();
            const Config::LogLevel configLevel = Config::GetLogLevel();
            auto lvl = ToSpdlog(configLevel);

            std::error_code ec;
            std::filesystem::create_directories(OSTPlatform::Encoding::PathFromUtf8(logDir), ec);
            SetLoggerLevel(Main, lvl);

            auto initOne = [&](std::shared_ptr<spdlog::logger>& logger, const char* name) {
                logger = MakeLogger(logDir, name);
                logger->set_level(lvl);
            };

            #define OST_MOD(v, n) initOne(v, n);
            #include "LogModules.def"
            #undef OST_MOD

            LOG_INFO("Module loggers initialised at {} level={}", logDir,
                     static_cast<int>(configLevel));
        } catch (const std::exception& e) {
            LOG_WARN("InitModules failed: {}", e.what());
        }
    }

    void ApplyConfigLevel() {
        if (!g_mainReady) return;

        const Config::LogLevel configLevel = Config::GetLogLevel();
        SetAllLoggerLevels(ToSpdlog(configLevel));
        LOG_INFO("Log level updated to {}", static_cast<int>(configLevel));
    }

    void InstallPlatformLogSink() {
        OSTPlatform::Log::SetSink(&PlatformLogSink);
        LOG_INFO("OSTPlatform log sink installed -> platform module");
    }

}

#endif  // OPENSTEAMTOOL_LOGGING_ENABLED
