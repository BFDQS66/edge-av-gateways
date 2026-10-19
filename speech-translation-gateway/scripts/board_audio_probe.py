#!/usr/bin/env python3
"""Measure microphone capture level without saving audio to disk."""

import argparse
import json
import math
import subprocess
import sys


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="hw:rockchipes8388,0")
    parser.add_argument("--seconds", type=int, default=3)
    args = parser.parse_args()
    capture = subprocess.run(
        ["arecord", "-D", args.device, "-f", "S16_LE", "-r", "48000", "-c", "2",
         "-d", str(args.seconds), "-t", "raw", "-"],
        capture_output=True, check=False,
    )
    if capture.returncode:
        print(capture.stderr.decode(errors="replace"), file=sys.stderr)
        raise SystemExit(capture.returncode)
    samples = memoryview(capture.stdout).cast("h")
    rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples)) if samples else 0
    peak = max((abs(sample) for sample in samples), default=0)
    channels = []
    for channel in (0, 1):
        part = samples[channel::2]
        channels.append({
            "rms": round(math.sqrt(sum(sample * sample for sample in part) / len(part)), 2),
            "peak": max((abs(sample) for sample in part), default=0),
        })
    print(json.dumps({"device": args.device, "sample_rate": 48000, "channels": 2,
                      "samples": len(samples), "rms": round(rms, 2), "peak": peak,
                      "per_channel": channels}))


if __name__ == "__main__":
    main()
