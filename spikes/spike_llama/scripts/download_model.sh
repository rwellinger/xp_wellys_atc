#!/usr/bin/env bash
# Downloads the Llama 3.2 3B Instruct GGUF model used by spike_llama and
# verifies its SHA256. The file lands in spikes/spike_llama/models/ which is
# gitignored.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODELS_DIR="${SCRIPT_DIR}/../models"
mkdir -p "${MODELS_DIR}"

NAME="Llama-3.2-3B-Instruct-Q4_K_M.gguf"
URL="https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/${NAME}"
SHA256="6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff"

DEST="${MODELS_DIR}/${NAME}"

if [[ -f "${DEST}" ]]; then
    echo "already present: ${DEST}"
else
    echo "downloading ${NAME} (~2.0 GB)…"
    curl -L --fail -o "${DEST}" "${URL}"
fi

echo "verifying SHA256…"
ACTUAL=$(shasum -a 256 "${DEST}" | awk '{print $1}')
if [[ "${ACTUAL}" != "${SHA256}" ]]; then
    echo "SHA256 mismatch!"
    echo "  expected: ${SHA256}"
    echo "  actual  : ${ACTUAL}"
    echo "Update the SHA256 constant in $(basename "$0") if the upstream file was rehashed."
    exit 1
fi
echo "ok: ${DEST}"
