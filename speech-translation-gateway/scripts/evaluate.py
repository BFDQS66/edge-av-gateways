#!/usr/bin/env python3
"""Reproducible public-sample ASR and synthetic-signal TTS evaluation."""

import argparse
import array
import json
import math
import pathlib
import random
import re
import subprocess
import sys
import unicodedata
import wave


ROOT = pathlib.Path(__file__).resolve().parents[1]
ASR_DIR = ROOT / "models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17"
TTS_DIR = ROOT / "models/kokoro-int8-multi-lang-v1_1"
CLI = ROOT / "build/speech_cli"
OUT = ROOT / "output/evaluation"


def run(*args: str) -> dict:
    result = subprocess.run([str(CLI), *map(str, args)], cwd=ROOT,
                            text=True, capture_output=True, check=True, timeout=120)
    return json.loads(result.stdout.strip().splitlines()[-1])


def edit_distance(first: list[str], second: list[str]) -> int:
    row = list(range(len(second) + 1))
    for i, left in enumerate(first, 1):
        next_row = [i]
        for j, right in enumerate(second, 1):
            next_row.append(min(next_row[-1] + 1, row[j] + 1,
                                row[j - 1] + (left != right)))
        row = next_row
    return row[-1]


def zh_chars(text: str) -> list[str]:
    normalized = unicodedata.normalize("NFKC", text).lower()
    normalized = normalized.translate(str.maketrans({"九": "9", "五": "5"}))
    return [char for char in normalized if char.isalnum()]


def en_words(text: str) -> list[str]:
    return re.findall(r"[a-z0-9]+", unicodedata.normalize("NFKC", text).lower())


def read_pcm16(path: pathlib.Path) -> tuple[int, array.array]:
    with wave.open(str(path), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
            raise ValueError(f"expected mono 16-bit WAV: {path}")
        rate = wav.getframerate()
        samples = array.array("h", wav.readframes(wav.getnframes()))
    if sys.byteorder != "little":
        samples.byteswap()
    return rate, samples


def write_pcm16(path: pathlib.Path, rate: int, samples: array.array) -> None:
    data = array.array("h", samples)
    if sys.byteorder != "little":
        data.byteswap()
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(data.tobytes())


def make_noise_variant(source: pathlib.Path, destination: pathlib.Path,
                       snr_db: float) -> None:
    rate, samples = read_pcm16(source)
    rms = math.sqrt(sum(value * value for value in samples) / len(samples))
    sigma = rms / 10 ** (snr_db / 20)
    rng = random.Random(20261015)
    mixed = array.array("h", (max(-32768, min(32767, round(value + rng.gauss(0, sigma))))
                              for value in samples))
    write_pcm16(destination, rate, mixed)


def audio_stats(path: pathlib.Path) -> dict:
    rate, samples = read_pcm16(path)
    rms = math.sqrt(sum(value * value for value in samples) / len(samples)) / 32768
    clipped = sum(abs(value) >= 32767 for value in samples) / len(samples)
    return {"duration_sec": round(len(samples) / rate, 3), "sample_rate": rate,
            "rms_dbfs": round(20 * math.log10(max(rms, 1e-12)), 2),
            "clipped_fraction": round(clipped, 6)}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--vad", action="store_true", help="segment audio with Silero VAD")
    parser.add_argument("--stt-only", action="store_true", help="reuse existing TTS evaluation")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    vad_args = ["--vad-model", ROOT / "models/silero_vad.onnx"] if args.vad else []
    official = [
        ("zh", ASR_DIR / "test_wavs/zh.wav", "开放时间早上9点至下午5点", zh_chars),
        ("en", ASR_DIR / "test_wavs/en.wav",
         "The tribal chieftain called for the boy and presented him with 50 pieces of gold.",
         en_words),
    ]
    report = {"asr_model": ASR_DIR.name, "tts_model": TTS_DIR.name,
              "threads": args.threads, "vad_enabled": args.vad,
              "official_samples": [], "noise_samples": [],
              "tts_samples": [], "silence": {}}
    for language, wav, reference, tokenizer in official:
        result = run("stt", "--model-dir", ASR_DIR, "--wav", wav,
                     "--threads", args.threads, "--warmup", 1, "--repeat", 7,
                     *vad_args)
        errors = edit_distance(tokenizer(reference), tokenizer(result["text"]))
        result.update({"language": language, "reference": reference,
                       "error_count": errors, "reference_units": len(tokenizer(reference)),
                       "error_rate": round(errors / len(tokenizer(reference)), 4),
                       "error_metric": "CER" if language == "zh" else "WER"})
        report["official_samples"].append(result)
        for snr in (20, 10):
            noisy = OUT / f"{language}_{snr}db.wav"
            make_noise_variant(wav, noisy, snr)
            noisy_result = run("stt", "--model-dir", ASR_DIR, "--wav", noisy,
                               "--threads", args.threads, "--warmup", 0, "--repeat", 1,
                               *vad_args)
            errors = edit_distance(tokenizer(reference), tokenizer(noisy_result["text"]))
            noisy_result.update({"language": language, "snr_db": snr,
                                 "error_count": errors,
                                 "reference_units": len(tokenizer(reference)),
                                 "error_rate": round(errors / len(tokenizer(reference)), 4),
                                 "error_metric": "CER" if language == "zh" else "WER"})
            report["noise_samples"].append(noisy_result)

    for language, text, speaker, tokenizer in (() if args.stt_only else (
        ("zh", "今天是星期四。欢迎使用离线语音系统。", 3, zh_chars),
        ("en", "Welcome to the offline speech system.", 0, en_words),
    )):
        output = OUT / f"tts_{language}.wav"
        tts = run("tts", "--model-dir", TTS_DIR, "--text", text,
                  "--output", output, "--speaker", speaker,
                  "--threads", args.threads, "--warmup", 1, "--repeat", 5)
        asr = run("stt", "--model-dir", ASR_DIR, "--wav", output,
                  "--threads", args.threads, "--warmup", 0, "--repeat", 1,
                  *vad_args)
        tts.update({"language": language, "input_text": text,
                    "audio_signal": audio_stats(output), "round_trip_text": asr["text"],
                    "round_trip_error_rate": round(
                        edit_distance(tokenizer(text), tokenizer(asr["text"])) /
                        len(tokenizer(text)), 4)})
        report["tts_samples"].append(tts)

    silence = OUT / "silence_1s.wav"
    write_pcm16(silence, 16000, array.array("h", [0] * 16000))
    report["silence"] = run("stt", "--model-dir", ASR_DIR, "--wav", silence,
                            "--threads", args.threads, "--warmup", 0, "--repeat", 1,
                            *vad_args)
    report_name = "report_vad.json" if args.vad else "report.json"
    (OUT / report_name).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                     encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
