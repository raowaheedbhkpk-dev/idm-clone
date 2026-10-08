#include "FileUtils.h"

#include <initguid.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <thread>
#include <vector>

#include "Logger.h"

namespace fileutils {

// ============================================================ strings ====

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    LOG_WIN32("MultiByteToWideChar", "size query");
    return std::wstring();
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0],
                          needed) <= 0) {
    LOG_WIN32("MultiByteToWideChar", "conversion");
    return std::wstring();
  }
  return wide;
}

std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    LOG_WIN32("WideCharToMultiByte", "size query");
    return std::string();
  }
  std::string utf8(static_cast<size_t>(needed), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &utf8[0],
                          needed, nullptr, nullptr) <= 0) {
    LOG_WIN32("WideCharToMultiByte", "conversion");
    return std::string();
  }
  return utf8;
}

std::string LastErrorString(DWORD errorCode) {
  wchar_t* buffer = nullptr;
  // WinHTTP error codes (12001-12185) live in winhttp.dll's message table, not in
  // the system table, so try both.
  DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (len == 0) {
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    if (winhttp != nullptr) {
      len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           winhttp, errorCode, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    }
  }
  std::string text = "error " + std::to_string(errorCode);
  if (len != 0 && buffer != nullptr) {
    std::wstring msg(buffer, len);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) {
      msg.pop_back();
    }
    text += ": " + WideToUtf8(msg);
  }
  if (buffer != nullptr) LocalFree(buffer);
  return text;
}

std::string UrlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
      out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string SanitizeFileName(const std::string& name) {
  static const char kBad[] = "<>:\"/\\|?*";
  std::string out;
  out.reserve(name.size());
  for (const char ch : name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || std::strchr(kBad, ch) != nullptr) {
      out += '_';
    } else {
      out += ch;
    }
  }
  // Windows silently drops trailing dots/spaces, which breaks later lookups.
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  size_t lead = 0;
  while (lead < out.size() && out[lead] == ' ') ++lead;
  out.erase(0, lead);
  if (out.empty()) return "download";

  // Reserved DOS device names are invalid even with an extension ("CON.txt").
  std::string stem = out.substr(0, out.find('.'));
  std::transform(stem.begin(), stem.end(), stem.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  bool reserved = (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL");
  if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) &&
      stem[3] >= '1' && stem[3] <= '9') {
    reserved = true;
  }
  if (reserved) out.insert(0, "_");

  constexpr size_t kMaxBytes = 180;  // leave room for ".partNN" and " (99)"
  if (out.size() > kMaxBytes) {
    std::string ext;
    const size_t dot = out.rfind('.');
    if (dot != std::string::npos && out.size() - dot <= 16) ext = out.substr(dot);
    std::string base = out.substr(0, kMaxBytes - ext.size());
    // Don't leave half of a multi-byte UTF-8 sequence at the cut.
    while (!base.empty() && (static_cast<unsigned char>(base.back()) & 0xC0) == 0x80) {
      base.pop_back();
    }
    if (!base.empty() && (static_cast<unsigned char>(base.back()) & 0xC0) == 0xC0) {
      base.pop_back();
    }
    out = base + ext;
  }
  return out;
}

std::string FileNameFromUrl(const std::string& url) {
  size_t start = url.find("://");
  start = (start == std::string::npos) ? 0 : start + 3;
  const size_t pathStart = url.find('/', start);
  if (pathStart == std::string::npos) return "download";
  size_t end = url.find_first_of("?#", pathStart);
  if (end == std::string::npos) end = url.size();
  std::string path = url.substr(pathStart, end - pathStart);
  const size_t slash = path.rfind('/');
  std::string last = (slash == std::string::npos) ? path : path.substr(slash + 1);
  last = UrlDecode(last);
  if (last.empty()) return "download";
  return SanitizeFileName(last);
}

// ============================================================== paths ====

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  if (last == '\\' || last == '/') return dir + name;
  return dir + "\\" + name;
}

std::string ExeDir() {
  wchar_t buffer[MAX_PATH * 2] = {0};
  const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
  if (len == 0) {
    LOG_WIN32("GetModuleFileNameW", "");
    return std::string();
  }
  std::wstring path(buffer, len);
  const size_t slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) path.resize(slash);
  return WideToUtf8(path);
}

