#include "DownloadManager.h"

#include <algorithm>
#include <chrono>
#include <ctime>

#include "FfmpegMerger.h"
#include "FileUtils.h"
#include "Logger.h"
#include "YtDlpWrapper.h"

using nlohmann::json;

namespace {
constexpr int kMaxCompletedHistory = 200;
enum StopMode { kRunning = 0, kPauseRequested = 1, kCancelRequested = 2 };

std::string BaseName(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool IsHttpUrl(const std::string& url) {
  return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

std::string JString(const json& j, const char* key, const std::string& fallback = std::string()) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}
double JNumber(const json& j, const char* key, double fallback = 0) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_number()) ? it->get<double>() : fallback;
}

json HeadersToJson(const http::RequestHeaders& h) {
  return {{"userAgent", h.userAgent}, {"referer", h.referer}, {"cookie", h.cookie}, {"extra", h.extra}};
}

http::RequestHeaders HeadersFromJson(const json& j) {
  http::RequestHeaders h;
  if (!j.is_object()) return h;
  h.userAgent = JString(j, "userAgent");
  h.referer = JString(j, "referer");
  h.cookie = JString(j, "cookie");
  const auto extra = j.find("extra");
  if (extra != j.end() && extra->is_object()) {
    for (auto it = extra->begin(); it != extra->end(); ++it) {
      if (it.value().is_string()) h.extra[it.key()] = it.value().get<std::string>();
    }
  }
  return h;
}

