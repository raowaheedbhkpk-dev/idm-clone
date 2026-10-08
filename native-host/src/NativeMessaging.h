// NativeMessaging.h - Chrome native messaging framing over stdin/stdout.
//
// Wire format (both directions): 4-byte little-endian unsigned length followed
// by that many bytes of UTF-8 JSON. Chrome rejects host->browser messages
// larger than 1 MiB and sends browser->host messages up to 4 GiB.
#pragma once

#include <string>

#include "nlohmann/json.hpp"

class NativeMessaging {
 public:
  enum class ReadStatus {
    Ok,           // `out` holds a parsed message
    EndOfStream,  // Chrome closed the pipe (browser exited / port disconnected)
    BadMessage    // frame was read fully but is not valid JSON; stream still aligned
  };

  // Must be called once, before any read/write. Windows opens stdin/stdout in
  // text mode by default, which would translate 0x0A/0x1A bytes in the length
  // prefix and corrupt the stream.
  static void InitBinaryStdio();

  // Blocks until a full message arrives.
  static ReadStatus ReadMessage(nlohmann::json& out, std::string& error);

  // Thread-safe. Returns false if the pipe is gone (host should exit).
  static bool WriteMessage(const nlohmann::json& message);

  static constexpr size_t kMaxOutgoingBytes = 1024 * 1024;       // Chrome's limit
  static constexpr size_t kMaxIncomingBytes = 64u * 1024 * 1024;  // our sanity cap
};
