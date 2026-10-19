#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps="${root}/deps"
commit="711780ffc75bbc5645173ed8647a7b3f328c052a"
archive="${deps}/llama.cpp-711780f.tar.gz"
expected="9aeb03d2df848342fbf194fa78a5fd04ac6345e4d66b4d4bfcd078d5946c77fd"
mkdir -p "${deps}"
if [[ ! -f "${archive}" ]] || \
   ! echo "${expected}  ${archive}" | sha256sum --check --status; then
  wget -c -O "${archive}" \
    "https://codeload.github.com/ggml-org/llama.cpp/tar.gz/${commit}"
fi
echo "${expected}  ${archive}" | sha256sum --check --status
if [[ ! -d "${deps}/llama.cpp" ]]; then
  tar -xzf "${archive}" -C "${deps}"
  mv "${deps}/llama.cpp-${commit}" "${deps}/llama.cpp"
fi
cmake -S "${deps}/llama.cpp" -B "${deps}/llama.cpp/build" \
  -DCMAKE_BUILD_TYPE=Release -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_USE_PREBUILT_UI=OFF
cmake --build "${deps}/llama.cpp/build" -j2 --target llama-server
