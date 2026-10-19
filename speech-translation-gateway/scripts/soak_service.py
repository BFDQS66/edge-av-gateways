#!/usr/bin/env python3
"""Exercise the resident HTTP service with valid, invalid and concurrent requests."""

import argparse
import concurrent.futures
import io
import json
import pathlib
import statistics
import time
import urllib.error
import urllib.request
import wave


ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE = "http://127.0.0.1:8765"
SAMPLE = (ROOT / "models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17"
          / "test_wavs/zh.wav").read_bytes()


def silence() -> bytes:
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(b"\0" * 32000)
    return buffer.getvalue()


SILENCE = silence()


def request(path: str, body: bytes | None = None,
            content_type: str = "application/octet-stream") -> tuple[int, bytes, float]:
    req = urllib.request.Request(BASE + path, body,
                                 {"Content-Type": content_type} if body is not None else {})
    start = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.status, response.read(), (time.perf_counter() - start) * 1000
    except urllib.error.HTTPError as error:
        return error.code, error.read(), (time.perf_counter() - start) * 1000


def metrics() -> dict:
    status, body, _ = request("/metrics")
    assert status == 200
    return json.loads(body)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cycles", type=int, default=300)
    args = parser.parse_args()
    assert args.cycles > 0
    assert request("/readyz")[0] == 200

    # Warm model caches and verify VAD state does not carry into the next request.
    assert "开饭时间" in json.loads(request("/v1/stt", SAMPLE)[1])["text"]
    assert json.loads(request("/v1/stt", SILENCE)[1])["text"] == ""
    before = metrics()
    latencies: list[float] = []
    start = time.monotonic()
    for index in range(args.cycles):
        if index % 3 == 0:
            status, body, elapsed = request("/v1/stt", SAMPLE, "audio/wav")
            assert status == 200 and json.loads(body)["text"]
        elif index % 3 == 1:
            status, body, elapsed = request("/v1/stt", SILENCE, "audio/wav")
            assert status == 200 and json.loads(body)["text"] == ""
        else:
            status, body, elapsed = request("/v1/tts/zh", "你好，欢迎使用语音服务。".encode(),
                                            "text/plain; charset=utf-8")
            assert status == 200 and body[:4] == b"RIFF"
        latencies.append(elapsed)
        if (index + 1) % 50 == 0:
            assert request("/readyz")[0] == 200

    # Invalid input must not poison the service.
    assert request("/v1/stt", b"bad")[0] == 400
    assert request("/v1/tts/en", b"\xff")[0] == 400
    assert request("/v1/tts/en", b"x" * 501)[0] == 413
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        burst = list(executor.map(lambda _: request("/v1/stt", SAMPLE, "audio/wav"),
                                  range(8)))
    codes = [item[0] for item in burst]
    assert set(codes) <= {200, 503} and 200 in codes
    assert request("/readyz")[0] == 200
    after = metrics()
    rss_growth = after["current_rss_mib"] - before["current_rss_mib"]
    report = {
        "cycles": args.cycles,
        "elapsed_sec": round(time.monotonic() - start, 3),
        "successful_sequential_requests": args.cycles,
        "concurrent_burst_statuses": codes,
        "before": before,
        "after": after,
        "current_rss_growth_mib": round(rss_growth, 3),
        "http_mean_ms": round(statistics.mean(latencies), 3),
        "http_p95_ms": round(sorted(latencies)[int(len(latencies) * .95) - 1], 3),
        "ready_after_test": True,
    }
    path = ROOT / "output/soak/report.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    if rss_growth > 128:
        raise SystemExit("resident RSS grew by more than 128 MiB after warmup")


if __name__ == "__main__":
    main()
