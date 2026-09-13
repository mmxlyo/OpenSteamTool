# ---------------------------------------------------------------------------
# OSTPlatform — internal static library that owns the OS-specific backends.
#
# Windows-only: the public OSTPlatform include headers stay OS-agnostic, but
# every backend implemented here is Win32 (WinHttp / BCrypt / Detours).
# The top-level CMakeLists returns early on non-Windows, so this file only ever
# runs on WIN32 and needs no platform guard of its own.
#
# Public headers must NOT expose Win32, WinHttp,
# registry, BCrypt, or remote-process primitives — those stay behind this library.
# ---------------------------------------------------------------------------
include(Detours)

add_library(OSTPlatform STATIC
    # Platform-agnostic
    Encoding.cpp
    Numbers.cpp
    Log.cpp

    # Windows backends
    Windows/Dialog.cpp
    Windows/Detour.cpp
    Windows/DirectoryWatch.cpp
    Windows/DynamicLibrary.cpp
    Windows/Hash.cpp
    Windows/Http.cpp
    Windows/Memory.cpp
    Windows/ByteSearch.cpp
    Windows/PE.cpp
    Windows/Process.cpp
    Windows/RemoteProcess.cpp
    Windows/SteamCredentialStore.cpp
    Windows/Thread.cpp
    Windows/Trap.cpp
)

target_include_directories(OSTPlatform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..   # src/ — for cross-module private headers
)

target_link_libraries(OSTPlatform PRIVATE
    winhttp
    Bcrypt
    detours
)

target_compile_definitions(OSTPlatform
    PUBLIC
        NOMINMAX
    PRIVATE
        $<$<CONFIG:Debug>:OPENSTEAMTOOL_LOGGING_ENABLED>
)
