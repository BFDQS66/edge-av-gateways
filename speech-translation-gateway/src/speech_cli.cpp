#include "sherpa-onnx/c-api/c-api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sys/resource.h>

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string escape_json(const std::string& value) {
    std::string output;
    for (const unsigned char c : value) {
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

double percentile(std::vector<double> samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    const size_t index = static_cast<size_t>(std::ceil(fraction * samples.size())) - 1;
    return samples.at(index);
}

void print_usage() {
    std::cerr << "Usage:\n"
              << "  speech_cli stt --model-dir DIR --wav FILE [--vad-model FILE]"
                 " [--language auto|zh|en]"
                 " [--threads N] [--warmup N] [--repeat N]\n"
              << "  speech_cli tts --backend kokoro|piper-zh|piper-en --model-dir DIR"
                 " --text TEXT --output FILE [--speaker N]"
                 " [--threads N] [--warmup N] [--repeat N]\n";
}

std::map<std::string, std::string> parse_options(int argc, char** argv) {
    std::map<std::string, std::string> options;
    for (int i = 2; i < argc; i += 2) {
        const std::string name = argv[i];
        if (name.rfind("--", 0) != 0 || i + 1 >= argc || !options.emplace(name, argv[i + 1]).second) {
            throw std::runtime_error("invalid or duplicate option: " + name);
        }
    }
    return options;
}

std::string required(const std::map<std::string, std::string>& options,
                     const std::string& key) {
    const auto it = options.find(key);
    if (it == options.end() || it->second.empty()) throw std::runtime_error("missing " + key);
    return it->second;
}

int integer_option(const std::map<std::string, std::string>& options,
                   const std::string& key, int fallback, int minimum, int maximum) {
    const auto it = options.find(key);
    if (it == options.end()) return fallback;
    size_t parsed = 0;
    const int value = std::stoi(it->second, &parsed);
    if (parsed != it->second.size() || value < minimum || value > maximum) {
        throw std::runtime_error("invalid " + key);
    }
    return value;
}

struct Stats {
    double load_ms = 0;
    std::vector<double> inference_ms;
    double audio_sec = 0;
    double peak_rss_mib = 0;
};

struct PreparedAudio {
    std::vector<std::vector<float>> segments;
    int sample_rate = 16000;
    double vad_load_ms = 0;
    double vad_ms = 0;
    double speech_sec = 0;
};

PreparedAudio prepare_audio(const SherpaOnnxWave* wave, const std::string& vad_model) {
    PreparedAudio prepared;
    if (vad_model.empty()) {
        prepared.sample_rate = wave->sample_rate;
        prepared.segments.emplace_back(wave->samples, wave->samples + wave->num_samples);
        prepared.speech_sec = static_cast<double>(wave->num_samples) / wave->sample_rate;
        return prepared;
    }
    if (!std::filesystem::is_regular_file(vad_model)) {
        throw std::runtime_error("VAD model is missing: " + vad_model);
    }
    std::vector<float> samples;
    if (wave->sample_rate == 16000) {
        samples.assign(wave->samples, wave->samples + wave->num_samples);
    } else {
        const auto* resampler = SherpaOnnxCreateLinearResampler(wave->sample_rate, 16000, 0, 0);
        if (!resampler) throw std::runtime_error("cannot create audio resampler");
        const auto* output = SherpaOnnxLinearResamplerResample(
            resampler, wave->samples, wave->num_samples, 1);
        if (!output) throw std::runtime_error("audio resampling failed");
        samples.assign(output->samples, output->samples + output->n);
        SherpaOnnxLinearResamplerResampleFree(output);
        SherpaOnnxDestroyLinearResampler(resampler);
    }
    SherpaOnnxVadModelConfig config{};
    config.silero_vad.model = vad_model.c_str();
    config.silero_vad.threshold = 0.25F;
    config.silero_vad.min_silence_duration = 0.5F;
    config.silero_vad.min_speech_duration = 0.3F;
    config.silero_vad.max_speech_duration = 20.0F;
    config.silero_vad.window_size = 512;
    config.sample_rate = 16000;
    config.num_threads = 1;
    const auto load_start = Clock::now();
    const auto* vad = SherpaOnnxCreateVoiceActivityDetector(
        &config, std::max(30.0F, static_cast<float>(samples.size()) / 16000.0F + 1.0F));
    prepared.vad_load_ms = elapsed_ms(load_start);
    if (!vad) throw std::runtime_error("cannot create Silero VAD");
    const auto start = Clock::now();
    for (size_t i = 0; i < samples.size(); i += 512) {
        const size_t count = std::min<size_t>(512, samples.size() - i);
        if (count == 512) {
            SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, samples.data() + i, 512);
        } else {
            float tail[512]{};
            std::copy_n(samples.data() + i, count, tail);
            SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, tail, 512);
        }
    }
    SherpaOnnxVoiceActivityDetectorFlush(vad);
    while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
        const auto* segment = SherpaOnnxVoiceActivityDetectorFront(vad);
        if (segment && segment->n > 0) {
            prepared.segments.emplace_back(segment->samples, segment->samples + segment->n);
            prepared.speech_sec += static_cast<double>(segment->n) / 16000;
        }
        SherpaOnnxDestroySpeechSegment(segment);
        SherpaOnnxVoiceActivityDetectorPop(vad);
    }
    prepared.vad_ms = elapsed_ms(start);
    SherpaOnnxDestroyVoiceActivityDetector(vad);
    return prepared;
}

