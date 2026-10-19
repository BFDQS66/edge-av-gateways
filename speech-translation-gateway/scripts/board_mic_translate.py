#!/usr/bin/env python3
"""Capture a short microphone utterance and translate entirely in memory."""

import argparse
from io import BytesIO
import json
import math
import subprocess
import sys
import time
import wave
from urllib.request import Request, urlopen


def post(url: str, payload: bytes, content_type: str, timeout: int) -> bytes:
    request = Request(url, payload, {"Content-Type": content_type})
    with urlopen(request, timeout=timeout) as response:
        return response.read()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", choices=("zh", "en"), required=True)
    parser.add_argument("--target", choices=("zh", "en"), required=True)
    parser.add_argument("--seconds", type=int, default=5)
    parser.add_argument("--device", default="hw:rockchipes8388,0")
    parser.add_argument("--play-device", default="plughw:rockchipes8388,0")
    parser.add_argument("--channel", choices=("left", "right", "mix"), default="left")
    parser.add_argument("--play", action="store_true", help="play translated voice on the same ALSA card")
    args = parser.parse_args()
    if args.source == args.target:
        parser.error("source and target must differ")
    if not 1 <= args.seconds <= 25:
        parser.error("seconds must be between 1 and 25")

    started = time.perf_counter()
    capture = subprocess.run(
        ["arecord", "-D", args.device, "-f", "S16_LE", "-r", "48000", "-c", "2",
         "-d", str(args.seconds), "-t", "raw", "-"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if capture.returncode:
        print(capture.stderr.decode(errors="replace"), file=sys.stderr)
        raise SystemExit(capture.returncode)
    converter = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "s16le", "-ar", "48000",
                 "-ac", "2", "-i", "pipe:0"]
    if args.channel != "mix":
        converter += ["-af", "pan=mono|c0=c0" if args.channel == "left" else "pan=mono|c0=c1"]
    converter += ["-f", "s16le", "-ar", "16000", "-ac", "1", "pipe:1"]
    converted = subprocess.run(
        converter,
        input=capture.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if converted.returncode:
        print(converted.stderr.decode(errors="replace"), file=sys.stderr)
        raise SystemExit(converted.returncode)
    samples = memoryview(converted.stdout).cast("h")
    rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples)) if samples else 0
    buffer = BytesIO()
    with wave.open(buffer, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        output.writeframes(converted.stdout)
    result = json.loads(post(
        f"http://127.0.0.1:8767/v1/speech-translate?source={args.source}&target={args.target}",
        buffer.getvalue(), "audio/wav", 180,
    ))
    result["capture_seconds"] = args.seconds
    result["capture_device"] = args.device
    result["capture_channel"] = args.channel
    result["capture_rms"] = round(rms, 2)
    if result.get("translation"):
        tts_started = time.perf_counter()
        audio = post(f"http://127.0.0.1:8765/v1/tts/{args.target}",
                     result["translation"].encode(), "text/plain; charset=utf-8", 60)
        result["tts_ms"] = round((time.perf_counter() - tts_started) * 1000, 3)
        result["output_wav_bytes"] = len(audio)
        if args.play:
            player = subprocess.run(["aplay", "-D", args.play_device, "-"], input=audio,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=False)
            result["playback_ok"] = player.returncode == 0
            if player.returncode:
                result["playback_error"] = player.stderr.decode(errors="replace")[:500]
    result["end_to_end_ms"] = round((time.perf_counter() - started) * 1000, 3)
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
