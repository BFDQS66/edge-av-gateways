#!/usr/bin/env python3
"""Compare VAD-gated and ungated STT on synthetic non-speech, without files."""

from io import BytesIO
from array import array
import json
from pathlib import Path
import random
from urllib.request import Request, urlopen
import wave


def wav(samples: bytes) -> bytes:
    buffer = BytesIO()
    with wave.open(buffer, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        output.writeframes(samples)
    return buffer.getvalue()


def request(audio: bytes, strategy: str) -> dict:
    url = "http://127.0.0.1:8765/v1/stt?strategy=" + strategy
    with urlopen(Request(url, audio, {"Content-Type": "audio/wav"}), timeout=30) as response:
        return json.load(response)


def main() -> None:
    random.seed(441)
    silence = b"\x00\x00" * (16000 * 2)
    noise = b"".join(int(random.randint(-90, 90)).to_bytes(2, "little", signed=True)
                     for _ in range(16000 * 2))
    for label, samples in (("silence", silence), ("quiet_noise", noise)):
        audio = wav(samples)
        for strategy in ("gate", "off"):
            result = request(audio, strategy)
            print(json.dumps({"audio": label, "strategy": strategy,
                              "text": result["text"], "latency_ms": result["latency_ms"]},
                             ensure_ascii=False))
    sample_path = (Path(__file__).resolve().parents[1] / "models" /
                   "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17" /
                   "test_wavs/zh.wav")
    with wave.open(str(sample_path), "rb") as source:
        original = array("h", source.readframes(source.getnframes()))
    for scale in (1.0, 0.1, 0.03, 0.01):
        scaled = array("h", (int(value * scale) for value in original))
        audio = wav(scaled.tobytes())
        for strategy in ("gate", "off"):
            result = request(audio, strategy)
            print(json.dumps({"audio": "official_zh", "scale": scale,
                              "strategy": strategy, "text": result["text"],
                              "latency_ms": result["latency_ms"]},
                             ensure_ascii=False))


if __name__ == "__main__":
    main()
