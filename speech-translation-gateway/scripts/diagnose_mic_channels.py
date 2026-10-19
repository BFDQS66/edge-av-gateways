#!/usr/bin/env python3
"""Inspect one temporary stereo microphone WAV and print diagnostics as JSON."""

from array import array
from io import BytesIO
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
from urllib.request import Request, urlopen
import wave


def wav_bytes(pcm: bytes) -> bytes:
    buffer = BytesIO()
    with wave.open(buffer, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        output.writeframes(pcm)
    return buffer.getvalue()


def transcript(raw_stereo: bytes, channel: int, strategy: str) -> dict:
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "s16le", "-ar", "48000",
         "-ac", "2", "-i", "pipe:0", "-af", f"pan=mono|c0=c{channel}",
         "-ar", "16000", "-ac", "1", "-f", "s16le", "pipe:1"],
        input=raw_stereo, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=20, check=False,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace")[:200])
    url = "http://127.0.0.1:8765/v1/stt?strategy=" + strategy
    with urlopen(Request(url, wav_bytes(result.stdout),
                         {"Content-Type": "audio/wav"}), timeout=40) as response:
        answer = json.load(response)
    return {"text": answer["text"], "speech_sec": answer["speech_sec"],
            "latency_ms": answer["latency_ms"]}


def main(path: Path) -> None:
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (2, 2, 48000):
            raise ValueError("expected 48 kHz stereo PCM16")
        frames = source.getnframes()
        raw = source.readframes(frames)
    second_bytes = 48000 * 2 * 2
    levels = [[], []]
    for offset in range(0, len(raw) - second_bytes + 1, second_bytes):
        block = array("h")
        block.frombytes(raw[offset:offset + second_bytes])
        for channel in (0, 1):
            values = block[channel::2]
            levels[channel].append(round(math.sqrt(
                sum(value * value for value in values) / len(values)), 1))
    window = min(10, min(map(len, levels)))
    ranked = []
    for start in range(len(levels[0]) - window + 1):
        score = sum(max(levels[0][i], levels[1][i]) ** 2
                    for i in range(start, start + window))
        ranked.append((score, start))
    best_start = max(ranked)[1]
    clip = raw[best_start * second_bytes:(best_start + window) * second_bytes]
    report = {
        "duration_sec": round(frames / 48000, 1),
        "analyzed_window_sec": [best_start, best_start + window],
        "channels": {},
    }
    for channel, label in ((0, "left"), (1, "right")):
        channel_levels = levels[channel]
        report["channels"][label] = {
            "median_rms": round(statistics.median(channel_levels), 1),
            "max_1s_rms": max(channel_levels),
            "window_1s_rms": channel_levels[best_start:best_start + window],
            "gate": transcript(clip, channel, "gate"),
            "off": transcript(clip, channel, "off"),
        }
    report["fixed_windows"] = {}
    for width, limit in ((4, 32), (8, 60), (10, 60), (12, 60)):
        windows = []
        for start in range(0, min(len(levels[0]), limit), width):
            if start + width > len(levels[0]):
                break
            chunk = raw[start * second_bytes:(start + width) * second_bytes]
            answer = transcript(chunk, 0, "gate")
            windows.append({"start_sec": start, "rms": round(statistics.mean(
                levels[0][start:start + width]), 1),
                "text": answer["text"], "speech_sec": answer["speech_sec"]})
        report["fixed_windows"][str(width)] = windows
    print(json.dumps(report, ensure_ascii=False, separators=(",", ":")))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