bool ParseState(const std::string& s, JobState& out) {
  static const std::pair<const char*, JobState> table[] = {
      {"QUEUED", JobState::Queued},       {"DOWNLOADING", JobState::Downloading},
      {"PAUSED", JobState::Paused},       {"MERGING", JobState::Merging},
      {"COMPLETED", JobState::Completed}, {"FAILED", JobState::Failed}};
  for (const auto& entry : table) {
    if (s == entry.first) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

bool ParseKind(const std::string& s, JobKind& out) {
  if (s == "direct") out = JobKind::Direct;
  else if (s == "merge") out = JobKind::MergeAv;
  else if (s == "ytdlp") out = JobKind::YtDlp;
  else return false;
  return true;
}

json SettingsJson(const Settings& s) {
  return {{"maxConcurrent", s.maxConcurrent},
          {"segments", s.segments},
          {"defaultFolder", s.defaultFolder},
          {"cookiesFromBrowser", s.cookiesFromBrowser}};
}

// Validates `patch` and applies the recognised keys to `s`.
bool ApplySettingsPatch(Settings& s, const json& patch, std::string& error) {
  if (!patch.is_object()) {
    error = "settings must be an object";
    return false;
  }
  Settings next = s;
  if (patch.contains("maxConcurrent")) {
    const double v = JNumber(patch, "maxConcurrent", -1);
    if (v < 1 || v > 10) {
      error = "maxConcurrent must be between 1 and 10";
      return false;
    }
    next.maxConcurrent = static_cast<int>(v);
  }
  if (patch.contains("segments")) {
    const double v = JNumber(patch, "segments", -1);
    if (v < 1 || v > 32) {
      error = "segments must be between 1 and 32";
      return false;
    }
    next.segments = static_cast<int>(v);
  }
  if (patch.contains("defaultFolder")) {
    const std::string folder = JString(patch, "defaultFolder");
    if (!fileutils::EnsureDirectory(folder, error)) return false;
    next.defaultFolder = folder;
  }
  if (patch.contains("cookiesFromBrowser")) {
    const std::string value = JString(patch, "cookiesFromBrowser");
    // Only a browser name (optionally with :profile) is meaningful; keep it tame.
    if (value.find_first_of("\r\n\"") != std::string::npos || value.compare(0, 1, "-") == 0) {
      error = "invalid cookiesFromBrowser value";
      return false;
    }
    next.cookiesFromBrowser = value;
  }
  s = next;
  return true;
}
}  // namespace

const char* JobStateName(JobState state) {
  switch (state) {
    case JobState::Queued: return "QUEUED";
    case JobState::Downloading: return "DOWNLOADING";
    case JobState::Paused: return "PAUSED";
    case JobState::Merging: return "MERGING";
    case JobState::Completed: return "COMPLETED";
    case JobState::Failed: return "FAILED";
  }
  return "FAILED";
}

const char* JobKindName(JobKind kind) {
  switch (kind) {
    case JobKind::Direct: return "direct";
    case JobKind::MergeAv: return "merge";
    case JobKind::YtDlp: return "ytdlp";
  }
  return "direct";
}

// ============================================================= the job ====

struct DownloadJob {
  std::string id;
  JobKind kind = JobKind::Direct;
  JobState state = JobState::Queued;
  std::string url, audioUrl, title, folder, fileName, finalPath, formatSelector, outputExt = "mp4", error;
  int segments = 8;
  http::RequestHeaders headers;
  uint64_t downloaded = 0, total = 0;
  double speed = 0, eta = -1, percent = 0;
  int stage = 0;  // MergeAv: 0 = video, 1 = audio, 2 = ffmpeg merge
  std::string videoTemp, audioTemp;
  int64_t createdAt = 0;

  // ---- runtime only, never persisted ----
  std::shared_ptr<SegmentedDownloader> downloader;
  std::atomic<int> stopMode{kRunning};
  std::atomic<bool> stopFlag{false};  // observed by RunProcess (ffmpeg / yt-dlp)
  std::string ytOutputHint;           // best guess of the partial file, for cancel cleanup

  bool IsRunning() const { return state == JobState::Downloading || state == JobState::Merging; }

  json ToJson(bool persistent) const {
    json j = {{"id", id},
              {"kind", JobKindName(kind)},
              {"state", JobStateName(state)},
              {"url", url},
              {"title", title},
              {"fileName", fileName},
              {"folder", folder},
              {"finalPath", finalPath},
              {"downloaded", downloaded},
              {"total", total},
              {"percent", percent},
              {"speed", speed},
              {"eta", eta},
              {"error", error},
              {"segments", segments},
              {"createdAt", createdAt}};
    if (persistent) {
      j["audioUrl"] = audioUrl;
      j["formatSelector"] = formatSelector;
      j["outputExt"] = outputExt;
      j["stage"] = stage;
      j["videoTemp"] = videoTemp;
      j["audioTemp"] = audioTemp;
      j["headers"] = HeadersToJson(headers);
    }
    return j;
  }
};

namespace {
// Removes whatever an unfinished job left on disk. Never touches a completed file.
void DeleteJobArtifacts(const std::string& folder, JobKind kind, const std::string& fileName,
                        const std::string& videoTemp, const std::string& audioTemp,
                        const std::string& ytHint) {
  switch (kind) {
    case JobKind::Direct:
      if (!fileName.empty()) SegmentedDownloader::DeletePartFiles(folder, fileName);
      break;
    case JobKind::MergeAv:
      for (const std::string& temp : {videoTemp, audioTemp}) {
        if (temp.empty()) continue;
        SegmentedDownloader::DeletePartFiles(folder, temp);
        fileutils::DeleteFileIfExists(fileutils::JoinPath(folder, temp));
      }
      break;
    case JobKind::YtDlp:
      if (!ytHint.empty()) {
        fileutils::DeleteFileIfExists(ytHint + ".part");
        fileutils::DeleteFileIfExists(ytHint + ".ytdl");
        fileutils::DeleteFileIfExists(ytHint);
      }
      break;
  }
}
}  // namespace

// =========================================================== life cycle ====

DownloadManager::DownloadManager(EventSink sink) : sink_(std::move(sink)) {
  settings_.defaultFolder = fileutils::DefaultDownloadFolder();
  storePath_ = fileutils::JoinPath(fileutils::AppDataDir(), "jobs.json");
}

DownloadManager::~DownloadManager() { Shutdown(); }

void DownloadManager::Start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    started_ = true;
  }
  LoadFromDisk();
  std::vector<json> events;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ScheduleLocked(events);  // jobs that were still QUEUED when we last exited
  }
  Emit(events);
  ticker_ = std::thread([this] { TickerLoop(); });
}

