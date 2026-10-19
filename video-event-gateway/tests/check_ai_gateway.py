#!/usr/bin/env python3
"""Optional real-image ncnn integration test for the C++ gateway."""

import argparse
import json
import pathlib
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request


def unused_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ("gateway", "generate", "image", "param", "bin", "test-dir"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    test_dir = pathlib.Path(args.test_dir)
    test_dir.mkdir(parents=True, exist_ok=True)
    video = test_dir / "people.avi"
    subprocess.run([args.generate, args.image, str(video)], check=True, timeout=10)
    port = unused_port()
    config = test_dir / "gateway-ai.yaml"
    config.write_text(
        "%YAML:1.0\n---\n"
        f"http_port: {port}\n"
        f"output_dir: {json.dumps(str(test_dir / 'events'))}\n"
        "detector: ncnn_yolov8\n"
        f"model_param: {json.dumps(str(pathlib.Path(args.param).resolve()))}\n"
        f"model_bin: {json.dumps(str(pathlib.Path(args.bin).resolve()))}\n"
        "model_size: 640\nclass_id: 0\nscore_threshold: 0.25\n"
        "min_track_hits: 2\nmatch_distance: 100.0\n"
        "zone: [0.01, 0.01, 0.99, 0.99]\ndwell_frames: 5\n"
        f"sources:\n  - {{ id: people, uri: {json.dumps(str(video))} }}\n",
        encoding="utf-8",
    )
    log_path = test_dir / "gateway.log"
    with log_path.open("w", encoding="utf-8") as log:
        gateway = subprocess.Popen(
            [args.gateway, "--config", str(config), "--max-frames", "12"],
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            deadline = time.monotonic() + 70
            health = None
            while time.monotonic() < deadline:
                if gateway.poll() is not None:
                    raise RuntimeError(f"gateway exited: {gateway.returncode}")
                try:
                    with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=2) as response:
                        health = json.load(response)
                except (urllib.error.URLError, ConnectionError):
                    time.sleep(0.2)
                    continue
                if health["cameras"][0]["state"] == "completed":
                    break
                if health["cameras"][0]["state"] == "failed":
                    raise RuntimeError(f"camera worker failed: {health}")
                time.sleep(0.2)
            else:
                raise TimeoutError(f"ncnn worker did not complete: {health}")
            assert health["cameras"][0]["processed_frames"] == 12, health
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/v1/events") as response:
                events = json.load(response)
            kinds = {event["type"] for event in events}
            assert {"zone_enter", "dwell"}.issubset(kinds), events
            assert all(pathlib.Path(e["snapshot"]).is_file() for e in events)
            print(f"ncnn gateway passed: {len(events)} events, {health['cameras'][0]}")
        except Exception:
            print(log_path.read_text(encoding="utf-8"))
            raise
        finally:
            if gateway.poll() is None:
                gateway.send_signal(signal.SIGTERM)
                try:
                    gateway.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    gateway.kill()
                    gateway.wait(timeout=5)


if __name__ == "__main__":
    main()
