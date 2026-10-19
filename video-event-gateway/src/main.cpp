#include "veg/core.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

struct Frame {
    std::int64_t index;
    double timestamp_ms;
    cv::Mat image;
};

class FrameQueue {
public:
    FrameQueue(std::size_t capacity, bool drop_oldest)
        : capacity_(capacity), drop_oldest_(drop_oldest) {}

    bool push(Frame frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!drop_oldest_) {
            space_.wait(lock, [this] { return closed_ || frames_.size() < capacity_; });
        }
        if (closed_) return false;
        if (frames_.size() == capacity_) {
            frames_.pop_front();
            ++dropped_;
        }
        frames_.push_back(std::move(frame));
        ready_.notify_one();
        return true;
    }

    std::optional<Frame> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !frames_.empty(); });
        if (frames_.empty()) return std::nullopt;
        Frame frame = std::move(frames_.front());
        frames_.pop_front();
        space_.notify_one();
        return frame;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
        space_.notify_all();
    }

    std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    const std::size_t capacity_;
    const bool drop_oldest_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable space_;
    std::deque<Frame> frames_;
    bool closed_ = false;
    std::uint64_t dropped_ = 0;
};

std::string escape_json(const std::string& text) {
    std::string escaped;
    for (unsigned char ch : text) {
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch < 0x20) {
                    constexpr char digits[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += digits[ch >> 4];
                    escaped += digits[ch & 0x0f];
                } else {
                    escaped += static_cast<char>(ch);
                }
        }
    }
    return escaped;
}

std::string safe_source_name(std::string source) {
    const std::size_t scheme = source.find("://");
    if (scheme == std::string::npos) return source;
    const std::size_t credentials_start = scheme + 3;
    const std::size_t path_start = source.find('/', credentials_start);
    const std::size_t at = source.find('@', credentials_start);
    if (at != std::string::npos &&
        (path_start == std::string::npos || at < path_start)) {
        source.replace(credentials_start, at - credentials_start, "***");
    }
    return source;
}

bool is_camera_number(const std::string& source) {
    return !source.empty() &&
           source.find_first_not_of("0123456789") == std::string::npos &&
           !std::filesystem::exists(source);
}

void save_event(const veg::Event& event, const cv::Mat& frame,
                const veg::Config& config, std::ofstream& events,
                const std::filesystem::path& output_dir) {
    const std::string name = "frame_" + std::to_string(event.frame_index) +
                             "_track_" + std::to_string(event.track_id) + "_" +
                             veg::event_type_name(event.type) + ".png";
    cv::Mat annotated = frame.clone();
    const int line_px = static_cast<int>(config.line_x * frame.cols);
    const cv::Rect zone_px(
        static_cast<int>(config.zone.x * frame.cols),
        static_cast<int>(config.zone.y * frame.rows),
        static_cast<int>(config.zone.width * frame.cols),
        static_cast<int>(config.zone.height * frame.rows));
    cv::line(annotated, {line_px, 0}, {line_px, frame.rows - 1}, {0, 255, 255}, 2);
    cv::rectangle(annotated, zone_px, {255, 255, 0}, 2);
    cv::rectangle(annotated, event.box, {0, 0, 255}, 2);
    cv::putText(annotated, veg::event_type_name(event.type),
                {event.box.x, std::max(16, event.box.y - 5)},
                cv::FONT_HERSHEY_SIMPLEX, 0.5, {0, 0, 255}, 1);
    if (!cv::imwrite((output_dir / name).string(), annotated)) {
        throw std::runtime_error("failed to save event image");
    }

    events << "{\"type\":\"" << veg::event_type_name(event.type)
           << "\",\"frame_index\":" << event.frame_index
           << ",\"timestamp_ms\":" << std::fixed << std::setprecision(2)
           << event.timestamp_ms << ",\"track_id\":" << event.track_id
           << ",\"source\":\"" << escape_json(safe_source_name(config.source))
           << "\",\"bbox\":[" << event.box.x << ',' << event.box.y << ','
           << event.box.width << ',' << event.box.height
           << "],\"snapshot\":\"" << escape_json(name) << "\"}\n";
    events.flush();
    if (!events) throw std::runtime_error("failed to write events.jsonl");
}

