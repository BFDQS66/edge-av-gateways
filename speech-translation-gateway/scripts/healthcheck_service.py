#!/usr/bin/env python3
"""Restart the local speech service if HTTP or inference stops responding."""

import json
import subprocess
import sys
import time
import urllib.request


URL = "http://127.0.0.1:8765/metrics"
MAX_INFLIGHT_SECONDS = 45


def main() -> None:
    reason = "HTTP metrics unreachable"
    for attempt in range(3):
        try:
            with urllib.request.urlopen(URL, timeout=2) as response:
                metrics = json.load(response)
            if metrics["inflight_sec"] <= MAX_INFLIGHT_SECONDS:
                print("healthy", "uptime_sec", metrics["uptime_sec"],
                      "inflight_sec", metrics["inflight_sec"])
                return
            reason = f"inference active for {metrics['inflight_sec']} seconds"
            break
        except Exception as error:
            reason = f"HTTP metrics unreachable: {error}"
            if attempt < 2:
                time.sleep(2)
    print("restarting speech-gateway.service:", reason, file=sys.stderr)
    subprocess.run(["systemctl", "--user", "restart", "speech-gateway.service"], check=True)


if __name__ == "__main__":
    main()
