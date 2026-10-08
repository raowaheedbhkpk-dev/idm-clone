// FfmpegMerger.h - muxes a video-only and an audio-only file with ffmpeg.exe.
#pragma once

#include <atomic>
#include <functional>
#include <string>

class FfmpegMerger {
 public:
  // percent: 0..100 (derived from ffmpeg's "time=" vs the input "Duration:").
  using ProgressCallback = std::function<void(double percent)>;

  // Runs: ffmpeg -y -i <video> -i <audio> -c copy <output>
  // (no re-encoding, so it takes seconds). On success the two input files are
  // deleted. On failure or cancel the partial output is deleted and `error` is
  // set; the inputs are left in place so the merge can be retried.
  static bool Merge(const std::string& videoPath, const std::string& audioPath,
                    const std::string& outputPath, const ProgressCallback& onProgress,
                    const std::atomic<bool>* cancel, std::string& error);
};
