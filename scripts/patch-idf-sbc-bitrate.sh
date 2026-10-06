#!/usr/bin/env bash
# Adapts the ESP-IDF A2DP source stack for WiFi + Bluetooth coexistence:
#  - DEFAULT_SBC_BITRATE (default 328 kbit/s): less airtime per second of audio
#  - MAX_PCM_FRAME_NUM_PER_TICK (default 21): how many SBC frames the stack may catch up per 30 ms tick.
#    When the Bluetooth task runs late a backlog builds up; with 21 the stack never catches up under frequent
#    delays and permanently delivers too little audio. The stack additionally limits to the free space in its
#    send queue (27), so 27 is safe.
# Affects ESPHome's shared ESP-IDF copy, i.e. all ESPHome projects on this machine.
#
#   scripts/patch-idf-sbc-bitrate.sh [bitrate] [frames_per_tick]   # default: 229 27
#   scripts/patch-idf-sbc-bitrate.sh --revert                      # restore the original
set -euo pipefail

IDF_VERSION="${IDF_VERSION:-5.5.5}"
FILE="$HOME/.cache/esphome/idf/frameworks/$IDF_VERSION/components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_source.c"
BACKUP="$FILE.orig"

[[ -f "$FILE" ]] || { echo "Not found: $FILE (compile once, then run again)" >&2; exit 1; }

if [[ "${1:-}" == "--revert" ]]; then
  [[ -f "$BACKUP" ]] || { echo "No backup found, nothing to do"; exit 0; }
  mv "$BACKUP" "$FILE"
  echo "Original restored"
  exit 0
fi

RATE="${1:-229}"
FRAMES="${2:-27}"
[[ -f "$BACKUP" ]] || cp "$FILE" "$BACKUP"
sed -i -E "s/^(#define DEFAULT_SBC_BITRATE[[:space:]]+)[0-9]+/\1$RATE/" "$FILE"
sed -i -E "s/^(#define MAX_PCM_FRAME_NUM_PER_TICK[[:space:]]+)[0-9]+/\1$FRAMES/" "$FILE"
grep -E "^#define (DEFAULT_SBC_BITRATE|MAX_PCM_FRAME_NUM_PER_TICK)" "$FILE"
