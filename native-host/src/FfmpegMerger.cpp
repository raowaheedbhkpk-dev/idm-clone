#include "FfmpegMerger.h"

#include <cstdio>
#include <deque>
#include <mutex>

#include "FileUtils.h"
#include "Logger.h"

namespace {

// Parses "HH:MM:SS.xx" starting at `text`; returns seconds or -1.
double ParseClock(const char* text) {
  int h = 0, m = 0;
  double s = 0;
  if (std::sscanf(text, "%d:%d:%lf", &h, &m, &s) == 3) return h * 3600.0 + m * 60.0 + s;
  return -1;
}

}  // namespace

bool FfmpegMerger::Merge(const std::string& videoPath, const std::string& audioPath,
                         const std::string& outputPath, const ProgressCallback& onProgress,
                         const std::atomic<bool>* cancel, std::string& error) {
  const std::string ffmpeg = fileutils::FindTool("ffmpeg.exe");
  if (ffmpeg.empty()) {
    error = "ffmpeg.exe not found. Put it next to idm-clone-host.exe (C:\\Program Files\\IDMClone).";
    LOG_ERROR(error);
    return false;
  }

  using fileutils::QuoteArg;
  using fileutils::Utf8ToWide;
  // -nostdin: never read the console. -map picks the first video and first audio
  // stream so stray cover-art/data streams cannot break the mux.
  std::wstring cmd = QuoteArg(Utf8ToWide(ffmpeg)) + L" -y -nostdin -hide_banner -i " +
                     QuoteArg(Utf8ToWide(videoPath)) + L" -i " + QuoteArg(Utf8ToWide(audioPath)) +
                     L" -map 0:v:0 -map 1:a:0 -c copy " + QuoteArg(Utf8ToWide(outputPath));
  LOG_INFO("ffmpeg merge: " + fileutils::WideToUtf8(cmd));

  std::mutex mutex;
  double durationSeconds = 0;
  std::deque<std::string> tail;  // last stderr lines, for error messages

  // ffmpeg writes its stats to stderr and redraws the line with '\r'.
  fileutils::LineSplitter splitter([&](const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    tail.push_back(line);
    if (tail.size() > 6) tail.pop_front();

    const size_t dur = line.find("Duration:");
    if (dur != std::string::npos) {
      const double d = ParseClock(line.c_str() + dur + 9);
      if (d > durationSeconds) durationSeconds = d;  // the longer input defines 100%
      return;
    }
    const size_t t = line.find("time=");
    if (t != std::string::npos && durationSeconds > 0 && onProgress) {
      const double now = ParseClock(line.c_str() + t + 5);
      if (now >= 0) {
        double pct = now / durationSeconds * 100.0;
        if (pct > 99.0) pct = 99.0;  // 100 is reported only after a clean exit
        onProgress(pct);
      }
    }
  });

  const fileutils::ProcessResult run = fileutils::RunProcess(
      cmd, nullptr, [&](const std::string& chunk) { splitter.Feed(chunk); }, cancel);
  splitter.Flush();

  if (!run.launched) {
    error = run.error;
    return false;
  }
  if (run.cancelled) {
    fileutils::DeleteFileIfExists(outputPath);
    error = "Merge cancelled";
    return false;
  }
  if (run.exitCode != 0) {
    fileutils::DeleteFileIfExists(outputPath);
    std::string last;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!tail.empty()) last = tail.back();
    }
    error = "ffmpeg failed (exit code " + std::to_string(run.exitCode) + ")" + (last.empty() ? "" : ": " + last);
    LOG_ERROR(error);
    return false;
  }

  if (onProgress) onProgress(100.0);
  // Temp files are only removed after ffmpeg reported success.
  fileutils::DeleteFileIfExists(videoPath);
  fileutils::DeleteFileIfExists(audioPath);
  return true;
}
