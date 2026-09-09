cmake_minimum_required(VERSION 3.20)
project(OpenSteamToolTools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)

add_executable(ipc_codegen
    ipc_codegen/ipc_codegen.cpp
)
target_compile_features(ipc_codegen PRIVATE cxx_std_20)


add_executable(extract_tickets
    extract_tickets/extract_tickets.cpp
)
target_compile_features(extract_tickets PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(extract_tickets PRIVATE /utf-8)
endif()
target_link_libraries(extract_tickets PRIVATE
    advapi32
)

if(WIN32)
    add_executable(ost-Injector
        Injector/Injector.cpp
        Injector/Injector.h
        Injector/ost-Injector.rc
    )
    target_compile_features(ost-Injector PRIVATE cxx_std_20)
    set_target_properties(ost-Injector PROPERTIES
        OUTPUT_NAME "ost-Injector"
        WIN32_EXECUTABLE TRUE
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    )
    target_link_libraries(ost-Injector PRIVATE
        kernel32
        user32
        advapi32
        shell32
    )
    add_custom_command(TARGET ost-Injector POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/CreateAutoInjectTask.bat"
            "$<TARGET_FILE_DIR:ost-Injector>/CreateAutoInjectTask.bat"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/DeleteAutoInjectTask.bat"
            "$<TARGET_FILE_DIR:ost-Injector>/DeleteAutoInjectTask.bat"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/config.ini"
            "$<TARGET_FILE_DIR:ost-Injector>/config.ini"
        COMMENT "Copying portable injector scripts and config template"
    )
endif()

