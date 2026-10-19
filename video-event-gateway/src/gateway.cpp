#include "veg/core.hpp"

#include <drogon/drogon.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef VEG_WITH_NCNN
#include "yoloV8.h"
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Source {
    std::string id;
    std::string uri;
};

struct GatewayConfig {
    veg::Config analysis;
    std::vector<Source> sources;
    int http_port = 8088;
    int reconnect_ms = 1000;
    std::string detector = "motion";
    std::string model_param;
    std::string model_bin;
    int model_size = 320;
    int class_id = 0;
    float score_threshold = 0.4F;
};

struct CameraState {
    explicit CameraState(Source input) : source(std::move(input)) {}

    Source source;
    std::mutex mutex;
    std::string state = "starting";
    std::deque<Json::Value> recent_events;
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> events{0};
    std::atomic<std::uint64_t> reconnects{0};
    std::atomic<std::int64_t> last_frame_ms{0};
    std::atomic<std::uint64_t> processing_us{0};
    std::atomic<bool> stop{false};
    std::thread worker;
};

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count();
}

std::int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

void set_state(const std::shared_ptr<CameraState>& camera, std::string state) {
    std::lock_guard<std::mutex> lock(camera->mutex);
    camera->state = std::move(state);
}

GatewayConfig load_gateway_config(const std::string& path) {
    GatewayConfig config;
    config.analysis = veg::load_config(path);
    cv::FileStorage storage(path, cv::FileStorage::READ);
    if (!storage.isOpened()) throw std::runtime_error("cannot open gateway config");
    const cv::FileNode port = storage["http_port"];
    const cv::FileNode reconnect = storage["reconnect_ms"];
    if (!port.empty()) port >> config.http_port;
    if (!reconnect.empty()) reconnect >> config.reconnect_ms;
    const cv::FileNode detector = storage["detector"];
    if (!detector.empty()) detector >> config.detector;
    const cv::FileNode param = storage["model_param"];
    const cv::FileNode bin = storage["model_bin"];
    const cv::FileNode size = storage["model_size"];
    const cv::FileNode class_id = storage["class_id"];
    const cv::FileNode score = storage["score_threshold"];
    if (!param.empty()) param >> config.model_param;
    if (!bin.empty()) bin >> config.model_bin;
    if (!size.empty()) size >> config.model_size;
    if (!class_id.empty()) class_id >> config.class_id;
    if (!score.empty()) score >> config.score_threshold;
    if (config.http_port < 1 || config.http_port > 65535 ||
        config.reconnect_ms < 100 || config.reconnect_ms > 60000) {
        throw std::runtime_error("invalid http_port or reconnect_ms");
    }
    if (config.detector != "motion" && config.detector != "ncnn_yolov8") {
        throw std::runtime_error("detector must be motion or ncnn_yolov8");
    }
    if (config.model_size < 128 || config.model_size > 1280 ||
        config.class_id < 0 || config.class_id > 79 ||
        config.score_threshold <= 0 || config.score_threshold >= 1) {
        throw std::runtime_error("invalid model_size, class_id or score_threshold");
    }
    if (config.detector == "ncnn_yolov8") {
#ifndef VEG_WITH_NCNN
        throw std::runtime_error("gateway was built without ncnn support");
#else
        const auto base = std::filesystem::absolute(path).parent_path();
        config.model_param = std::filesystem::absolute(base / config.model_param).string();
        config.model_bin = std::filesystem::absolute(base / config.model_bin).string();
        if (!std::filesystem::is_regular_file(config.model_param) ||
            !std::filesystem::is_regular_file(config.model_bin)) {
            throw std::runtime_error("ncnn model files are missing");
        }
#endif
    }
    const cv::FileNode sources = storage["sources"];
    if (!sources.isSeq() || sources.empty()) {
        throw std::runtime_error("sources must be a nonempty YAML sequence");
    }
    for (const auto& node : sources) {
        Source source;
        node["id"] >> source.id;
        node["uri"] >> source.uri;
        if (source.id.empty() || source.uri.empty() ||
            source.id.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" ) !=
                std::string::npos) {
            throw std::runtime_error("source id must be alphanumeric, _ or -, and uri nonempty");
        }
        if (source.uri.find("://") == std::string::npos &&
            !std::filesystem::is_regular_file(source.uri)) {
            throw std::runtime_error("source file does not exist: " + source.uri);
        }
        for (const auto& existing : config.sources) {
            if (existing.id == source.id) {
                throw std::runtime_error("duplicate source id: " + source.id);
            }
        }
        config.sources.push_back(std::move(source));
    }
    return config;
}

