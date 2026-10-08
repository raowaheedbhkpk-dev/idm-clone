#include "Logger.h"

#include <share.h>

#include "FileUtils.h"

namespace {
constexpr unsigned long long kMaxLogBytes = 5ull * 1024 * 1024;

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
  }
  return "?????";
}
}  // namespace

Logger& Logger::Instance() {
  static Logger instance;
  return instance;
}

Logger::~Logger() { Shutdown(); }

void Logger::Init(const std::string& logFilePathUtf8) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_ != nullptr) return;

  const std::wstring path = fileutils::Utf8ToWide(logFilePathUtf8);

  // Rotate: host.log -> host.log.old when too big.
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    const unsigned long long size =
        (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size > kMaxLogBytes) {
      const std::wstring old = path + L".old";
      MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
  }

  // _SH_DENYWR lets us (and a user's text editor) read while we append.
  file_ = _wfsopen(path.c_str(), L"ab", _SH_DENYWR);
  // If the log can't be opened we silently run without logging; there is no
  // other channel (stdout belongs to the protocol).
}

void Logger::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

void Logger::SetMinLevel(LogLevel level) {
  std::lock_guard<std::mutex> lock(mutex_);
  minLevel_ = level;
}

void Logger::Log(LogLevel level, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_ == nullptr || level < minLevel_) return;

  SYSTEMTIME st{};
  GetLocalTime(&st);
  std::fprintf(file_, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] [t%lu] %s\n", st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               LevelName(level), static_cast<unsigned long>(GetCurrentThreadId()),
               message.c_str());
  std::fflush(file_);
}

void Logger::LogWin32(const char* api, const std::string& context, DWORD errorCode) {
  std::string text = std::string(api) + " failed";
  if (!context.empty()) text += ": " + context;
  text += " (" + fileutils::LastErrorString(errorCode) + ")";
  Log(LogLevel::Error, text);
}
