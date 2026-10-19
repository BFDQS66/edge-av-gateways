#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
revision="4683b04af03d2d9549064c7d72060a9a94bb6046"
base="https://huggingface.co/datasets/google/fleurs/resolve/${revision}/data"

fetch() {
  local url="$1" path="$2" hash="$3" partial
  if [[ -f "${path}" ]] && printf '%s  %s\n' "${hash}" "${path}" | sha256sum -c --status; then
    return
  fi
  partial="${path}.part"
  wget -q -c --tries=10 --waitretry=3 --timeout=30 -O "${partial}" "${url}"
  if ! printf '%s  %s\n' "${hash}" "${partial}" | sha256sum -c --status; then
    rm -f -- "${partial}"
    echo "SHA-256 mismatch: ${url}" >&2
    return 1
  fi
  mv -f -- "${partial}" "${path}"
}

for locale in cmn_hans_cn en_us; do
  mkdir -p "${root}/data/fleurs/${locale}"
done
fetch "${base}/cmn_hans_cn/dev.tsv" "${root}/data/fleurs/cmn_hans_cn/dev.tsv" \
  6b4efd804b543048feb278db06f3b58b5ea171cdd4ba072e328ad630ca25384b
fetch "${base}/cmn_hans_cn/audio/dev.tar.gz" "${root}/data/fleurs/cmn_hans_cn/dev.tar.gz" \
  3bc33212d5974eef7feb04bc4792458d6cd7e14ff10a1a24772f3c45ea87a822
fetch "${base}/en_us/dev.tsv" "${root}/data/fleurs/en_us/dev.tsv" \
  9d57ee7e91e9d4c92edb39f6bbea668ef8dc2a3ff96eb510d5580b2ad05d17ec
fetch "${base}/en_us/audio/dev.tar.gz" "${root}/data/fleurs/en_us/dev.tar.gz" \
  2658fda72f199e12676ecac9415094667a4e14e149b146e568ea00b2a2f0954c

for locale in cmn_hans_cn en_us; do
  directory="${root}/data/fleurs/${locale}"
  if [[ ! -d "${directory}/dev" ]]; then
    tar -xzf "${directory}/dev.tar.gz" -C "${directory}"
  fi
  expected="$(wc -l < "${directory}/dev.tsv")"
  actual="$(find "${directory}/dev" -maxdepth 1 -type f -name '*.wav' | wc -l)"
  if [[ "${actual}" -ne "${expected}" ]]; then
    echo "Unexpected FLEURS file count for ${locale}: ${actual}, expected ${expected}" >&2
    exit 1
  fi
done
echo "Pinned FLEURS development sets are ready."
