#include "HttpRangeClient.h"

#include <cctype>
#include <cstdlib>
#include <iterator>
#include <vector>

#include "FileUtils.h"
#include "Logger.h"

namespace http {

using fileutils::Utf8ToWide;
using fileutils::WideToUtf8;

// ------------------------------------------------------------- helpers ----

bool CrackUrl(const std::string& url, UrlParts& out, std::string& error) {
  const std::wstring wide = Utf8ToWide(url);
  if (wide.empty()) {
    error = "Empty URL";
    return false;
  }
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  // Non-zero length + null pointer asks WinHttpCrackUrl to point into our buffer.
  uc.dwSchemeLength = static_cast<DWORD>(-1);
  uc.dwHostNameLength = static_cast<DWORD>(-1);
  uc.dwUrlPathLength = static_cast<DWORD>(-1);
  uc.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &uc)) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("WinHttpCrackUrl", url, err);
    error = "Invalid URL (" + fileutils::LastErrorString(err) + ")";
    return false;
  }
  if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
    error = "Only http:// and https:// URLs are supported";
    return false;
  }
  out.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
  out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
  out.port = uc.nPort;
  out.pathAndQuery.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.dwExtraInfoLength > 0) out.pathAndQuery.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  if (out.pathAndQuery.empty()) out.pathAndQuery = L"/";
  return true;
}

std::string DescribeWinHttpError(DWORD code) {
  switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
      return "Cannot resolve the host name (no internet connection, DNS failure or wrong address)";
    case ERROR_WINHTTP_CANNOT_CONNECT:
      return "Cannot connect to the server (no internet connection or server is down)";
    case ERROR_WINHTTP_TIMEOUT:
      return "The connection timed out";
    case ERROR_WINHTTP_CONNECTION_ERROR:
      return "The connection with the server was reset or terminated";
    case ERROR_WINHTTP_SECURE_FAILURE:
      return "TLS/SSL certificate or protocol failure";
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
      return "Invalid URL";
    case ERROR_WINHTTP_REDIRECT_FAILED:
      return "Too many redirects or redirect to an unsupported scheme";
    case ERROR_WINHTTP_LOGIN_FAILURE:
      return "Authentication failed";
    default:
      return "Network error (" + fileutils::LastErrorString(code) + ")";
  }
}

bool IsRetryableWinHttpError(DWORD code) {
  switch (code) {
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
    case ERROR_WINHTTP_REDIRECT_FAILED:
    case ERROR_WINHTTP_LOGIN_FAILURE:
    case ERROR_WINHTTP_OPERATION_CANCELLED:
      return false;
    default:
      return true;  // DNS blips, resets, timeouts: worth another try
  }
}

std::string HttpStatusText(DWORD status) {
  switch (status) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized (login required)";
    case 403: return "Forbidden (access denied or link expired)";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 410: return "Gone (link expired)";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
  }
}

std::string FileNameFromContentDisposition(const std::string& value) {
  // filename*=UTF-8''percent%20encoded.ext   (preferred, RFC 5987)
  auto lower = value;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  size_t pos = lower.find("filename*=");
  if (pos != std::string::npos) {
    pos += 10;
    const size_t quote = value.find("''", pos);
    if (quote != std::string::npos) {
      size_t end = value.find(';', quote);
      if (end == std::string::npos) end = value.size();
      std::string raw = value.substr(quote + 2, end - quote - 2);
      while (!raw.empty() && (raw.back() == ' ' || raw.back() == '"')) raw.pop_back();
      const std::string decoded = fileutils::UrlDecode(raw);
      if (!decoded.empty()) return fileutils::SanitizeFileName(decoded);
    }
  }
  // filename="name.ext"  or  filename=name.ext
  pos = lower.find("filename=");
  if (pos != std::string::npos) {
    pos += 9;
    std::string raw;
    if (pos < value.size() && value[pos] == '"') {
      const size_t end = value.find('"', pos + 1);
      raw = value.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
    } else {
      size_t end = value.find(';', pos);
      if (end == std::string::npos) end = value.size();
      raw = value.substr(pos, end - pos);
      while (!raw.empty() && raw.back() == ' ') raw.pop_back();
    }
    // Some servers percent-encode even the plain parameter.
    const std::string decoded = fileutils::UrlDecode(raw);
    if (!decoded.empty()) return fileutils::SanitizeFileName(decoded);
  }
  return std::string();
}