void print_stats(const std::string& task, const Stats& stats, const std::string& extra) {
    const double mean = std::accumulate(stats.inference_ms.begin(), stats.inference_ms.end(), 0.0) /
                        stats.inference_ms.size();
    std::cout << std::fixed << std::setprecision(3)
              << "{\"task\":\"" << task << "\",\"load_ms\":" << stats.load_ms
              << ",\"repeat\":" << stats.inference_ms.size()
              << ",\"audio_sec\":" << stats.audio_sec
              << ",\"inference_mean_ms\":" << mean
              << ",\"inference_p50_ms\":" << percentile(stats.inference_ms, 0.50)
              << ",\"inference_p95_ms\":" << percentile(stats.inference_ms, 0.95)
              << ",\"rtf\":" << (mean / 1000.0 / stats.audio_sec)
              << ",\"peak_rss_mib\":" << stats.peak_rss_mib
              << extra << "}\n";
}

Stats finalize_stats(Stats stats) {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        stats.peak_rss_mib = static_cast<double>(usage.ru_maxrss) / 1024.0;
    }
    return stats;
}

void run_stt(const std::map<std::string, std::string>& options) {
    const auto model_dir = std::filesystem::path(required(options, "--model-dir"));
    const auto wav_path = required(options, "--wav");
    const auto vad_it = options.find("--vad-model");
    const std::string vad_model = vad_it == options.end() ? "" : vad_it->second;
    const auto language_it = options.find("--language");
    const std::string language = language_it == options.end() ? "auto" : language_it->second;
    if (language != "auto" && language != "zh" && language != "en") {
        throw std::runtime_error("language must be auto, zh or en");
    }
    const int threads = integer_option(options, "--threads", 2, 1, 16);
    const int warmup = integer_option(options, "--warmup", 1, 0, 20);
    const int repeat = integer_option(options, "--repeat", 5, 1, 100);
    const auto model = (model_dir / "model.int8.onnx").string();
    const auto tokens = (model_dir / "tokens.txt").string();
    if (!std::filesystem::is_regular_file(model) || !std::filesystem::is_regular_file(tokens)) {
        throw std::runtime_error("SenseVoice model.int8.onnx or tokens.txt is missing");
    }
    const SherpaOnnxWave* wave = SherpaOnnxReadWave(wav_path.c_str());
    if (!wave) throw std::runtime_error("cannot read WAV: " + wav_path);

    SherpaOnnxOfflineRecognizerConfig config{};
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.model_config.num_threads = threads;
    config.model_config.provider = "cpu";
    config.model_config.tokens = tokens.c_str();
    config.model_config.sense_voice.model = model.c_str();
    config.model_config.sense_voice.language = language.c_str();
    config.model_config.sense_voice.use_itn = 1;
    config.decoding_method = "greedy_search";
    const auto load_start = Clock::now();
    const SherpaOnnxOfflineRecognizer* recognizer = SherpaOnnxCreateOfflineRecognizer(&config);
    const double load_ms = elapsed_ms(load_start);
    if (!recognizer) {
        SherpaOnnxFreeWave(wave);
        throw std::runtime_error("cannot create SenseVoice recognizer");
    }

    Stats stats;
    stats.load_ms = load_ms;
    stats.audio_sec = static_cast<double>(wave->num_samples) / wave->sample_rate;
    const PreparedAudio prepared = prepare_audio(wave, vad_model);
    std::string text;
    for (int i = 0; i < warmup + repeat; ++i) {
        const auto start = Clock::now();
        text.clear();
        for (const auto& segment : prepared.segments) {
            const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(recognizer);
            if (!stream) throw std::runtime_error("cannot create ASR stream");
            SherpaOnnxAcceptWaveformOffline(stream, prepared.sample_rate, segment.data(),
                                           static_cast<int32_t>(segment.size()));
            SherpaOnnxDecodeOfflineStream(recognizer, stream);
            const SherpaOnnxOfflineRecognizerResult* result = SherpaOnnxGetOfflineStreamResult(stream);
            if (!result) throw std::runtime_error("ASR returned no result");
            if (result->text && *result->text) {
                if (!text.empty()) text += ' ';
                text += result->text;
            }
            SherpaOnnxDestroyOfflineRecognizerResult(result);
            SherpaOnnxDestroyOfflineStream(stream);
        }
        const double duration = elapsed_ms(start);
        if (i >= warmup) stats.inference_ms.push_back(duration);
    }
    stats = finalize_stats(std::move(stats));
    SherpaOnnxDestroyOfflineRecognizer(recognizer);
    SherpaOnnxFreeWave(wave);
    print_stats("stt", stats,
                ",\"text\":\"" + escape_json(text) + "\"" +
                ",\"speech_segments\":" + std::to_string(prepared.segments.size()) +
                ",\"speech_sec\":" + std::to_string(prepared.speech_sec) +
                ",\"vad_load_ms\":" + std::to_string(prepared.vad_load_ms) +
                ",\"vad_ms\":" + std::to_string(prepared.vad_ms));
}

