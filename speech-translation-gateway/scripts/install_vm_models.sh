#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="v1.13.8"
runtime="sherpa-onnx-${version}-linux-x64-shared-lib"
binary="sherpa-onnx-${version}-linux-x64-shared"
asr="sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17"
tts="kokoro-int8-multi-lang-v1_1"
piper_zh="vits-piper-zh_CN-chaowen-medium"
piper_en="vits-piper-en_US-lessac-medium"

mkdir -p "${root}/deps" "${root}/models"

fetch() {
  local url="$1" path="$2" hash="$3"
  if [[ -f "${path}" ]] && printf '%s  %s\n' "${hash}" "${path}" | sha256sum -c --status; then
    return
  fi
  local partial="${path}.part"
  wget -q -c --tries=10 --waitretry=3 --timeout=30 -O "${partial}" "${url}"
  if ! printf '%s  %s\n' "${hash}" "${partial}" | sha256sum -c --status; then
    rm -f -- "${partial}"
    echo "SHA-256 mismatch: ${url}" >&2
    return 1
  fi
  mv -f -- "${partial}" "${path}"
}

fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/${version}/${runtime}.tar.bz2" \
  "${root}/deps/${runtime}.tar.bz2" \
  3892d184be41027e18165e67f549cd4e4cdd8dcd73ac5579e97afd55e14e30b6
fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/${version}/${binary}.tar.bz2" \
  "${root}/deps/${binary}.tar.bz2" \
  c0bdb7907d3a74bba1d55d22bf4d9fa75586cf1530614ebe88a27b9118e015c4
fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/${asr}.tar.bz2" \
  "${root}/models/${asr}.tar.bz2" \
  7d1efa2138a65b0b488df37f8b89e3d91a60676e416f515b952358d83dfd347e
fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/${tts}.tar.bz2" \
  "${root}/models/${tts}.tar.bz2" \
  a1e94694776049035c4f2c6529f003aaece993c76aae9a78995831c3c4dcafc6
fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx" \
  "${root}/models/silero_vad.onnx" \
  9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6

fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/${piper_zh}.tar.bz2" \
  "${root}/models/${piper_zh}.tar.bz2" \
  1b69bdcf10bb6331c7f8a7591940febeb175c6bb86caa3802d96531f8e4ad211
fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/${piper_en}.tar.bz2" \
  "${root}/models/${piper_en}.tar.bz2" \
  9e3febfacf0abf4270172d2958bcec246032b7e88efc2720840cc80c93de334e

mkdir -p "${root}/deps/cpp-httplib"
fetch "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.56.0/httplib.h" \
  "${root}/deps/cpp-httplib/httplib.h" \
  1f99e51881c4c9d0649b27c611442c2f4d9bcfec5a22a14d5fcd1f8106f730b4

for name in "${runtime}" "${binary}"; do
  if [[ ! -d "${root}/deps/${name}" ]]; then
    tar -xjf "${root}/deps/${name}.tar.bz2" -C "${root}/deps"
  fi
done
for name in "${asr}" "${tts}" "${piper_zh}" "${piper_en}"; do
  if [[ ! -d "${root}/models/${name}" ]]; then
    tar -xjf "${root}/models/${name}.tar.bz2" -C "${root}/models"
  fi
done

header="${root}/deps/${runtime}/include/sherpa-onnx/c-api/c-api.h"
mkdir -p "$(dirname "${header}")"
if [[ ! -s "${header}" ]]; then
  wget -q -O "${header}" \
    "https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/${version}/sherpa-onnx/c-api/c-api.h"
fi

echo "sherpa-onnx ${version}, cpp-httplib v0.56.0, SenseVoice INT8, Kokoro INT8, Piper zh/en and Silero VAD are ready."
