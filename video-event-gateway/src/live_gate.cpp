// A deliberately small, receive-only RTMP ingress and delayed packet gate.
// This is a transport MVP: --demo-allow / --demo-block are not AI moderation.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

namespace {

std::atomic<bool> stopping{false};

void stop_handler(int) { stopping.store(true); }

int interrupt_io(void*) { return stopping.load() ? 1 : 0; }

std::string ff_error(int code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buffer, sizeof(buffer));
  return buffer;
}

void check(int code, const char* action) {
  if (code < 0) throw std::runtime_error(std::string(action) + ": " + ff_error(code));
}

struct Options {
  std::string listen;
  std::string publish;
  int delay_ms = 8000;
  size_t max_bytes = 64U * 1024U * 1024U;
  bool demo_allow = false;
  std::vector<std::pair<double, double>> blocks;
};

double positive_number(const std::string& value, const char* name) {
  char* end = nullptr;
  errno = 0;
  const double result = std::strtod(value.c_str(), &end);
  if (errno || end == value.c_str() || *end || !std::isfinite(result) || result < 0)
    throw std::runtime_error(std::string("invalid ") + name + ": " + value);
  return result;
}

Options parse(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--demo-allow") {
      options.demo_allow = true;
      continue;
    }
    if (i + 1 >= argc) throw std::runtime_error("missing value after " + arg);
    const std::string value = argv[++i];
    if (arg == "--listen") options.listen = value;
    else if (arg == "--publish") options.publish = value;
    else if (arg == "--delay-ms") {
      const double delay = positive_number(value, "delay");
      if (delay > 60000) throw std::runtime_error("delay exceeds 60 seconds");
      options.delay_ms = static_cast<int>(delay);
    } else if (arg == "--max-buffer-mib") {
      const double mib = positive_number(value, "buffer size");
      if (mib < 1 || mib > 1024) throw std::runtime_error("buffer size must be 1..1024 MiB");
      options.max_bytes = static_cast<size_t>(mib * 1024 * 1024);
    }
    else if (arg == "--demo-block") {
      const auto colon = value.find(':');
      if (colon == std::string::npos) throw std::runtime_error("--demo-block needs START:END");
      const double start = positive_number(value.substr(0, colon), "block start");
      const double end = positive_number(value.substr(colon + 1), "block end");
      if (end <= start) throw std::runtime_error("block end must exceed start");
      options.blocks.emplace_back(start, end);
    } else throw std::runtime_error("unknown option: " + arg);
  }
  if (options.listen.empty() || options.publish.empty() || !options.demo_allow)
    throw std::runtime_error("required: --listen URL --publish URL --demo-allow");
  if (options.listen == options.publish) throw std::runtime_error("input and output URL must differ");
  return options;
}

struct PacketDeleter {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

struct Gop {
  std::vector<PacketPtr> packets;
  int64_t start_us = INT64_MAX;
  int64_t end_us = AV_NOPTS_VALUE;
  int64_t completed_at_us = 0;
  bool has_keyframe = false;
  size_t bytes = 0;
};

struct InputDeleter {
  void operator()(AVFormatContext* context) const { avformat_close_input(&context); }
};

int64_t packet_us(const AVPacket& packet, const AVStream& stream) {
  const int64_t stamp = packet.dts != AV_NOPTS_VALUE ? packet.dts : packet.pts;
  return stamp == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
                                 : av_rescale_q(stamp, stream.time_base, AV_TIME_BASE_Q);
}

class Output {
 public:
  Output(AVFormatContext* input, std::string url, int64_t base_us)
      : input_(input), url_(std::move(url)), base_us_(base_us) {
    check(avformat_alloc_output_context2(&context_, nullptr, "flv", url_.c_str()),
          "create FLV output");
    if (!context_) throw std::runtime_error("FLV output unavailable");
    mapping_.assign(input_->nb_streams, -1);
    for (unsigned i = 0; i < input_->nb_streams; ++i) {
      const auto type = input_->streams[i]->codecpar->codec_type;
      if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) continue;
      AVStream* stream = avformat_new_stream(context_, nullptr);
      if (!stream) throw std::runtime_error("create output stream failed");
      check(avcodec_parameters_copy(stream->codecpar, input_->streams[i]->codecpar),
            "copy codec parameters");
      stream->codecpar->codec_tag = 0;
      stream->time_base = input_->streams[i]->time_base;
      mapping_[i] = stream->index;
    }
    if (context_->nb_streams == 0) throw std::runtime_error("no audio/video streams");
    last_dts_.assign(context_->nb_streams, AV_NOPTS_VALUE);
    check(avio_open(&context_->pb, url_.c_str(), AVIO_FLAG_WRITE), "connect to public output");
    AVDictionary* mux_options = nullptr;
    av_dict_set(&mux_options, "flvflags", "no_duration_filesize", 0);
    const int header_status = avformat_write_header(context_, &mux_options);
    av_dict_free(&mux_options);
    check(header_status, "write FLV header");
    header_written_ = true;
    std::cout << "output_connected=" << url_ << std::endl;
  }