// --------------------------------------------------------- HttpSession ----

HttpSession::HttpSession(const std::string& userAgent) {
  const std::wstring ua = Utf8ToWide(userAgent.empty() ? "IDMClone/1.0" : userAgent);
  // AUTOMATIC_PROXY (Windows 8.1+) honours the system proxy settings and WPAD/PAC,
  // which is what users behind corporate proxies expect.
  HINTERNET h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (h == nullptr) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("WinHttpOpen", "", err);
    error_ = "WinHttpOpen failed: " + fileutils::LastErrorString(err);
    return;
  }
  handle_.reset(h);
  // resolve, connect, send, receive (ms). The receive timeout acts as a stall
  // detector between data chunks, not as a limit on total download time.
  if (!WinHttpSetTimeouts(h, 15000, 20000, 30000, 30000)) LOG_WIN32("WinHttpSetTimeouts", "");
}

// --------------------------------------------------------- HttpRequest ----

void HttpRequest::SetWinError(const char* api, DWORD code) {
  winError_ = code;
  Logger::Instance().LogWin32(api, "", code);
  error_ = DescribeWinHttpError(code);
}

bool HttpRequest::SendOnce(const std::string& url, const RequestHeaders& headers, bool useRange,
                           uint64_t rangeFrom, uint64_t rangeTo) {
  status_ = 0;
  winError_ = 0;
  error_.clear();
  request_.reset();
  connect_.reset();

  UrlParts parts;
  if (!CrackUrl(url, parts, error_)) {
    winError_ = ERROR_WINHTTP_INVALID_URL;
    return false;
  }

  HINTERNET connect = WinHttpConnect(session_.Handle(), parts.host.c_str(), parts.port, 0);
  if (connect == nullptr) {
    SetWinError("WinHttpConnect", GetLastError());
    return false;
  }
  connect_.reset(connect);

  // WINHTTP_FLAG_SECURE switches the connection to TLS (https).
  const DWORD flags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request = WinHttpOpenRequest(connect, L"GET", parts.pathAndQuery.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (request == nullptr) {
    SetWinError("WinHttpOpenRequest", GetLastError());
    return false;
  }
  request_.reset(request);

  // We supply Cookie ourselves (the browser's cookies); WinHTTP's own cookie jar
  // would otherwise ignore or duplicate a manual Cookie header.
  DWORD disable = WINHTTP_DISABLE_COOKIES;
  if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable))) {
    LOG_WIN32("WinHttpSetOption", "WINHTTP_DISABLE_COOKIES");
  }

  // Redirects are followed by Send() so that credentials can be dropped when the host changes.
  DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy))) {
    LOG_WIN32("WinHttpSetOption", "WINHTTP_OPTION_REDIRECT_POLICY");
  }

  std::string h = "Accept: */*\r\n";
  // identity: a compressed body would make Range offsets and sizes meaningless.
  h += "Accept-Encoding: identity\r\n";
  if (useRange) {
    h += "Range: bytes=" + std::to_string(rangeFrom) + "-";
    if (rangeTo != kOpenEnded) h += std::to_string(rangeTo);
    h += "\r\n";
  }
  if (!headers.referer.empty()) h += "Referer: " + headers.referer + "\r\n";
  if (!headers.cookie.empty()) h += "Cookie: " + headers.cookie + "\r\n";
  for (const auto& kv : headers.extra) {
    // Never let callers smuggle a second header via CR/LF.
    if (kv.first.find_first_of("\r\n:") != std::string::npos ||
        kv.second.find_first_of("\r\n") != std::string::npos) {
      continue;
    }
    h += kv.first + ": " + kv.second + "\r\n";
  }
  const std::wstring wideHeaders = Utf8ToWide(h);

  // dwHeadersLength == -1 means "NUL terminated".
  if (!WinHttpSendRequest(request, wideHeaders.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA,
                          0, 0, 0)) {
    SetWinError("WinHttpSendRequest", GetLastError());
    return false;
  }
  // Redirects (301/302/307/308) are followed inside WinHttpReceiveResponse.
  if (!WinHttpReceiveResponse(request, nullptr)) {
    SetWinError("WinHttpReceiveResponse", GetLastError());
    return false;
  }

  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
    SetWinError("WinHttpQueryHeaders(status)", GetLastError());
    return false;
  }
  status_ = status;
  return true;
}

