#!/usr/bin/env bash
# Downloads a Piper voice (ONNX model + JSON config) used by spike_piper and
# verifies SHA256 of the model. Files land in spikes/spike_piper/models/
# which is gitignored.
#
# Default voice: en_US-lessac-medium (~63 MB) — clear, neutral US accent,
# the standard real-time-friendly choice. Override with PIPER_VOICE env var.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODELS_DIR="${SCRIPT_DIR}/../models"
mkdir -p "${MODELS_DIR}"

VOICE="${PIPER_VOICE:-en_US-lessac-medium}"

case "${VOICE}" in
    en_US-lessac-medium)
        SUBPATH="en/en_US/lessac/medium"
        SHA256_ONNX="5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f"
        ;;
    en_US-ryan-high)
        SUBPATH="en/en_US/ryan/high"
        SHA256_ONNX="b3990d7606e183ec8dbfba70a4607074f162de1a0c412e0180d1ff60bb154eca"
        ;;
    *)
        echo "unknown voice: ${VOICE}" >&2
        echo "supported: en_US-lessac-medium, en_US-ryan-high" >&2
        exit 2
        ;;
esac

BASE_URL="https://huggingface.co/rhasspy/piper-voices/resolve/main/${SUBPATH}"
ONNX_URL="${BASE_URL}/${VOICE}.onnx"
JSON_URL="${BASE_URL}/${VOICE}.onnx.json"

ONNX_DST="${MODELS_DIR}/${VOICE}.onnx"
JSON_DST="${MODELS_DIR}/${VOICE}.onnx.json"

download_if_missing() {
    local url="$1" dst="$2"
    if [[ -f "${dst}" ]]; then
        echo "already present: ${dst}"
    else
        echo "downloading $(basename "${dst}")…"
        curl -L --fail -o "${dst}" "${url}"
    fi
}

download_if_missing "${ONNX_URL}" "${ONNX_DST}"
download_if_missing "${JSON_URL}" "${JSON_DST}"

echo "verifying SHA256 of ${VOICE}.onnx…"
ACTUAL=$(shasum -a 256 "${ONNX_DST}" | awk '{print $1}')
if [[ "${SHA256_ONNX}" == "UNVERIFIED" ]]; then
    echo "SHA256 not yet pinned for ${VOICE}; observed: ${ACTUAL}" >&2
    echo "Update the SHA256 constant in $(basename "$0") to pin." >&2
elif [[ "${ACTUAL}" != "${SHA256_ONNX}" ]]; then
    echo "SHA256 mismatch for ${VOICE}.onnx" >&2
    echo "  expected: ${SHA256_ONNX}" >&2
    echo "  actual  : ${ACTUAL}" >&2
    echo "Update the SHA256 constant in $(basename "$0") if upstream rehashed." >&2
    exit 1
fi

echo "ok:"
echo "  ${ONNX_DST}"
echo "  ${JSON_DST}"
