#!/usr/bin/env python3
"""Record outputs, timings and simple number-preservation checks."""

import argparse
import json
from pathlib import Path
import re
import statistics
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=Path, default=ROOT / "data/translation_smoke.json")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "output/evaluation/translation_smoke.json")
    args = parser.parse_args()
    cases = json.loads(args.cases.read_text(encoding="utf-8"))
    warmup = Request("http://127.0.0.1:8767/v1/translate",
                     json.dumps({"text": "你好。", "source": "zh", "target": "en"},
                                ensure_ascii=False).encode("utf-8"),
                     {"Content-Type": "application/json"})
    with urlopen(warmup, timeout=150) as response:
        warmup_result = json.load(response)
    results = []
    for item in cases:
        body = json.dumps({key: item[key] for key in ("text", "source", "target")},
                          ensure_ascii=False).encode("utf-8")
        request = Request("http://127.0.0.1:8767/v1/translate", body,
                          {"Content-Type": "application/json"})
        with urlopen(request, timeout=150) as response:
            output = json.load(response)
        numbers = re.findall(r"\d+(?:[.:]%?\d+)*%?", item["text"])
        result = {**item, **output,
                  "numbers_in_source": numbers,
                  "literal_numbers_preserved": all(number in output["translation"]
                                                   for number in numbers)}
        results.append(result)
        print(item["source"], "->", item["target"],
              f"{output['translation_ms']:.0f}ms", output["translation"])
    latencies = sorted(item["translation_ms"] for item in results)
    report = {"model": "qwen3-1.7b-q4km", "warmup_ms": warmup_result["translation_ms"],
              "cases": results,
              "mean_ms": statistics.mean(latencies),
              "p95_ms": latencies[min(len(latencies) - 1,
                                       int(0.95 * len(latencies)))],
              "numbered_cases": sum(bool(item["numbers_in_source"]) for item in results),
              "literal_numbers_preserved_cases": sum(
                  bool(item["numbers_in_source"]) and item["literal_numbers_preserved"]
                  for item in results)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    print("report:", args.output)


if __name__ == "__main__":
    main()
