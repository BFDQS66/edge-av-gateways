#!/usr/bin/env python3
"""Run a finite gateway replay and report Linux process memory and progress."""

import argparse
import json
import pathlib
import re
import signal
import subprocess
import time
import urllib.error
import urllib.request


def read_peak_rss_kib(pid: int) -> int:
    status = pathlib.Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
    match = re.search(r"^VmHWM:\s+(\d+)\s+kB$", status, re.MULTILINE)
    if match is None:
        raise RuntimeError("VmHWM is missing from /proc status")
    return int(match.group(1))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gateway", type=pathlib.Path, required=True)
    parser.add_argument("--config", type=pathlib.Path, required=True)
    parser.add_argument("--max-frames", type=int, default=12)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if args.max_frames < 1 or args.timeout <= 0:
        parser.error("max-frames and timeout must be positive")
    config_text = args.config.read_text(encoding="utf-8")
    match = re.search(r"^http_port:\s*(\d+)\s*$", config_text, re.MULTILINE)
    if match is None:
        parser.error("config must have a numeric http_port")
    port = int(match.group(1))
    started = time.monotonic()
    process = subprocess.Popen(
        [str(args.gateway.resolve()), "--config", str(args.config.resolve()),
         "--max-frames", str(args.max_frames)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        deadline = started + args.timeout
        health = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"gateway exited with status {process.returncode}")
            try:
                with urllib.request.urlopen(
                    f"http://127.0.0.1:{port}/health", timeout=1
                ) as response:
                    health = json.load(response)
            except urllib.error.URLError:
                time.sleep(0.05)
                continue
            states = [camera["state"] for camera in health["cameras"]]
            if "failed" in states:
                raise RuntimeError(f"camera failed: {health}")
            if states and all(state == "completed" for state in states):
                print(json.dumps({
                    "wall_ms": round((time.monotonic() - started) * 1000, 1),
                    "peak_rss_mib": round(read_peak_rss_kib(process.pid) / 1024, 1),
                    "cameras": health["cameras"],
                }, ensure_ascii=False, indent=2))
                return
            time.sleep(0.05)
        raise TimeoutError(f"gateway did not complete: {health}")
    except Exception:
        process.terminate()
        try:
            _, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            _, stderr = process.communicate(timeout=5)
        if stderr:
            print(stderr[-4000:])
        raise
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


if __name__ == "__main__":
    main()