void DownloadManager::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || shuttingDown_) return;
    shuttingDown_ = true;
    for (auto& entry : jobs_) {
      const auto& job = entry.second;
      if (job->IsRunning()) {
        job->stopMode = kPauseRequested;
        job->stopFlag = true;
        if (job->downloader) job->downloader->Pause();
      }
    }
  }
  {
    std::unique_lock<std::mutex> lock(mutex_);
    // Give worker threads a few seconds to flush their part files and exit.
    if (!threadsCv_.wait_for(lock, std::chrono::seconds(8), [this] { return activeThreads_ == 0; })) {
      LOG_WARN("Shutdown: worker threads still running after 8 s");
    }
    PersistLocked();
  }
  {
    std::lock_guard<std::mutex> lock(tickerMutex_);
    tickerStop_ = true;
  }
  tickerCv_.notify_all();
  if (ticker_.joinable()) ticker_.join();
}

void DownloadManager::Emit(const std::vector<json>& events) {
  if (!sink_) return;
  for (const json& e : events) sink_(e);
}

void DownloadManager::TickerLoop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(tickerMutex_);
      if (tickerCv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return tickerStop_; })) return;
    }
    json running = json::array();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& entry : jobs_) {
        if (entry.second->IsRunning()) running.push_back(entry.second->ToJson(false));
      }
    }
    if (!running.empty() && sink_) sink_({{"event", "progress"}, {"jobs", running}});
  }
}

// ============================================================ persistence ====

void DownloadManager::PersistLocked() {
  json jobs = json::array();
  for (const auto& entry : jobs_) jobs.push_back(entry.second->ToJson(true));
  const json root = {{"version", 1}, {"settings", SettingsJson(settings_)}, {"jobs", jobs}};
  std::string error;
  if (!fileutils::WriteFileAtomic(storePath_, root.dump(2, ' ', false, json::error_handler_t::replace), error)) {
    LOG_ERROR("Cannot save " + storePath_ + ": " + error);
  }
}

