#!/bin/sh
# Record the full TS from all four tuners at the same time and check it.
#
# Usage: tools/quad-test.sh [SECONDS] [FREQ0 FREQ1 FREQ2 FREQ3]
#
# Frequencies are in Hz. The defaults are four multiplexes with good
# reception at the development site (Costa Rica, ISDB-Tb UHF 15/17/22/40);
# pass your own elsewhere. Needs dvb-tools (dvbv5-zap) and membership of
# the 'video' group.
set -eu

SECS=${1:-20}
[ $# -gt 0 ] && shift
[ $# -eq 4 ] || set -- 479142857 491142857 521142857 629142857

TOOLS=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/tbs6814-quad.XXXXXX")
CONF="$OUT/freqs.conf"

i=0
for f in "$@"; do
	printf '[T%d]\n\tDELIVERY_SYSTEM = ISDBT\n\tFREQUENCY = %s\n\tBANDWIDTH_HZ = 6000000\n\n' "$i" "$f" >> "$CONF"
	i=$((i + 1))
done

echo "Recording ${SECS}s from adapters 0-3 into $OUT ..."
for a in 0 1 2 3; do
	timeout $((SECS + 10)) dvbv5-zap -a "$a" -c "$CONF" -P -t "$SECS" \
		-o "$OUT/adapter$a.ts" "T$a" > "$OUT/adapter$a.log" 2>&1 &
done
wait

for a in 0 1 2 3; do
	printf 'adapter%d: ' "$a"
	grep -E 'Lock +\(0x1f\)' "$OUT/adapter$a.log" | tail -n 1 | tr -s ' ' || echo "NO LOCK"
done
python3 -I "$TOOLS/tscheck.py" --duration "$SECS" "$OUT"/adapter*.ts
