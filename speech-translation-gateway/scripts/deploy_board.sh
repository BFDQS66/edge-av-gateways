#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
units="${HOME}/.config/systemd/user"
test -x "${root}/deps/llama.cpp/build/bin/llama-server"
test -x "${root}/build/speech_server"
test -x "${root}/build/quality_tts_server"
test -x "${root}/build/translation_server"
test -s "${root}/models/qwen3-1.7b-q4km/Qwen3-1.7B-Q4_K_M.gguf"
test -s "${root}/models/silero_vad.onnx"
test -s "${root}/models/kokoro-int8-multi-lang-v1_1/model.int8.onnx"
test -s "${root}/web/live.html"
test -s "${root}/scripts/live_translate.py"
mkdir -p "${units}"
for file in llama-translate.service speech-gateway.service translation-gateway.service quality-tts.service \
            speech-gateway-health.service translation-health.service \
            live-translate.service live-translate-display.service live-translate-health.service \
            live-translate-health.timer; do
  install -m 0644 "${root}/deploy/board/${file}" "${units}/${file}"
done
for file in speech-gateway-health.timer translation-health.timer; do
  install -m 0644 "${root}/deploy/${file}" "${units}/${file}"
done
systemctl --user daemon-reload
systemctl --user enable --now speech-gateway.service llama-translate.service translation-gateway.service quality-tts.service live-translate.service live-translate-display.service
systemctl --user enable --now speech-gateway-health.timer translation-health.timer live-translate-health.timer
python3 - <<'PY'
import time
import urllib.request

for attempt in range(180):
    try:
        with urllib.request.urlopen('http://127.0.0.1:8767/readyz', timeout=2) as response:
            if response.status == 200:
                print('board translation gateway ready')
                break
    except Exception:
        time.sleep(1)
else:
    raise SystemExit('board translation gateway did not become ready')

for attempt in range(30):
    try:
        with urllib.request.urlopen('http://127.0.0.1:8768/healthz', timeout=2) as response:
            if response.status == 200:
                print('board quality TTS ready')
                break
    except Exception:
        time.sleep(1)
else:
    raise SystemExit('board quality TTS did not become ready')

for attempt in range(30):
    try:
        with urllib.request.urlopen('http://127.0.0.1:8770/healthz', timeout=2) as response:
            if response.status == 200:
                print('board live translation ready')
                break
    except Exception:
        time.sleep(1)
else:
    raise SystemExit('board live translation did not become ready')
PY
