#include "YtDlpWrapper.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "FileUtils.h"
#include "Logger.h"

using nlohmann::json;

namespace {

// ---- null-safe JSON accessors (yt-dlp emits null for unknown fields) -------
std::string JStr(const json& j, const char* key) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}
double JNum(const json& j, const char* key) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_number()) ? it->get<double>() : 0.0;
}

std::wstring BuildCommandLine(const std::string& exe, const std::vector<std::string>& args) {
  std::wstring cmd = fileutils::QuoteArg(fileutils::Utf8ToWide(exe));
  for (const auto& a : args) {
    cmd += L' ';
    cmd += fileutils::QuoteArg(fileutils::Utf8ToWide(a));
  }
  return cmd;
}

// Options shared by the info and download invocations.
void AddCommonArgs(const YtOptions& o, std::vector<std::string>& args) {
  args.push_back("--no-warnings");
  args.push_back("--no-playlist");
  args.push_back("--ignore-config");  // a user's yt-dlp.conf must not break our parsing
  if (!o.referer.empty()) {
    args.push_back("--referer");
    args.push_back(o.referer);
  }
  if (!o.userAgent.empty()) {
    args.push_back("--user-agent");
    args.push_back(o.userAgent);
  }
  if (!o.cookie.empty() && o.cookie.find_first_of("\r\n") == std::string::npos) {
    args.push_back("--add-header");
    args.push_back("Cookie: " + o.cookie);
  }
  if (!o.cookiesFromBrowser.empty()) {
    args.push_back("--cookies-from-browser");
    args.push_back(o.cookiesFromBrowser);
  }
}

bool ValidHttpUrl(const std::string& url) {
  return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

// Last "ERROR:" line of yt-dlp's stderr, which is the useful message.
std::string ExtractError(const std::string& stderrText) {
  std::string best;
  size_t pos = 0;
  while (pos < stderrText.size()) {
    size_t end = stderrText.find('\n', pos);
    if (end == std::string::npos) end = stderrText.size();
    std::string line = stderrText.substr(pos, end - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.compare(0, 6, "ERROR:") == 0) best = line.substr(6);
    else if (best.empty() && !line.empty()) best = line;
    pos = end + 1;
  }
  const size_t first = best.find_first_not_of(' ');
  return first == std::string::npos ? std::string() : best.substr(first);
}

double ParseEta(const std::string& s) {  // "MM:SS" or "HH:MM:SS"
  int a = 0, b = 0, c = 0;
  if (std::sscanf(s.c_str(), "%d:%d:%d", &a, &b, &c) == 3) return a * 3600.0 + b * 60.0 + c;
  if (std::sscanf(s.c_str(), "%d:%d", &a, &b) == 2) return a * 60.0 + b;
  return -1;
}

double UnitMultiplier(const std::string& unit) {
  if (unit == "B") return 1;
  if (unit == "KiB" || unit == "KB") return 1024.0;
  if (unit == "MiB" || unit == "MB") return 1024.0 * 1024;
  if (unit == "GiB" || unit == "GB") return 1024.0 * 1024 * 1024;
  return 0;
}

}  // namespace

bool YtDlpWrapper::Available() { return !fileutils::FindTool("yt-dlp.exe").empty(); }

