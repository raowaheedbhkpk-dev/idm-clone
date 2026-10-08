// YtDlpWrapper.h - drives yt-dlp.exe as a subprocess.
//
// We deliberately do not reimplement YouTube's signature/"n" parameter
// deciphering or PO-token handling: that changes every few weeks. yt-dlp is
// maintained for exactly that, so we just call it.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

struct YtFormat {
  std::string formatId;
  std::string ext;
  std::string resolution;  // "1920x1080" or "audio only"
  std::string vcodec;      // "none" for audio-only
  std::string acodec;      // "none" for video-only
  std::string note;
  std::string url;
  int64_t filesize = 0;    // bytes; 0 if unknown (uses filesize_approx when present)
  int height = 0;
  double fps = 0;
  double tbr = 0;          // total bitrate in kbit/s
  std::map<std::string, std::string> httpHeaders;
};

struct YtInfo {
  std::string id;
  std::string title;
  std::string thumbnail;
  std::string uploader;
  std::string webpageUrl;
  double duration = 0;
  std::vector<YtFormat> formats;
};

// Everything that can influence a yt-dlp invocation.
struct YtOptions {
  std::string url;
  std::string folder;           // download only
  std::string fileNameBase;     // download only; "" = yt-dlp's own title
  std::string formatSelector;   // e.g. "137+140"; "" = best video + best audio
  std::string mergeFormat = "mp4";
  std::string referer;
  std::string userAgent;
  std::string cookie;               // raw Cookie header value from the browser
  std::string cookiesFromBrowser;   // e.g. "chrome"; optional
};

struct YtProgress {
  double percent = 0;            // overall 0..100 across all streams
  double speedBytesPerSec = 0;
  double etaSeconds = -1;
  bool merging = false;
  std::string outputPath;        // last known destination file
};

class YtDlpWrapper {
 public:
  using ProgressCallback = std::function<void(const YtProgress&)>;

  static bool Available();

  // yt-dlp -J --no-warnings <url>
  static bool GetFormats(const YtOptions& options, YtInfo& info, std::string& error);

  // yt-dlp -f <v>+<a> --merge-output-format mp4 -o <path> <url>
  // Blocks until done. `outputPath` receives the final file on success.
  static bool Download(const YtOptions& options, const ProgressCallback& onProgress,
                       const std::atomic<bool>* cancel, std::string& outputPath, std::string& error);

  static nlohmann::json ToJson(const YtInfo& info);
};
