#!/usr/bin/env bash
# Fetches all 8 catalogued Piper voices from HuggingFace
# (rhasspy/piper-voices) and prints filename, size, sha256 in
# manifest-ready C++ form. Run once before bumping the manifest.
#
# Output is *not* committed — paste the values into
# src/persistence/model_manifest.cpp.
#
# Network-only operation. Voices are ~60 MB each * 8 = ~500 MB total
# transient (streamed to a temp dir, then removed).

set -euo pipefail

base="https://huggingface.co/rhasspy/piper-voices/resolve/main"

# Format: <sub_path>:<id>:<role>:<optional>
voices=(
  "en/en_US/lessac/medium/en_US-lessac-medium:en_US-lessac-medium:Atis:false"
  "en/en_US/ryan/high/en_US-ryan-high:en_US-ryan-high:Tower:false"
  "en/en_US/amy/medium/en_US-amy-medium:en_US-amy-medium:Ground:false"
  "en/en_GB/alan/medium/en_GB-alan-medium:en_GB-alan-medium:Center:false"
  "en/en_US/libritts_r/medium/en_US-libritts_r-medium:en_US-libritts_r-medium::true"
  "en/en_US/hfc_female/medium/en_US-hfc_female-medium:en_US-hfc_female-medium::true"
  "en/en_US/norman/medium/en_US-norman-medium:en_US-norman-medium::true"
  "en/en_GB/northern_english_male/medium/en_GB-northern_english_male-medium:en_GB-northern_english_male-medium::true"
)

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

printf "%-50s %-12s %-64s\n" "filename" "size" "sha256"
printf "%.0s-" {1..130}; echo

for entry in "${voices[@]}"; do
  IFS=":" read -r sub_path voice_id role optional <<<"$entry"
  for ext in onnx onnx.json; do
    fname="$(basename "$sub_path").$ext"
    url="$base/$sub_path.$ext"
    out="$tmpdir/$fname"
    curl -sSL -o "$out" "$url"
    size=$(stat -f%z "$out")
    hash=$(shasum -a 256 "$out" | awk '{print $1}')
    printf "%-50s %-12s %s\n" "$fname" "$size" "$hash"
    rm -f "$out"
  done
done
