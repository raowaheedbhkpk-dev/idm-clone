# IDM Clone

A multi-connection download manager for Windows 10/11 x64 made of two parts:

* **Native host** (`native-host/`) – C++17 / Win32 / WinHTTP. Segmented downloads (8 connections by default), pause/resume, retry with backoff, queue, persistence, ffmpeg merging and yt-dlp integration.
* **Chrome extension** (`extension/`) – Manifest V3, strict TypeScript. Media sniffing, YouTube detection, download interception, and a dark IDM-style popup.

They talk over **Chrome Native Messaging**: each message is a 4-byte little-endian length followed by UTF-8 JSON, on the host's stdin/stdout in binary mode.

```
 ┌──────────────────────┐   native messaging   ┌────────────────────────────┐
 │ Chrome extension     │ ───────────────────▶ │ idm-clone-host.exe         │
 │  background.ts       │ ◀─────────────────── │  DownloadManager (queue)   │
 │  content.ts / popup  │   len32 + JSON       │  SegmentedDownloader x N   │
 └──────────────────────┘                      │  HttpRangeClient (WinHTTP) │
                                               │  FfmpegMerger  ─▶ ffmpeg   │
                                               │  YtDlpWrapper  ─▶ yt-dlp   │
                                               └────────────────────────────┘
```

> **Not affiliated with Tonec Inc. or Internet Download Manager.** This is an independent educational project.

## Legal notes

* This project is **not affiliated with Tonec Inc. / Internet Download Manager**.
* Downloading from YouTube **violates YouTube's Terms of Service**. This is provided for **educational and personal use only**.
* The **Chrome Web Store will reject this extension**. It is for **sideloading only** (Load unpacked).
* You are **responsible for complying with the laws** of your country and with the terms of any site you download from.

## Layout

```
README.md
native-host/
  CMakeLists.txt
  manifest/com.idmclone.host.json
  third_party/json.hpp
  src/ Logger  NativeMessaging  HttpRangeClient  SegmentedDownloader
       FfmpegMerger  YtDlpWrapper  DownloadManager  FileUtils  main.cpp
installer/register_host.reg
extension/
  package.json  tsconfig.json  manifest.json
  src/ types.ts nativeBridge.ts youtubeExtractor.ts content.ts relay.ts
       background.ts popup.html popup.css popup.ts
```

## Build and run

1. **Prerequisites**: Visual Studio 2022 Build Tools (C++ workload, Windows 10/11 SDK), CMake ≥ 3.20, Node.js ≥ 18.
2. **Tools**: put `yt-dlp.exe` and `ffmpeg.exe` in `C:\Program Files\IDMClone`. (They are optional; without them the matching features are disabled and the popup shows it.)
3. **Build the host** (from `native-host`):
   ```bat
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```
   The post-build step also gathers the exe and manifest into `build\dist`.
4. **Install the host**: copy `build\Release\idm-clone-host.exe` and `manifest\com.idmclone.host.json` to `C:\Program Files\IDMClone\` (run as administrator), then double-click `installer\register_host.reg`.
5. **Build the extension** (from `extension`):
   ```bat
   npm install
   npm run build
   ```
6. **Load it**: open `chrome://extensions`, enable *Developer mode*, *Load unpacked*, select `extension\dist`.
7. **Extension ID**: `extension/manifest.json` contains a pinned `key`, so the ID is always `foalgppohbghdfbkmmdnfafnhegjjpda`, which is already in the host manifest. If you remove the `key` or the ID shown in Chrome differs, put the displayed ID in `allowed_origins` of `C:\Program Files\IDMClone\com.idmclone.host.json` (as `chrome-extension://<id>/`) and run `register_host.reg` again.
8. **Test**: open the popup – the header should show *Connected*. Start any download from the Media tab, or just click a download link in Chrome; it is picked up by the host, which shows progress in the Downloads tab. Logs: `%APPDATA%\idm-clone\host.log`. Job state: `%APPDATA%\idm-clone\jobs.json`.

## How it works

**Downloads.** A probe (`GET` with `Range: bytes=0-0`) learns size, range support, file name and final URL. The file is split into up to 8 segments (at least 256 KB each), one thread and one `.partN` file each. Resume is derived from part-file sizes, so even a killed host can continue. Transient failures (timeouts, 5xx, 429, network drops) retry with 1→2→4→8→16 s backoff. Progress is emitted every 500 ms. When all segments finish the parts are concatenated and deleted. Without range support the file is fetched on a single connection.

**Redirects** are followed manually (max 10, HTTPS→HTTP refused). Cookie, `Authorization` and `Proxy-Authorization` are dropped when a redirect leaves the original host.

**Jobs** have the states `QUEUED, DOWNLOADING, PAUSED, MERGING, COMPLETED, FAILED`; at most 3 run at once (configurable). They persist to `%APPDATA%\idm-clone\jobs.json`; after a restart, unfinished jobs come back as PAUSED.

**Merging.** Separate video+audio streams are merged with `ffmpeg -y -i video -i audio -c copy output`; progress comes from the `time=` lines on stderr.

**yt-dlp.** Formats come from `yt-dlp -J --no-warnings <url>`; downloads run yt-dlp with `-f v+a --merge-output-format mp4`. Signature deciphering is not reimplemented: streams that need it (`signatureCipher`) are sent to yt-dlp.

**Errors handled:** no internet, 404, 403, range not supported, disk full (checked up front, plus write errors), permission denied, host killed (the extension reconnects with backoff, and Chrome's own download is only cancelled after the host accepted the job).

### Protocol (extension → host)

Requests are `{"id": n, "cmd": "...", ...}`; responses `{"id": n, "ok": true|false, ...}`; the host also pushes `{"event": "ready|progress|state|removed", ...}`.

Commands: `ping, start_download, start_merge_download, start_ytdlp_download, ytdlp_formats, pause, resume, cancel, remove, pause_all, get_status, list_downloads, get_settings, set_settings, open_folder, shutdown`.

## Verification status (please read)

The sandbox this was written in has no Windows or MSVC, so:

* The host was compiled with **MinGW-w64** (`-Wall -Wextra -Wshadow`, no warnings) and run under **Wine**, with local HTTP servers and stand-in `ffmpeg.exe` / `yt-dlp.exe`. This covered range downloads, pause/resume, retries, redirects and credential stripping, merging and the command protocol.
* The extension builds with strict `tsc`, and was driven in **Chromium** with real native messaging to the Wine host.
* **Not done:** an actual MSVC `/W4` build, a run on real Windows, and use of the real yt-dlp, ffmpeg or YouTube. Expect to fix small MSVC-specific diagnostics on the first build. YouTube changes frequently; keep `yt-dlp.exe` updated.
* Extension content scripts run on `<all_urls>`; the `cookies` permission is used only to attach the page's cookies to a download of that page.
