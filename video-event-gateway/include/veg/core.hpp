#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace veg {

struct Config {
    std::string source;
    std::string output_dir = "output";
    int queue_capacity = 3;
    int warmup_frames = 20;
    int threshold = 35;
    double min_area = 300.0;
    double background_alpha = 0.01;
    double match_distance = 70.0;
    int min_track_hits = 3;
    int max_missed_frames = 5;
    double line_x = 0.5;
    cv::Rect2f zone{0.58F, 0.2F, 0.32F, 0.65F};
    int dwell_frames = 0;
};

Config load_config(const std::string& path);
void validate_config(const Config& config);

enum class EventType { line_cross, zone_enter, dwell };
const char* event_type_name(EventType type);

struct Event {
    EventType type;
    std::int64_t frame_index;
    double timestamp_ms;
    int track_id;
    cv::Rect box;
};

class MotionDetector {
public:
    explicit MotionDetector(const Config& config);
    std::vector<cv::Rect> detect(const cv::Mat& frame);

private:
    Config config_;
    cv::Mat background_;
    int seen_frames_ = 0;
};

class EventTracker {
public:
    explicit EventTracker(const Config& config);
    std::vector<Event> update(const std::vector<cv::Rect>& detections,
                              std::int64_t frame_index, double timestamp_ms,
                              int frame_width, int frame_height);

private:
    struct Track {
        int id;
        cv::Rect box;
        cv::Point2f center;
        int hits = 1;
        int missed = 0;
        bool line_fired = false;
        bool zone_fired = false;
        bool in_zone = false;
        int zone_frames = 0;
        bool dwell_fired = false;
    };

    Config config_;
    std::vector<Track> tracks_;
    int next_id_ = 1;
};

class VideoEventProcessor {
public:
    explicit VideoEventProcessor(const Config& config);
    std::vector<Event> process(const cv::Mat& frame, std::int64_t frame_index,
                               double timestamp_ms);
    std::vector<Event> process_detections(const std::vector<cv::Rect>& boxes,
                                          std::int64_t frame_index,
                                          double timestamp_ms,
                                          int frame_width, int frame_height);

private:
    MotionDetector detector_;
    EventTracker tracker_;
};

}  // namespace veg
