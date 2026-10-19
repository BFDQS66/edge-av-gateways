#!/usr/bin/env python3
"""Download the pinned Qwen3 GGUF through resumable HTTP ranges and verify SHA-256."""

from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import os
from pathlib import Path
import shutil
import time
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "models/qwen3-1.7b-q4km/Qwen3-1.7B-Q4_K_M.gguf"
PARTS = DEST.parent / ".parts"
SOURCE = ("https://modelscope.cn/models/ggml-org/Qwen3-1.7B-GGUF/resolve/"
          "master/Qwen3-1.7B-Q4_K_M.gguf")
SIZE = 1_282_439_264
SHA256 = "d2387ca2dbfee2ffabce7120d3770dadca0b293052bc2f0e138fdc940d9bc7b5"
CHUNK = 16 * 1024 * 1024


def checksum(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(index: int) -> int:
    start = index * CHUNK
    stop = min(SIZE, start + CHUNK) - 1
    target = PARTS / f"{index:04d}.part"
    expected = stop - start + 1
    if target.exists() and target.stat().st_size == expected:
        return index
    for attempt in range(5):
        temporary = target.with_suffix(".tmp")
        try:
            request = Request(SOURCE, headers={"Range": f"bytes={start}-{stop}"})
            with urlopen(request, timeout=90) as response:
                content_range = response.headers.get("Content-Range", "")
                if response.status != 206 or not content_range.startswith(
                        f"bytes {start}-{stop}/"):
                    raise RuntimeError(f"unexpected range response: {response.status} {content_range}")
                with temporary.open("wb") as output:
                    shutil.copyfileobj(response, output, 1024 * 1024)
            if temporary.stat().st_size != expected:
                raise RuntimeError(f"short range {index}: {temporary.stat().st_size}")
            os.replace(temporary, target)
            return index
        except Exception as error:
            print(f"range {index} attempt {attempt + 1}: {error}", flush=True)
            if attempt == 4:
                raise
            time.sleep(min(10, 2 ** attempt))
    raise AssertionError("unreachable")


def main() -> None:
    DEST.parent.mkdir(parents=True, exist_ok=True)
    if DEST.exists() and DEST.stat().st_size == SIZE and checksum(DEST) == SHA256:
        print("model already verified:", DEST)
        return
    PARTS.mkdir(parents=True, exist_ok=True)
    count = (SIZE + CHUNK - 1) // CHUNK
    print("downloading", count, "ranges", flush=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = {pool.submit(fetch, i): i for i in range(count)}
        for done, future in enumerate(as_completed(futures), 1):
            future.result()
            if done % 8 == 0 or done == count:
                print(f"{done}/{count} ranges complete", flush=True)
    temporary = DEST.with_suffix(".assembling")
    digest = hashlib.sha256()
    with temporary.open("wb") as output:
        for index in range(count):
            with (PARTS / f"{index:04d}.part").open("rb") as source:
                for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
                    output.write(block)
                    digest.update(block)
    actual = digest.hexdigest()
    actual_size = temporary.stat().st_size
    if actual != SHA256 or actual_size != SIZE:
        temporary.unlink(missing_ok=True)
        shutil.rmtree(PARTS)
        raise SystemExit(f"model checksum/size mismatch: {actual}, {actual_size}")
    os.replace(temporary, DEST)
    shutil.rmtree(PARTS)
    print("verified", DEST, actual, flush=True)


if __name__ == "__main__":
    main()