static std::string KnownFolder(const KNOWNFOLDERID& id) {
  PWSTR raw = nullptr;
  const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw);
  std::string result;
  if (SUCCEEDED(hr) && raw != nullptr) {
    result = WideToUtf8(raw);
  } else {
    LOG_ERROR("SHGetKnownFolderPath failed, HRESULT=" + std::to_string(static_cast<long>(hr)));
  }
  if (raw != nullptr) CoTaskMemFree(raw);
  return result;
}

std::string AppDataDir() {
  std::string base = KnownFolder(FOLDERID_RoamingAppData);
  if (base.empty()) base = ExeDir();
  const std::string dir = JoinPath(base, "idm-clone");
  std::string err;
  if (!EnsureDirectory(dir, err)) LOG_ERROR("Cannot create app data dir " + dir + ": " + err);
  return dir;
}

std::string DefaultDownloadFolder() {
  std::string dir = KnownFolder(FOLDERID_Downloads);
  if (dir.empty()) {
    wchar_t profile[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
      dir = JoinPath(WideToUtf8(profile), "Downloads");
    } else {
      dir = ExeDir();
    }
  }
  return dir;
}

bool EnsureDirectory(const std::string& dir, std::string& error) {
  const std::wstring wide = Utf8ToWide(dir);
  if (wide.empty()) {
    error = "Empty folder path";
    return false;
  }
  const bool absolute = (wide.size() >= 3 && wide[1] == L':' && (wide[2] == L'\\' || wide[2] == L'/')) ||
                        wide.compare(0, 2, L"\\\\") == 0;
  if (!absolute) {
    error = "Folder must be an absolute path: " + dir;
    return false;
  }
  DWORD attrs = GetFileAttributesW(wide.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES) {
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return true;
    error = "A file with this name already exists: " + dir;
    return false;
  }
  const int rc = SHCreateDirectoryExW(nullptr, wide.c_str(), nullptr);
  if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
    error = "Cannot create folder " + dir + " (" + LastErrorString(static_cast<DWORD>(rc)) + ")";
    return false;
  }
  attrs = GetFileAttributesW(wide.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    error = "Cannot create folder " + dir;
    return false;
  }
  return true;
}

// ============================================================== files ====

