// main.cpp - Chrome native messaging loop and command dispatcher.
//
// Protocol (JSON objects):
//   request : {"id": <any>, "cmd": "<name>", ...parameters}
//   response: {"id": <same>, "ok": true,  ...results}
//             {"id": <same>, "ok": false, "error": "<message>"}
//   events  : {"event": "progress"|"state"|"removed"|"ready", ...}   (no id)
//
// Commands: ping, start_download, start_merge_download, start_ytdlp_download,
//           ytdlp_formats, pause, resume, cancel, remove, pause_all, get_status,
//           list_downloads, get_settings, set_settings, open_folder, shutdown
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <thread>

#include "DownloadManager.h"
#include "FileUtils.h"
#include "Logger.h"
#include "NativeMessaging.h"
#include "YtDlpWrapper.h"

using nlohmann::json;

namespace {

constexpr const char* kHostVersion = "1.0.0";

std::string Str(const json& j, const char* key) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

int Int(const json& j, const char* key, int fallback) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_number()) ? it->get<int>() : fallback;
}

json Reply(const json& request, json body = json::object()) {
  body["ok"] = true;
  if (request.contains("id")) body["id"] = request["id"];
  return body;
}

json Fail(const json& request, const std::string& message) {
  json body = {{"ok", false}, {"error", message}};
  if (request.is_object() && request.contains("id")) body["id"] = request["id"];
  return body;
}

// Optional request headers the browser forwards so that downloads behave like
// the browser's own (login cookies, referer for hotlink protection, ...).
http::RequestHeaders ReadHeaders(const json& req) {
  http::RequestHeaders h;
  h.userAgent = Str(req, "userAgent");
  h.referer = Str(req, "referer");
  h.cookie = Str(req, "cookie");
  const auto extra = req.find("headers");
  if (extra != req.end() && extra->is_object()) {
    for (auto it = extra->begin(); it != extra->end(); ++it) {
      if (it.value().is_string()) h.extra[it.key()] = it.value().get<std::string>();
    }
  }
  return h;
}

void FillCommon(const json& req, StartRequest& s) {
  s.folder = Str(req, "folder");
  s.segments = Int(req, "segments", 0);
  s.title = Str(req, "title");
  s.headers = ReadHeaders(req);
  const std::string ext = Str(req, "outputExt");
  if (!ext.empty()) s.outputExt = ext;
}

