#include "audio_asr.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace veg {
namespace {

void check(int code, const char* operation) {
  if (code >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(code, message, sizeof(message));
  throw std::runtime_error(std::string(operation) + ": " + message);
}

void append_u16(std::string& out, uint16_t value) {
  out.push_back(static_cast<char>(value));
  out.push_back(static_cast<char>(value >> 8));
}

void append_u32(std::string& out, uint32_t value) {
  append_u16(out, static_cast<uint16_t>(value));
  append_u16(out, static_cast<uint16_t>(value >> 16));
}

std::string wav(const std::vector<int16_t>& samples) {
  if (samples.size() > 16000U * 30U) throw std::runtime_error("ASR segment exceeds 30 seconds");
  std::string out;
  out.reserve(44 + samples.size() * 2);
  out += "RIFF";
  append_u32(out, static_cast<uint32_t>(36 + samples.size() * 2));
  out += "WAVEfmt ";
  append_u32(out, 16);
  append_u16(out, 1);
  append_u16(out, 1);
  append_u32(out, 16000);
  append_u32(out, 32000);
  append_u16(out, 2);
  append_u16(out, 16);
  out += "data";
  append_u32(out, static_cast<uint32_t>(samples.size() * 2));
  for (const int16_t value : samples) append_u16(out, static_cast<uint16_t>(value));
  return out;
}

std::string json_text(std::string_view body) {
  const size_t key = body.find("\"text\":\"");
  if (key == std::string_view::npos) throw std::runtime_error("ASR response has no text");
  std::string text;
  for (size_t i = key + 8; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '"') return text;
    if (c == '\\') {
      if (++i == body.size()) break;
      switch (body[i]) {
        case '"': text.push_back('"'); break;
        case '\\': text.push_back('\\'); break;
        case 'n': text.push_back('\n'); break;
        case 't': text.push_back('\t'); break;
        default: throw std::runtime_error("unsupported JSON escape in ASR text");
      }
    } else {
      text.push_back(c);
    }
  }
  throw std::runtime_error("truncated ASR response");
}

class Socket {
 public:
  Socket() : fd_(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    if (fd_ < 0) throw std::runtime_error("create ASR socket failed");
  }
  ~Socket() { close(fd_); }
  int get() const { return fd_; }
 private:
  int fd_;
};

}  // namespace

AudioAsr::AudioAsr(AVFormatContext* input, int port, int timeout_ms)
    : port_(port), timeout_ms_(timeout_ms) {
  try {
    for (unsigned i = 0; i < input->nb_streams; ++i) {
      if (input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        stream_index_ = static_cast<int>(i);
        const auto* decoder = avcodec_find_decoder(input->streams[i]->codecpar->codec_id);
        if (!decoder) throw std::runtime_error("audio decoder unavailable");
        codec_ = avcodec_alloc_context3(decoder);
        if (!codec_) throw std::runtime_error("audio decoder allocation failed");
        check(avcodec_parameters_to_context(codec_, input->streams[i]->codecpar), "copy audio parameters");
        check(avcodec_open2(codec_, decoder, nullptr), "open audio decoder");
        break;
      }
    }
    if (stream_index_ < 0) throw std::runtime_error("ASR mode requires an audio stream");
  } catch (...) {
    avcodec_free_context(&codec_);
    throw;
  }
}

AudioAsr::~AudioAsr() {
  swr_free(&resampler_);
  av_channel_layout_uninit(&source_layout_);
  avcodec_free_context(&codec_);
}

void AudioAsr::receive_frames(std::vector<int16_t>& samples) {
  AVFrame* frame = av_frame_alloc();
  if (!frame) throw std::runtime_error("audio frame allocation failed");
  try {
    int status = 0;
    while ((status = avcodec_receive_frame(codec_, frame)) == 0) {
      const auto format = static_cast<AVSampleFormat>(frame->format);
      if (!resampler_ || format != source_format_ || frame->sample_rate != source_rate_ ||
          av_channel_layout_compare(&frame->ch_layout, &source_layout_) != 0) {
        swr_free(&resampler_);
        av_channel_layout_uninit(&source_layout_);
        check(av_channel_layout_copy(&source_layout_, &frame->ch_layout), "copy audio layout");
        AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        check(swr_alloc_set_opts2(&resampler_, &mono, AV_SAMPLE_FMT_S16, 16000,
                                  &frame->ch_layout, format, frame->sample_rate, 0, nullptr),
              "configure resampler");
        check(swr_init(resampler_), "initialize resampler");
        source_format_ = format;
        source_rate_ = frame->sample_rate;
      }
      const int capacity = swr_get_out_samples(resampler_, frame->nb_samples);
      if (capacity < 0) check(capacity, "calculate sample count");
      if (capacity == 0) {
        av_frame_unref(frame);
        continue;
      }
      const size_t start = samples.size();
      samples.resize(start + static_cast<size_t>(capacity));
      uint8_t* output[] = {reinterpret_cast<uint8_t*>(samples.data() + start)};
      const int count = swr_convert(resampler_, output, capacity,
                                    const_cast<const uint8_t**>(frame->extended_data),
                                    frame->nb_samples);
      check(count, "resample audio");
      samples.resize(start + static_cast<size_t>(count));
      av_frame_unref(frame);
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF)
      check(status, "receive decoded audio");
  } catch (...) {
    av_frame_free(&frame);
    throw;
  }
  av_frame_free(&frame);
}

std::string AudioAsr::recognize(const std::vector<AVPacket*>& packets) {
  std::vector<int16_t> samples;
  for (AVPacket* packet : packets) {
    if (packet->stream_index != stream_index_) continue;
    check(avcodec_send_packet(codec_, packet), "decode audio packet");
    receive_frames(samples);
  }
  if (samples.empty()) throw std::runtime_error("no decoded audio in segment");
  return post_wav(samples);
}

std::string AudioAsr::post_wav(const std::vector<int16_t>& samples) const {
  const std::string body = wav(samples);
  const std::string header = "POST /v1/stt?strategy=gate HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\nContent-Type: audio/wav\r\n"
                             "Connection: close\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n";
  Socket connection;
  const timeval timeout{timeout_ms_ / 1000, (timeout_ms_ % 1000) * 1000};
  setsockopt(connection.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(connection.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port_));
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(connection.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    throw std::runtime_error("connect local ASR failed");
  const auto send_all = [&](std::string_view data) {
    while (!data.empty()) {
      const ssize_t sent = send(connection.get(), data.data(), data.size(), MSG_NOSIGNAL);
      if (sent <= 0) throw std::runtime_error("send ASR request failed");
      data.remove_prefix(static_cast<size_t>(sent));
    }
  };
  send_all(header);
  send_all(body);
  std::string response;
  char buffer[4096];
  while (response.size() < 16384) {
    const ssize_t received = recv(connection.get(), buffer, sizeof(buffer), 0);
    if (received == 0) break;
    if (received < 0) throw std::runtime_error("ASR response timeout or read failure");
    response.append(buffer, static_cast<size_t>(received));
  }
  if (response.rfind("HTTP/1.1 200 ", 0) != 0 && response.rfind("HTTP/1.0 200 ", 0) != 0)
    throw std::runtime_error("ASR returned non-200 status");
  const size_t split = response.find("\r\n\r\n");
  if (split == std::string::npos) throw std::runtime_error("invalid ASR HTTP response");
  return json_text(std::string_view(response).substr(split + 4));
}

}  // namespace veg
