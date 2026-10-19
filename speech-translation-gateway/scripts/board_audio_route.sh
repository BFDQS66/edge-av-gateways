#!/usr/bin/env bash
# Select ES8388 input routes, without recording or saving a WAV on the board.
set -euo pipefail
mode="${1:-main}"
if [[ "$mode" != main && "$mode" != headset ]]; then
  echo 'usage: board_audio_route.sh main|headset' >&2
  exit 2
fi
card="$(arecord -l | sed -n '/es8388/s/^card \([0-9][0-9]*\):.*/\1/p' | head -n 1)"
test -n "$card"
amixer -c "$card" cset name='Capture Digital Volume' 192 >/dev/null
amixer -c "$card" sset PCM 160 >/dev/null
if [[ "$mode" == main ]]; then
  amixer -c "$card" cset name='Left PGA Mux' 'Line 2' >/dev/null
  amixer -c "$card" cset name='Right PGA Mux' 'Line 2' >/dev/null
  amixer -c "$card" cset name='Differential Mux' 'Line 2' >/dev/null
else
  amixer -c "$card" cset name='Left PGA Mux' 'Line 1' >/dev/null
  amixer -c "$card" cset name='Right PGA Mux' 'Line 1' >/dev/null
  amixer -c "$card" cset name='Differential Mux' 'Line 1' >/dev/null
fi
printf 'capture route=%s card=%s\n' "$mode" "$card"
