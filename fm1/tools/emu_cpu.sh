#!/bin/sh
# The render load of every song in build/songs (engine/test/run.sh makes them) in fm1-emu's
# fm1-live: a flash image of the current build/rb338.fwsc with the kit of build/rb338.fm1save and
# the song in the song slot, PLAY at 1.5 s, then SECONDS of playback; prints the worst render half
# since PLAY and the running average at the end (rb338_main.c cpu_peak, cpu_avg_q8), in percent
# of the half's 2.9 ms.
#   FM1_LIVE=.../fm1-live tools/emu_cpu.sh [SECONDS] [SONG ...]
set -e
cd "$(dirname "$0")/.."
B=$(pwd)/build
LIVE=${FM1_LIVE:?set FM1_LIVE to the fm1-live binary (fm1-emu)}
SECS=${1:-300}
[ $# -gt 0 ] && shift
SONGS=${*:-$(cd "$B/songs" && ls *.bin | sed 's/\.bin$//')}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
FM1_BENCH_NOKEYS=1 "$LIVE" --fwsc "$B/rb338.fwsc" --flash "$T/base.bin" --bench 300 >/dev/null 2>&1
for n in $SONGS; do
    python3 - "$B" "$n" "$T" <<'EOF'
import base64, json, struct, sys, zlib
b, n, t = sys.argv[1:]
f = bytearray(open(f"{t}/base.bin", "rb").read())
for x in json.load(open(f"{b}/rb338.fm1save"))["sectors"]:
    f[x["at"]:x["at"] + 4096] = base64.b64decode(x["data"])
d = open(f"{b}/songs/{n}.bin", "rb").read()
area = b"RBA1" + struct.pack("<III", len(d), zlib.crc32(d), 0) + d
f[0x97000:0x97000 + len(area)] = area
open(f"{t}/run.bin", "wb").write(f)
EOF
    FM1_DUMPRAM="$T/ram.bin" FM1_BENCH_KEYS='1500:12,1650:12^' \
        "$LIVE" --fwsc "$B/rb338.fwsc" --flash "$T/run.bin" --bench $((SECS * 1000 + 1500)) >/dev/null 2>&1
    python3 - "$B" "$n" "$T" <<'EOF'
import struct, sys
b, n, t = sys.argv[1:]
sym = {p[-1]: int(p[0], 16) for p in (l.split() for l in open(f"{b}/rb338.sym")) if len(p) > 3 and len(p[0]) == 8}
r = open(f"{t}/ram.bin", "rb").read()
v = lambda s: struct.unpack("<I", r[sym[s] - 0x1c00000:][:4])[0]
print(f"{n:30} peak {v('cpu_peak') / 10:5.1f}%  average {v('cpu_avg_q8') / 2560:5.1f}%")
EOF
done
