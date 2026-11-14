#!/usr/bin/env python3
"""Decode RTSP and RTMP from a separate client using OpenCV's FFmpeg backend."""

import argparse
import json
import time

import cv2


def check(name: str, url: str, frame_count: int, timeout_ms: int) -> dict:
    started = time.monotonic()
    capture = cv2.VideoCapture(
        url, cv2.CAP_FFMPEG,
        [cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, timeout_ms,
         cv2.CAP_PROP_READ_TIMEOUT_MSEC, timeout_ms],
    )
    opened_ms = round((time.monotonic() - started) * 1000, 1)
    frames = 0
    width = 0
    height = 0
    try:
        if capture.isOpened():
            for _ in range(frame_count):
                ok, frame = capture.read()
                if not ok:
                    break
                frames += 1
                height, width = frame.shape[:2]
    finally:
        capture.release()
    return {
        "protocol": name, "frames": frames, "width": width, "height": height,
        "open_ms": opened_ms,
        "total_ms": round((time.monotonic() - started) * 1000, 1),
        "ok": frames == frame_count,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--rtsp-port", type=int, default=8554)
    parser.add_argument("--rtmp-port", type=int, default=1935)
    parser.add_argument("--frames", type=int, default=10)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    args = parser.parse_args()
    if args.frames < 1 or args.timeout_ms < 1:
        parser.error("frames and timeout-ms must be positive")
    results = [
        check("RTSP", f"rtsp://{args.host}:{args.rtsp_port}/live/synthetic",
              args.frames, args.timeout_ms),
        check("RTMP", f"rtmp://{args.host}:{args.rtmp_port}/live/synthetic",
              args.frames, args.timeout_ms),
    ]
    print(json.dumps(results, ensure_ascii=False, indent=2))
    if not all(result["ok"] for result in results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