void run_tts(const std::map<std::string, std::string>& options) {
    const auto model_dir = std::filesystem::path(required(options, "--model-dir"));
    const auto backend_it = options.find("--backend");
    const std::string backend = backend_it == options.end() ? "kokoro" : backend_it->second;
    if (backend != "kokoro" && backend != "piper-zh" && backend != "piper-en") {
        throw std::runtime_error("backend must be kokoro, piper-zh or piper-en");
    }
    const auto text = required(options, "--text");
    const auto output = required(options, "--output");
    const int threads = integer_option(options, "--threads", 2, 1, 16);
    const int warmup = integer_option(options, "--warmup", 1, 0, 20);
    const int repeat = integer_option(options, "--repeat", 5, 1, 100);
    const int speaker = integer_option(options, "--speaker", backend == "kokoro" ? 3 : 0,
                                       0, backend == "kokoro" ? 102 : 0);

    const auto model = (model_dir / (backend == "kokoro" ? "model.int8.onnx" :
                                    backend == "piper-zh" ? "zh_CN-chaowen-medium.onnx" :
                                                             "en_US-lessac-medium.onnx")).string();
    const auto voices = (model_dir / "voices.bin").string();
    const auto tokens = (model_dir / "tokens.txt").string();
    const auto data_dir = (model_dir / "espeak-ng-data").string();
    const auto dict_dir = (model_dir / "dict").string();
    const auto lexicon = (model_dir / "lexicon-us-en.txt").string() + "," +
                         (model_dir / "lexicon-zh.txt").string();
    if (!std::filesystem::is_regular_file(model) || !std::filesystem::is_regular_file(tokens) ||
        (backend == "kokoro" && !std::filesystem::is_regular_file(voices))) {
        throw std::runtime_error("TTS model, voices or tokens are missing");
    }
    SherpaOnnxOfflineTtsConfig config{};
    const auto piper_zh_lexicon = (model_dir / "lexicon.txt").string();
    const auto rule_fsts = (model_dir / "phone.fst").string() + "," +
                           (model_dir / "date.fst").string() + "," +
                           (model_dir / "number.fst").string();
    if (backend == "kokoro") {
        config.model.kokoro.model = model.c_str();
        config.model.kokoro.voices = voices.c_str();
        config.model.kokoro.tokens = tokens.c_str();
        config.model.kokoro.data_dir = data_dir.c_str();
        config.model.kokoro.dict_dir = dict_dir.c_str();
        config.model.kokoro.lexicon = lexicon.c_str();
    } else {
        config.model.vits.model = model.c_str();
        config.model.vits.tokens = tokens.c_str();
        config.model.vits.noise_scale = 0.667F;
        config.model.vits.noise_scale_w = 0.8F;
        config.model.vits.length_scale = 1.0F;
        if (backend == "piper-zh") {
            config.model.vits.lexicon = piper_zh_lexicon.c_str();
            config.rule_fsts = rule_fsts.c_str();
        } else {
            config.model.vits.data_dir = data_dir.c_str();
        }
    }
    config.model.num_threads = threads;
    const auto load_start = Clock::now();
    const SherpaOnnxOfflineTts* tts = SherpaOnnxCreateOfflineTts(&config);
    const double load_ms = elapsed_ms(load_start);
    if (!tts) throw std::runtime_error("cannot create TTS engine for backend " + backend);

    SherpaOnnxGenerationConfig generation{};
    generation.sid = speaker;
    generation.speed = 1.0F;
    generation.silence_scale = 0.2F;
    Stats stats;
    stats.load_ms = load_ms;
    int sample_rate = 0;
    for (int i = 0; i < warmup + repeat; ++i) {
        const auto start = Clock::now();
        const SherpaOnnxGeneratedAudio* audio = SherpaOnnxOfflineTtsGenerateWithConfig(
            tts, text.c_str(), &generation, nullptr, nullptr);
        if (!audio || audio->n <= 0 || audio->sample_rate <= 0) {
            throw std::runtime_error("TTS returned no audio");
        }
        const double duration = elapsed_ms(start);
        sample_rate = audio->sample_rate;
        stats.audio_sec = static_cast<double>(audio->n) / audio->sample_rate;
        if (i == warmup + repeat - 1 &&
            !SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate, output.c_str())) {
            throw std::runtime_error("cannot write generated WAV");
        }
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        if (i >= warmup) stats.inference_ms.push_back(duration);
    }
    stats = finalize_stats(std::move(stats));
    SherpaOnnxDestroyOfflineTts(tts);
    print_stats("tts", stats, ",\"backend\":\"" + backend + "\"" +
                ",\"sample_rate\":" + std::to_string(sample_rate) +
                ",\"speaker\":" + std::to_string(speaker) +
                ",\"output\":\"" + escape_json(output) + "\"");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) { print_usage(); return 1; }
        const std::string task = argv[1];
        const auto options = parse_options(argc, argv);
        if (task == "stt") run_stt(options);
        else if (task == "tts") run_tts(options);
        else { print_usage(); return 1; }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "speech_cli: " << error.what() << '\n';
        return 1;
    }
}
