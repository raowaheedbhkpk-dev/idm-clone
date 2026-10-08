// Logger.h - thread-safe file logger.
//
// IMPORTANT: a Chrome native messaging host must NEVER write anything except
// framed messages to stdout, and stderr is discarded by Chrome. All diagnostics
// therefore go to %APPDATA%\idm-clone\host.log.
#pragma once

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
 public:
  static Logger& Instance();

  // Opens (appending) the log file; rotates it to ".old" when it exceeds 5 MB.
  void Init(const std::string& logFilePathUtf8);
  void Shutdown();
  void SetMinLevel(LogLevel level);

  void Log(LogLevel level, const std::string& message);

  // Logs "<api> failed: <context> (error N: text)". The error code must be
  // captured with GetLastError() *before* anything else can clobber it - use
  // the LOG_WIN32 macro below, which does exactly that.
  void LogWin32(const char* api, const std::string& context, DWORD errorCode);

 private:
  Logger() = default;
  ~Logger();
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  std::mutex mutex_;
  FILE* file_ = nullptr;
  LogLevel minLevel_ = LogLevel::Info;
};

#define LOG_DEBUG(msg) Logger::Instance().Log(LogLevel::Debug, (msg))
#define LOG_INFO(msg) Logger::Instance().Log(LogLevel::Info, (msg))
#define LOG_WARN(msg) Logger::Instance().Log(LogLevel::Warn, (msg))
#define LOG_ERROR(msg) Logger::Instance().Log(LogLevel::Error, (msg))

// Capture GetLastError() first, then evaluate the (possibly allocating)
// context expression, so the error code can't be overwritten.
#define LOG_WIN32(api, context)                                      \
  do {                                                               \
    const DWORD idm_last_error_ = ::GetLastError();                  \
    Logger::Instance().LogWin32((api), (context), idm_last_error_);  \
  } while (0)
