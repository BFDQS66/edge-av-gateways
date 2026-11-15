#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace veg {

// Decodes the AAC packets already received by the gate; no media is written
// to disk. One instance persists across GOPs so codec delay is preserved.
class AudioAsr {
 public:
  AudioAsr(AVFormatContext* input, int port, int timeout_ms);
  ~AudioAsr();
  AudioAsr(const AudioAsr&) = delete;
  AudioAsr& operator=(const AudioAsr&) = delete;

  std::string recognize(const std::vector<AVPacket*>& packets);

 private:
  void receive_frames(std::vector<int16_t>& samples);
  std::string post_wav(const std::vector<int16_t>& samples) const;

  int stream_index_ = -1;
  int port_;
  int timeout_ms_;
  AVCodecContext* codec_ = nullptr;
  SwrContext* resampler_ = nullptr;
  AVSampleFormat source_format_ = AV_SAMPLE_FMT_NONE;
  int source_rate_ = 0;
  AVChannelLayout source_layout_{};
};

}  // namespace veg
