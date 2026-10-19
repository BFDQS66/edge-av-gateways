#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
name="sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25"
archive="${root}/models/${name}.tar.bz2"
hash="1da8b737ecc5e29f36759a4460c754863e7c919a4ba325aea187331fbfc83274"
url="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/${name}.tar.bz2"
mkdir -p "${root}/models"
if [[ ! -f "${archive}" ]] || ! printf '%s  %s\n' "${hash}" "${archive}" | sha256sum -c --status; then
  partial="${archive}.part"
  wget -q -c --tries=10 --waitretry=3 --timeout=30 -O "${partial}" "${url}"
  printf '%s  %s\n' "${hash}" "${partial}" | sha256sum -c --status
  mv -f -- "${partial}" "${archive}"
fi
if [[ ! -s "${root}/models/${name}/model.int8.onnx" ]]; then
  tar -xjf "${archive}" -C "${root}/models"
fi
test -s "${root}/models/${name}/model.int8.onnx"
test -s "${root}/models/${name}/tokens.txt"
echo "FireRedASR2 CTC INT8 is ready."
