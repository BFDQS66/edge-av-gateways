#include "httplib.h"
#include "sherpa-onnx/c-api/c-api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using WavePtr = std::unique_ptr<const SherpaOnnxWave, decltype(&SherpaOnnxFreeWave)>;
using SegmentPtr = std::unique_ptr<const SherpaOnnxSpeechSegment,
                                   decltype(&SherpaOnnxDestroySpeechSegment)>;
using AudioPtr = std::unique_ptr<const SherpaOnnxGeneratedAudio,
                                 decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio)>;
constexpr size_t kMaxWavBytes = 8 * 1024 * 1024;
constexpr double kMaxAudioSeconds = 30.0;
constexpr size_t kMaxTextBytes = 500;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string escape_json(const std::string& input) {
    std::string output;
    for (const unsigned char c : input) {
        switch (c) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                output += buffer;
            } else {
                output += static_cast<char>(c);
            }
        }
    }
    return output;
}

std::string number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

bool valid_utf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
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

void json_error(httplib::Response& response, int code, const std::string& message) {
    response.status = code;
    response.set_content("{\"error\":\"" + escape_json(message) + "\"}\n",
                         "application/json; charset=utf-8");
}

class ActiveRequest {
public:
    ActiveRequest(std::atomic<double>& started_ms, Clock::time_point epoch)
        : started_ms_(started_ms) { started_ms_.store(ms_since(epoch)); }
    ~ActiveRequest() { started_ms_.store(0); }
    ActiveRequest(const ActiveRequest&) = delete;
    ActiveRequest& operator=(const ActiveRequest&) = delete;
private:
    std::atomic<double>& started_ms_;
};

class SpeechEngine {
public:
    SpeechEngine(const std::filesystem::path& root, bool use_itn, std::string asr_backend)
        : started_(Clock::now()), use_itn_(use_itn), asr_backend_(std::move(asr_backend)) {
        const auto asr = root / (asr_backend_ == "firered" ?
            "models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25" :
            "models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17");
        const auto asr_model = (asr / "model.int8.onnx").string();
        const auto tokens = (asr / "tokens.txt").string();
        const auto vad_model = (root / "models/silero_vad.onnx").string();
        SherpaOnnxOfflineRecognizerConfig config{};
        config.feat_config.sample_rate = 16000;
        config.feat_config.feature_dim = 80;
        config.model_config.num_threads = 2;
        config.model_config.provider = "cpu";
        config.model_config.tokens = tokens.c_str();
        if (asr_backend_ == "firered") {
            config.model_config.fire_red_asr_ctc.model = asr_model.c_str();
        } else {
            config.model_config.sense_voice.model = asr_model.c_str();
            config.model_config.sense_voice.language = "auto";
            config.model_config.sense_voice.use_itn = use_itn ? 1 : 0;
        }
        config.decoding_method = "greedy_search";
        recognizer_ = SherpaOnnxCreateOfflineRecognizer(&config);
        if (!recognizer_) throw std::runtime_error("cannot load ASR backend " + asr_backend_);

        SherpaOnnxVadModelConfig vad_config{};
        vad_config.silero_vad.model = vad_model.c_str();
        vad_config.silero_vad.threshold = 0.25F;
        vad_config.silero_vad.min_silence_duration = 0.5F;
        vad_config.silero_vad.min_speech_duration = 0.3F;
        vad_config.silero_vad.max_speech_duration = 20.0F;
        vad_config.silero_vad.window_size = 512;
        vad_config.sample_rate = 16000;
        vad_config.num_threads = 1;
        vad_ = SherpaOnnxCreateVoiceActivityDetector(&vad_config, 32.0F);
        if (!vad_) throw std::runtime_error("cannot load Silero VAD");

        zh_tts_ = create_piper(root / "models/vits-piper-zh_CN-chaowen-medium", true);
        if (!zh_tts_) throw std::runtime_error("cannot load Chinese Piper");
        en_tts_ = create_piper(root / "models/vits-piper-en_US-lessac-medium", false);
        if (!en_tts_) throw std::runtime_error("cannot load English Piper");
    }

    SpeechEngine(const SpeechEngine&) = delete;
    SpeechEngine& operator=(const SpeechEngine&) = delete;

    ~SpeechEngine() {
        if (en_tts_) SherpaOnnxDestroyOfflineTts(en_tts_);
        if (zh_tts_) SherpaOnnxDestroyOfflineTts(zh_tts_);
        if (vad_) SherpaOnnxDestroyVoiceActivityDetector(vad_);
        if (recognizer_) SherpaOnnxDestroyOfflineRecognizer(recognizer_);
    }

