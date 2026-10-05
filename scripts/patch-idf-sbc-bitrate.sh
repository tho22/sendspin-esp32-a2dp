#!/usr/bin/env bash
# Passt den A2DP-Source-Stack von ESP-IDF für WLAN+Bluetooth-Koexistenz an:
#  - DEFAULT_SBC_BITRATE (Standard 328 kbit/s): weniger Funkzeit pro Sekunde Audio
#  - MAX_PCM_FRAME_NUM_PER_TICK (Standard 21): wie viele SBC-Frames der Stack pro 30-ms-Takt nachholen darf.
#    Kommt der Bluetooth-Task verspätet dran, baut sich Rückstand auf; mit 21 holt er ihn bei häufigen
#    Verspätungen nie auf und liefert dauerhaft zu wenig. Zusätzlich begrenzt der Stack ohnehin auf den
#    freien Platz in seiner Sendewarteschlange (27), daher ist 27 sicher.
# Betrifft die gemeinsame ESP-IDF-Kopie von ESPHome, also alle ESPHome-Projekte auf diesem Rechner.
#
#   scripts/patch-idf-sbc-bitrate.sh [bitrate] [frames_per_tick]   # Standard: 229 27
#   scripts/patch-idf-sbc-bitrate.sh --revert                      # Original wiederherstellen
set -euo pipefail

IDF_VERSION="${IDF_VERSION:-5.5.5}"
FILE="$HOME/.cache/esphome/idf/frameworks/$IDF_VERSION/components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_source.c"
BACKUP="$FILE.orig"

[[ -f "$FILE" ]] || { echo "Nicht gefunden: $FILE (einmal kompilieren, dann erneut ausführen)" >&2; exit 1; }

if [[ "${1:-}" == "--revert" ]]; then
  [[ -f "$BACKUP" ]] || { echo "Keine Sicherung vorhanden, nichts zu tun"; exit 0; }
  mv "$BACKUP" "$FILE"
  echo "Original wiederhergestellt"
  exit 0
fi

RATE="${1:-229}"
FRAMES="${2:-27}"
[[ -f "$BACKUP" ]] || cp "$FILE" "$BACKUP"
sed -i -E "s/^(#define DEFAULT_SBC_BITRATE[[:space:]]+)[0-9]+/\1$RATE/" "$FILE"
sed -i -E "s/^(#define MAX_PCM_FRAME_NUM_PER_TICK[[:space:]]+)[0-9]+/\1$FRAMES/" "$FILE"
grep -E "^#define (DEFAULT_SBC_BITRATE|MAX_PCM_FRAME_NUM_PER_TICK)" "$FILE"
