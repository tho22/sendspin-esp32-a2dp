"""Key figures of a serial log, from SETTLE seconds after the stream start: analyze_log.py LOG [SETTLE_S]"""
import re
import statistics
import sys
from datetime import datetime

path = sys.argv[1]
settle = float(sys.argv[2]) if len(sys.argv) > 2 else 120
ansi = re.compile(r"\x1b\[[0-9;]*m")
lines = [ansi.sub("", l) for l in open(path, errors="replace")]


def ts(line):
    m = re.match(r"\[(\d\d:\d\d:\d\d\.\d+)\]", line)
    return datetime.strptime(m.group(1), "%H:%M:%S.%f") if m else None


start = next((ts(l) for l in lines if "codec header" in l), None)
if start is None:
    sys.exit("no stream start found")
end = ts(lines[-1])
window = [l for l in lines if ts(l) and (ts(l) - start).total_seconds() >= settle]
minutes = max((end - start).total_seconds() - settle, 1) / 60

def count(pattern):
    return sum(1 for l in window if pattern in l)

errors = [int(m.group(1)) / 1000 for l in window if (m := re.search(r"best max_error: (\d+)", l))]
timeouts = count("timed out")
idle0 = [float(m.group(1)) for l in window if (m := re.search(r"IDLE0\[c0\]=([\d.]+)%", l))]
peaks = [int(m.group(1)) for l in window if (m := re.search(r"output peak (-?\d+) dBFS", l))]
pulls = [int(m.group(1)) for l in window if (m := re.search(r"pull rate (\d+) Hz", l))]

print(f"window {minutes:.1f} min after {settle:.0f}s settle")
print(f"  lost sync      {count('Lost sync'):4d}  ({count('Lost sync') / minutes:.1f}/min)")
print(f"  hard syncs     {count('Hard sync'):4d}")
print(f"  underruns      {count('Buffer underrun'):4d}")
print(f"  failed chunks  {count('Failed to send'):4d}")
print(f"  bt congestion  {count('is_cong'):4d}")
print(f"  time timeouts  {timeouts:4d}")
if errors:
    print(f"  sync rtt/2 ms  median {statistics.median(errors):.0f}  max {max(errors):.0f}  (n={len(errors)})")
if idle0:
    print(f"  core0 idle %   {statistics.mean(idle0):.0f}")
if peaks:
    print(f"  output peak    min {min(peaks)} dBFS")
if pulls:
    print(f"  pull rate Hz   min {min(pulls)}")
