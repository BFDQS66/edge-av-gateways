#!/usr/bin/env bash
# Publish a synthetic H.264 stream and decode it through both RTSP and RTMP.
set -euo pipefail

if [[ $# -lt 3 || $# -gt 5 ]]; then
  echo "Usage: $0 MEDIASERVER CONFIG BOARD_ETHERNET_IP [RTSP_PORT] [RTMP_PORT]" >&2
  exit 2
fi

server=$1
config=$2
board_ip=$3
rtsp_port=${4:-8554}
rtmp_port=${5:-1935}
for port in "$rtsp_port" "$rtmp_port"; do
  if [[ ! $port =~ ^[0-9]+$ ]] || (( port < 1 || port > 65535 )); then
    echo "invalid port: $port" >&2
    exit 2
  fi
done
runtime_dir=$(mktemp -d /dev/shm/edge-av-zlm.XXXXXX)
server_pid=
publisher_pid=

cleanup() {
  status=$?
  if (( status != 0 )); then
    echo "Streaming smoke test failed (exit $status)" >&2
    for output in server.stdout publisher.stdout; do
      if [[ -f "$runtime_dir/$output" ]]; then
        echo "--- $output ---" >&2
        tail -n 25 "$runtime_dir/$output" >&2
      fi
    done
  fi
  if [[ -n $publisher_pid ]]; then
    kill "$publisher_pid" 2>/dev/null || true
    wait "$publisher_pid" 2>/dev/null || true
  fi
  if [[ -n $server_pid ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -r -- "$runtime_dir"
}
trap cleanup EXIT

"$server" --config "$config" --log-dir "$runtime_dir" --level 4 --threads 2 \
  >"$runtime_dir/server.stdout" 2>&1 &
server_pid=$!
sleep 2
kill -0 "$server_pid"

ffmpeg -hide_banner -loglevel error -re \
  -f lavfi -i 'testsrc2=size=640x360:rate=15' \
  -t 30 -an -c:v libx264 -preset ultrafast -tune zerolatency \
  -pix_fmt yuv420p -g 30 -f flv \
  "rtmp://$board_ip:$rtmp_port/live/synthetic" \
  >"$runtime_dir/publisher.stdout" 2>&1 &
publisher_pid=$!
sleep 3
kill -0 "$publisher_pid"

timeout 12s ffmpeg -hide_banner -loglevel error -rtsp_transport tcp \
  -i "rtsp://$board_ip:$rtsp_port/live/synthetic" \
  -frames:v 10 -f null -
echo 'RTSP_OK: decoded 10 video frames'

timeout 12s ffmpeg -hide_banner -loglevel error \
  -i "rtmp://$board_ip:$rtmp_port/live/synthetic" \
  -frames:v 10 -f null -
echo 'RTMP_OK: decoded 10 video frames'