namespace {
bool IsRedirectStatus(DWORD s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }
constexpr int kMaxRedirects = 10;

std::string LowerCopy(std::string s) {
  for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

std::string HostPort(const UrlParts& p) {
  return LowerCopy(WideToUtf8(p.host)) + ":" + std::to_string(p.port);
}

std::string Origin(const UrlParts& p) {
  std::string host = WideToUtf8(p.host);
  if (host.find(':') != std::string::npos && host.front() != '[') host = "[" + host + "]";  // IPv6 literal
  const bool defaultPort = (p.secure && p.port == 443) || (!p.secure && p.port == 80);
  return std::string(p.secure ? "https://" : "http://") + host + (defaultPort ? "" : ":" + std::to_string(p.port));
}
}  // namespace

std::string ResolveRedirect(const std::string& base, const std::string& location) {
  std::string loc = location;
  const size_t hash = loc.find('#');
  if (hash != std::string::npos) loc.erase(hash);
  if (loc.empty()) return std::string();
  const std::string lower = LowerCopy(loc.substr(0, 8));
  if (lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0) return loc;

  UrlParts parts;
  std::string ignored;
  if (!CrackUrl(base, parts, ignored)) return std::string();
  if (loc.compare(0, 2, "//") == 0) return std::string(parts.secure ? "https:" : "http:") + loc;
  if (loc[0] == '/') return Origin(parts) + loc;

  std::string path = WideToUtf8(parts.pathAndQuery);
  const size_t query = path.find('?');
  const std::string pathOnly = query == std::string::npos ? path : path.substr(0, query);
  if (loc[0] == '?') return Origin(parts) + pathOnly + loc;
  const size_t slash = pathOnly.rfind('/');
  return Origin(parts) + pathOnly.substr(0, slash == std::string::npos ? 0 : slash + 1) + loc;
}

bool DropCredentialsIfCrossHost(const std::string& fromUrl, const std::string& toUrl, RequestHeaders& headers) {
  UrlParts from, to;
  std::string ignored;
  if (!CrackUrl(fromUrl, from, ignored) || !CrackUrl(toUrl, to, ignored)) return false;
  if (HostPort(from) == HostPort(to)) return false;
  bool dropped = !headers.cookie.empty();
  headers.cookie.clear();
  for (auto it = headers.extra.begin(); it != headers.extra.end();) {
    const std::string name = LowerCopy(it->first);
    if (name == "authorization" || name == "cookie" || name == "proxy-authorization") {
      it = headers.extra.erase(it);
      dropped = true;
    } else {
      ++it;
    }
  }
  return dropped;
}

bool HttpRequest::Send(const std::string& url, const RequestHeaders& headers, bool useRange,
                       uint64_t rangeFrom, uint64_t rangeTo) {
  std::string current = url;
  RequestHeaders hop = headers;  // credentials are stripped from this copy on cross-host redirects
  finalUrl_ = url;

  for (int redirects = 0;; ++redirects) {
    if (!SendOnce(current, hop, useRange, rangeFrom, rangeTo)) return false;
    finalUrl_ = current;
    if (!IsRedirectStatus(status_)) return true;

    if (redirects >= kMaxRedirects) {
      winError_ = ERROR_WINHTTP_REDIRECT_FAILED;
      error_ = "Too many redirects";
      return false;
    }
    const std::string next = ResolveRedirect(current, Header(L"Location"));
    UrlParts from, to;
    std::string parseError;
    if (next.empty() || !CrackUrl(current, from, parseError) || !CrackUrl(next, to, parseError)) {
      winError_ = ERROR_WINHTTP_REDIRECT_FAILED;
      error_ = "The server sent an invalid redirect";
      return false;
    }
    if (from.secure && !to.secure) {  // never silently downgrade TLS
      winError_ = ERROR_WINHTTP_REDIRECT_FAILED;
      error_ = "Redirect from HTTPS to HTTP refused";
      return false;
    }
    if (DropCredentialsIfCrossHost(current, next, hop)) {
      LOG_INFO("Cross-host redirect to " + WideToUtf8(to.host) + ": credentials dropped");
    }
    current = next;
  }
}

std::string HttpRequest::Header(const wchar_t* name) const {
  if (request_ == nullptr) return std::string();
  DWORD bytes = 0;
  // First call: ask how large the value is. It "fails" with INSUFFICIENT_BUFFER.
  WinHttpQueryHeaders(request_.get(), WINHTTP_QUERY_CUSTOM, name, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return std::string();  // header absent
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  if (!WinHttpQueryHeaders(request_.get(), WINHTTP_QUERY_CUSTOM, name, buffer.data(), &bytes,
                           WINHTTP_NO_HEADER_INDEX)) {
    return std::string();
  }
  return WideToUtf8(std::wstring(buffer.data(), bytes / sizeof(wchar_t)));
}

bool HttpRequest::ReadChunk(void* buffer, DWORD capacity, DWORD& got) {
  got = 0;
  if (!WinHttpReadData(request_.get(), buffer, capacity, &got)) {
    SetWinError("WinHttpReadData", GetLastError());
    return false;
  }
  return true;  // got == 0 with TRUE means end of body
}

// --------------------------------------------------------------- Probe ----

ProbeResult Probe(const std::string& url, const RequestHeaders& headers) {
  ProbeResult result;
  HttpSession session(headers.userAgent);
  if (!session.IsOpen()) {
    result.error = session.Error();
    return result;
  }
  HttpRequest request(session);
  // A ranged GET for one byte works on servers that reject HEAD (many CDNs do),
  // and tells us directly whether Range is honoured: 206 = yes, 200 = no.
  if (!request.Send(url, headers, true, 0, 0)) {
    result.winError = request.WinError();
    result.error = request.Error();
    result.retryable = IsRetryableWinHttpError(request.WinError());
    return result;
  }

  result.httpStatus = request.StatusCode();
  result.finalUrl = request.FinalUrl();
  if (result.finalUrl.empty()) result.finalUrl = url;
  result.contentType = request.Header(L"Content-Type");
  result.fileName = FileNameFromContentDisposition(request.Header(L"Content-Disposition"));

  const DWORD status = result.httpStatus;
  if (status == 206) {
    // Content-Range: bytes 0-0/123456
    const std::string range = request.Header(L"Content-Range");
    const size_t slash = range.rfind('/');
    if (slash != std::string::npos && range.compare(slash + 1, 1, "*") != 0) {
      char* end = nullptr;
      const unsigned long long total = std::strtoull(range.c_str() + slash + 1, &end, 10);
      if (end != range.c_str() + slash + 1 && total > 0) {
        result.sizeKnown = true;
        result.totalSize = total;
      }
    }
    result.acceptsRanges = result.sizeKnown;
    result.ok = true;
  } else if (status == 200) {
    // Server ignored Range: single connection, no resume.
    const std::string length = request.Header(L"Content-Length");
    if (!length.empty()) {
      result.sizeKnown = true;
      result.totalSize = std::strtoull(length.c_str(), nullptr, 10);
    }
    result.acceptsRanges = false;
    result.ok = true;
  } else if (status == 416) {
    // Range unsatisfiable on a zero-length file: "Content-Range: bytes */0".
    result.sizeKnown = true;
    result.totalSize = 0;
    result.ok = true;
  } else {
    const std::string text = HttpStatusText(status);
    result.error = "HTTP " + std::to_string(status) + (text.empty() ? "" : " " + text);
    result.retryable = (status == 408 || status == 429 || status >= 500);
  }
  // Closing the request without reading the body aborts the transfer, so a 200
  // response for a multi-GB file costs nothing.
  return result;
}

}  // namespace http
