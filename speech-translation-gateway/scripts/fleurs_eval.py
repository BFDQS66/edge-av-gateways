#!/usr/bin/env python3
"""Evaluate the resident STT service on pinned FLEURS development audio."""

import argparse
import array
import io
import json
import pathlib
import re
import statistics
import struct
import sys
import unicodedata
import urllib.error
import urllib.request
import wave

from evaluate import edit_distance


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORPUS = ROOT / "data/fleurs"
URL = "http://127.0.0.1:{}/v1/stt?strategy={}"


def pcm16_wav(path: pathlib.Path) -> bytes:
    source = path.read_bytes()
    if source[:4] != b"RIFF" or source[8:12] != b"WAVE":
        raise ValueError(f"not RIFF/WAVE: {path}")
    position = 12
    format_chunk = None
    audio_chunk = None
    while position + 8 <= len(source):
        name = source[position:position + 4]
        length = struct.unpack_from("<I", source, position + 4)[0]
        payload = source[position + 8:position + 8 + length]
        if name == b"fmt ":
            format_chunk = payload
        elif name == b"data":
            audio_chunk = payload
        position += 8 + length + (length & 1)
    if format_chunk is None or audio_chunk is None:
        raise ValueError(f"missing WAV chunks: {path}")
    kind, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", format_chunk)
    if kind != 3 or channels != 1 or rate != 16000 or bits != 32:
        raise ValueError(f"unexpected FLEURS audio format: {path}")
    samples = array.array("h", (max(-32768, min(32767, round(value[0] * 32767)))
                                for value in struct.iter_unpack("<f", audio_chunk)))
    if sys.byteorder != "little":
        samples.byteswap()
    result = io.BytesIO()
    with wave.open(result, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(samples.tobytes())
    return result.getvalue()


def tokens(language: str, text: str) -> list[str]:
    value = unicodedata.normalize("NFKC", text).lower()
    if language == "en":
        return re.findall(r"[a-z0-9]+", value)
    return [character for character in value if character.isalnum()]


def rows(language: str, limit: int) -> list[tuple[str, str, str, int]]:
    directory = CORPUS / ("cmn_hans_cn" if language == "zh" else "en_us")
    entries = [line.rstrip("\n").split("\t")
               for line in (directory / "dev.tsv").read_text(encoding="utf-8").splitlines()]
    if limit and limit < len(entries):
        entries = [entries[i * len(entries) // limit] for i in range(limit)]
    return [(str(directory / "dev" / row[1]), row[2], row[6], int(row[5]))
            for row in entries]


def infer(audio: bytes, strategy: str, port: int) -> dict:
    request = urllib.request.Request(URL.format(port, strategy), audio,
                                     {"Content-Type": "audio/wav"})
    with urllib.request.urlopen(request, timeout=45) as response:
        return json.load(response)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--limit", type=int, default=100,
                        help="evenly spaced samples per language; 0 means all")
    parser.add_argument("--languages", default="zh,en")
    parser.add_argument("--strategies", default="segments,gate,off")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--label", default="itn1")
    args = parser.parse_args()
    languages = args.languages.split(",")
    strategies = args.strategies.split(",")
    if any(value not in ("zh", "en") for value in languages):
        raise SystemExit("languages must be zh and/or en")
    if any(value not in ("segments", "gate", "off") for value in strategies):
        raise SystemExit("strategies must be segments, gate and/or off")
    report = {"dataset": "google/fleurs dev, pinned at 4683b04af03d2d9549064c7d72060a9a94bb6046",
              "port": args.port, "label": args.label,
              "sample_limit_per_language": args.limit, "skipped_over_30s": [], "results": []}
    for language in languages:
        cases = rows(language, args.limit)
        accumulators = {name: {"errors": 0, "units": 0, "latencies": [],
                               "empty_outputs": 0, "cases": []} for name in strategies}
        for index, (path, reference, gender, sample_count) in enumerate(cases, 1):
            if sample_count > 30 * 16000:
                report["skipped_over_30s"].append(pathlib.Path(path).name)
                continue
            audio = pcm16_wav(pathlib.Path(path))
            reference_tokens = tokens(language, reference)
            for strategy in strategies:
                result = infer(audio, strategy, args.port)
                hypothesis = result["text"]
                distance = edit_distance(reference_tokens, tokens(language, hypothesis))
                accumulator = accumulators[strategy]
                accumulator["errors"] += distance
                accumulator["units"] += len(reference_tokens)
                accumulator["latencies"].append(result["latency_ms"])
                accumulator["empty_outputs"] += not bool(hypothesis)
                accumulator["cases"].append({"file": pathlib.Path(path).name,
                                             "gender": gender, "reference": reference,
                                             "hypothesis": hypothesis, "errors": distance,
                                             "reference_units": len(reference_tokens),
                                             "latency_ms": result["latency_ms"]})
            if index % 25 == 0:
                print(f"{language}: {index}/{len(cases)}", file=sys.stderr, flush=True)
        for strategy, accumulator in accumulators.items():
            latencies = sorted(accumulator["latencies"])
            report["results"].append({
                "language": language, "strategy": strategy,
                "metric": "CER" if language == "zh" else "WER",
                "samples": len(accumulator["cases"]), "errors": accumulator["errors"],
                "reference_units": accumulator["units"],
                "error_rate": round(accumulator["errors"] / accumulator["units"], 5),
                "empty_outputs": accumulator["empty_outputs"],
                "latency_mean_ms": round(statistics.mean(latencies), 3),
                "latency_p95_ms": round(latencies[int(len(latencies) * .95) - 1], 3),
                "cases": accumulator["cases"],
                "worst_examples": sorted(accumulator["cases"],
                                         key=lambda case: case["errors"] / max(case["reference_units"], 1),
                                         reverse=True)[:10],
            })
    out = ROOT / "output/evaluation" / (
        f"fleurs_{'-'.join(languages)}_{args.limit or 'all'}_"
        f"{'-'.join(strategies)}_{args.label}.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "results"},
                     ensure_ascii=False))
    for item in report["results"]:
        print(item["language"], item["strategy"], item["samples"], item["metric"],
              item["error_rate"], "empty", item["empty_outputs"],
              "mean_ms", item["latency_mean_ms"])


if __name__ == "__main__":
    main()