    void stt(const httplib::Request& request, httplib::Response& response) {
        requests_++;
        const std::string strategy = request.has_param("strategy")
                                         ? request.get_param_value("strategy") : "gate";
        if (strategy != "segments" && strategy != "gate" && strategy != "off") {
            errors_++;
            json_error(response, 400, "strategy must be segments, gate or off");
            return;
        }
        if (request.body.empty() || request.body.size() > kMaxWavBytes) {
            errors_++;
            json_error(response, 413, "WAV must be between 1 byte and 8 MiB");
            return;
        }
        std::unique_lock<std::mutex> lock(inference_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            busy_++;
            json_error(response, 503, "inference busy; retry later");
            return;
        }
        ActiveRequest active(active_since_ms_, started_);
        try {
            const auto start = Clock::now();
            WavePtr wave(SherpaOnnxReadWaveFromBinaryData(
                             request.body.data(), static_cast<int32_t>(request.body.size())),
                         SherpaOnnxFreeWave);
            if (!wave || wave->sample_rate < 8000 || wave->sample_rate > 48000 ||
                wave->num_samples <= 0) {
                throw std::invalid_argument("expected mono PCM16 WAV, 8-48 kHz");
            }
            const double duration = static_cast<double>(wave->num_samples) / wave->sample_rate;
            if (duration > kMaxAudioSeconds) {
                json_error(response, 413, "audio duration exceeds 30 seconds");
                errors_++;
                return;
            }
            std::vector<float> samples;
            if (wave->sample_rate == 16000) {
                samples.assign(wave->samples, wave->samples + wave->num_samples);
            } else {
                const auto* resampler = SherpaOnnxCreateLinearResampler(
                    wave->sample_rate, 16000, 0, 0);
                if (!resampler) throw std::runtime_error("resampler creation failed");
                const auto* output = SherpaOnnxLinearResamplerResample(
                    resampler, wave->samples, wave->num_samples, 1);
                if (!output) {
                    SherpaOnnxDestroyLinearResampler(resampler);
                    throw std::runtime_error("resampling failed");
                }
                samples.assign(output->samples, output->samples + output->n);
                SherpaOnnxLinearResamplerResampleFree(output);
                SherpaOnnxDestroyLinearResampler(resampler);
            }

            auto decode = [this](const float* data, int32_t count) {
                const auto* stream = SherpaOnnxCreateOfflineStream(recognizer_);
                if (!stream) throw std::runtime_error("cannot create ASR stream");
                SherpaOnnxAcceptWaveformOffline(stream, 16000, data, count);
                SherpaOnnxDecodeOfflineStream(recognizer_, stream);
                const auto* result = SherpaOnnxGetOfflineStreamResult(stream);
                if (!result) {
                    SherpaOnnxDestroyOfflineStream(stream);
                    throw std::runtime_error("ASR decode failed");
                }
                const std::string text = result->text ? result->text : "";
                SherpaOnnxDestroyOfflineRecognizerResult(result);
                SherpaOnnxDestroyOfflineStream(stream);
                return text;
            };
            std::string transcript;
            int segments = 0;
            double speech_seconds = 0;
            if (strategy == "off") {
                transcript = decode(samples.data(), static_cast<int32_t>(samples.size()));
                segments = 1;
                speech_seconds = duration;
            } else {
                SherpaOnnxVoiceActivityDetectorClear(vad_);
                SherpaOnnxVoiceActivityDetectorReset(vad_);
                for (size_t i = 0; i < samples.size(); i += 512) {
                    float window[512]{};
                    std::copy_n(samples.data() + i, std::min<size_t>(512, samples.size() - i),
                                window);
                    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, window, 512);
                }
                SherpaOnnxVoiceActivityDetectorFlush(vad_);
                while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
                    SegmentPtr segment(SherpaOnnxVoiceActivityDetectorFront(vad_),
                                       SherpaOnnxDestroySpeechSegment);
                    if (segment && segment->n > 0) {
                        if (strategy == "segments") {
                            const std::string part = decode(segment->samples, segment->n);
                            if (!part.empty()) {
                                if (!transcript.empty()) transcript += ' ';
                                transcript += part;
                            }
                        }
                        ++segments;
                        speech_seconds += static_cast<double>(segment->n) / 16000;
                    }
                    segment.reset();
                    SherpaOnnxVoiceActivityDetectorPop(vad_);
                }
                if (strategy == "gate" && segments > 0) {
                    transcript = decode(samples.data(), static_cast<int32_t>(samples.size()));
                }
            }
            const double latency = ms_since(start);
            successes_++;
            last_latency_ms_.store(latency);
            response.set_content(
                "{\"text\":\"" + escape_json(transcript) + "\",\"strategy\":\"" +
                    strategy + "\",\"audio_sec\":" +
                    number(duration) + ",\"speech_sec\":" + number(speech_seconds) +
                    ",\"segments\":" + std::to_string(segments) +
                    ",\"latency_ms\":" + number(latency) + "}\n",
                "application/json; charset=utf-8");
        } catch (const std::invalid_argument& error) {
            errors_++;
            json_error(response, 400, error.what());
        } catch (const std::exception& error) {
            errors_++;
            json_error(response, 500, error.what());
        }
    }

    void tts(const httplib::Request& request, httplib::Response& response, bool chinese) {
        requests_++;
        if (request.body.empty() || request.body.size() > kMaxTextBytes) {
            errors_++;
            json_error(response, 413, "text must be between 1 and 500 UTF-8 bytes");
            return;
        }
        if (!valid_utf8(request.body)) {
            errors_++;
            json_error(response, 400, "text must be valid UTF-8 without NUL bytes");
            return;
        }
        std::unique_lock<std::mutex> lock(inference_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            busy_++;
            json_error(response, 503, "inference busy; retry later");
            return;
        }
        ActiveRequest active(active_since_ms_, started_);
        try {
            const auto start = Clock::now();
            SherpaOnnxGenerationConfig config{};
            config.sid = 0;
            config.speed = 1.0F;
            config.silence_scale = 0.2F;
            AudioPtr audio(SherpaOnnxOfflineTtsGenerateWithConfig(
                               chinese ? zh_tts_ : en_tts_, request.body.c_str(),
                               &config, nullptr, nullptr),
                           SherpaOnnxDestroyOfflineTtsGeneratedAudio);
            if (!audio || audio->n <= 0 || audio->sample_rate <= 0) {
                throw std::invalid_argument("TTS could not synthesize this text");
            }
            const int64_t bytes = SherpaOnnxWaveFileSize(audio->n);
            if (bytes <= 0 || bytes > static_cast<int64_t>(kMaxWavBytes)) {
                throw std::runtime_error("generated WAV exceeds response size limit");
            }
            std::string wav(static_cast<size_t>(bytes), '\0');
            SherpaOnnxWriteWaveToBuffer(audio->samples, audio->n, audio->sample_rate,
                                        wav.data());
            const double latency = ms_since(start);
            successes_++;
            last_latency_ms_.store(latency);
            response.set_header("X-Inference-Ms", number(latency));
            response.set_header("X-Audio-Seconds",
                                number(static_cast<double>(audio->n) / audio->sample_rate));
            response.set_content(std::move(wav), "audio/wav");
        } catch (const std::invalid_argument& error) {
            errors_++;
            json_error(response, 400, error.what());
        } catch (const std::exception& error) {
            errors_++;
            json_error(response, 500, error.what());
        }
    }

    std::string metrics() const {
        struct rusage usage {};
        const double rss = getrusage(RUSAGE_SELF, &usage) == 0
                               ? static_cast<double>(usage.ru_maxrss) / 1024 : 0.0;
        double current_rss = 0.0;
        std::ifstream statm("/proc/self/statm");
        uint64_t total_pages = 0, resident_pages = 0;
        if (statm >> total_pages >> resident_pages) {
            current_rss = static_cast<double>(resident_pages) * sysconf(_SC_PAGESIZE) /
                          (1024.0 * 1024.0);
        }
        const double active_since = active_since_ms_.load();
        const double inflight_sec = active_since > 0
                                        ? std::max(0.0, (ms_since(started_) - active_since) / 1000)
                                        : 0.0;
        return "{\"uptime_sec\":" + number(ms_since(started_) / 1000) +
               ",\"use_itn\":" + (use_itn_ ? "true" : "false") +
               ",\"asr_backend\":\"" + asr_backend_ + "\"" +
               ",\"requests\":" + std::to_string(requests_.load()) +
               ",\"successes\":" + std::to_string(successes_.load()) +
               ",\"errors\":" + std::to_string(errors_.load()) +
               ",\"busy\":" + std::to_string(busy_.load()) +
               ",\"last_latency_ms\":" + number(last_latency_ms_.load()) +
               ",\"inflight_sec\":" + number(inflight_sec) +
               ",\"current_rss_mib\":" + number(current_rss) +
               ",\"peak_rss_mib\":" + number(rss) + "}\n";
    }

