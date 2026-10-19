#!/usr/bin/env python3
"""Exercise systemd restart after crashes of the LLM and gateway processes."""

import argparse
import json
from pathlib import Path
import subprocess
import time
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[1]


def pid(unit: str) -> int:
    return int(subprocess.check_output(
        ["systemctl", "--user", "show", unit, "-p", "MainPID", "--value"],
        text=True).strip())


def ready() -> bool:
    try:
        with urlopen("http://127.0.0.1:8767/readyz", timeout=2) as response:
            return response.status == 200
    except Exception:
        return False


def recover(unit: str) -> dict:
    previous = pid(unit)
    assert previous > 0 and ready()
    started = time.monotonic()
    subprocess.run(["systemctl", "--user", "kill", "--signal=SIGKILL", unit],
                   check=True)
    for attempt in range(90):
        current = pid(unit)
        if current > 0 and current != previous and ready():
            return {"unit": unit, "old_pid": previous, "new_pid": current,
                    "ready_after_sec": round(time.monotonic() - started, 3)}
        time.sleep(1)
    raise RuntimeError(f"{unit} did not recover within 90 checks")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default=str(ROOT / "output/soak/translation_recovery.json"),
                        help="report path, or - to print without writing a file")
    args = parser.parse_args()
    outcomes = [recover("llama-translate.service"),
                recover("translation-gateway.service")]
    if args.output != "-":
        destination = Path(args.output)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(outcomes, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(outcomes, indent=2))


if __name__ == "__main__":
    main()
