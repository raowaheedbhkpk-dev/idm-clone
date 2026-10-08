#include "SegmentedDownloader.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "FileUtils.h"
#include "Logger.h"

using fileutils::FileSizeOrZero;
using fileutils::UniqueHandle;
using fileutils::Utf8ToWide;

namespace {
constexpr uint64_t kMinSegmentBytes = 256 * 1024;  // never split smaller than this
constexpr int kMaxSegments = 32;
constexpr int kMaxRetries = 5;  // waits 1s,2s,4s,8s,16s; the 6th consecutive failure is final
constexpr size_t kReadBufferBytes = 128 * 1024;
constexpr int kMaxPartFiles = 64;

std::string PartPath(const std::string& finalPath, int index) {
  return finalPath + ".part" + std::to_string(index);
}

std::string BaseName(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string WriteErrorText(DWORD err, const std::string& path) {
  if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
    return "Disk full while writing " + BaseName(path);
  }
  if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
    return "Permission denied (or file in use) writing " + BaseName(path) + ": " + fileutils::LastErrorString(err);
  }
  return "Cannot write " + BaseName(path) + ": " + fileutils::LastErrorString(err);
}

std::string HumanBytes(uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = static_cast<double>(bytes);
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
  return buf;
}
}  // namespace

SegmentedDownloader::SegmentedDownloader(DownloadOptions options, ProgressCallback onProgress)
    : options_(std::move(options)), onProgress_(std::move(onProgress)) {}

void SegmentedDownloader::Pause() {
  pause_.store(true);
  cv_.notify_all();
}

void SegmentedDownloader::Cancel() {
  cancel_.store(true);
  cv_.notify_all();
}

void SegmentedDownloader::Fail(const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_.empty()) error_ = message;
  }
  LOG_ERROR("Download failed: " + message);
  failed_.store(true);
  cv_.notify_all();
}

