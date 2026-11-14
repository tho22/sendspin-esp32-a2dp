#!/usr/bin/env bash
# Builds/flashes the Bluetooth bridge with the ESP-IDF 5.5.5 that ESPHome installed (including the A2DP patch,
# see ../scripts/patch-idf-sbc-bitrate.sh).
#
#   ./build.sh build
#   ./build.sh -p /dev/ttyUSB0 flash monitor
set -euo pipefail
cd "$(dirname "$0")"

export IDF_TOOLS_PATH="$HOME/.cache/esphome/idf"
export IDF_PATH="$IDF_TOOLS_PATH/frameworks/5.5.5"
export IDF_PYTHON_ENV_PATH="$IDF_TOOLS_PATH/penvs/5.5.5"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null

# The configuration is fully described by sdkconfig.defaults; regenerate it every time
rm -f sdkconfig
idf.py "$@"
