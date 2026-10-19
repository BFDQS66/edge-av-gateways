#!/usr/bin/env python3
"""Run WAV -> STT -> local LLM translation -> TTS and save the result."""

import argparse
import json
from pathlib import Path
import time
from urllib.request import Request, urlopen


def post(url: str, data: bytes, content_type: str, timeout: int) -> bytes:
    request = Request(url, data, {"Content-Type": content_type})
    with urlopen(request, timeout=timeout) as response:
        return response.read()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wav", type=Path, required=True)
    parser.add_argument("--source", choices=("zh", "en"), required=True)
    parser.add_argument("--target", choices=("zh", "en"), required=True)
    parser.add_argument("--output-dir", type=Path, default=Path("output/voice_translate"))
    args = parser.parse_args()
    if args.source == args.target:
        parser.error("source and target must differ")

    started = time.perf_counter()
    result = json.loads(post(
        f"http://127.0.0.1:8767/v1/speech-translate?source={args.source}&target={args.target}",
        args.wav.read_bytes(), "audio/wav", 150))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    result["input_wav"] = str(args.wav)
    if result["translation"]:
        generated = post(f"http://127.0.0.1:8765/v1/tts/{args.target}",
                         result["translation"].encode("utf-8"),
                         "text/plain; charset=utf-8", 40)
        output = args.output_dir / f"translated_{args.target}.wav"
        output.write_bytes(generated)
        result["output_wav"] = str(output)
        result["output_wav_bytes"] = len(generated)
    result["end_to_end_ms"] = round((time.perf_counter() - started) * 1000, 3)
    report = args.output_dir / f"translated_{args.target}.json"
    report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                      encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