bool SegmentedDownloader::SleepInterruptible(unsigned milliseconds) {
  for (unsigned waited = 0; waited < milliseconds; waited += 100) {
    if (ShouldStop()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return !ShouldStop();
}

void SegmentedDownloader::WorkerExited() {
  std::lock_guard<std::mutex> lock(mutex_);
  --activeWorkers_;
  cv_.notify_all();
}

void SegmentedDownloader::DeletePartFiles(const std::string& folder, const std::string& fileName) {
  const std::string base = fileutils::JoinPath(folder, fileName);
  for (int i = 0; i < kMaxPartFiles; ++i) fileutils::DeleteFileIfExists(PartPath(base, i));
}

void SegmentedDownloader::Emit(DownloadPhase phase, uint64_t downloaded, double speed) {
  if (!onProgress_) return;
  ProgressInfo info;
  info.phase = phase;
  info.downloaded = downloaded;
  info.total = total_;
  info.speedBytesPerSec = speed;
  info.fileName = fileName_;
  info.resumable = ranged_;
  if (total_ > 0 && speed > 1.0 && downloaded <= total_) {
    info.etaSeconds = static_cast<double>(total_ - downloaded) / speed;
  }
  onProgress_(info);
}

// ================================================================== Run ====

DownloadResult SegmentedDownloader::Run() {
  std::string dirError;
  if (!fileutils::EnsureDirectory(options_.folder, dirError)) {
    Fail(dirError);
    return DownloadResult::Failed;
  }

  // ---- 1. Probe (size, range support, name, redirects) with retries ----
  for (int attempt = 0;; ++attempt) {
    probe_ = http::Probe(options_.url, options_.headers);
    if (probe_.ok) break;
    if (cancel_.load()) return DownloadResult::Cancelled;
    if (pause_.load()) return DownloadResult::Paused;
    LOG_WARN("Probe failed (attempt " + std::to_string(attempt + 1) + "): " + probe_.error);
    if (!probe_.retryable || attempt >= kMaxRetries) {
      Fail(probe_.error.empty() ? "Cannot reach the server" : probe_.error);
      return DownloadResult::Failed;
    }
    if (!SleepInterruptible(1000u << attempt)) {
      return cancel_.load() ? DownloadResult::Cancelled : DownloadResult::Paused;
    }
  }

  // Workers talk to the post-redirect URL directly; never carry credentials to another host.
  workerHeaders_ = options_.headers;
  if (!probe_.finalUrl.empty()) http::DropCredentialsIfCrossHost(options_.url, probe_.finalUrl, workerHeaders_);

  // ---- 2. Decide the file name. A leftover .part0 means "resume this one". ----
  std::string name;
  if (!options_.fileName.empty()) {
    name = fileutils::SanitizeFileName(options_.fileName);
  } else if (!probe_.fileName.empty()) {
    name = probe_.fileName;
  } else {
    name = fileutils::FileNameFromUrl(probe_.finalUrl.empty() ? options_.url : probe_.finalUrl);
  }
  const std::string sameName = fileutils::JoinPath(options_.folder, name);
  finalPath_ = fileutils::FileExists(PartPath(sameName, 0)) ? sameName
                                                            : fileutils::UniqueFilePath(options_.folder, name);
  fileName_ = BaseName(finalPath_);

  total_ = probe_.sizeKnown ? probe_.totalSize : 0;
  ranged_ = probe_.acceptsRanges && total_ > 0;

  // Empty file: nothing to download.
  if (probe_.sizeKnown && total_ == 0) {
    UniqueHandle f(CreateFileW(Utf8ToWide(finalPath_).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr));
    if (f.get() == INVALID_HANDLE_VALUE) {
      const DWORD err = GetLastError();
      Logger::Instance().LogWin32("CreateFileW", finalPath_, err);
      Fail(WriteErrorText(err, finalPath_));
      return DownloadResult::Failed;
    }
    Emit(DownloadPhase::Downloading, 0, 0);
    return DownloadResult::Completed;
  }

  // ---- 3. Segment layout ----
  int segmentCount = 1;
  if (ranged_) {
    const uint64_t byMinSize = (std::max<uint64_t>)(1, total_ / kMinSegmentBytes);
    segmentCount = static_cast<int>((std::min<uint64_t>)(
        static_cast<uint64_t>((std::clamp)(options_.segments, 1, kMaxSegments)), byMinSize));
  }
  // Part files from a run with a different layout can't be reused.
  for (int i = segmentCount; i < kMaxPartFiles; ++i) fileutils::DeleteFileIfExists(PartPath(finalPath_, i));
  if (!ranged_) fileutils::DeleteFileIfExists(PartPath(finalPath_, 0));  // can't resume: start over

  uint64_t alreadyOnDisk = 0;
  const uint64_t baseLength = ranged_ ? total_ / static_cast<uint64_t>(segmentCount) : 0;
  for (int i = 0; i < segmentCount; ++i) {
    auto seg = std::make_unique<Segment>();
    seg->index = i;
    seg->partPath = PartPath(finalPath_, i);
    if (ranged_) {
      seg->start = baseLength * static_cast<uint64_t>(i);
      seg->end = (i == segmentCount - 1) ? total_ - 1 : seg->start + baseLength - 1;
      uint64_t size = FileSizeOrZero(seg->partPath);
      if (size > seg->Length()) {  // corrupt or from a different file: restart this segment
        LOG_WARN("Part file larger than its segment, restarting: " + seg->partPath);
        fileutils::DeleteFileIfExists(seg->partPath);
        size = 0;
      }
      seg->done = size;
      alreadyOnDisk += size;
    } else {
      seg->start = 0;
      seg->hasLength = (total_ > 0);
      seg->end = seg->hasLength ? total_ - 1 : UINT64_MAX;
    }
    segments_.push_back(std::move(seg));
  }
  downloaded_ = alreadyOnDisk;

  // ---- 4. Disk space check up front (fail fast instead of at 99%) ----
  if (total_ > 0) {
    uint64_t freeBytes = 0;
    const uint64_t needed = total_ > alreadyOnDisk ? total_ - alreadyOnDisk : 0;
    if (fileutils::FreeDiskSpace(options_.folder, freeBytes) && freeBytes < needed + 16ull * 1024 * 1024) {
      Fail("Not enough disk space: need " + HumanBytes(needed) + ", only " + HumanBytes(freeBytes) + " free");
      return DownloadResult::Failed;
    }
  }

  LOG_INFO("Downloading " + options_.url + " -> " + finalPath_ + " (" + std::to_string(segmentCount) +
           " segment(s), size " + (total_ ? HumanBytes(total_) : std::string("unknown")) + ", resume " +
           (ranged_ ? "yes" : "no") + ", already on disk " + HumanBytes(alreadyOnDisk) + ")");
  Emit(DownloadPhase::Downloading, downloaded_.load(), 0);  // lets the caller learn name/size now

  // ---- 5. Workers: one thread, one WinHTTP session, one part file per segment ----
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& seg : segments_) {
      if (!seg->hasLength || seg->done.load() < seg->Length()) ++activeWorkers_;
    }
  }
  for (const auto& seg : segments_) {
    if (seg->hasLength && seg->done.load() >= seg->Length()) continue;  // already complete
    threads.emplace_back([this, s = seg.get()] { Worker(s); });
  }

  // ---- 6. Supervisor: progress every 500 ms ----
  using Clock = std::chrono::steady_clock;
  auto lastTick = Clock::now();
  uint64_t lastBytes = downloaded_.load();
  double speed = 0;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    while (activeWorkers_ > 0) {
      cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return activeWorkers_ == 0; });
      const auto now = Clock::now();
      const double dt = std::chrono::duration<double>(now - lastTick).count();
      const uint64_t current = downloaded_.load();
      if (dt >= 0.3) {
        const double instant = static_cast<double>(current - lastBytes) / dt;
        speed = (speed == 0) ? instant : 0.7 * speed + 0.3 * instant;  // smooth jitter
        lastTick = now;
        lastBytes = current;
      }
      lock.unlock();
      Emit(DownloadPhase::Downloading, current, speed);
      lock.lock();
    }
  }
  for (auto& t : threads) t.join();

  if (cancel_.load()) {
    DeletePartFiles(options_.folder, fileName_);
    return DownloadResult::Cancelled;
  }
  if (failed_.load()) return DownloadResult::Failed;  // parts stay so the user can retry/resume
  if (pause_.load()) return DownloadResult::Paused;

  // ---- 7. Assemble ----
  Emit(DownloadPhase::Concatenating, downloaded_.load(), 0);
  if (!Merge()) {
    if (cancel_.load()) {
      DeletePartFiles(options_.folder, fileName_);
      return DownloadResult::Cancelled;
    }
    if (pause_.load()) return DownloadResult::Paused;
    return DownloadResult::Failed;
  }
  Emit(DownloadPhase::Downloading, total_, 0);
  return DownloadResult::Completed;
}

