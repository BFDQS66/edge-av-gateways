#!/usr/bin/env python3
"""Atomically enable the approved-stream publish hook in an existing ZLM config."""

import argparse
import os
from pathlib import Path
import re
import tempfile


def configure(contents: str, hook_url: str) -> str:
    section = ""
    seen = set()
    result = []
    for line in contents.splitlines(keepends=True):
        match = re.match(r"^\[([^]]+)\]", line)
        if match:
            section = match.group(1)
        elif section == "hook":
            match = re.match(r"^(enable|on_publish|timeoutSec)=", line)
            if match:
                key = match.group(1)
                if key in seen:
                    raise ValueError(f"duplicate hook key: {key}")
                seen.add(key)
                value = {"enable": "1", "on_publish": hook_url,
                         "timeoutSec": "2"}[key]
                line = f"{key}={value}\n"
        result.append(line)
    if seen != {"enable", "on_publish", "timeoutSec"}:
        raise ValueError(f"missing hook keys: {sorted({'enable', 'on_publish', 'timeoutSec'} - seen)}")
    return "".join(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--hook-url", default="http://127.0.0.1:18993/on_publish")
    args = parser.parse_args()
    if not re.fullmatch(r"http://127\.0\.0\.1:[0-9]{4,5}/on_publish", args.hook_url):
        parser.error("hook URL must use loopback and /on_publish")
    original = args.config.read_text(encoding="utf-8")
    updated = configure(original, args.hook_url)
    if updated == original:
        print("publish hook already configured")
        return
    descriptor, name = tempfile.mkstemp(prefix=".zlm-hook-", dir=args.config.parent)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(updated)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, args.config)
    finally:
        if os.path.exists(name):
            os.unlink(name)
    print("publish hook enabled")


if __name__ == "__main__":
    main()
