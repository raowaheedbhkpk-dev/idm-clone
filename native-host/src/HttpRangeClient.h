// HttpRangeClient.h - thin RAII wrapper over WinHTTP for ranged GET requests.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace http {

// ------------------------------------------------------------------ RAII ----
// HINTERNET is a void*; WinHttpCloseHandle must be used instead of CloseHandle.
struct InternetDeleter {
  using pointer = HINTERNET;
  void operator()(HINTERNET h) const noexcept {
    if (h != nullptr) WinHttpCloseHandle(h);
  }
};
using UniqueInternet = std::unique_ptr<void, InternetDeleter>;

constexpr uint64_t kOpenEnded = UINT64_MAX;  // "Range: bytes=N-"

// Extra request headers (all UTF-8).
struct RequestHeaders {
  std::string userAgent;
  std::string referer;
  std::string cookie;
  std::map<std::string, std::string> extra;
};

struct UrlParts {
  bool secure = false;
  std::wstring host;
  INTERNET_PORT port = 0;
  std::wstring pathAndQuery;
};

bool CrackUrl(const std::string& url, UrlParts& out, std::string& error);

// Human readable text + retry policy for WinHTTP error codes (12xxx).
std::string DescribeWinHttpError(DWORD code);
bool IsRetryableWinHttpError(DWORD code);
std::string HttpStatusText(DWORD status);
// Parses RFC 6266 Content-Disposition (filename* and filename).
std::string FileNameFromContentDisposition(const std::string& headerValue);

// --------------------------------------------------------------- probing ----
struct ProbeResult {
  bool ok = false;
  DWORD httpStatus = 0;     // 0 when no HTTP response was received
  DWORD winError = 0;       // WinHTTP error when httpStatus == 0
  bool retryable = false;   // transient problem (timeout, 5xx, 429...)
  bool sizeKnown = false;
  uint64_t totalSize = 0;
  bool acceptsRanges = false;
  std::string finalUrl;     // after redirects
  std::string fileName;     // from Content-Disposition, may be empty
  std::string contentType;
  std::string error;
};

// ------------------------------------------------------- session/request ----
class HttpSession {
 public:
  explicit HttpSession(const std::string& userAgent);
  bool IsOpen() const { return handle_ != nullptr; }
  HINTERNET Handle() const { return handle_.get(); }
  const std::string& Error() const { return error_; }

 private:
  UniqueInternet handle_;
  std::string error_;
};

// One connection + one request. Not thread safe; use one per thread.
class HttpRequest {
 public:
  explicit HttpRequest(HttpSession& session) : session_(session) {}

  // Sends a GET and waits for the response headers. With useRange the request
  // asks for bytes [rangeFrom, rangeTo] (rangeTo == kOpenEnded for "to the end").
  bool Send(const std::string& url, const RequestHeaders& headers, bool useRange,
            uint64_t rangeFrom, uint64_t rangeTo);

  DWORD StatusCode() const { return status_; }
  std::string Header(const wchar_t* name) const;
  // URL of the last hop, i.e. after redirects were followed.
  const std::string& FinalUrl() const { return finalUrl_; }

  // Reads up to `capacity` bytes. Returns false on error; *got == 0 means EOF.
  bool ReadChunk(void* buffer, DWORD capacity, DWORD& got);

  DWORD WinError() const { return winError_; }
  const std::string& Error() const { return error_; }

 private:
  void SetWinError(const char* api, DWORD code);
  // One HTTP exchange without following redirects.
  bool SendOnce(const std::string& url, const RequestHeaders& headers, bool useRange,
                uint64_t rangeFrom, uint64_t rangeTo);

  HttpSession& session_;
  UniqueInternet connect_;   // declared before request_ so the request closes first
  UniqueInternet request_;
  DWORD status_ = 0;
  DWORD winError_ = 0;
  std::string error_;
  std::string finalUrl_;
};

// Browser-like credential policy: when `toUrl` is on a different host/port than `fromUrl`,
// the Cookie header, Authorization and Proxy-Authorization are removed from `headers`.
// Returns true if anything was dropped.
bool DropCredentialsIfCrossHost(const std::string& fromUrl, const std::string& toUrl, RequestHeaders& headers);

// Resolves a Location header (absolute, scheme-relative, absolute-path or relative) against `base`.
std::string ResolveRedirect(const std::string& base, const std::string& location);

// GET with "Range: bytes=0-0" to learn size, range support, name and final URL.
ProbeResult Probe(const std::string& url, const RequestHeaders& headers);

}  // namespace http
