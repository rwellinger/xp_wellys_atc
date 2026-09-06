#!/usr/bin/env bash
# Regenerates the ATC test fixture using macOS `say` + `afconvert`.
# Output: 16 kHz / mono / 16-bit PCM WAV — whisper.cpp's native format,
# so no resampling is needed inside the spike.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/../test_audio"
mkdir -p "${OUT_DIR}"

# Voice that pronounces aviation phraseology cleanly. Daniel (en_GB) reads
# numbers and the NATO alphabet without weird liaisons; Samantha (en_US) is
# fine too but slightly more conversational. Pick Daniel for ATC neutrality.
VOICE="Daniel"
RATE_WPM=170   # Real ATC sits around 180 wpm; 170 leaves some margin for STT.

generate() {
    local name="$1"; shift
    local text="$1"; shift

    local aiff="${OUT_DIR}/${name}.aiff"
    local wav="${OUT_DIR}/${name}.wav"

    say -v "${VOICE}" -r "${RATE_WPM}" -o "${aiff}" "${text}"
    afconvert "${aiff}" "${wav}" \
        -d LEI16@16000 \
        -f WAVE \
        -c 1
    rm -f "${aiff}"
    echo "wrote ${wav}"
}

# Standard pattern-departure call at a towered EU field — typical of what the
# plugin will see in milestone 06. Roughly 5 s at 170 wpm.
generate "tower_ready_for_departure" \
    "Bern Tower, Hotel Bravo Romeo Charlie Delta, holding short runway one four, ready for departure."
