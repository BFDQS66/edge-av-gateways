#!/usr/bin/env python3
"""Run two C++ camera workers against one RTSP server and inspect the API."""

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


def get_json(url: str):
    with urllib.request.urlopen(url, timeout=2) as response:
        return json.load(response)


def stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--gateway", required=True)
    parser.add_argument("--generate", required=True)
    parser.add_argument("--test-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.test_dir.mkdir(parents=True, exist_ok=True)
    fixture = args.test_dir / "fixture.avi"
    output = args.test_dir / "events"
    subprocess.run([args.generate, str(fixture)], check=True, timeout=10)
    rtsp_port, http_port = unused_port(), unused_port()
    config = args.test_dir / "gateway.yaml"
    config.write_text(
        "%YAML:1.0\n---\n"
        f"http_port: {http_port}\n"
        "reconnect_ms: 200\n"
        f"output_dir: {json.dumps(str(output))}\n"
        "dwell_frames: 4\n"
        "sources:\n"
        f"  - {{ id: camera_a, uri: \"rtsp://127.0.0.1:{rtsp_port}/crossing\" }}\n"
        f"  - {{ id: camera_b, uri: \"rtsp://127.0.0.1:{rtsp_port}/crossing\" }}\n",
        encoding="utf-8",
    )
    with (args.test_dir / "server.log").open("w", encoding="utf-8") as server_log, (
        args.test_dir / "gateway.log"
    ).open("w", encoding="utf-8") as gateway_log:
        server = subprocess.Popen(
            [args.server, "--file", str(fixture), "--port", str(rtsp_port)],
            stdout=server_log,
            stderr=subprocess.STDOUT,
        )
        gateway = subprocess.Popen(
            [args.gateway, "--config", str(config), "--max-frames", "120"],
            stdout=gateway_log,
            stderr=subprocess.STDOUT,
        )
        try:
            deadline = time.monotonic() + 25
            health = None
            while time.monotonic() < deadline:
                if server.poll() is not None or gateway.poll() is not None:
                    raise RuntimeError("RTSP server or gateway exited early")
                try:
                    health = get_json(f"http://127.0.0.1:{http_port}/health")
                except (urllib.error.URLError, ConnectionError):
                    time.sleep(0.2)
                    continue
                if all(camera["state"] == "completed" for camera in health["cameras"]):
                    break
                time.sleep(0.2)
            else:
                raise TimeoutError(f"camera workers did not finish: {health}")

            assert len(health["cameras"]) == 2, health
            assert all(c["processed_frames"] == 120 for c in health["cameras"]), health
            events = get_json(f"http://127.0.0.1:{http_port}/api/v1/events")
            for camera_id in ("camera_a", "camera_b"):
                camera_events = [e for e in events if e["camera_id"] == camera_id]
                kinds = {e["type"] for e in camera_events}
                assert {"line_cross", "zone_enter", "dwell"}.issubset(kinds), camera_events
                assert all(pathlib.Path(e["snapshot"]).is_file() for e in camera_events)
                assert (output / camera_id / "events.jsonl").is_file()
            with urllib.request.urlopen(f"http://127.0.0.1:{http_port}/metrics") as response:
                metrics = response.read().decode("utf-8")
            assert 'veg_frames_total{camera="camera_a"} 120' in metrics, metrics
            assert 'veg_frames_total{camera="camera_b"} 120' in metrics, metrics
            print(f"gateway loopback passed: 2 cameras, {len(events)} events")
        except Exception:
            print((args.test_dir / "server.log").read_text(encoding="utf-8"))
            print((args.test_dir / "gateway.log").read_text(encoding="utf-8"))
            raise
        finally:
            stop(gateway)
            stop(server)


if __name__ == "__main__":
    main()
