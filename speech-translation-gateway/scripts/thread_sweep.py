#!/usr/bin/env python3
"""Compare CPU thread counts with the same C++ inference executable."""

import json
import pathlib
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
CLI = ROOT / "build/speech_cli"
ASR = ROOT / "models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17"
TTS = ROOT / "models/kokoro-int8-multi-lang-v1_1"
OUT = ROOT / "output/thread_sweep"


def run(*args: str) -> dict:
    result = subprocess.run([str(CLI), *map(str, args)], cwd=ROOT, check=True,
                            text=True, capture_output=True, timeout=120)
    return json.loads(result.stdout.strip().splitlines()[-1])


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    report = []
    for threads in (1, 2, 4):
        stt = run("stt", "--model-dir", ASR, "--wav", ASR / "test_wavs/zh.wav",
                  "--threads", threads, "--warmup", 1, "--repeat", 7)
        tts = run("tts", "--model-dir", TTS, "--text", "你好，欢迎使用语音服务。",
                  "--output", OUT / f"tts_threads_{threads}.wav", "--speaker", 3,
                  "--threads", threads, "--warmup", 1, "--repeat", 3)
        report.append({"threads": threads, "stt": stt, "tts": tts})
    (OUT / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                     encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
