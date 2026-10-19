#!/usr/bin/env python3
"""Repeat bilingual LLM translation and record latency and RSS drift."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import statistics
import subprocess
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
CASES = [
    {"text": "设备编号是 7020，不是 7200。", "source": "zh", "target": "en"},
    {"text": "Do not disconnect the camera while recording.",
     "source": "en", "target": "zh"},
]


def request(case: dict) -> tuple[int, dict, float]:
    body = json.dumps(case, ensure_ascii=False).encode("utf-8")
    req = Request("http://127.0.0.1:8767/v1/translate", body,
                  {"Content-Type": "application/json"})
    started = time.perf_counter()
    try:
        with urlopen(req, timeout=150) as response:
            return response.status, json.load(response), (time.perf_counter() - started) * 1000
    except HTTPError as error:
        return error.code, json.load(error), (time.perf_counter() - started) * 1000


def rss_mib(unit: str) -> float:
    pid = int(subprocess.check_output(
        ["systemctl", "--user", "show", unit, "-p", "MainPID", "--value"],
        text=True).strip())
    if pid <= 0:
        raise RuntimeError(f"{unit} is not running")
    pages = int(Path(f"/proc/{pid}/statm").read_text().split()[1])
    return round(pages * os.sysconf("SC_PAGE_SIZE") / 1048576, 3)


def memory() -> dict:
    return {unit: rss_mib(unit) for unit in (
        "speech-gateway.service", "llama-translate.service",
        "translation-gateway.service")}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cycles", type=int, default=30)
    parser.add_argument("--output", default=str(ROOT / "output/soak/translation.json"),
                        help="report path, or - to print without writing a file")
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("cycles must be positive")
    status, output, _ = request(CASES[0])
    assert status == 200 and output.get("translation"), (status, output)
    before = memory()
    latencies = []
    for index in range(args.cycles):
        status, output, latency = request(CASES[index % len(CASES)])
        assert status == 200 and output.get("translation"), (index, status, output)
        latencies.append(latency)
        if (index + 1) % 10 == 0:
            print(f"{index + 1}/{args.cycles}", flush=True)
    with ThreadPoolExecutor(max_workers=4) as pool:
        burst = list(pool.map(request, [CASES[0]] * 4))
    after = memory()
    ordered = sorted(latencies)
    report = {"cycles": args.cycles,
              "successful_sequential_requests": len(latencies),
              "burst_statuses": [item[0] for item in burst],
              "http_mean_ms": round(statistics.mean(latencies), 3),
              "http_p95_ms": round(ordered[min(len(ordered) - 1,
                                                int(0.95 * len(ordered)))], 3),
              "rss_before_mib": before, "rss_after_mib": after,
              "rss_growth_mib": {unit: round(after[unit] - before[unit], 3)
                                 for unit in before}}
    if args.output != "-":
        destination = Path(args.output)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