// ============================================================== Worker ====

void SegmentedDownloader::Worker(Segment* segment) {
  http::HttpSession session(options_.headers.userAgent);
  if (!session.IsOpen()) {
    Fail(session.Error());
    WorkerExited();
    return;
  }
  std::vector<char> buffer(kReadBufferBytes);
  int failures = 0;

  while (!ShouldStop()) {
    std::string problem;
    const uint64_t before = segment->done.load();
    const AttemptResult result = AttemptSegment(segment, session, buffer, problem);
    if (result == AttemptResult::Done || result == AttemptResult::Stopped) break;
    if (result == AttemptResult::Fatal) {
      Fail(problem);
      break;
    }
    // Retry: a failure after making progress doesn't count against the limit.
    if (segment->done.load() > before) failures = 0;
    ++failures;
    LOG_WARN("Segment " + std::to_string(segment->index) + " failed (" + std::to_string(failures) + "/" +
             std::to_string(kMaxRetries + 1) + "): " + problem);
    if (failures > kMaxRetries) {
      Fail(problem);
      break;
    }
    // Exponential backoff 1s, 2s, 4s, 8s, 16s.
    if (!SleepInterruptible(1000u << (failures - 1))) break;
  }
  WorkerExited();
}

SegmentedDownloader::AttemptResult SegmentedDownloader::AttemptSegment(Segment* seg,
                                                                      http::HttpSession& session,
                                                                      std::vector<char>& buffer,
                                                                      std::string& problem) {
  // Re-sync the counters with what is really on disk (a failed write may have
  // left extra bytes; a non-resumable server forces a restart from zero).
  uint64_t onDisk = ranged_ ? FileSizeOrZero(seg->partPath) : 0;
  if (ranged_ && onDisk > seg->Length()) {
    fileutils::DeleteFileIfExists(seg->partPath);
    onDisk = 0;
  }
  const uint64_t counted = seg->done.load();
  if (onDisk != counted) {
    downloaded_.fetch_add(onDisk - counted);  // unsigned wrap-around makes subtraction work
    seg->done = onDisk;
  }
  if (seg->hasLength && seg->done.load() >= seg->Length()) return AttemptResult::Done;

  http::HttpRequest request(session);
  const uint64_t from = seg->start + seg->done.load();
  const std::string& url = probe_.finalUrl.empty() ? options_.url : probe_.finalUrl;
  if (!request.Send(url, workerHeaders_, ranged_, from, seg->hasLength ? seg->end : http::kOpenEnded)) {
    problem = request.Error();
    return http::IsRetryableWinHttpError(request.WinError()) ? AttemptResult::Retry : AttemptResult::Fatal;
  }

  const DWORD status = request.StatusCode();
  if (ranged_) {
    if (status == 200) {
      problem = "The server stopped honouring Range requests";
      return AttemptResult::Fatal;
    }
    if (status == 416) {
      if (seg->done.load() >= seg->Length()) return AttemptResult::Done;
      fileutils::DeleteFileIfExists(seg->partPath);  // our idea of the offset is wrong
      problem = "Range rejected (HTTP 416); restarting segment";
      return AttemptResult::Retry;
    }
  }
  if (status != (ranged_ ? 206u : 200u)) {
    const std::string text = http::HttpStatusText(status);
    problem = "HTTP " + std::to_string(status) + (text.empty() ? "" : " " + text);
    const bool transient = (status == 408 || status == 429 || status >= 500);
    return transient ? AttemptResult::Retry : AttemptResult::Fatal;
  }
  if (ranged_) {
    // Defend against a misbehaving CDN answering 206 with a different window,
    // which would silently corrupt the file.
    const std::string range = request.Header(L"Content-Range");  // "bytes S-E/T"
    if (range.compare(0, 6, "bytes ") == 0) {
      const unsigned long long got = std::strtoull(range.c_str() + 6, nullptr, 10);
      if (got != from) {
        problem = "Server returned the wrong byte range";
        return AttemptResult::Retry;
      }
    }
  }

  // Open the part file. Ranged: append where we left off. Otherwise: start empty.
  UniqueHandle file(CreateFileW(Utf8ToWide(seg->partPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                ranged_ ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("CreateFileW", seg->partPath, err);
    problem = WriteErrorText(err, seg->partPath);
    return AttemptResult::Fatal;
  }
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(file.get(), zero, nullptr, FILE_END)) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("SetFilePointerEx", seg->partPath, err);
    problem = WriteErrorText(err, seg->partPath);
    return AttemptResult::Fatal;
  }

  while (!ShouldStop()) {
    size_t cap = buffer.size();
    if (seg->hasLength) {
      const uint64_t remaining = seg->Length() - seg->done.load();
      if (remaining == 0) return AttemptResult::Done;
      cap = static_cast<size_t>((std::min<uint64_t>)(cap, remaining));  // never write past the segment
    }
    DWORD got = 0;
    if (!request.ReadChunk(buffer.data(), static_cast<DWORD>(cap), got)) {
      problem = request.Error();
      return http::IsRetryableWinHttpError(request.WinError()) ? AttemptResult::Retry : AttemptResult::Fatal;
    }
    if (got == 0) {  // end of body
      if (seg->hasLength && seg->done.load() < seg->Length()) {
        problem = "The connection closed before the segment was complete";
        return AttemptResult::Retry;
      }
      return AttemptResult::Done;
    }
    DWORD written = 0;
    if (!WriteFile(file.get(), buffer.data(), got, &written, nullptr) || written != got) {
      const DWORD err = GetLastError();
      Logger::Instance().LogWin32("WriteFile", seg->partPath, err);
      problem = WriteErrorText(err, seg->partPath);
      return AttemptResult::Fatal;  // disk full / permissions: retrying won't help
    }
    seg->done.fetch_add(got);
    downloaded_.fetch_add(got);
  }
  return AttemptResult::Stopped;
}

