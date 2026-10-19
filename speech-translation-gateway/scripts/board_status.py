#!/usr/bin/env python3
"""Print one board health snapshot as JSON; never write a local file."""

import json
from pathlib import Path
import shutil
import subprocess
import time
from urllib.request import urlopen


def fetch(url: str):
    try:
        with urlopen(url, timeout=3) as response:
            data = response.read()
            return json.loads(data) if response.headers.get_content_type() == "application/json" else response.status
    except Exception as error:
        return {"error": str(error)}


def service(name: str):
    result = subprocess.run(
        ["systemctl", "--user", "show", name, "--property=ActiveState,SubState,NRestarts,MainPID"],
        capture_output=True, text=True, check=False,
    )
    return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)


def main() -> None:
    root = shutil.disk_usage(Path.home())
    thermal = Path("/sys/class/thermal/thermal_zone0/temp")
    status = {
        "timestamp_unix": int(time.time()),
        "disk_free_bytes": root.free,
        "disk_used_bytes": root.used,
        "cpu_temp_c": round(int(thermal.read_text()) / 1000, 1) if thermal.exists() else None,
        "services": {name: service(name) for name in (
            "speech-gateway.service", "llama-translate.service", "translation-gateway.service",
            "quality-tts.service", "live-translate.service", "live-translate-display.service")},
        "speech_metrics": fetch("http://127.0.0.1:8765/metrics"),
        "quality_tts_metrics": fetch("http://127.0.0.1:8768/metrics"),
        "translation_metrics": fetch("http://127.0.0.1:8767/metrics"),
        "translation_ready": fetch("http://127.0.0.1:8767/readyz"),
        "live_metrics": fetch("http://127.0.0.1:8770/api/metrics"),
    }
    print(json.dumps(status, ensure_ascii=False, separators=(",", ":")))


if __name__ == "__main__":
    main()