void DownloadManager::LoadFromDisk() {
  std::string text;
  if (!fileutils::ReadWholeFile(storePath_, text)) return;  // first run
  const json root = json::parse(text, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    LOG_ERROR("jobs.json is corrupt; keeping a copy as jobs.json.bad and starting empty");
    MoveFileExW(fileutils::Utf8ToWide(storePath_).c_str(), fileutils::Utf8ToWide(storePath_ + ".bad").c_str(),
                MOVEFILE_REPLACE_EXISTING);
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto settings = root.find("settings");
  if (settings != root.end()) {
    std::string ignored;
    ApplySettingsPatch(settings_, *settings, ignored);
  }
  const auto jobs = root.find("jobs");
  if (jobs == root.end() || !jobs->is_array()) return;

  for (const json& j : *jobs) {
    try {
      if (!j.is_object()) continue;
      auto job = std::make_shared<DownloadJob>();
      job->id = JString(j, "id");
      if (job->id.empty() || !ParseKind(JString(j, "kind"), job->kind) ||
          !ParseState(JString(j, "state"), job->state)) {
        continue;
      }
      job->url = JString(j, "url");
      job->audioUrl = JString(j, "audioUrl");
      job->title = JString(j, "title");
      job->folder = JString(j, "folder");
      job->fileName = JString(j, "fileName");
      job->finalPath = JString(j, "finalPath");
      job->formatSelector = JString(j, "formatSelector");
      job->outputExt = JString(j, "outputExt", "mp4");
      job->error = JString(j, "error");
      job->segments = static_cast<int>(JNumber(j, "segments", 8));
      job->downloaded = static_cast<uint64_t>(JNumber(j, "downloaded"));
      job->total = static_cast<uint64_t>(JNumber(j, "total"));
      job->percent = JNumber(j, "percent");
      job->stage = static_cast<int>(JNumber(j, "stage"));
      job->videoTemp = JString(j, "videoTemp");
      job->audioTemp = JString(j, "audioTemp");
      job->createdAt = static_cast<int64_t>(JNumber(j, "createdAt"));
      const auto headers = j.find("headers");
      if (headers != j.end()) job->headers = HeadersFromJson(*headers);
      // The previous host process is gone, so nothing can still be running.
      if (job->state == JobState::Downloading || job->state == JobState::Merging) {
        job->state = JobState::Paused;
      }
      job->speed = 0;
      job->eta = -1;
      jobs_[job->id] = job;
      if (job->state == JobState::Completed || job->state == JobState::Failed) job->speed = 0;
    } catch (const std::exception& e) {
      LOG_WARN(std::string("Skipping unreadable job entry: ") + e.what());
    }
  }

  // Keep the history bounded: drop the oldest finished entries.
  int completed = 0;
  for (const auto& entry : jobs_) completed += (entry.second->state == JobState::Completed) ? 1 : 0;
  for (auto it = jobs_.begin(); it != jobs_.end() && completed > kMaxCompletedHistory;) {
    if (it->second->state == JobState::Completed) {
      it = jobs_.erase(it);
      --completed;
    } else {
      ++it;
    }
  }
  LOG_INFO("Restored " + std::to_string(jobs_.size()) + " job(s) from " + storePath_);
}

json DownloadManager::StateEventLocked(const DownloadJob& job) const {
  return {{"event", "state"}, {"job", job.ToJson(false)}};
}

// ============================================================ scheduling ====

void DownloadManager::ScheduleLocked(std::vector<json>& events) {
  if (shuttingDown_) return;
  int active = 0;
  for (const auto& entry : jobs_) active += entry.second->IsRunning() ? 1 : 0;

  bool changed = false;
  for (auto& entry : jobs_) {
    if (active >= settings_.maxConcurrent) break;
    const std::shared_ptr<DownloadJob> job = entry.second;
    if (job->state != JobState::Queued) continue;
    job->state = JobState::Downloading;
    job->stopMode = kRunning;
    job->stopFlag = false;
    job->speed = 0;
    job->error.clear();
    ++active;
    ++activeThreads_;
    changed = true;
    events.push_back(StateEventLocked(*job));
    // Each job runs on its own thread; activeThreads_ lets Shutdown() wait for it.
    std::thread([this, job] { RunJob(job); }).detach();
  }
  if (changed) PersistLocked();
}

// ============================================================ public API ====

bool DownloadManager::AddJob(const StartRequest& req, std::string& jobId, std::string& error) {
  if (!IsHttpUrl(req.url)) {
    error = "Only http:// and https:// URLs are supported";
    return false;
  }
  if (req.kind == JobKind::MergeAv && !IsHttpUrl(req.audioUrl)) {
    error = "audioUrl must be an http(s) URL";
    return false;
  }
  const Settings settings = GetSettings();
  const std::string folder = req.folder.empty() ? settings.defaultFolder : req.folder;
  if (!fileutils::EnsureDirectory(folder, error)) return false;

  auto job = std::make_shared<DownloadJob>();
  job->kind = req.kind;
  job->url = req.url;
  job->audioUrl = req.audioUrl;
  job->title = req.title;
  job->folder = folder;
  job->headers = req.headers;
  job->formatSelector = req.formatSelector;
  job->segments = (std::clamp)(req.segments > 0 ? req.segments : settings.segments, 1, 32);
  job->createdAt = static_cast<int64_t>(std::time(nullptr));

  // Container extension: lowercase letters/digits only, so it can't smuggle a path.
  std::string ext;
  for (const char c : req.outputExt) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ext += c;
  }
  job->outputExt = (ext.size() >= 2 && ext.size() <= 4) ? ext : "mp4";

  std::vector<json> events;
  {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& entry : jobs_) {  // refuse exact duplicates still in progress
      const DownloadJob& other = *entry.second;
      if (other.state != JobState::Completed && other.url == req.url && other.folder == folder &&
          other.audioUrl == req.audioUrl && other.formatSelector == req.formatSelector) {
        error = "This download is already in the list";
        return false;
      }
    }

    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const unsigned long long stamp = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    char idBuf[40];
    std::snprintf(idBuf, sizeof(idBuf), "%016llx-%04llx", stamp, ++idCounter_ & 0xFFFFull);  // sorts by age
    job->id = idBuf;

    if (job->kind == JobKind::Direct) {
      job->fileName = req.fileName.empty() ? std::string() : fileutils::SanitizeFileName(req.fileName);
      if (job->title.empty()) job->title = job->fileName.empty() ? fileutils::FileNameFromUrl(req.url) : job->fileName;
    } else if (job->kind == JobKind::MergeAv) {
      // Pick the final name now (and keep it) so temp files and the result are
      // stable across pause/resume, and two jobs with one title don't collide.
      const std::string base = fileutils::SanitizeFileName(job->title.empty() ? "video" : job->title);
      std::string candidate = BaseName(fileutils::UniqueFilePath(folder, base + "." + job->outputExt));
      for (int i = 1;; ++i) {
        const bool taken = std::any_of(jobs_.begin(), jobs_.end(), [&](const auto& e) {
          // Finished jobs are covered by the on-disk check in UniqueFilePath; only jobs
          // still in progress (which have not created their file yet) reserve a name.
          return e.second->state != JobState::Completed && e.second->folder == folder &&
                 _stricmp(e.second->fileName.c_str(), candidate.c_str()) == 0;
        });
        if (!taken) break;
        candidate = BaseName(fileutils::UniqueFilePath(folder, base + " (" + std::to_string(i) + ")." + job->outputExt));
      }
      job->fileName = candidate;
      job->videoTemp = candidate + ".video.tmp";
      job->audioTemp = candidate + ".audio.tmp";
      if (job->title.empty()) job->title = base;
    } else if (job->title.empty()) {
      job->title = req.url;
    }

    job->state = JobState::Queued;
    jobs_[job->id] = job;
    jobId = job->id;
    events.push_back(StateEventLocked(*job));
    PersistLocked();
    ScheduleLocked(events);  // may flip it to Downloading right away
  }
  Emit(events);
  return true;
}