// ============================================================== Merge ====

bool SegmentedDownloader::Merge() {
  uint64_t sum = 0;
  for (const auto& seg : segments_) {
    const uint64_t size = FileSizeOrZero(seg->partPath);
    if (seg->hasLength && size != seg->Length()) {
      Fail("Segment " + std::to_string(seg->index) + " is incomplete (" + std::to_string(size) + " of " +
           std::to_string(seg->Length()) + " bytes)");
      return false;
    }
    sum += size;
  }
  if (total_ == 0) total_ = sum;  // size was unknown until now

  uint64_t freeBytes = 0;
  if (fileutils::FreeDiskSpace(options_.folder, freeBytes) && freeBytes < sum) {
    Fail("Not enough disk space to assemble the file: need " + HumanBytes(sum) + ", only " +
         HumanBytes(freeBytes) + " free");
    return false;
  }

  const std::wstring wideFinal = Utf8ToWide(finalPath_);
  UniqueHandle out(CreateFileW(wideFinal.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (out.get() == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("CreateFileW", finalPath_, err);
    Fail(WriteErrorText(err, finalPath_));
    return false;
  }

  // Pre-allocate the final size so a full disk is detected now and the file is
  // not fragmented by incremental growth.
  LARGE_INTEGER size{};
  size.QuadPart = static_cast<LONGLONG>(sum);
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(out.get(), size, nullptr, FILE_BEGIN) || !SetEndOfFile(out.get()) ||
      !SetFilePointerEx(out.get(), zero, nullptr, FILE_BEGIN)) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("SetEndOfFile", finalPath_, err);
    Fail(WriteErrorText(err, finalPath_));
    out.reset();
    fileutils::DeleteFileIfExists(finalPath_);
    return false;
  }

  std::vector<char> chunk(1024 * 1024);
  bool ok = true;
  for (const auto& seg : segments_) {
    UniqueHandle in(CreateFileW(Utf8ToWide(seg->partPath).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (in.get() == INVALID_HANDLE_VALUE) {
      const DWORD err = GetLastError();
      Logger::Instance().LogWin32("CreateFileW", seg->partPath, err);
      Fail("Cannot read " + BaseName(seg->partPath) + ": " + fileutils::LastErrorString(err));
      ok = false;
      break;
    }
    for (;;) {
      if (cancel_.load() || pause_.load()) {
        ok = false;
        break;
      }
      DWORD read = 0;
      if (!ReadFile(in.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
        const DWORD err = GetLastError();
        Logger::Instance().LogWin32("ReadFile", seg->partPath, err);
        Fail("Cannot read " + BaseName(seg->partPath) + ": " + fileutils::LastErrorString(err));
        ok = false;
        break;
      }
      if (read == 0) break;
      DWORD written = 0;
      if (!WriteFile(out.get(), chunk.data(), read, &written, nullptr) || written != read) {
        const DWORD err = GetLastError();
        Logger::Instance().LogWin32("WriteFile", finalPath_, err);
        Fail(WriteErrorText(err, finalPath_));
        ok = false;
        break;
      }
    }
    if (!ok) break;
  }

  if (ok && !FlushFileBuffers(out.get())) LOG_WIN32("FlushFileBuffers", finalPath_);
  out.reset();
  if (!ok) {
    fileutils::DeleteFileIfExists(finalPath_);  // parts are kept; Merge can be retried
    return false;
  }
  // Only now that the final file is complete do we throw the parts away.
  for (const auto& seg : segments_) fileutils::DeleteFileIfExists(seg->partPath);
  LOG_INFO("Completed " + finalPath_);
  return true;
}
