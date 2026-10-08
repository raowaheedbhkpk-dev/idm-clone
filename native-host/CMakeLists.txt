cmake_minimum_required(VERSION 3.20)
project(idm_clone_host VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "idm-clone-host targets Windows 10/11 x64 only (WinHTTP, Win32).")
endif()

if(MSVC)
  # /MT: statically link the C++ runtime so the host runs on machines without the
  # Visual C++ redistributable (Debug builds use /MTd).
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  # Treat nlohmann/json as a system header so /W4 only reports our own code.
  set(CMAKE_INCLUDE_SYSTEM_FLAG_CXX "/external:I ")
endif()

set(HOST_SOURCES
  src/main.cpp
  src/Logger.cpp
  src/FileUtils.cpp
  src/NativeMessaging.cpp
  src/HttpRangeClient.cpp
  src/SegmentedDownloader.cpp
  src/DownloadManager.cpp
  src/FfmpegMerger.cpp
  src/YtDlpWrapper.cpp
)

add_executable(idm-clone-host ${HOST_SOURCES})

target_include_directories(idm-clone-host
  PRIVATE src
  SYSTEM PRIVATE third_party)

target_compile_definitions(idm-clone-host PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00        # Windows 10: WinHTTP automatic proxy, job objects, etc.
  WINVER=0x0A00
  _CRT_SECURE_NO_WARNINGS)

if(MSVC)
  target_compile_options(idm-clone-host PRIVATE /W4 /permissive- /utf-8 /EHsc /external:W0)
else()
  target_compile_options(idm-clone-host PRIVATE -Wall -Wextra)
  target_link_options(idm-clone-host PRIVATE -static)
endif()

# winhttp: HTTP client. shell32: SHGetKnownFolderPath/SHCreateDirectoryEx/ShellExecute.
# ole32: CoTaskMemFree for SHGetKnownFolderPath results.
target_link_libraries(idm-clone-host PRIVATE winhttp shell32 ole32)

# Gather everything needed for installation next to the exe:
#   <build>/dist/idm-clone-host.exe, com.idmclone.host.json, third_party/nlohmann/json.hpp
set(DIST_DIR "${CMAKE_BINARY_DIR}/dist")
add_custom_command(TARGET idm-clone-host POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/third_party/nlohmann"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:idm-clone-host>" "${DIST_DIR}/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/manifest/com.idmclone.host.json" "${DIST_DIR}/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp" "${DIST_DIR}/third_party/nlohmann/"
  COMMENT "Copying host exe, native messaging manifest and nlohmann/json.hpp to ${DIST_DIR}")