bool YtDlpWrapper::GetFormats(const YtOptions& options, YtInfo& info, std::string& error) {
  if (!ValidHttpUrl(options.url)) {
    error = "Only http(s) URLs are supported";
    return false;
  }
  const std::string exe = fileutils::FindTool("yt-dlp.exe");
  if (exe.empty()) {
    error = "yt-dlp.exe not found. Put it next to idm-clone-host.exe (C:\\Program Files\\IDMClone).";
    return false;
  }

  std::vector<std::string> args = {"-J"};
  AddCommonArgs(options, args);
  args.push_back("--");  // a URL can never be mistaken for an option
  args.push_back(options.url);

  std::string out, err;  // each filled by exactly one reader thread
  const auto run = fileutils::RunProcess(
      BuildCommandLine(exe, args), [&](const std::string& c) { out += c; },
      [&](const std::string& c) { if (err.size() < 64 * 1024) err += c; }, nullptr);
  if (!run.launched) {
    error = run.error;
    return false;
  }
  if (run.exitCode != 0) {
    const std::string detail = ExtractError(err);
    error = "yt-dlp failed (exit " + std::to_string(run.exitCode) + ")" + (detail.empty() ? "" : ": " + detail);
    LOG_ERROR(error);
    return false;
  }

  const json root = json::parse(out, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    error = "yt-dlp returned output that is not valid JSON";
    LOG_ERROR(error);
    return false;
  }

  info = YtInfo();
  info.id = JStr(root, "id");
  info.title = JStr(root, "title");
  info.thumbnail = JStr(root, "thumbnail");
  info.uploader = JStr(root, "uploader");
  info.webpageUrl = JStr(root, "webpage_url");
  info.duration = JNum(root, "duration");

  const auto formats = root.find("formats");
  if (formats != root.end() && formats->is_array()) {
    for (const json& f : *formats) {
      if (!f.is_object()) continue;
      YtFormat fmt;
      fmt.formatId = JStr(f, "format_id");
      fmt.ext = JStr(f, "ext");
      fmt.note = JStr(f, "format_note");
      if (fmt.ext == "mhtml" || fmt.note.find("storyboard") != std::string::npos) continue;  // thumbnails
      fmt.resolution = JStr(f, "resolution");
      fmt.vcodec = JStr(f, "vcodec");
      fmt.acodec = JStr(f, "acodec");
      fmt.url = JStr(f, "url");
      fmt.filesize = static_cast<int64_t>(JNum(f, "filesize"));
      if (fmt.filesize == 0) fmt.filesize = static_cast<int64_t>(JNum(f, "filesize_approx"));
      fmt.height = static_cast<int>(JNum(f, "height"));
      fmt.fps = JNum(f, "fps");
      fmt.tbr = JNum(f, "tbr");
      const auto headers = f.find("http_headers");
      if (headers != f.end() && headers->is_object()) {
        for (auto it = headers->begin(); it != headers->end(); ++it) {
          if (it.value().is_string()) fmt.httpHeaders[it.key()] = it.value().get<std::string>();
        }
      }
      if (!fmt.formatId.empty()) info.formats.push_back(std::move(fmt));
    }
  }
  return true;
}

json YtDlpWrapper::ToJson(const YtInfo& info) {
  json formats = json::array();
  for (const YtFormat& f : info.formats) {
    formats.push_back({{"formatId", f.formatId},
                       {"ext", f.ext},
                       {"resolution", f.resolution},
                       {"vcodec", f.vcodec},
                       {"acodec", f.acodec},
                       {"note", f.note},
                       {"filesize", f.filesize},
                       {"height", f.height},
                       {"fps", f.fps},
                       {"tbr", f.tbr},
                       {"url", f.url},
                       {"httpHeaders", f.httpHeaders}});
  }
  return {{"id", info.id},
          {"title", info.title},
          {"thumbnail", info.thumbnail},
          {"uploader", info.uploader},
          {"webpageUrl", info.webpageUrl},
          {"duration", info.duration},
          {"formats", formats}};
}