void OpenInExplorer(const std::string& path, bool isFile) {
  const std::wstring wide = fileutils::Utf8ToWide(path);
  HINSTANCE rc;
  if (isFile) {
    // explorer /select highlights the file inside its folder.
    const std::wstring args = L"/select,\"" + wide + L"\"";
    rc = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  } else {
    rc = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  // ShellExecute returns a value > 32 on success.
  if (reinterpret_cast<INT_PTR>(rc) <= 32) LOG_WIN32("ShellExecuteW", path);
}

// Returns false when the host should exit.
bool Dispatch(DownloadManager& manager, const json& req) {
  if (!req.is_object()) {
    NativeMessaging::WriteMessage(Fail(req, "request must be a JSON object"));
    return true;
  }
  const std::string cmd = Str(req, "cmd");
  std::string error;

  try {
    if (cmd == "ping") {
      NativeMessaging::WriteMessage(Reply(
          req, {{"version", kHostVersion},
                {"ffmpeg", !fileutils::FindTool("ffmpeg.exe").empty()},
                {"ytdlp", YtDlpWrapper::Available()},
                {"settings", manager.SettingsToJson()}}));

    } else if (cmd == "start_download") {
      StartRequest s;
      s.kind = JobKind::Direct;
      s.url = Str(req, "url");
      s.fileName = Str(req, "fileName");
      FillCommon(req, s);
      std::string id;
      if (!manager.AddJob(s, id, error)) {
        NativeMessaging::WriteMessage(Fail(req, error));
      } else {
        NativeMessaging::WriteMessage(Reply(req, {{"jobId", id}}));
      }

    } else if (cmd == "start_merge_download") {
      // Separate video-only + audio-only streams (YouTube DASH): download both, merge with ffmpeg.
      StartRequest s;
      s.kind = JobKind::MergeAv;
      s.url = Str(req, "videoUrl");
      s.audioUrl = Str(req, "audioUrl");
      FillCommon(req, s);
      std::string id;
      if (fileutils::FindTool("ffmpeg.exe").empty()) {
        NativeMessaging::WriteMessage(Fail(req, "ffmpeg.exe not found next to the host (C:\\Program Files\\IDMClone)"));
      } else if (!manager.AddJob(s, id, error)) {
        NativeMessaging::WriteMessage(Fail(req, error));
      } else {
        NativeMessaging::WriteMessage(Reply(req, {{"jobId", id}}));
      }

    } else if (cmd == "start_ytdlp_download") {
      StartRequest s;
      s.kind = JobKind::YtDlp;
      s.url = Str(req, "url");
      s.formatSelector = Str(req, "formatSelector");
      FillCommon(req, s);
      std::string id;
      if (!YtDlpWrapper::Available()) {
        NativeMessaging::WriteMessage(Fail(req, "yt-dlp.exe not found next to the host (C:\\Program Files\\IDMClone)"));
      } else if (!manager.AddJob(s, id, error)) {
        NativeMessaging::WriteMessage(Fail(req, error));
      } else {
        NativeMessaging::WriteMessage(Reply(req, {{"jobId", id}}));
      }

    } else if (cmd == "ytdlp_formats") {
      // yt-dlp can take 5-20 s; answer from a worker thread so the loop stays responsive.
      YtOptions options;
      options.url = Str(req, "url");
      options.referer = Str(req, "referer");
      options.userAgent = Str(req, "userAgent");
      options.cookie = Str(req, "cookie");
      options.cookiesFromBrowser = manager.GetSettings().cookiesFromBrowser;
      std::thread([req, options] {
        YtInfo info;
        std::string err;
        if (!YtDlpWrapper::GetFormats(options, info, err)) {
          NativeMessaging::WriteMessage(Fail(req, err));
          return;
        }
        json out = YtDlpWrapper::ToJson(info);
        // Chrome drops host messages over 1 MiB: shed the heavy per-format fields if needed.
        if (out.dump(-1, ' ', false, json::error_handler_t::replace).size() > 900 * 1024) {
          for (json& f : out["formats"]) {
            f.erase("url");
            f.erase("httpHeaders");
          }
        }
        NativeMessaging::WriteMessage(Reply(req, {{"info", out}}));
      }).detach();

    } else if (cmd == "pause" || cmd == "resume" || cmd == "cancel" || cmd == "remove") {
      const std::string id = Str(req, "jobId");
      bool ok = false;
      if (cmd == "pause") ok = manager.Pause(id, error);
      else if (cmd == "resume") ok = manager.Resume(id, error);
      else ok = manager.Remove(id, error);  // cancel and remove share semantics
      NativeMessaging::WriteMessage(ok ? Reply(req) : Fail(req, error));

    } else if (cmd == "pause_all") {
      manager.PauseAll();
      NativeMessaging::WriteMessage(Reply(req));

    } else if (cmd == "get_status") {
      json job;
      if (manager.GetJob(Str(req, "jobId"), job)) NativeMessaging::WriteMessage(Reply(req, {{"job", job}}));
      else NativeMessaging::WriteMessage(Fail(req, "Unknown download id"));

    } else if (cmd == "list_downloads") {
      NativeMessaging::WriteMessage(Reply(req, {{"jobs", manager.ListJobs()}}));

    } else if (cmd == "get_settings") {
      NativeMessaging::WriteMessage(Reply(req, {{"settings", manager.SettingsToJson()}}));

    } else if (cmd == "set_settings") {
      const auto it = req.find("settings");
      if (it == req.end() || !manager.SetSettings(*it, error)) {
        NativeMessaging::WriteMessage(Fail(req, it == req.end() ? "missing settings" : error));
      } else {
        NativeMessaging::WriteMessage(Reply(req, {{"settings", manager.SettingsToJson()}}));
      }

    } else if (cmd == "open_folder") {
      bool isFile = false;
      std::string path = manager.PathForJob(Str(req, "jobId"), isFile);
      if (path.empty()) path = manager.GetSettings().defaultFolder;
      OpenInExplorer(path, isFile);
      NativeMessaging::WriteMessage(Reply(req));

    } else if (cmd == "shutdown") {
      NativeMessaging::WriteMessage(Reply(req));
      return false;

    } else {
      NativeMessaging::WriteMessage(Fail(req, "Unknown command: " + cmd));
    }
  } catch (const std::exception& e) {
    // A malformed field must never crash the host (it would drop every download).
    LOG_ERROR("Exception while handling '" + cmd + "': " + e.what());
    NativeMessaging::WriteMessage(Fail(req, std::string("Internal error: ") + e.what()));
  }
  return true;
}

}  // namespace

int main() {
  Logger::Instance().Init(fileutils::JoinPath(fileutils::AppDataDir(), "host.log"));
  LOG_INFO(std::string("idm-clone-host ") + kHostVersion + " starting");

  // yt-dlp is a PyInstaller/Python program: force UTF-8 so titles survive the pipes.
  SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
  SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");

  NativeMessaging::InitBinaryStdio();

  DownloadManager manager([](const json& event) { NativeMessaging::WriteMessage(event); });
  manager.Start();
  NativeMessaging::WriteMessage({{"event", "ready"}, {"version", kHostVersion}});

  for (;;) {
    json request;
    std::string readError;
    const NativeMessaging::ReadStatus status = NativeMessaging::ReadMessage(request, readError);
    if (status == NativeMessaging::ReadStatus::EndOfStream) {
      LOG_INFO("stdin closed (browser exited or disconnected)");
      break;
    }
    if (status == NativeMessaging::ReadStatus::BadMessage) {
      NativeMessaging::WriteMessage(Fail(json::object(), readError));
      continue;
    }
    if (!Dispatch(manager, request)) break;
  }

  // Pause running jobs, flush part files, persist jobs.json. They resume on the next launch.
  manager.Shutdown();
  LOG_INFO("idm-clone-host exiting");
  return 0;
}
