#include "httplib.h"
#include "sherpa-onnx/c-api/c-api.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
using Audio = std::unique_ptr<const SherpaOnnxGeneratedAudio,
                              decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio)>;
constexpr size_t kMaxTextBytes = 500;
constexpr size_t kMaxWaveBytes = 8 * 1024 * 1024;

void error(httplib::Response& response, int status, const char* message) {
    response.status = status;
    response.set_content(std::string("{\"error\":\"") + message + "\"}\n",
                         "application/json; charset=utf-8");
}

bool valid_utf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first == 0) return false;
        if (first < 0x80) { ++i; continue; }
        const size_t width = first >= 0xC2 && first <= 0xDF ? 2 :
                             first >= 0xE0 && first <= 0xEF ? 3 :
                             first >= 0xF0 && first <= 0xF4 ? 4 : 0;
        if (!width || i + width > text.size()) return false;
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if ((second & 0xC0) != 0x80 ||
            (first == 0xE0 && second < 0xA0) ||
            (first == 0xED && second >= 0xA0) ||
            (first == 0xF0 && second < 0x90) ||
            (first == 0xF4 && second >= 0x90)) return false;
        for (size_t j = 2; j < width; ++j) {
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) return false;
        }
        i += width;
    }
    return true;
}

class Engine {
public:
    explicit Engine(const std::filesystem::path& root) : started_(Clock::now()) {
        const auto dir = root / "models/kokoro-int8-multi-lang-v1_1";
        const auto model = (dir / "model.int8.onnx").string();
        const auto voices = (dir / "voices.bin").string();
        const auto tokens = (dir / "tokens.txt").string();
        const auto data_dir = (dir / "espeak-ng-data").string();
        const auto dict_dir = (dir / "dict").string();
        const auto lexicon = (dir / "lexicon-us-en.txt").string() + "," +
                             (dir / "lexicon-zh.txt").string();
        SherpaOnnxOfflineTtsConfig config{};
        config.model.kokoro.model = model.c_str();
        config.model.kokoro.voices = voices.c_str();
        config.model.kokoro.tokens = tokens.c_str();
        config.model.kokoro.data_dir = data_dir.c_str();
        config.model.kokoro.dict_dir = dict_dir.c_str();
        config.model.kokoro.lexicon = lexicon.c_str();
        config.model.num_threads = 4;
        tts_ = SherpaOnnxCreateOfflineTts(&config);
        if (!tts_) throw std::runtime_error("cannot load Kokoro TTS model");
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine() { SherpaOnnxDestroyOfflineTts(tts_); }

    void synthesize(const httplib::Request& request, httplib::Response& response,
                    bool chinese) {
        if (request.body.empty() || request.body.size() > kMaxTextBytes ||
            !valid_utf8(request.body)) {
            errors_++;
            error(response, 400, "expected 1-500 bytes of valid UTF-8 text");
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            busy_++;
            error(response, 503, "TTS is busy; retry later");
            return;
        }
        requests_++;
        const auto start = Clock::now();
        struct ActiveGuard {
            std::atomic<double>& value;
            ~ActiveGuard() { value.store(0); }
        } active{active_since_ms_};
        active_since_ms_.store(std::chrono::duration<double, std::milli>(
            start - started_).count());
        SherpaOnnxGenerationConfig config{};
        config.sid = chinese ? 3 : 0;
        config.speed = 1.0F;
        config.silence_scale = 0.2F;
        Audio audio(SherpaOnnxOfflineTtsGenerateWithConfig(
                        tts_, request.body.c_str(), &config, nullptr, nullptr),
                    SherpaOnnxDestroyOfflineTtsGeneratedAudio);
        if (!audio || audio->n <= 0 || audio->sample_rate <= 0) {
            errors_++;
            error(response, 422, "TTS returned no audio");
            return;
        }
        const int64_t size = SherpaOnnxWaveFileSize(audio->n);
        if (size <= 0 || size > static_cast<int64_t>(kMaxWaveBytes)) {
            errors_++;
            error(response, 413, "generated audio is too large");
            return;
        }
        std::string wav(static_cast<size_t>(size), '\0');
        SherpaOnnxWriteWaveToBuffer(audio->samples, audio->n, audio->sample_rate,
                                    wav.data());
        successes_++;
        const double elapsed = std::chrono::duration<double, std::milli>(
            Clock::now() - start).count();
        response.set_header("X-Inference-Ms", std::to_string(elapsed));
        response.set_header("X-Audio-Seconds",
                            std::to_string(static_cast<double>(audio->n) / audio->sample_rate));
        response.set_content(std::move(wav), "audio/wav");
    }

    std::string metrics() const {
        const auto uptime = std::chrono::duration<double>(Clock::now() - started_).count();
        const double active_since = active_since_ms_.load();
        const double inflight = active_since > 0 ?
            uptime - active_since / 1000.0 : 0.0;
        return "{\"backend\":\"kokoro\",\"uptime_sec\":" +
               std::to_string(uptime) + ",\"requests\":" +
               std::to_string(requests_.load()) + ",\"successes\":" +
               std::to_string(successes_.load()) + ",\"errors\":" +
               std::to_string(errors_.load()) + ",\"inflight_sec\":" +
               std::to_string(inflight) + ",\"busy\":" +
               std::to_string(busy_.load()) + "}\n";
    }

private:
    const SherpaOnnxOfflineTts* tts_ = nullptr;
    std::mutex mutex_;
    Clock::time_point started_;
    std::atomic<uint64_t> requests_{0}, successes_{0}, errors_{0}, busy_{0};
    std::atomic<double> active_since_ms_{0};
};

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("usage: quality_tts_server PROJECT_ROOT");
        Engine engine(std::filesystem::canonical(argv[1]));
        httplib::Server server;
        server.set_payload_max_length(1024);
        server.set_read_timeout(10);
        server.set_write_timeout(90);
        server.new_task_queue = [] { return new httplib::ThreadPool(2, 2, 8); };
        server.Get("/healthz", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("ok\n", "text/plain; charset=utf-8");
        });
        server.Get("/metrics", [&engine](const httplib::Request&, httplib::Response& response) {
            response.set_content(engine.metrics(), "application/json; charset=utf-8");
        });
        server.Post("/v1/tts/zh", [&engine](const httplib::Request& request,
                                            httplib::Response& response) {
            engine.synthesize(request, response, true);
        });
        server.Post("/v1/tts/en", [&engine](const httplib::Request& request,
                                            httplib::Response& response) {
            engine.synthesize(request, response, false);
        });
        if (!server.listen("127.0.0.1", 8768)) throw std::runtime_error("listen failed");
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "quality_tts_server: %s\n", exception.what());
        return 1;
    }
}