bool DownloadManager::Pause(const std::string& id, std::string& error) {
  std::vector<json> events;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
      error = "Unknown download id";
      return false;
    }
    const auto& job = it->second;
    if (job->state == JobState::Queued) {
      job->state = JobState::Paused;
      PersistLocked();
      events.push_back(StateEventLocked(*job));
    } else if (job->IsRunning()) {
      // The worker thread notices within ~100 ms and then reports PAUSED itself.
      job->stopMode = kPauseRequested;
      job->stopFlag = true;
      if (job->downloader) job->downloader->Pause();
    } else if (job->state != JobState::Paused) {
      error = "Download is not running";
      return false;
    }
  }
  Emit(events);
  return true;
}

void DownloadManager::PauseAll() {
  std::vector<std::string> ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : jobs_) {
      if (entry.second->IsRunning() || entry.second->state == JobState::Queued) ids.push_back(entry.first);
    }
  }
  std::string ignored;
  for (const auto& id : ids) Pause(id, ignored);
}

bool DownloadManager::Resume(const std::string& id, std::string& error) {
  std::vector<json> events;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
      error = "Unknown download id";
      return false;
    }
    const auto& job = it->second;
    if (job->state == JobState::Completed) {
      error = "Download is already completed";
      return false;
    }
    if (job->state == JobState::Paused || job->state == JobState::Failed) {
      job->state = JobState::Queued;
      job->error.clear();
      job->speed = 0;
      events.push_back(StateEventLocked(*job));
      PersistLocked();
      ScheduleLocked(events);
    }
    // Already queued/running: nothing to do.
  }
  Emit(events);
  return true;
}

