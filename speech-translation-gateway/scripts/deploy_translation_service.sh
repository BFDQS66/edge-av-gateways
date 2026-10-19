#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
units="${HOME}/.config/systemd/user"
test -x "${root}/deps/llama.cpp/build/bin/llama-server"
test -s "${root}/models/qwen3-1.7b-q4km/Qwen3-1.7B-Q4_K_M.gguf"
test -x "${root}/build/translation_server"
mkdir -p "${units}"
for file in llama-translate.service translation-gateway.service \
            translation-health.service translation-health.timer; do
  install -m 0644 "${root}/deploy/${file}" "${units}/${file}"
done
systemctl --user daemon-reload
systemctl --user enable --now llama-translate.service
systemctl --user enable --now translation-gateway.service
systemctl --user enable --now translation-health.timer
loginctl enable-linger "$(id -un)"

python3 - <<'PY'
import time
import urllib.request

for attempt in range(120):
    try:
        with urllib.request.urlopen('http://127.0.0.1:8767/readyz', timeout=2) as response:
            if response.status == 200:
                print('translation gateway is ready')
                break
    except Exception:
        time.sleep(1)
else:
    raise SystemExit('translation gateway did not become ready within 120 checks')
PY
