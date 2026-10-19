#!/usr/bin/env python3
"""Check HTTP input limits and that bad requests do not affect readiness."""

import json
from urllib.error import HTTPError
from urllib.request import Request, urlopen


BASE = "http://127.0.0.1:8767"


def status(path: str, body: bytes, content_type: str = "application/json") -> int:
    request = Request(BASE + path, body, {"Content-Type": content_type})
    try:
        with urlopen(request, timeout=10) as response:
            response.read()
            return response.status
    except HTTPError as error:
        error.read()
        return error.code


def main() -> None:
    assert status("/v1/translate", b"not json") == 400
    assert status("/v1/translate", json.dumps({
        "text": "", "source": "zh", "target": "en"}).encode()) == 400
    assert status("/v1/translate", json.dumps({
        "text": ".", "source": "en", "target": "zh"}).encode()) == 400
    assert status("/v1/translate", json.dumps({
        "text": "hello", "source": "en", "target": "en"}).encode()) == 400
    assert status("/v1/translate", b"x" * 4097) == 413
    assert status("/v1/speech-translate?source=zh&target=en",
                  b"not a WAV", "audio/wav") == 400
    with urlopen(BASE + "/readyz", timeout=5) as response:
        assert response.status == 200
    print("translation API rejection and readiness checks passed")


if __name__ == "__main__":
    main()
