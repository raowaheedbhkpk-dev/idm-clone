// FileUtils.h - UTF-8 <-> UTF-16 conversion, paths, disk helpers, RAII handle
// wrappers and a hidden-window child process runner.
//
// Policy: every string inside the program is UTF-8 (std::string). We convert to
// UTF-16 only at the Win32 API boundary.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fileutils {

// ---------------------------------------------------------------- RAII ----
// unique_ptr<void, Deleter> with Deleter::pointer = HANDLE gives us a move-only
// owner that treats both nullptr and INVALID_HANDLE_VALUE as "empty".
struct HandleDeleter {
  using pointer = HANDLE;
  void operator()(HANDLE h) const noexcept {
    if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
};
using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

// ----------------------------------------------------------- strings ----
std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// "error 5: Access is denied." Works for system and WinHTTP (12xxx) codes.
std::string LastErrorString(DWORD errorCode);

std::string UrlDecode(const std::string& s);

// Replaces characters illegal in Windows file names, strips trailing dots and
// spaces, avoids reserved device names (CON, NUL, COM1...) and limits length.
std::string SanitizeFileName(const std::string& name);
std::string FileNameFromUrl(const std::string& url);

// ------------------------------------------------------------- paths ----
std::string JoinPath(const std::string& dir, const std::string& name);
std::string ExeDir();
std::string AppDataDir();           // %APPDATA%\idm-clone (created on demand)
std::string DefaultDownloadFolder();  // the user's Downloads folder
bool EnsureDirectory(const std::string& dir, std::string& error);

// ------------------------------------------------------------- files ----
bool FileExists(const std::string& path);
uint64_t FileSizeOrZero(const std::string& path);
bool DeleteFileIfExists(const std::string& path);
bool FreeDiskSpace(const std::string& dir, uint64_t& freeBytes);

// Returns a full path "dir\name", or "dir\name (1).ext", "(2)", ... if the file
// or a ".part0" left from an unfinished download already exists.
std::string UniqueFilePath(const std::string& dir, const std::string& name);

bool ReadWholeFile(const std::string& path, std::string& contents);
// Writes to "<path>.tmp", flushes, then atomically replaces <path>.
bool WriteFileAtomic(const std::string& path, const std::string& contents, std::string& error);

// Looks next to the host exe, then in C:\Program Files\IDMClone, then PATH.
// Returns "" if not found.
std::string FindTool(const std::string& exeName);

// ---------------------------------------------------------- processes ----
// Quotes one command line argument following the MSVCRT/CommandLineToArgvW rules.
std::wstring QuoteArg(const std::wstring& arg);

// Splits a byte stream into lines on '\n' or '\r' (ffmpeg and yt-dlp redraw their
// progress lines with '\r'). Empty lines are dropped.
class LineSplitter {
 public:
  explicit LineSplitter(std::function<void(const std::string&)> onLine)
      : onLine_(std::move(onLine)) {}
  void Feed(const std::string& chunk);
  void Flush();

 private:
  std::function<void(const std::string&)> onLine_;
  std::string pending_;
};

struct ProcessResult {
  bool launched = false;
  bool cancelled = false;
  DWORD exitCode = 0;
  std::string error;
};

using OutputCallback = std::function<void(const std::string&)>;

// Runs a console program with no window, stdin = NUL and stdout/stderr captured
// through two anonymous pipes. The child (and its descendants, e.g. the ffmpeg
// that yt-dlp starts) lives in a kill-on-close Job Object, so cancelling - or the
// host dying - never leaves orphans. Callbacks run on reader threads.
ProcessResult RunProcess(const std::wstring& commandLine, const OutputCallback& onStdout,
                         const OutputCallback& onStderr, const std::atomic<bool>* cancel);

}  // namespace fileutils
