// DownloadManager.h - job queue, scheduling, persistence and progress events.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "HttpRangeClient.h"
#include "SegmentedDownloader.h"
#include "nlohmann/json.hpp"

enum class JobState { Queued, Downloading, Paused, Merging, Completed, Failed };
enum class JobKind {
  Direct,   // one URL -> one file, multi-connection
  MergeAv,  // separate video + audio URLs (e.g. YouTube DASH), merged with ffmpeg
  YtDlp     // delegated to yt-dlp.exe (page URLs, HLS/DASH manifests, ciphered YouTube)
};

const char* JobStateName(JobState state);
const char* JobKindName(JobKind kind);

struct Settings {
  int maxConcurrent = 3;
  int segments = 8;
  std::string defaultFolder;
  std::string cookiesFromBrowser;  // passed to yt-dlp as --cookies-from-browser (optional)
};

// What the browser extension asks for.
struct StartRequest {
  JobKind kind = JobKind::Direct;
  std::string url;             // direct URL, video URL, or page/manifest URL
  std::string audioUrl;        // MergeAv only
  std::string title;           // display name; base of the file name for MergeAv/YtDlp
  std::string fileName;        // Direct only: explicit name (optional)
  std::string folder;          // optional, defaults to Settings::defaultFolder
  int segments = 0;            // 0 = Settings::segments
  std::string outputExt = "mp4";
  std::string formatSelector;  // YtDlp only
  http::RequestHeaders headers;
};

struct DownloadJob;  // defined in DownloadManager.cpp

class DownloadManager {
 public:
  // Called for state changes and progress ticks. May be called from any thread,
  // never while the manager's internal mutex is held.
  using EventSink = std::function<void(const nlohmann::json&)>;

  explicit DownloadManager(EventSink sink);
  ~DownloadManager();
  DownloadManager(const DownloadManager&) = delete;
  DownloadManager& operator=(const DownloadManager&) = delete;

  // Restores jobs.json, starts the 500 ms progress ticker and the scheduler.
  void Start();
  // Pauses everything, waits for worker threads, persists. Safe to call twice.
  void Shutdown();

  bool AddJob(const StartRequest& request, std::string& jobId, std::string& error);
  bool Pause(const std::string& id, std::string& error);
  bool Resume(const std::string& id, std::string& error);
  // Stops the job if running and removes it. Unfinished data is deleted; a
  // completed file is never deleted, only the list entry.
  bool Remove(const std::string& id, std::string& error);
  void PauseAll();

  bool GetJob(const std::string& id, nlohmann::json& out) const;
  nlohmann::json ListJobs() const;
  // Completed -> final file, otherwise the target folder. Empty if unknown id.
  std::string PathForJob(const std::string& id, bool& isFile) const;

  Settings GetSettings() const;
  bool SetSettings(const nlohmann::json& patch, std::string& error);
  nlohmann::json SettingsToJson() const;

 private:
  enum class Outcome { Completed, Paused, Cancelled, Failed };

  void RunJob(std::shared_ptr<DownloadJob> job);
  Outcome RunDirect(const std::shared_ptr<DownloadJob>& job, std::string& error);
  Outcome RunMergeAv(const std::shared_ptr<DownloadJob>& job, std::string& error);
  Outcome RunYtDlp(const std::shared_ptr<DownloadJob>& job, std::string& error);
  Outcome RunSegmented(const std::shared_ptr<DownloadJob>& job, const std::string& url,
                       const std::string& fileName, double base, double span, bool adoptName,
                       std::string& error);
  void Finish(const std::shared_ptr<DownloadJob>& job, Outcome outcome, const std::string& error);
  void OnSegmentProgress(const std::shared_ptr<DownloadJob>& job, const ProgressInfo& info,
                         double base, double span, bool adoptName);

  // *Locked functions require mutex_ to be held by the caller.
  void ScheduleLocked(std::vector<nlohmann::json>& events);
  void PersistLocked();
  nlohmann::json StateEventLocked(const DownloadJob& job) const;
  void Emit(const std::vector<nlohmann::json>& events);
  void LoadFromDisk();
  void TickerLoop();

  EventSink sink_;
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<DownloadJob>> jobs_;  // ids sort chronologically
  Settings settings_;
  std::string storePath_;
  uint64_t idCounter_ = 0;
  int activeThreads_ = 0;
  bool shuttingDown_ = false;
  bool started_ = false;
  std::condition_variable threadsCv_;

  std::thread ticker_;
  std::mutex tickerMutex_;
  std::condition_variable tickerCv_;
  bool tickerStop_ = false;
};