bool YtDlpWrapper::Download(const YtOptions& options, const ProgressCallback& onProgress,
                            const std::atomic<bool>* cancel, std::string& outputPath, std::string& error) {
  if (!ValidHttpUrl(options.url)) {
    error = "Only http(s) URLs are supported";
    return false;
  }
  const std::string exe = fileutils::FindTool("yt-dlp.exe");
  if (exe.empty()) {
    error = "yt-dlp.exe not found. Put it next to idm-clone-host.exe (C:\\Program Files\\IDMClone).";
    return false;
  }
  std::string dirError;
  if (!fileutils::EnsureDirectory(options.folder, dirError)) {
    error = dirError;
    return false;
  }

  const std::string selector = options.formatSelector.empty() ? "bv*+ba/b" : options.formatSelector;
  const std::string base = options.fileNameBase.empty() ? "%(title).150B" : [&] {
    // The base is user/page supplied: escape yt-dlp's template metacharacter.
    std::string s = fileutils::SanitizeFileName(options.fileNameBase);
    std::string escaped;
    for (const char c : s) escaped += (c == '%') ? std::string("%%") : std::string(1, c);
    return escaped;
  }();
  const std::string outputTemplate = fileutils::JoinPath(options.folder, base + ".%(ext)s");

  std::vector<std::string> args = {"-f", selector, "--merge-output-format", options.mergeFormat,
                                   "-o", outputTemplate, "--newline", "--no-mtime", "--windows-filenames"};
  const std::string ffmpeg = fileutils::FindTool("ffmpeg.exe");
  if (!ffmpeg.empty()) {
    args.push_back("--ffmpeg-location");
    args.push_back(ffmpeg);
  }
  AddCommonArgs(options, args);
  args.push_back("--");
  args.push_back(options.url);

  // Each "a+b" in the selector means one more stream that restarts at 0%.
  const int streams = static_cast<int>((std::min<size_t>)(
      2, 1 + static_cast<size_t>(std::count(selector.begin(), selector.end(), '+'))));

  std::mutex mutex;
  YtProgress progress;
  int destinationsSeen = 0;
  std::string stderrText;
  std::string lastOutput;

  fileutils::LineSplitter splitter([&](const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    auto after = [&](const char* marker) -> std::string {
      const size_t p = line.find(marker);
      return p == std::string::npos ? std::string() : line.substr(p + std::strlen(marker));
    };
    auto stripQuotes = [](std::string s) {
      while (!s.empty() && (s.back() == '"' || s.back() == ' ')) s.pop_back();
      if (!s.empty() && s.front() == '"') s.erase(0, 1);
      return s;
    };

    if (line.compare(0, 10, "[download]") == 0) {
      const std::string dest = after("Destination: ");
      if (!dest.empty()) {
        ++destinationsSeen;
        lastOutput = dest;
        progress.outputPath = dest;
        return;
      }
      if (line.find(" has already been downloaded") != std::string::npos) {
        std::string path = line.substr(11, line.find(" has already been downloaded") - 11);
        lastOutput = path;
        progress.outputPath = path;
        return;
      }
      // "[download]  45.3% of ~ 120.00MiB at   5.20MiB/s ETA 00:12"
      char* end = nullptr;
      const char* p = line.c_str() + 10;
      const double pct = std::strtod(p, &end);
      if (end != p && *end == '%') {
        const int idx = (std::min)(destinationsSeen > 0 ? destinationsSeen - 1 : 0, streams - 1);
        progress.percent = (idx + pct / 100.0) / streams * 100.0;
        const std::string atPart = after(" at ");
        if (!atPart.empty()) {
          char* unitEnd = nullptr;
          const double value = std::strtod(atPart.c_str(), &unitEnd);
          if (unitEnd != atPart.c_str()) {
            const size_t unitStart = static_cast<size_t>(unitEnd - atPart.c_str());
            const size_t slash = atPart.find("/s", unitStart);
            if (slash != std::string::npos) {
              std::string unit = atPart.substr(unitStart, slash - unitStart);
              while (!unit.empty() && unit.front() == ' ') unit.erase(0, 1);
              progress.speedBytesPerSec = value * UnitMultiplier(unit);
            }
          }
        }
        const std::string eta = after("ETA ");
        progress.etaSeconds = eta.empty() ? -1 : ParseEta(eta);
        if (onProgress) onProgress(progress);
      }
    } else if (line.compare(0, 8, "[Merger]") == 0) {
      const std::string path = after("Merging formats into ");
      if (!path.empty()) {
        lastOutput = stripQuotes(path);
        progress.outputPath = lastOutput;
      }
      progress.merging = true;
      progress.percent = 99;
      if (onProgress) onProgress(progress);
    } else if (line.compare(0, 14, "[ExtractAudio]") == 0) {
      const std::string dest = after("Destination: ");
      if (!dest.empty()) {
        lastOutput = dest;
        progress.outputPath = dest;
      }
    }
  });

  LOG_INFO("yt-dlp download: " + options.url + " format=" + selector);
  const auto run = fileutils::RunProcess(
      BuildCommandLine(exe, args), [&](const std::string& c) { splitter.Feed(c); },
      [&](const std::string& c) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stderrText.size() < 64 * 1024) stderrText += c;
      },
      cancel);
  splitter.Flush();

  if (!run.launched) {
    error = run.error;
    return false;
  }
  if (run.cancelled) {
    error = "Cancelled";
    return false;
  }
  if (run.exitCode != 0) {
    const std::string detail = ExtractError(stderrText);
    error = "yt-dlp failed (exit " + std::to_string(run.exitCode) + ")" + (detail.empty() ? "" : ": " + detail);
    LOG_ERROR(error);
    return false;
  }
  outputPath = lastOutput;
  return true;
}