void print_usage() {
    std::cout << "Usage: veg_detect [--config config.yaml] [--source file|rtsp-url|camera-index]"
                 " [--output directory] [--live] [--max-frames count]\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string config_path;
        std::string source_override;
        std::string output_override;
        bool live_override = false;
        std::int64_t max_frames = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                print_usage();
                return 0;
            }
            if ((arg == "--config" || arg == "--source" || arg == "--output" ||
                 arg == "--max-frames") &&
                i + 1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--config") config_path = value;
                else if (arg == "--source") source_override = value;
                else if (arg == "--output") output_override = value;
                else max_frames = std::stoll(value);
            } else if (arg == "--live") {
                live_override = true;
            } else {
                print_usage();
                throw std::runtime_error("unknown or incomplete argument: " + arg);
            }
        }
        if (max_frames < 0) throw std::runtime_error("max-frames cannot be negative");

        veg::Config config = config_path.empty() ? veg::Config{}
                                                  : veg::load_config(config_path);
        if (!source_override.empty()) config.source = source_override;
        if (!output_override.empty()) config.output_dir = output_override;
        veg::validate_config(config);
        if (config.source.empty()) {
            print_usage();
            throw std::runtime_error("source is required");
        }

        const bool live = live_override || is_camera_number(config.source) ||
                          config.source.rfind("rtsp://", 0) == 0 ||
                          config.source.rfind("rtsps://", 0) == 0;
        cv::VideoCapture capture;
        const bool opened = is_camera_number(config.source)
                                ? capture.open(std::stoi(config.source))
                                : capture.open(config.source);
        if (!opened) throw std::runtime_error("cannot open source: " + config.source);

        const double capture_fps = capture.get(cv::CAP_PROP_FPS);
        const double fallback_fps = capture_fps > 0 && capture_fps < 1000
                                        ? capture_fps : 25.0;
        const std::filesystem::path output_dir(config.output_dir);
        std::filesystem::create_directories(output_dir);
        std::ofstream events(output_dir / "events.jsonl", std::ios::trunc);
        if (!events) throw std::runtime_error("cannot create events.jsonl");

        FrameQueue queue(static_cast<std::size_t>(config.queue_capacity), live);
        std::exception_ptr producer_error;
        const auto started = std::chrono::steady_clock::now();
        std::thread producer([&] {
            try {
                std::int64_t index = 0;
                cv::Mat image;
                while (capture.read(image)) {
                    double timestamp_ms;
                    if (live) {
                        timestamp_ms = std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - started).count();
                    } else {
                        timestamp_ms = capture.get(cv::CAP_PROP_POS_MSEC);
                        if (!std::isfinite(timestamp_ms) ||
                            (timestamp_ms <= 0 && index > 0)) {
                            timestamp_ms = index * 1000.0 / fallback_fps;
                        }
                    }
                    if (!queue.push({index++, timestamp_ms, std::move(image)})) break;
                    if (max_frames > 0 && index >= max_frames) break;
                }
            } catch (...) {
                producer_error = std::current_exception();
            }
            queue.close();
        });

        std::uint64_t processed = 0;
        std::uint64_t event_count = 0;
        try {
            veg::VideoEventProcessor processor(config);
            while (auto frame = queue.pop()) {
                const auto found = processor.process(frame->image, frame->index,
                                                     frame->timestamp_ms);
                for (const auto& event : found) {
                    save_event(event, frame->image, config, events, output_dir);
                    ++event_count;
                }
                ++processed;
            }
        } catch (...) {
            queue.close();
            producer.join();
            throw;
        }
        producer.join();
        if (producer_error) std::rethrow_exception(producer_error);

        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - started).count();
        std::cout << "{\"processed_frames\":" << processed
                  << ",\"dropped_frames\":" << queue.dropped()
                  << ",\"events\":" << event_count
                  << ",\"elapsed_seconds\":" << std::fixed << std::setprecision(3)
                  << elapsed << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "veg_detect: " << error.what() << '\n';
        return 1;
    }
}