bool DownloadManager::Remove(const std::string& id, std::string& error) {
  std::vector<json> events;
  std::shared_ptr<DownloadJob> removed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
      error = "Unknown download id";
      return false;
    }
    const auto& job = it->second;
    if (job->IsRunning()) {
      // Finish() removes the job and deletes its files once the thread has stopped.
      job->stopMode = kCancelRequested;
      job->stopFlag = true;
      if (job->downloader) job->downloader->Cancel();
      return true;
    }
    removed = job;
    jobs_.erase(it);
    PersistLocked();
    events.push_back({{"event", "removed"}, {"id", id}});
    ScheduleLocked(events);
  }
  if (removed->state != JobState::Completed) {
    DeleteJobArtifacts(removed->folder, removed->kind, removed->fileName, removed->videoTemp,
                       removed->audioTemp, removed->ytOutputHint);
  }
  Emit(events);
  return true;
}

bool DownloadManager::GetJob(const std::string& id, json& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = jobs_.find(id);
  if (it == jobs_.end()) return false;
  out = it->second->ToJson(false);
  return true;
}

json DownloadManager::ListJobs() const {
  json list = json::array();
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& entry : jobs_) list.push_back(entry.second->ToJson(false));
  return list;
}

std::string DownloadManager::PathForJob(const std::string& id, bool& isFile) const {
  isFile = false;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = jobs_.find(id);
  if (it == jobs_.end()) return std::string();
  const DownloadJob& job = *it->second;
  if (job.state == JobState::Completed && !job.finalPath.empty()) {
    isFile = true;
    return job.finalPath;
  }
  return job.folder;
}

Settings DownloadManager::GetSettings() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_;
}

json DownloadManager::SettingsToJson() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SettingsJson(settings_);
}

bool DownloadManager::SetSettings(const json& patch, std::string& error) {
  std::vector<json> events;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ApplySettingsPatch(settings_, patch, error)) return false;
    PersistLocked();
    ScheduleLocked(events);  // a higher max may let queued jobs start
  }
  Emit(events);
  return true;
}

// ============================================================= job runner ====

void DownloadManager::RunJob(std::shared_ptr<DownloadJob> job) {
  Outcome outcome = Outcome::Failed;
  std::string error;
  try {
    switch (job->kind) {
      case JobKind::Direct: outcome = RunDirect(job, error); break;
      case JobKind::MergeAv: outcome = RunMergeAv(job, error); break;
      case JobKind::YtDlp: outcome = RunYtDlp(job, error); break;
    }
  } catch (const std::exception& e) {
    error = std::string("Internal error: ") + e.what();
    LOG_ERROR(error);
  } catch (...) {
    error = "Internal error";
    LOG_ERROR(error);
  }
  Finish(job, outcome, error);
}

void DownloadManager::Finish(const std::shared_ptr<DownloadJob>& job, Outcome outcome, const std::string& error) {
  std::vector<json> events;
  bool removeFiles = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job->downloader.reset();
    job->speed = 0;
    job->eta = -1;
    switch (outcome) {
      case Outcome::Completed:
        job->state = JobState::Completed;
        job->percent = 100;
        job->error.clear();
        if (job->total > 0) job->downloaded = job->total;
        break;
      case Outcome::Paused:
        job->state = JobState::Paused;
        break;
      case Outcome::Failed:
        job->state = JobState::Failed;
        job->error = error.empty() ? "Download failed" : error;
        break;
      case Outcome::Cancelled:
        removeFiles = true;
        jobs_.erase(job->id);
        events.push_back({{"event", "removed"}, {"id", job->id}});
        break;
    }
    if (outcome != Outcome::Cancelled) events.push_back(StateEventLocked(*job));
    PersistLocked();
    ScheduleLocked(events);  // a slot just freed up
  }
  if (removeFiles) {
    DeleteJobArtifacts(job->folder, job->kind, job->fileName, job->videoTemp, job->audioTemp, job->ytOutputHint);
  }
  Emit(events);
  {
    // Last thing this thread does with `this`: Shutdown() waits on this counter.
    std::lock_guard<std::mutex> lock(mutex_);
    --activeThreads_;
    threadsCv_.notify_all();
  }
}