bool FileExists(const std::string& path) {
  return GetFileAttributesW(Utf8ToWide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

uint64_t FileSizeOrZero(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(Utf8ToWide(path).c_str(), GetFileExInfoStandard, &data)) return 0;
  return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool DeleteFileIfExists(const std::string& path) {
  if (DeleteFileW(Utf8ToWide(path).c_str())) return true;
  const DWORD err = GetLastError();
  if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return true;
  Logger::Instance().LogWin32("DeleteFileW", path, err);
  return false;
}

bool FreeDiskSpace(const std::string& dir, uint64_t& freeBytes) {
  ULARGE_INTEGER avail{}, total{}, totalFree{};
  if (!GetDiskFreeSpaceExW(Utf8ToWide(dir).c_str(), &avail, &total, &totalFree)) {
    LOG_WIN32("GetDiskFreeSpaceExW", dir);
    return false;
  }
  freeBytes = avail.QuadPart;
  return true;
}

std::string UniqueFilePath(const std::string& dir, const std::string& name) {
  const std::string safe = SanitizeFileName(name);
  std::string candidate = JoinPath(dir, safe);
  if (!FileExists(candidate) && !FileExists(candidate + ".part0")) return candidate;

  const size_t dot = safe.rfind('.');
  const bool hasExt = dot != std::string::npos && dot > 0;
  const std::string base = hasExt ? safe.substr(0, dot) : safe;
  const std::string ext = hasExt ? safe.substr(dot) : std::string();
  for (int i = 1; i < 10000; ++i) {
    candidate = JoinPath(dir, base + " (" + std::to_string(i) + ")" + ext);
    if (!FileExists(candidate) && !FileExists(candidate + ".part0")) return candidate;
  }
  return JoinPath(dir, base + "_" + std::to_string(GetTickCount64()) + ext);
}

bool ReadWholeFile(const std::string& path, std::string& contents) {
  UniqueHandle file(CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err != ERROR_FILE_NOT_FOUND) Logger::Instance().LogWin32("CreateFileW", path, err);
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size)) {
    LOG_WIN32("GetFileSizeEx", path);
    return false;
  }
  contents.clear();
  contents.resize(static_cast<size_t>(size.QuadPart));
  size_t offset = 0;
  while (offset < contents.size()) {
    DWORD read = 0;
    const DWORD want = static_cast<DWORD>((std::min<size_t>)(contents.size() - offset, 1u << 20));
    if (!ReadFile(file.get(), &contents[offset], want, &read, nullptr) || read == 0) {
      LOG_WIN32("ReadFile", path);
      return false;
    }
    offset += read;
  }
  return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& contents, std::string& error) {
  const std::string tmp = path + ".tmp";
  {
    UniqueHandle file(CreateFileW(Utf8ToWide(tmp).c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
      const DWORD err = GetLastError();
      Logger::Instance().LogWin32("CreateFileW", tmp, err);
      error = LastErrorString(err);
      return false;
    }
    size_t offset = 0;
    while (offset < contents.size()) {
      DWORD written = 0;
      const DWORD want = static_cast<DWORD>((std::min<size_t>)(contents.size() - offset, 1u << 20));
      if (!WriteFile(file.get(), contents.data() + offset, want, &written, nullptr)) {
        const DWORD err = GetLastError();
        Logger::Instance().LogWin32("WriteFile", tmp, err);
        error = LastErrorString(err);
        return false;
      }
      offset += written;
    }
    if (!FlushFileBuffers(file.get())) LOG_WIN32("FlushFileBuffers", tmp);
  }
  if (!MoveFileExW(Utf8ToWide(tmp).c_str(), Utf8ToWide(path).c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("MoveFileExW", path, err);
    error = LastErrorString(err);
    return false;
  }
  return true;
}

std::string FindTool(const std::string& exeName) {
  const std::string beside = JoinPath(ExeDir(), exeName);
  if (FileExists(beside)) return beside;
  const std::string standard = JoinPath("C:\\Program Files\\IDMClone", exeName);
  if (FileExists(standard)) return standard;
  wchar_t found[MAX_PATH * 2] = {0};
  const DWORD len = SearchPathW(nullptr, Utf8ToWide(exeName).c_str(), nullptr,
                                static_cast<DWORD>(sizeof(found) / sizeof(found[0])), found, nullptr);
  if (len > 0 && len < sizeof(found) / sizeof(found[0])) return WideToUtf8(std::wstring(found, len));
  return std::string();
}

// ========================================================== processes ====

std::wstring QuoteArg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring quoted = L"\"";
  for (auto it = arg.begin();; ++it) {
    size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      // Backslashes before the closing quote must be doubled.
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
    } else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(*it);
    }
  }
  quoted.push_back(L'"');
  return quoted;
}

void LineSplitter::Feed(const std::string& chunk) {
  for (const char c : chunk) {
    if (c == '\n' || c == '\r') {
      if (!pending_.empty()) {
        onLine_(pending_);
        pending_.clear();
      }
    } else {
      pending_ += c;
    }
  }
}

void LineSplitter::Flush() {
  if (!pending_.empty()) {
    onLine_(pending_);
    pending_.clear();
  }
}

namespace {

struct AttributeListGuard {
  LPPROC_THREAD_ATTRIBUTE_LIST list;
  ~AttributeListGuard() { DeleteProcThreadAttributeList(list); }
};

void PumpPipe(HANDLE pipe, const OutputCallback& callback) {
  char buffer[8192];
  DWORD got = 0;
  // ReadFile fails with ERROR_BROKEN_PIPE once every write end is closed.
  while (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    if (callback) callback(std::string(buffer, got));
  }
}

}  // namespace