  Output(const Output&) = delete;
  Output& operator=(const Output&) = delete;

  ~Output() {
    if (context_) {
      if (header_written_) av_write_trailer(context_);
      if (context_->pb) avio_closep(&context_->pb);
      avformat_free_context(context_);
    }
  }

  void skip(int64_t duration_us) { skipped_us_ += std::max<int64_t>(duration_us, 0); }

  void send(Gop& gop) {
    for (auto& packet : gop.packets) {
      const int source_index = packet->stream_index;
      if (source_index < 0 || static_cast<size_t>(source_index) >= mapping_.size() ||
          mapping_[source_index] < 0) continue;
      const int destination_index = mapping_[source_index];
      const AVRational source_base = input_->streams[source_index]->time_base;
      const AVRational destination_base = context_->streams[destination_index]->time_base;
      const auto shifted = [&](int64_t stamp) -> int64_t {
        if (stamp == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
        const int64_t us = av_rescale_q(stamp, source_base, AV_TIME_BASE_Q) - base_us_ - skipped_us_;
        return av_rescale_q(std::max<int64_t>(0, us), AV_TIME_BASE_Q, destination_base);
      };
      int64_t dts = shifted(packet->dts);
      int64_t pts = shifted(packet->pts);
      if (dts != AV_NOPTS_VALUE && last_dts_[destination_index] != AV_NOPTS_VALUE &&
          dts <= last_dts_[destination_index]) {
        const int64_t correction = last_dts_[destination_index] + 1 - dts;
        dts += correction;
        if (pts != AV_NOPTS_VALUE) pts += correction;
      }
      if (pts != AV_NOPTS_VALUE && dts != AV_NOPTS_VALUE) pts = std::max(pts, dts);
      packet->dts = dts;
      packet->pts = pts;
      packet->duration = av_rescale_q(packet->duration, source_base, destination_base);
      packet->stream_index = destination_index;
      packet->pos = -1;
      check(av_interleaved_write_frame(context_, packet.get()), "publish approved packet");
      if (dts != AV_NOPTS_VALUE) last_dts_[destination_index] = dts;
    }
  }

 private:
  AVFormatContext* input_ = nullptr;
  AVFormatContext* context_ = nullptr;
  std::string url_;
  int64_t base_us_ = 0;
  int64_t skipped_us_ = 0;
  bool header_written_ = false;
  std::vector<int> mapping_;
  std::vector<int64_t> last_dts_;
};

class Gate {
 public:
  Gate(AVFormatContext* input, const Options& options)
      : input_(input), options_(options) {}