Json::Value camera_json(const std::shared_ptr<CameraState>& camera) {
    Json::Value value;
    value["id"] = camera->source.id;
    {
        std::lock_guard<std::mutex> lock(camera->mutex);
        value["state"] = camera->state;
    }
    value["processed_frames"] = static_cast<Json::UInt64>(camera->frames.load());
    value["events"] = static_cast<Json::UInt64>(camera->events.load());
    value["reconnects"] = static_cast<Json::UInt64>(camera->reconnects.load());
    const auto frames = camera->frames.load();
    value["mean_processing_ms"] = frames == 0 ? 0.0 :
        static_cast<double>(camera->processing_us.load()) / frames / 1000.0;
    const auto last = camera->last_frame_ms.load();
    value["last_frame_age_ms"] = last == 0 ? Json::Value(Json::nullValue)
                                           : Json::Value(static_cast<Json::Int64>(steady_ms() - last));
    return value;
}

void save_event(const std::shared_ptr<CameraState>& camera, const veg::Event& event,
                const cv::Mat& frame, std::ofstream& journal,
                const std::filesystem::path& directory) {
    const std::string image_name = "frame_" + std::to_string(event.frame_index) +
        "_track_" + std::to_string(event.track_id) + "_" +
        veg::event_type_name(event.type) + ".png";
    cv::Mat annotated = frame.clone();
    cv::rectangle(annotated, event.box, {0, 0, 255}, 2);
    cv::putText(annotated, veg::event_type_name(event.type),
                {event.box.x, std::max(18, event.box.y - 5)},
                cv::FONT_HERSHEY_SIMPLEX, 0.6, {0, 0, 255}, 2);
    if (!cv::imwrite((directory / image_name).string(), annotated)) {
        throw std::runtime_error("failed to save event snapshot");
    }

    Json::Value record;
    record["camera_id"] = camera->source.id;
    record["type"] = veg::event_type_name(event.type);
    record["track_id"] = event.track_id;
    record["frame_index"] = static_cast<Json::Int64>(event.frame_index);
    record["video_timestamp_ms"] = event.timestamp_ms;
    record["unix_ms"] = static_cast<Json::Int64>(unix_ms());
    record["bbox"].append(event.box.x);
    record["bbox"].append(event.box.y);
    record["bbox"].append(event.box.width);
    record["bbox"].append(event.box.height);
    record["snapshot"] = (directory / image_name).string();
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    journal << Json::writeString(writer, record) << '\n';
    journal.flush();
    if (!journal) throw std::runtime_error("failed to append event journal");
    {
        std::lock_guard<std::mutex> lock(camera->mutex);
        camera->recent_events.push_back(record);
        if (camera->recent_events.size() > 200) camera->recent_events.pop_front();
    }
    ++camera->events;
}

void run_camera(const std::shared_ptr<CameraState>& camera,
                const GatewayConfig& config, std::uint64_t max_frames) {
    try {
        const auto directory = std::filesystem::path(config.analysis.output_dir) /
                               camera->source.id;
        std::filesystem::create_directories(directory);
        std::ofstream journal(directory / "events.jsonl", std::ios::trunc);
        if (!journal) throw std::runtime_error("cannot open event journal");
        const bool file_source = std::filesystem::is_regular_file(camera->source.uri);
#ifdef VEG_WITH_NCNN
        YoloV8 model;
        if (config.detector == "ncnn_yolov8" &&
            model.load(config.model_size, config.model_param, config.model_bin) != 0) {
            throw std::runtime_error("failed to load ncnn model");
        }
#endif

        while (!camera->stop.load()) {
            set_state(camera, "connecting");
            cv::VideoCapture capture(camera->source.uri);
            if (!capture.isOpened()) {
                set_state(camera, "reconnecting");
                ++camera->reconnects;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config.reconnect_ms));
                continue;
            }
            veg::Config analysis = config.analysis;
            analysis.source = camera->source.uri;
            veg::VideoEventProcessor processor(analysis);
            const double fps = capture.get(cv::CAP_PROP_FPS);
            const double safe_fps = fps > 0 && fps < 1000 ? fps : 25.0;
            const auto session_start = Clock::now();
            set_state(camera, "running");
            cv::Mat frame;
            bool read_any = false;
            while (!camera->stop.load() && capture.read(frame)) {
                read_any = true;
                const std::int64_t index = static_cast<std::int64_t>(camera->frames.load());
                double timestamp = file_source
                    ? capture.get(cv::CAP_PROP_POS_MSEC)
                    : std::chrono::duration<double, std::milli>(
                          Clock::now() - session_start).count();
                if (timestamp <= 0 && index > 0) timestamp = index * 1000.0 / safe_fps;
                const auto processing_start = Clock::now();
                std::vector<veg::Event> found;
                if (config.detector == "motion") {
                    found = processor.process(frame, index, timestamp);
                }
#ifdef VEG_WITH_NCNN
                else {
                    std::vector<Object> objects;
                    if (model.detect(frame, objects, config.score_threshold, 0.45F) != 0) {
                        throw std::runtime_error("ncnn inference failed");
                    }
                    std::vector<cv::Rect> boxes;
                    for (const auto& object : objects) {
                        if (object.label == config.class_id && object.rect.area() > 0) {
                            boxes.push_back(object.rect);
                        }
                    }
                    found = processor.process_detections(boxes, index, timestamp,
                                                         frame.cols, frame.rows);
                }
#endif
                for (const auto& event : found) {
                    save_event(camera, event, frame, journal, directory);
                }
                camera->processing_us += std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - processing_start).count();
                ++camera->frames;
                camera->last_frame_ms.store(steady_ms());
                if (max_frames > 0 && camera->frames.load() >= max_frames) {
                    set_state(camera, "completed");
                    return;
                }
            }
            capture.release();
            if (file_source) {
                set_state(camera, read_any ? "completed" : "failed");
                return;
            }
            if (!camera->stop.load()) {
                set_state(camera, "reconnecting");
                ++camera->reconnects;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config.reconnect_ms));
            }
        }
        set_state(camera, "stopped");
    } catch (const std::exception& error) {
        std::cerr << "camera " << camera->source.id << ": " << error.what() << '\n';
        set_state(camera, "failed");
    }
}

