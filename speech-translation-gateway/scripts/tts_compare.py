#!/usr/bin/env python3
"""Compare Kokoro and Piper on identical short prompts using the C++ CLI."""

import json

from evaluate import ASR_DIR, OUT, ROOT, audio_stats, edit_distance, en_words, run, zh_chars


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    cases = (
        ("zh", "你好，欢迎使用语音服务。", zh_chars,
         (("kokoro", "kokoro-int8-multi-lang-v1_1", 3),
          ("piper-zh", "vits-piper-zh_CN-chaowen-medium", 0))),
        ("en", "Welcome to the offline speech system.", en_words,
         (("kokoro", "kokoro-int8-multi-lang-v1_1", 0),
          ("piper-en", "vits-piper-en_US-lessac-medium", 0))),
    )
    report = {"threads": 4, "warmup": 1, "repeat": 5, "cases": []}
    for language, prompt, tokenize, backends in cases:
        for backend, directory, speaker in backends:
            wav = OUT / f"compare_{backend}_{language}.wav"
            tts = run("tts", "--backend", backend, "--model-dir", ROOT / "models" / directory,
                      "--text", prompt, "--output", wav, "--speaker", speaker,
                      "--threads", 4, "--warmup", 1, "--repeat", 5)
            stt = run("stt", "--model-dir", ASR_DIR, "--wav", wav,
                      "--vad-model", ROOT / "models/silero_vad.onnx",
                      "--threads", 4, "--warmup", 1, "--repeat", 3)
            report["cases"].append({"language": language, "prompt": prompt,
                                    "backend": backend, "tts": tts,
                                    "audio_signal": audio_stats(wav),
                                    "round_trip_stt": stt["text"],
                                    "round_trip_error_rate": round(
                                        edit_distance(tokenize(prompt), tokenize(stt["text"])) /
                                        len(tokenize(prompt)), 4)})
    (OUT / "tts_compare.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