void DownloadManager::OnSegmentProgress(const std::shared_ptr<DownloadJob>& job, const ProgressInfo& info,
                                        double base, double span, bool adoptName) {
  json stateEvent;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // The downloader decides the final (collision-free) name; remember it so a
    // resume after restart finds the same .partN files.
    if (adoptName && !info.fileName.empty() && job->fileName != info.fileName) {
      job->fileName = info.fileName;
      PersistLocked();
    }
    job->downloaded = info.downloaded;
    job->total = info.total;
    job->speed = info.speedBytesPerSec;
    job->eta = info.etaSeconds;
    if (info.total > 0) {
      job->percent = base + span * static_cast<double>(info.downloaded) / static_cast<double>(info.total);
    }
    if (info.phase == DownloadPhase::Concatenating && job->kind == JobKind::Direct &&
        job->state == JobState::Downloading) {
      job->state = JobState::Merging;  // joining the .partN files
      PersistLocked();
      stateEvent = StateEventLocked(*job);
    }
  }
  if (!stateEvent.is_null() && sink_) sink_(stateEvent);
}

DownloadManager::Outcome DownloadManager::RunSegmented(const std::shared_ptr<DownloadJob>& job,
                                                       const std::string& url, const std::string& fileName,
                                                       double base, double span, bool adoptName,
                                                       std::string& error) {
  DownloadOptions options;
  options.url = url;
  options.folder = job->folder;
  options.fileName = fileName;
  options.segments = job->segments;
  options.headers = job->headers;

  auto downloader = std::make_shared<SegmentedDownloader>(
      options, [this, job, base, span, adoptName](const ProgressInfo& info) {
        OnSegmentProgress(job, info, base, span, adoptName);
      });
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job->downloader = downloader;
    // A pause/cancel that arrived before the downloader existed must not be lost.
    if (job->stopMode == kPauseRequested) downloader->Pause();
    if (job->stopMode == kCancelRequested) downloader->Cancel();
  }

  const DownloadResult result = downloader->Run();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    job->downloader.reset();
    if (result == DownloadResult::Completed && adoptName) {
      job->finalPath = downloader->FinalPath();
      job->fileName = BaseName(job->finalPath);
      job->total = fileutils::FileSizeOrZero(job->finalPath);
    }
  }
  switch (result) {
    case DownloadResult::Completed: return Outcome::Completed;
    case DownloadResult::Paused: return Outcome::Paused;
    case DownloadResult::Cancelled: return Outcome::Cancelled;
    case DownloadResult::Failed: break;
  }
  error = downloader->Error();
  return Outcome::Failed;
}

DownloadManager::Outcome DownloadManager::RunDirect(const std::shared_ptr<DownloadJob>& job, std::string& error) {
  return RunSegmented(job, job->url, job->fileName, 0.0, 100.0, true, error);
}

