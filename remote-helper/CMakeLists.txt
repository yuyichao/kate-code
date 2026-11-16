# kate-code-helper: Kate Code's helper on the hosts of remote folders.
#
# A project of its own, not part of the plugin's build: it is built for each
# host platform by .github/workflows/remote-helper.yml and released
# separately (tags remote-helper-v<version>); Kate Code downloads the build
# for a host. Only the C++ standard library and the system's APIs: no Qt or
# KDE Frameworks, so it links statically and runs on any host.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build

cmake_minimum_required(VERSION 3.16)
# The helper's version (the protocol has its own: Server.h ProtocolVersion).
project(kate-code-helper VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KATE_CODE_HELPER_STATIC "Link statically (Linux release builds: with musl)" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# MSVC: the static C/C++ runtime, so no Visual C++ redistributable is needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

configure_file(src/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/Version.h @ONLY)

set(SOURCES
    src/main.cpp
    src/Codec.cpp
    src/FilesCommon.cpp
    src/Json.cpp
    src/Server.cpp
    src/Watcher.cpp
)
if(WIN32)
    enable_language(RC)
    configure_file(src/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
    list(APPEND SOURCES src/FilesWindows.cpp src/WatcherWindows.cpp ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SOURCES src/FilesPosix.cpp src/WatcherInotify.cpp)
else()
    list(APPEND SOURCES src/FilesPosix.cpp src/WatcherPolling.cpp)
endif()

add_executable(kate-code-helper ${SOURCES})
target_include_directories(kate-code-helper PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

find_package(Threads REQUIRED)
target_link_libraries(kate-code-helper PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(kate-code-helper PRIVATE /W4 /utf-8 /permissive-)
    target_compile_definitions(kate-code-helper PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(kate-code-helper PRIVATE -Wall -Wextra)
endif()
if(MINGW)
    target_link_options(kate-code-helper PRIVATE -static)
endif()
if(KATE_CODE_HELPER_STATIC AND NOT WIN32 AND NOT APPLE)
    target_link_options(kate-code-helper PRIVATE -static)
endif()
if(APPLE)
    # Apple Silicon runs only signed code. The linker signs arm64 output ad
    # hoc, but not x86_64 slices: sign the finished (universal) program.
    add_custom_command(TARGET kate-code-helper POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_FILE:kate-code-helper>
        VERBATIM)
endif()

install(TARGETS kate-code-helper RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # Drives the built helper through the protocol only (PROTOCOL.md),
        # so other implementations can be checked with it too.
        add_test(NAME protocol
            COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol.py $<TARGET_FILE:kate-code-helper>)
        set_tests_properties(protocol PROPERTIES TIMEOUT 120)
    endif()
endif()
