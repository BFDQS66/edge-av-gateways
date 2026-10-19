#!/usr/bin/env python3
"""Recover stopped or unresponsive local translation processes."""

import subprocess
import time
import urllib.request


def healthy(url: str) -> bool:
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=2) as response:
                if response.status == 200:
                    return True
        except Exception:
            pass
        if attempt < 2:
            time.sleep(2)
    return False


def restart(unit: str) -> None:
    print("restarting", unit, flush=True)
    subprocess.run(["systemctl", "--user", "restart", unit], check=True)


def main() -> None:
    if not healthy("http://127.0.0.1:8766/health"):
        restart("llama-translate.service")
        return
    if not healthy("http://127.0.0.1:8767/readyz"):
        restart("translation-gateway.service")
        return
    print("translation services healthy", flush=True)


if __name__ == "__main__":
    main()
