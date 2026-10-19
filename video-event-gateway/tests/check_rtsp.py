#!/usr/bin/env python3
"""Loopback RTSP integration test; uses only Python's standard library."""

import argparse
import json
import pathlib
import signal
import socket
import subprocess
import time


def wait_for_port(port: int, process: subprocess.Popen, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"RTSP server exited with status {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError("RTSP server did not open its port")


def unused_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--detect", required=True)
    parser.add_argument("--generate", required=True)
    parser.add_argument("--test-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()

    args.test_dir.mkdir(parents=True, exist_ok=True)
    video = args.test_dir / "fixture.avi"
    output = args.test_dir / "events"
    subprocess.run([args.generate, str(video)], check=True, timeout=10)
    port = unused_port()
    with (args.test_dir / "server.log").open("w", encoding="utf-8") as log:
        server = subprocess.Popen(
            [args.server, "--file", str(video), "--port", str(port), "--mount", "/crossing"],
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            wait_for_port(port, server)
            result = subprocess.run(
                [
                    args.detect,
                    "--source", f"rtsp://127.0.0.1:{port}/crossing",
                    "--output", str(output),
                    "--max-frames", "120",
                ],
                check=True,
                timeout=25,
                capture_output=True,
                text=True,
            )
            summary = json.loads(result.stdout.strip().splitlines()[-1])
            if summary["processed_frames"] < 100:
                raise AssertionError(f"received too few frames: {summary}")
            events = [
                json.loads(line)
                for line in (output / "events.jsonl").read_text(encoding="utf-8").splitlines()
            ]
            kinds = {event["type"] for event in events}
            if not {"line_cross", "zone_enter"}.issubset(kinds):
                raise AssertionError(f"expected line and zone events, got {events}")
            for event in events:
                if not (output / event["snapshot"]).is_file():
                    raise AssertionError(f"missing snapshot: {event['snapshot']}")
            print(f"RTSP loopback passed: {summary}, event_types={sorted(kinds)}")
        finally:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)


if __name__ == "__main__":
    main()
