#include "NativeMessaging.h"

#include <fcntl.h>
#include <io.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

#include "Logger.h"

namespace {
std::mutex g_writeMutex;  // several threads emit progress/state events

bool ReadExactly(void* dest, size_t count) {
  auto* p = static_cast<unsigned char*>(dest);
  size_t total = 0;
  while (total < count) {
    const size_t got = std::fread(p + total, 1, count - total, stdin);
    if (got == 0) return false;  // EOF or error
    total += got;
  }
  return true;
}
}  // namespace

void NativeMessaging::InitBinaryStdio() {
  if (_setmode(_fileno(stdin), _O_BINARY) == -1) LOG_ERROR("_setmode(stdin, binary) failed");
  if (_setmode(_fileno(stdout), _O_BINARY) == -1) LOG_ERROR("_setmode(stdout, binary) failed");
  // Chrome reads our frames as soon as they are flushed; no stdio buffering games.
  std::setvbuf(stdout, nullptr, _IOFBF, 64 * 1024);
}

NativeMessaging::ReadStatus NativeMessaging::ReadMessage(nlohmann::json& out, std::string& error) {
  error.clear();
  unsigned char header[4];
  if (!ReadExactly(header, sizeof(header))) return ReadStatus::EndOfStream;

  // Decode explicitly instead of memcpy so we do not depend on host endianness.
  const uint32_t length = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
                          (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
  if (length > kMaxIncomingBytes) {
    // We cannot resynchronise after an absurd length, so treat it as fatal.
    LOG_ERROR("Incoming message too large: " + std::to_string(length) + " bytes");
    error = "message too large";
    return ReadStatus::EndOfStream;
  }

  std::vector<char> payload(length);
  if (length > 0 && !ReadExactly(payload.data(), length)) {
    LOG_WARN("Truncated message body; Chrome closed the pipe");
    return ReadStatus::EndOfStream;
  }

  // allow_exceptions=false: malformed input must not take the host down.
  out = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, false);
  if (out.is_discarded()) {
    error = "invalid JSON";
    LOG_WARN("Received invalid JSON message (" + std::to_string(length) + " bytes)");
    return ReadStatus::BadMessage;
  }
  return ReadStatus::Ok;
}

bool NativeMessaging::WriteMessage(const nlohmann::json& message) {
  // error_handler_t::replace: titles/URLs from the web may contain invalid UTF-8;
  // dump() would otherwise throw.
  std::string payload = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

  if (payload.size() > kMaxOutgoingBytes) {
    LOG_ERROR("Outgoing message exceeds Chrome's 1 MiB limit (" + std::to_string(payload.size()) + " bytes)");
    nlohmann::json err = {{"ok", false}, {"error", "response too large for native messaging"}};
    if (message.is_object() && message.contains("id")) err["id"] = message["id"];
    payload = err.dump();
  }

  const uint32_t length = static_cast<uint32_t>(payload.size());
  const unsigned char header[4] = {
      static_cast<unsigned char>(length & 0xFF), static_cast<unsigned char>((length >> 8) & 0xFF),
      static_cast<unsigned char>((length >> 16) & 0xFF), static_cast<unsigned char>((length >> 24) & 0xFF)};

  std::lock_guard<std::mutex> lock(g_writeMutex);
  if (std::fwrite(header, 1, sizeof(header), stdout) != sizeof(header) ||
      std::fwrite(payload.data(), 1, payload.size(), stdout) != payload.size() ||
      std::fflush(stdout) != 0) {
    LOG_ERROR("Writing to stdout failed - Chrome probably closed the pipe");
    return false;
  }
  return true;
}
