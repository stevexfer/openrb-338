#!/bin/sh
# The C++ engine against the Rust engine (fm1/pack's references), on every shipped song:
#   (a) the host's bit-exact build (ORB_REAL=double), the full tables and the PCM kit: every
#       song must match the Rust render sample for sample;
#   (b) the FM-1-like build (ORB_REAL=float, f16 delay line, a 2048-node pool, 2048-frame
#       render calls), the FM-1's tables (filters computed) and its kit (fm1pack's default
#       codec): the SNR against the reference.
# Plus the kit decode check and the engine's RAM footprint in build (b).
#   run.sh [--no-pack]     --no-pack reuses the blobs and references of the last run
# Everything goes into fm1/build/ (gitignored).
set -e
cd "$(dirname "$0")/../.."
FM1=$(pwd)
ROOT=$(cd .. && pwd)
B="$FM1/build"
E="$FM1/engine"
mkdir -p "$B/test" "$B/blobs" "$B/songs"

PACK=1
[ "$1" = "--no-pack" ] && PACK=0

CXX="${CXX:-clang++}"
COMMON="-std=c++17 -O2 -ffp-contract=off -fno-exceptions -fno-rtti -Wall -Wextra -Werror"
DOUBLE="-DORB_REAL=double"
FLOAT="-DORB_REAL=float -DORB_DELAY_F16 -DORB_POOL_NODES=2048 -DORB_MAX_FRAMES=2048 -Wdouble-promotion -Werror=double-promotion"
SRC="$E/comp.cc $E/delay.cc $E/dist.cc $E/drums.cc $E/export.cc $E/frame.cc $E/orb_samples.cc $E/orb_tables.cc \
     $E/pcf.cc $E/seq.cc $E/tb303.cc $E/tr808.cc $E/tr909.cc"

echo "== building"
$CXX $COMMON $DOUBLE $SRC "$E/test/render.cc" -o "$B/test/render_double"
$CXX $COMMON $FLOAT $SRC "$E/test/render.cc" -o "$B/test/render_float"
$CXX $COMMON $FLOAT $SRC "$E/test/sizes.cc" -o "$B/test/sizes_float"
$CXX $COMMON $FLOAT "$E/orb_samples.cc" "$E/test/kit_check.cc" -o "$B/test/kit_check"
$CXX $COMMON $FLOAT $SRC "$E/test/save_check.cc" -o "$B/test/save_check"

name_of() { basename "$1" .rbs | sed 's/[^A-Za-z0-9]/_/g'; }

if [ $PACK = 1 ]; then
    echo "== packing (fm1pack)"
    export PATH="$HOME/.cargo/bin:$PATH"
    pack() { (cd "$FM1/pack" && cargo run -q --release -- "$@") >> "$B/test/pack.log"; }
    : > "$B/test/pack.log"
    pack tables "$B/blobs/tables_full.bin" --full
    pack tables "$B/blobs/tables.bin"
    pack kit "$B/blobs/kitpcm" --codec pcm --regions 4000000
    pack kit "$B/blobs/kit"
    for f in "$ROOT"/tests/songs/shipped/*.rbs; do
        n=$(name_of "$f")
        pack song "$f" "$B/songs/$n.bin" --ref "$B/songs/$n.ref.wav"
    done
fi

echo "== kit decode check (FM-1 kit vs PCM kit; per sample in $B/test/kit_check.log)"
"$B/test/kit_check" "$B/blobs/kitpcm_0.bin" "$B/blobs/kit_0.bin" "$B/blobs/kit_1.bin" "$B/blobs/kit_2.bin" \
    > "$B/test/kit_check.log" && KIT=ok || KIT=FAIL
grep -E "WRONG|differ|PCM kit's|ADPCM|SNR [0-9.]+ dB$|^kit:" "$B/test/kit_check.log" | grep -v "rounded exactly" || true

echo "== song save (Engine::save_blob): every song saves back unchanged, edits reload"
"$B/test/save_check" "$B/blobs/tables.bin" "$B"/songs/*.bin > "$B/test/save_check.log" && SAVE=ok || SAVE=FAIL
grep -E "DIFFERS|LOST|new song|differ" "$B/test/save_check.log" || true

echo "== songs: (a) double + full tables + PCM kit, (b) float + FM-1 tables + hybrid kit"
printf "%-30s %-12s %-24s %10s %9s\n" song "(a) exact" "(a) first diff frame" "(b) SNR dB" "(b) x RT"
FAIL=0
for f in "$ROOT"/tests/songs/shipped/*.rbs; do
    n=$(name_of "$f")
    s="$B/songs/$n.bin"
    r="$B/songs/$n.ref.wav"
    "$B/test/render_double" "$B/blobs/tables_full.bin" "$s" "$B/test/$n.double.wav" "$B/blobs/kitpcm_0.bin" \
        --ref "$r" --exact > "$B/test/$n.double.log" && A=yes || { A=NO; FAIL=1; }
    "$B/test/render_float" "$B/blobs/tables.bin" "$s" "$B/test/$n.float.wav" "$B/blobs/kit_0.bin" \
        "$B/blobs/kit_1.bin" "$B/blobs/kit_2.bin" --ref "$r" > "$B/test/$n.float.log" || FAIL=1
    first=$(sed -n 's/.*first difference at frame \([0-9]*\).*/\1/p' "$B/test/$n.double.log")
    snr=$(sed -n 's/.*SNR \([-0-9.inf]*\) dB.*/\1/p' "$B/test/$n.float.log")
    grep -q BIT-EXACT "$B/test/$n.float.log" && snr=exact
    rt=$(sed -n 's/.*: \([0-9.]*\)x realtime.*/\1/p' "$B/test/$n.float.log")
    printf "%-30s %-12s %-24s %10s %9s\n" "$n" "$A" "${first:--}" "$snr" "$rt"
done

echo "== RAM footprint, build (b)"
"$B/test/sizes_float"

echo "== kit check: $KIT, save check: $SAVE"
if [ $FAIL = 1 ] || [ $KIT != ok ] || [ $SAVE != ok ]; then
    echo "FAILED (logs in $B/test)"
    exit 1
fi
echo "all shipped songs bit-exact in build (a)"
