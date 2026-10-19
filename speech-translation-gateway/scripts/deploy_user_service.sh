#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
units="${HOME}/.config/systemd/user"
test -x "${root}/build/speech_server"
mkdir -p "${units}"
install -m 0644 "${root}/deploy/speech-gateway.service" "${units}/speech-gateway.service"
install -m 0644 "${root}/deploy/speech-gateway-health.service" \
  "${units}/speech-gateway-health.service"
install -m 0644 "${root}/deploy/speech-gateway-health.timer" \
  "${units}/speech-gateway-health.timer"
systemctl --user daemon-reload
systemctl --user enable --now speech-gateway.service
systemctl --user restart speech-gateway.service
systemctl --user enable --now speech-gateway-health.timer
loginctl enable-linger "$(id -un)"

python3 - <<'PY'
import time
import urllib.request

for attempt in range(30):
    try:
        with urllib.request.urlopen('http://127.0.0.1:8765/readyz', timeout=2) as response:
            if response.status == 200:
                print('speech-gateway.service is ready')
                break
    except Exception:
        time.sleep(1)
else:
    raise SystemExit('speech gateway did not become ready within 30 checks')
PY
