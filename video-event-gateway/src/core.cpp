#include "veg/core.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace veg {
namespace {

cv::Point2f center_of(const cv::Rect& box) {
    return {box.x + box.width * 0.5F, box.y + box.height * 0.5F};
}

template <typename T>
void read_if_present(const cv::FileStorage& storage, const char* name, T& value) {
    const cv::FileNode node = storage[name];
    if (!node.empty()) {
        node >> value;
    }
}

}  // namespace

Config load_config(const std::string& path) {
    cv::FileStorage storage(path, cv::FileStorage::READ);
    if (!storage.isOpened()) {
        throw std::runtime_error("cannot open configuration: " + path);
    }

    Config config;
    read_if_present(storage, "source", config.source);
    read_if_present(storage, "output_dir", config.output_dir);
    read_if_present(storage, "queue_capacity", config.queue_capacity);
    read_if_present(storage, "warmup_frames", config.warmup_frames);
    read_if_present(storage, "threshold", config.threshold);
    read_if_present(storage, "min_area", config.min_area);
    read_if_present(storage, "background_alpha", config.background_alpha);
    read_if_present(storage, "match_distance", config.match_distance);
    read_if_present(storage, "min_track_hits", config.min_track_hits);
    read_if_present(storage, "max_missed_frames", config.max_missed_frames);
    read_if_present(storage, "line_x", config.line_x);
    read_if_present(storage, "dwell_frames", config.dwell_frames);

    const cv::FileNode zone = storage["zone"];
    if (!zone.empty()) {
        if (!zone.isSeq() || zone.size() != 4) {
            throw std::runtime_error("zone must contain [left, top, right, bottom]");
        }
        const float left = static_cast<float>(static_cast<double>(zone[0]));
        const float top = static_cast<float>(static_cast<double>(zone[1]));
        const float right = static_cast<float>(static_cast<double>(zone[2]));
        const float bottom = static_cast<float>(static_cast<double>(zone[3]));
        config.zone = {left, top, right - left, bottom - top};
    }
    validate_config(config);
    return config;
}

void validate_config(const Config& config) {
    if (config.queue_capacity < 1 || config.queue_capacity > 64 ||
        config.warmup_frames < 1 || config.threshold < 1 || config.threshold > 255 ||
        config.min_area <= 0 || config.background_alpha <= 0 ||
        config.background_alpha > 1 || config.match_distance <= 0 ||
        config.min_track_hits < 1 || config.max_missed_frames < 0 ||
        config.line_x <= 0 || config.line_x >= 1 ||
        config.zone.x < 0 || config.zone.y < 0 || config.zone.width <= 0 ||
        config.zone.height <= 0 || config.zone.x + config.zone.width > 1 ||
        config.zone.y + config.zone.height > 1 || config.dwell_frames < 0) {
        throw std::runtime_error("invalid numeric configuration");
    }
    if (config.output_dir.empty()) {
        throw std::runtime_error("output_dir cannot be empty");
    }
}

const char* event_type_name(EventType type) {
    switch (type) {
        case EventType::line_cross: return "line_cross";
        case EventType::zone_enter: return "zone_enter";
        case EventType::dwell: return "dwell";
    }
    return "unknown";
}

MotionDetector::MotionDetector(const Config& config) : config_(config) {}

