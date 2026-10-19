#!/usr/bin/env python3
"""Restart the live capture UI if the mic stream stops moving."""

import subprocess
import time
import json
from urllib.request import urlopen


def main() -> None:
    for attempt in range(3):
        try:
            with urlopen("http://127.0.0.1:8768/metrics", timeout=3) as response:
                metrics = json.load(response)
                if response.status == 200 and metrics.get("inflight_sec", 0) < 90:
                    break
        except Exception:
            pass
        if attempt < 2:
            time.sleep(2)
    else:
        subprocess.run(["systemctl", "--user", "restart", "quality-tts.service"],
                       check=True)

    for attempt in range(3):
        try:
            with urlopen("http://127.0.0.1:8770/healthz", timeout=3) as response:
                if response.status == 200:
                    return
        except Exception:
            pass
        if attempt < 2:
            time.sleep(2)
    subprocess.run(["systemctl", "--user", "restart", "live-translate.service"], check=True)


if __name__ == "__main__":
    main()