void usage() {
    std::cout << "Usage: veg_gateway --config config/gateway.yaml"
                 " [--max-frames N]\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string config_path;
        std::uint64_t max_frames = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") { usage(); return 0; }
            if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
            else if (arg == "--max-frames" && i + 1 < argc) {
                max_frames = std::stoull(argv[++i]);
            } else {
                usage();
                throw std::runtime_error("unknown or incomplete argument: " + arg);
            }
        }
        if (config_path.empty()) { usage(); return 1; }
        const GatewayConfig config = load_gateway_config(config_path);
        std::vector<std::shared_ptr<CameraState>> cameras;
        cameras.reserve(config.sources.size());
        for (const auto& source : config.sources) {
            cameras.push_back(std::make_shared<CameraState>(source));
        }

        drogon::app().registerHandler(
            "/health",
            [cameras](const drogon::HttpRequestPtr&,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                Json::Value result;
                result["status"] = "ok";
                for (const auto& camera : cameras) {
                    result["cameras"].append(camera_json(camera));
                }
                callback(drogon::HttpResponse::newHttpJsonResponse(result));
            }, {drogon::Get});

        drogon::app().registerHandler(
            "/api/v1/events",
            [cameras](const drogon::HttpRequestPtr& request,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                const std::string filter = request->getParameter("camera_id");
                Json::Value result(Json::arrayValue);
                for (const auto& camera : cameras) {
                    if (!filter.empty() && camera->source.id != filter) continue;
                    std::lock_guard<std::mutex> lock(camera->mutex);
                    for (const auto& event : camera->recent_events) result.append(event);
                }
                callback(drogon::HttpResponse::newHttpJsonResponse(result));
            }, {drogon::Get});

        drogon::app().registerHandler(
            "/metrics",
            [cameras](const drogon::HttpRequestPtr&,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                std::string body;
                for (const auto& camera : cameras) {
                    const std::string label = "{camera=\"" + camera->source.id + "\"} ";
                    body += "veg_frames_total" + label +
                            std::to_string(camera->frames.load()) + "\n";
                    body += "veg_events_total" + label +
                            std::to_string(camera->events.load()) + "\n";
                    body += "veg_reconnects_total" + label +
                            std::to_string(camera->reconnects.load()) + "\n";
                    body += "veg_processing_microseconds_total" + label +
                            std::to_string(camera->processing_us.load()) + "\n";
                }
                auto response = drogon::HttpResponse::newHttpResponse();
                response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                response->setBody(body);
                callback(response);
            }, {drogon::Get});

        for (const auto& camera : cameras) {
            camera->worker = std::thread(run_camera, camera, std::cref(config), max_frames);
        }
        std::cout << "Gateway API ready: http://0.0.0.0:" << config.http_port
                  << "/health" << std::endl;
        drogon::app().setLogLevel(trantor::Logger::kWarn)
                     .setThreadNum(2)
                     .addListener("0.0.0.0", config.http_port)
                     .run();
        for (const auto& camera : cameras) camera->stop.store(true);
        for (const auto& camera : cameras) camera->worker.join();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "veg_gateway: " << error.what() << '\n';
        return 1;
    }
}