std::vector<cv::Rect> MotionDetector::detect(const cv::Mat& frame) {
    if (frame.empty()) {
        return {};
    }

    cv::Mat gray;
    if (frame.channels() == 3) {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    } else if (frame.channels() == 1) {
        gray = frame;
    } else {
        throw std::runtime_error("only BGR and grayscale frames are supported");
    }
    cv::GaussianBlur(gray, gray, {5, 5}, 0);

    if (background_.empty()) {
        gray.convertTo(background_, CV_32F);
    }
    if (seen_frames_++ < config_.warmup_frames) {
        cv::accumulateWeighted(gray, background_, 0.08);
        return {};
    }

    cv::Mat background_u8, delta, mask;
    background_.convertTo(background_u8, CV_8U);
    cv::absdiff(gray, background_u8, delta);
    cv::threshold(delta, mask, config_.threshold, 255, cv::THRESH_BINARY);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, {3, 3});
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
    cv::dilate(mask, mask, kernel, {-1, -1}, 1);

    cv::Mat static_pixels;
    cv::bitwise_not(mask, static_pixels);
    cv::accumulateWeighted(gray, background_, config_.background_alpha, static_pixels);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::vector<cv::Rect> boxes;
    for (const auto& contour : contours) {
        if (cv::contourArea(contour) >= config_.min_area) {
            boxes.push_back(cv::boundingRect(contour));
        }
    }
    return boxes;
}

EventTracker::EventTracker(const Config& config) : config_(config) {}

std::vector<Event> EventTracker::update(const std::vector<cv::Rect>& detections,
                                        std::int64_t frame_index, double timestamp_ms,
                                        int frame_width, int frame_height) {
    const float line_px = static_cast<float>(config_.line_x * frame_width);
    const cv::Rect2f zone_px(config_.zone.x * frame_width,
                             config_.zone.y * frame_height,
                             config_.zone.width * frame_width,
                             config_.zone.height * frame_height);
    std::vector<bool> used(detections.size(), false);
    std::vector<Event> events;

    for (Track& track : tracks_) {
        int best = -1;
        double best_distance = config_.match_distance;
        for (std::size_t i = 0; i < detections.size(); ++i) {
            if (used[i]) continue;
            const double distance = cv::norm(center_of(detections[i]) - track.center);
            if (distance < best_distance) {
                best_distance = distance;
                best = static_cast<int>(i);
            }
        }
        if (best < 0) {
            ++track.missed;
            continue;
        }

        used[best] = true;
        const cv::Point2f previous = track.center;
        track.box = detections[best];
        track.center = center_of(track.box);
        track.missed = 0;
        ++track.hits;

        if (track.hits >= config_.min_track_hits) {
            if (!track.line_fired && previous.x < line_px && track.center.x >= line_px) {
                events.push_back({EventType::line_cross, frame_index, timestamp_ms,
                                  track.id, track.box});
                track.line_fired = true;
            }
            const bool inside = zone_px.contains(track.center);
            if (!track.zone_fired && inside && !track.in_zone) {
                events.push_back({EventType::zone_enter, frame_index, timestamp_ms,
                                  track.id, track.box});
                track.zone_fired = true;
            }
            track.zone_frames = inside ? track.zone_frames + 1 : 0;
            if (!track.dwell_fired && config_.dwell_frames > 0 &&
                track.zone_frames >= config_.dwell_frames) {
                events.push_back({EventType::dwell, frame_index, timestamp_ms,
                                  track.id, track.box});
                track.dwell_fired = true;
            }
            track.in_zone = inside;
        }
    }

    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [this](const Track& track) {
                                     return track.missed > config_.max_missed_frames;
                                 }), tracks_.end());
    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (!used[i]) {
            tracks_.push_back({next_id_++, detections[i], center_of(detections[i])});
        }
    }
    return events;
}

VideoEventProcessor::VideoEventProcessor(const Config& config)
    : detector_(config), tracker_(config) {}

std::vector<Event> VideoEventProcessor::process(const cv::Mat& frame,
                                                 std::int64_t frame_index,
                                                 double timestamp_ms) {
    return tracker_.update(detector_.detect(frame), frame_index, timestamp_ms,
                           frame.cols, frame.rows);
}

std::vector<Event> VideoEventProcessor::process_detections(
    const std::vector<cv::Rect>& boxes, std::int64_t frame_index,
    double timestamp_ms, int frame_width, int frame_height) {
    return tracker_.update(boxes, frame_index, timestamp_ms,
                           frame_width, frame_height);
}

}  // namespace veg
