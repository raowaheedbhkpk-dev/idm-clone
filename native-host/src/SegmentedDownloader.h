// SegmentedDownloader.h - multi-connection downloader for one file.
//
// Layout on disk while downloading:   <name>.part0 ... <name>.partN-1
// Each segment owns its part file, so resume is simply "ask for the bytes after
// the end of each part". On success the parts are concatenated into <name> and
// deleted.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "HttpRangeClient.h"

struct DownloadOptions {
  std::string url;
  std::string folder;
  std::string fileName;  // optional; detected from the server when empty
  int segments = 8;      // desired connections (1..32)
  http::RequestHeaders headers;
};

enum class DownloadPhase { Downloading, Concatenating };

struct ProgressInfo {
  DownloadPhase phase = DownloadPhase::Downloading;
  uint64_t downloaded = 0;
  uint64_t total = 0;  // 0 when the server did not announce a size
  double speedBytesPerSec = 0;
  double etaSeconds = -1;  // -1 = unknown
  std::string fileName;    // final file name (no folder)
  bool resumable = false;
};

enum class DownloadResult { Completed, Paused, Cancelled, Failed };

class SegmentedDownloader {
 public:
  using ProgressCallback = std::function<void(const ProgressInfo&)>;

  SegmentedDownloader(DownloadOptions options, ProgressCallback onProgress);

  // Blocks the calling thread until the download finishes, is paused/cancelled or
  // fails. Call Pause()/Cancel() from another thread.
  DownloadResult Run();

  void Pause();
  void Cancel();

  const std::string& Error() const { return error_; }
  const std::string& FinalPath() const { return finalPath_; }

  // Deletes <folder>\<fileName>.part0..partN (used when a job is cancelled while
  // not running).
  static void DeletePartFiles(const std::string& folder, const std::string& fileName);

 private:
  struct Segment {
    int index = 0;
    uint64_t start = 0;
    uint64_t end = 0;       // inclusive; UINT64_MAX when the size is unknown
    bool hasLength = true;
    std::string partPath;
    std::atomic<uint64_t> done{0};
    uint64_t Length() const { return end - start + 1; }
  };

  enum class AttemptResult { Done, Retry, Fatal, Stopped };

  bool ShouldStop() const { return pause_.load() || cancel_.load() || failed_.load(); }
  void Fail(const std::string& message);
  bool SleepInterruptible(unsigned milliseconds);
  void Worker(Segment* segment);
  // One connection attempt for one segment; `problem` explains Retry/Fatal.
  AttemptResult AttemptSegment(Segment* segment, http::HttpSession& session,
                               std::vector<char>& buffer, std::string& problem);
  void WorkerExited();
  bool Merge();
  void Emit(DownloadPhase phase, uint64_t downloaded, double speed);

  DownloadOptions options_;
  ProgressCallback onProgress_;

  std::atomic<bool> pause_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> failed_{false};

  std::mutex mutex_;  // guards error_, activeWorkers_
  std::condition_variable cv_;
  int activeWorkers_ = 0;
  std::string error_;

  // Set up by Run() before the workers start.
  http::ProbeResult probe_;
  http::RequestHeaders workerHeaders_;  // options_.headers minus credentials when the final host differs
  std::string finalPath_;
  std::string fileName_;
  std::vector<std::unique_ptr<Segment>> segments_;
  bool ranged_ = false;
  uint64_t total_ = 0;
  std::atomic<uint64_t> downloaded_{0};
};