private:
    static const SherpaOnnxOfflineTts* create_piper(const std::filesystem::path& dir,
                                                    bool chinese) {
        const auto model = (dir / (chinese ? "zh_CN-chaowen-medium.onnx" :
                                          "en_US-lessac-medium.onnx")).string();
        const auto tokens = (dir / "tokens.txt").string();
        const auto lexicon = (dir / "lexicon.txt").string();
        const auto data_dir = (dir / "espeak-ng-data").string();
        const auto rule_fsts = (dir / "phone.fst").string() + "," +
                               (dir / "date.fst").string() + "," +
                               (dir / "number.fst").string();
        SherpaOnnxOfflineTtsConfig config{};
        config.model.vits.model = model.c_str();
        config.model.vits.tokens = tokens.c_str();
        config.model.vits.noise_scale = 0.667F;
        config.model.vits.noise_scale_w = 0.8F;
        config.model.vits.length_scale = 1.0F;
        if (chinese) {
            config.model.vits.lexicon = lexicon.c_str();
            config.rule_fsts = rule_fsts.c_str();
        } else {
            config.model.vits.data_dir = data_dir.c_str();
        }
        config.model.num_threads = 2;
        return SherpaOnnxCreateOfflineTts(&config);
    }

    const SherpaOnnxOfflineRecognizer* recognizer_ = nullptr;
    const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
    const SherpaOnnxOfflineTts* zh_tts_ = nullptr;
    const SherpaOnnxOfflineTts* en_tts_ = nullptr;
    std::mutex inference_mutex_;
    Clock::time_point started_;
    bool use_itn_;
    std::string asr_backend_;
    std::atomic<uint64_t> requests_{0}, successes_{0}, errors_{0}, busy_{0};
    std::atomic<double> last_latency_ms_{0};
    std::atomic<double> active_since_ms_{0};
};

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 4 && argc != 5)
            throw std::invalid_argument(
                "usage: speech_server PROJECT_ROOT [PORT USE_ITN_0_OR_1 [sensevoice|firered]]");
        const int port = argc >= 4 ? std::stoi(argv[2]) : 8765;
        if (port < 1024 || port > 65535 || (argc >= 4 &&
            std::string(argv[3]) != "0" && std::string(argv[3]) != "1")) {
            throw std::invalid_argument("invalid port or USE_ITN value");
        }
        const bool use_itn = argc >= 4 ? std::string(argv[3]) == "1" : true;
        const std::string asr_backend = argc == 5 ? argv[4] : "sensevoice";
        if (asr_backend != "sensevoice" && asr_backend != "firered") {
            throw std::invalid_argument("ASR backend must be sensevoice or firered");
        }
        SpeechEngine engine(std::filesystem::canonical(argv[1]), use_itn, asr_backend);
        httplib::Server server;
        server.set_payload_max_length(kMaxWavBytes);
        server.set_read_timeout(10, 0);
        server.set_write_timeout(30, 0);
        server.new_task_queue = [] { return new httplib::ThreadPool(2, 2, 8); };
        server.Get("/healthz", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("ok\n", "text/plain; charset=utf-8");
        });
        server.Get("/readyz", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("ready\n", "text/plain; charset=utf-8");
        });
        server.Get("/metrics", [&engine](const httplib::Request&, httplib::Response& response) {
            response.set_content(engine.metrics(), "application/json; charset=utf-8");
        });
        server.Post("/v1/stt", [&engine](const httplib::Request& request,
                                         httplib::Response& response) {
            engine.stt(request, response);
        });
        server.Post("/v1/tts/zh", [&engine](const httplib::Request& request,
                                            httplib::Response& response) {
            engine.tts(request, response, true);
        });
        server.Post("/v1/tts/en", [&engine](const httplib::Request& request,
                                            httplib::Response& response) {
            engine.tts(request, response, false);
        });
        std::cerr << "speech_server ready on 127.0.0.1:" << port << '\n';
        if (!server.listen("127.0.0.1", port)) throw std::runtime_error("listen failed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "speech_server: " << error.what() << '\n';
        return 1;
    }
}
