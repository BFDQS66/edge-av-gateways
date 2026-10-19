#!/usr/bin/env bash
# Temporary RAM-only diagnostic capture; always restore the live service.
set -euo pipefail
trap 'systemctl --user start live-translate.service' EXIT
systemctl --user stop live-translate.service
arecord -q -D hw:rockchipes8388,0 -f S16_LE -r 48000 -c 2 \
  -t wav -d 60 /dev/shm/mic_route_probe.wav