  void accept(PacketPtr packet) {
    if (packet->stream_index < 0 ||
        static_cast<unsigned>(packet->stream_index) >= input_->nb_streams) return;
    const AVStream& stream = *input_->streams[packet->stream_index];
    if (stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
        stream.codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return;
    const int64_t stamp = packet_us(*packet, stream);
    if (stamp == AV_NOPTS_VALUE) throw std::runtime_error("packet lacks PTS/DTS; fail closed");
    base_us_ = std::min(base_us_, stamp);
    const bool keyframe = stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                          (packet->flags & AV_PKT_FLAG_KEY);
    if (keyframe && current_.has_keyframe) finish_current(stamp);
    if (keyframe) current_.has_keyframe = true;
    current_.start_us = std::min(current_.start_us, stamp);
    const int64_t duration = av_rescale_q(packet->duration, stream.time_base, AV_TIME_BASE_Q);
    current_.end_us = std::max(current_.end_us, stamp + std::max<int64_t>(duration, 1));
    current_.bytes += static_cast<size_t>(packet->size);
    buffered_bytes_ += static_cast<size_t>(packet->size);
    current_.packets.push_back(std::move(packet));
    if (buffered_bytes_ > options_.max_bytes)
      throw std::runtime_error("buffer limit exceeded; fail closed");
    drain(false);
  }

  void finish() {
    if (!current_.packets.empty()) finish_current(current_.end_us);
    drain(true);
    std::cout << "allowed_gops=" << allowed_ << " blocked_gops=" << blocked_
              << " filtered_audio_packets=" << filtered_audio_packets_
              << " peak_buffer_bytes=" << peak_bytes_ << std::endl;
  }

 private:
  void finish_current(int64_t next_key_us) {
    current_.end_us = std::max(current_.end_us, next_key_us);
    current_.completed_at_us = av_gettime_relative();
    ready_.push_back(std::move(current_));
    current_ = Gop{};
    peak_bytes_ = std::max(peak_bytes_, buffered_bytes_);
  }

  bool is_blocked(const Gop& gop) const {
    const double start = static_cast<double>(gop.start_us - base_us_) / AV_TIME_BASE;
    const double end = static_cast<double>(gop.end_us - base_us_) / AV_TIME_BASE;
    for (const auto& range : options_.blocks)
      if (start < range.second && end > range.first) return true;
    return false;
  }

  bool blocked_audio_packet(const AVPacket& packet) const {
    const AVStream& stream = *input_->streams[packet.stream_index];
    if (stream.codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return false;
    const int64_t stamp = packet_us(packet, stream);
    const double start = static_cast<double>(stamp - base_us_) / AV_TIME_BASE;
    const double duration = static_cast<double>(av_rescale_q(
        packet.duration, stream.time_base, AV_TIME_BASE_Q)) / AV_TIME_BASE;
    const double end = start + std::max(duration, 0.000001);
    for (const auto& range : options_.blocks)
      if (start < range.second && end > range.first) return true;
    return false;
  }

  void drain(bool wait) {
    while (!ready_.empty() && !stopping.load()) {
      const int64_t deadline = ready_.front().completed_at_us +
                               static_cast<int64_t>(options_.delay_ms) * 1000;
      if (!wait && av_gettime_relative() < deadline) return;
      if (wait && av_gettime_relative() < deadline)
        std::this_thread::sleep_for(std::chrono::microseconds(deadline - av_gettime_relative()));
      Gop gop = std::move(ready_.front());
      ready_.pop_front();
      buffered_bytes_ -= gop.bytes;
      const int64_t duration = std::max<int64_t>(0, gop.end_us - gop.start_us);
      if (!gop.has_keyframe || is_blocked(gop)) {
        // Dropping a complete GOP avoids publishing undecodable dependent frames.
        if (output_) output_->skip(duration);
        else skipped_before_output_us_ += duration;
        ++blocked_;
        std::cout << "blocked_gop start_ms=" << (gop.start_us - base_us_) / 1000
                  << " end_ms=" << (gop.end_us - base_us_) / 1000 << std::endl;
        continue;
      }
      if (!output_) {
        output_ = std::make_unique<Output>(input_, options_.publish, base_us_);
        output_->skip(skipped_before_output_us_);
      }
      // Audio may arrive on the other side of a video keyframe. Filter it by
      // its own timestamp so a boundary packet cannot escape a blocked range.
      const auto old_size = gop.packets.size();
      gop.packets.erase(std::remove_if(gop.packets.begin(), gop.packets.end(),
                                       [&](const PacketPtr& packet) {
                                         return blocked_audio_packet(*packet);
                                       }),
                        gop.packets.end());
      filtered_audio_packets_ += old_size - gop.packets.size();
      output_->send(gop);
      ++allowed_;
    }
  }

  AVFormatContext* input_;
  const Options& options_;
  int64_t base_us_ = INT64_MAX;
  Gop current_;
  std::deque<Gop> ready_;
  std::unique_ptr<Output> output_;
  size_t buffered_bytes_ = 0;
  size_t peak_bytes_ = 0;
  int64_t skipped_before_output_us_ = 0;
  uint64_t allowed_ = 0;
  uint64_t blocked_ = 0;
  uint64_t filtered_audio_packets_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse(argc, argv);
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    AVFormatContext* raw_input = avformat_alloc_context();
    if (!raw_input) throw std::runtime_error("allocate input context failed");
    raw_input->interrupt_callback = {interrupt_io, nullptr};
    AVDictionary* settings = nullptr;
    av_dict_set(&settings, "listen", "1", 0);
    const int opened = avformat_open_input(&raw_input, options.listen.c_str(), nullptr, &settings);
    av_dict_free(&settings);
    check(opened, "receive OBS RTMP stream");
    std::unique_ptr<AVFormatContext, InputDeleter> input(raw_input);
    check(avformat_find_stream_info(input.get(), nullptr), "read stream metadata");
    std::cout << "ingest_connected=" << options.listen << " delay_ms=" << options.delay_ms
              << " mode=DEMO_ONLY" << std::endl;
    Gate gate(input.get(), options);
    uint64_t received_packets = 0;
    while (!stopping.load()) {
      PacketPtr packet(av_packet_alloc());
      if (!packet) throw std::runtime_error("allocate packet failed");
      const int code = av_read_frame(input.get(), packet.get());
      // FFmpeg's RTMP listener reports a publisher's normal disconnect as EIO.
      if (code == AVERROR_EOF || (code == AVERROR(EIO) && received_packets > 0)) break;
      if (code < 0) {
        if (stopping.load()) break;
        throw std::runtime_error("read input: " + ff_error(code));
      }
      gate.accept(std::move(packet));
      ++received_packets;
    }
    if (!stopping.load()) gate.finish();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "live_gate: " << error.what() << std::endl;
    return 1;
  }
}