ProcessResult RunProcess(const std::wstring& commandLine, const OutputCallback& onStdout,
                         const OutputCallback& onStderr, const std::atomic<bool>* cancel) {
  ProcessResult result;
  const std::string cmdUtf8 = WideToUtf8(commandLine);

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE rawRead = nullptr;
  HANDLE rawWrite = nullptr;
  if (!CreatePipe(&rawRead, &rawWrite, &sa, 0)) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("CreatePipe", "stdout", err);
    result.error = "CreatePipe failed: " + LastErrorString(err);
    return result;
  }
  UniqueHandle outRead(rawRead), outWrite(rawWrite);
  if (!CreatePipe(&rawRead, &rawWrite, &sa, 0)) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("CreatePipe", "stderr", err);
    result.error = "CreatePipe failed: " + LastErrorString(err);
    return result;
  }
  UniqueHandle errRead(rawRead), errWrite(rawWrite);
  // The read ends stay private to us.
  if (!SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0)) LOG_WIN32("SetHandleInformation", "stdout read");
  if (!SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0)) LOG_WIN32("SetHandleInformation", "stderr read");

  // The host's own stdin is the Chrome message pipe. A child that read it would
  // steal protocol bytes, so the child gets NUL instead.
  UniqueHandle nulIn(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (nulIn.get() == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("CreateFileW", "NUL", err);
    result.error = "Cannot open NUL: " + LastErrorString(err);
    return result;
  }

  // Restrict handle inheritance to exactly the three handles we want, so the
  // child cannot also inherit e.g. our protocol pipes.
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);  // fails by design: size query
  std::vector<unsigned char> attrBuffer(attrSize);
  LPPROC_THREAD_ATTRIBUTE_LIST attrList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
  if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("InitializeProcThreadAttributeList", "", err);
    result.error = "InitializeProcThreadAttributeList failed: " + LastErrorString(err);
    return result;
  }
  AttributeListGuard attrGuard{attrList};
  HANDLE inheritList[3] = {nulIn.get(), outWrite.get(), errWrite.get()};
  if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList,
                                 sizeof(inheritList), nullptr, nullptr)) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("UpdateProcThreadAttribute", "", err);
    result.error = "UpdateProcThreadAttribute failed: " + LastErrorString(err);
    return result;
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.StartupInfo.wShowWindow = SW_HIDE;
  si.StartupInfo.hStdInput = nulIn.get();
  si.StartupInfo.hStdOutput = outWrite.get();
  si.StartupInfo.hStdError = errWrite.get();
  si.lpAttributeList = attrList;

  // CreateProcessW may modify the command line buffer, so it must be writable.
  std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
  mutableCmd.push_back(L'\0');

  // Job object: kill the whole process tree when we cancel, or if we die.
  UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
  if (job.get() != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
      LOG_WIN32("SetInformationJobObject", "");
    }
  } else {
    LOG_WIN32("CreateJobObjectW", "");
  }

  PROCESS_INFORMATION pi{};
  // CREATE_SUSPENDED: put the process into the job before it can spawn children.
  const DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
  if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                      reinterpret_cast<LPSTARTUPINFOW>(&si), &pi)) {
    const DWORD err = GetLastError();
    Logger::Instance().LogWin32("CreateProcessW", cmdUtf8, err);
    result.error = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                       ? "Program not found: " + cmdUtf8
                       : "CreateProcessW failed: " + LastErrorString(err);
    return result;
  }
  UniqueHandle process(pi.hProcess), thread(pi.hThread);
  result.launched = true;

  if (job.get() != nullptr && !AssignProcessToJobObject(job.get(), process.get())) {
    LOG_WIN32("AssignProcessToJobObject", cmdUtf8);
  }
  if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) LOG_WIN32("ResumeThread", cmdUtf8);

  // Close our copies of the write ends; otherwise the readers never see EOF.
  outWrite.reset();
  errWrite.reset();
  nulIn.reset();

  // Both pipes must be drained concurrently or a chatty child can block on a
  // full pipe while we wait on the other one.
  std::thread outThread([&] { PumpPipe(outRead.get(), onStdout); });
  std::thread errThread([&] { PumpPipe(errRead.get(), onStderr); });

  for (;;) {
    const DWORD waited = WaitForSingleObject(process.get(), 100);
    if (waited == WAIT_OBJECT_0) break;
    if (waited == WAIT_FAILED) {
      LOG_WIN32("WaitForSingleObject", cmdUtf8);
      break;
    }
    if (cancel != nullptr && cancel->load()) {
      result.cancelled = true;
      if (job.get() == nullptr || !TerminateJobObject(job.get(), 1)) {
        if (!TerminateProcess(process.get(), 1)) LOG_WIN32("TerminateProcess", cmdUtf8);
      }
      WaitForSingleObject(process.get(), 5000);
      break;
    }
  }

  // Kill stragglers (a grandchild holding the pipe would block the readers).
  if (job.get() != nullptr) TerminateJobObject(job.get(), 0);
  outThread.join();
  errThread.join();

  if (!GetExitCodeProcess(process.get(), &result.exitCode)) {
    LOG_WIN32("GetExitCodeProcess", cmdUtf8);
    result.exitCode = 1;
  }
  return result;
}

}  // namespace fileutils