DownloadManager::Outcome DownloadManager::RunMergeAv(const std::shared_ptr<DownloadJob>& job, std::string& error) {
  const std::string videoPath = fileutils::JoinPath(job->folder, job->videoTemp);
  const std::string audioPath = fileutils::JoinPath(job->folder, job->audioTemp);
  const std::string outputPath = fileutils::JoinPath(job->folder, job->fileName);

  int stage = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stage = job->stage;
  }

  if (stage == 0) {
    // A finished temp file without parts would make the downloader pick a new
    // name ("x (1)") instead of overwriting; clear such leftovers first.
    if (fileutils::FileExists(videoPath) && !fileutils::FileExists(videoPath + ".part0")) {
      fileutils::DeleteFileIfExists(videoPath);
    }
    const Outcome o = RunSegmented(job, job->url, job->videoTemp, 0.0, 45.0, false, error);
    if (o != Outcome::Completed) return o;
    std::lock_guard<std::mutex> lock(mutex_);
    job->stage = stage = 1;
    PersistLocked();
  }
  if (stage == 1) {
    if (fileutils::FileExists(audioPath) && !fileutils::FileExists(audioPath + ".part0")) {
      fileutils::DeleteFileIfExists(audioPath);
    }
    const Outcome o = RunSegmented(job, job->audioUrl, job->audioTemp, 45.0, 45.0, false, error);
    if (o != Outcome::Completed) return o;
    std::lock_guard<std::mutex> lock(mutex_);
    job->stage = stage = 2;
    PersistLocked();
  }

  // Stage 2: mux with ffmpeg (stream copy, seconds even for big files).
  json stateEvent;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job->state = JobState::Merging;
    job->speed = 0;
    job->percent = 90;
    PersistLocked();
    stateEvent = StateEventLocked(*job);
  }
  if (sink_) sink_(stateEvent);

  std::string mergeError;
  const bool ok = FfmpegMerger::Merge(
      videoPath, audioPath, outputPath,
      [this, job](double pct) {
        std::lock_guard<std::mutex> lock(mutex_);
        job->percent = 90.0 + pct * 0.10;
      },
      &job->stopFlag, mergeError);

  if (!ok) {
    if (job->stopMode == kCancelRequested) return Outcome::Cancelled;
    if (job->stopMode == kPauseRequested) return Outcome::Paused;  // temp files kept; merge restarts on resume
    error = mergeError;
    return Outcome::Failed;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  job->finalPath = outputPath;
  job->total = fileutils::FileSizeOrZero(outputPath);
  job->downloaded = job->total;
  return Outcome::Completed;
}

DownloadManager::Outcome DownloadManager::RunYtDlp(const std::shared_ptr<DownloadJob>& job, std::string& error) {
  YtOptions options;
  options.url = job->url;
  options.folder = job->folder;
  options.fileNameBase = job->title == job->url ? std::string() : job->title;
  options.formatSelector = job->formatSelector;
  options.mergeFormat = job->outputExt;
  options.referer = job->headers.referer;
  options.userAgent = job->headers.userAgent;
  options.cookie = job->headers.cookie;
  options.cookiesFromBrowser = GetSettings().cookiesFromBrowser;

  std::string outputPath;
  const bool ok = YtDlpWrapper::Download(
      options,
      [this, job](const YtProgress& p) {
        json stateEvent;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          job->percent = p.percent;
          job->speed = p.speedBytesPerSec;
          job->eta = p.etaSeconds;
          if (!p.outputPath.empty()) job->ytOutputHint = p.outputPath;
          if (p.merging && job->state == JobState::Downloading) {
            job->state = JobState::Merging;
            stateEvent = StateEventLocked(*job);
          }
        }
        if (!stateEvent.is_null() && sink_) sink_(stateEvent);
      },
      &job->stopFlag, outputPath, error);

  if (!ok) {
    if (job->stopMode == kCancelRequested) return Outcome::Cancelled;
    if (job->stopMode == kPauseRequested) return Outcome::Paused;  // yt-dlp resumes from its .part file
    return Outcome::Failed;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!outputPath.empty()) {
    job->finalPath = outputPath;
    job->fileName = BaseName(outputPath);
    job->total = fileutils::FileSizeOrZero(outputPath);
    job->downloaded = job->total;
  }
  return Outcome::Completed;
}
