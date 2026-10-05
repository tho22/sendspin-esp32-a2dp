#!/usr/bin/env bash
# One A/B run: build + flash with the current sdkconfig.test, log MINUTES, print key figures.
#   ./run_test.sh NAME [MINUTES]
set -euo pipefail
cd "$(dirname "$0")"
name="$1"; minutes="${2:-13}"
mkdir -p ../testruns
cp -f sdkconfig.test "../testruns/$name.sdkconfig" 2>/dev/null || true
./build.sh -p /dev/ttyUSB0 build flash > "../testruns/$name.build.log" 2>&1
../.venv/bin/python serial_log.py /dev/ttyUSB0 $((minutes * 60)) --reset > "../testruns/$name.log" 2>&1
echo "=== $name ==="
cat sdkconfig.test 2>/dev/null
../.venv/bin/python analyze_log.py "../testruns/$name.log" 120
