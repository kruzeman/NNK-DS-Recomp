#!/bin/sh
# check_determinism.sh — run gx_cube_test 3 times as separate processes and
# verify the PPM output is byte-identical across runs (and matches the golden
# reference when present).
set -e
BIN=build/gx_cube_test
OUT=tests/out

mkdir -p "$OUT"
for i in 1 2 3; do
    "$BIN" --runs 1 --out "$OUT/det_$i.ppm" > "$OUT/det_$i.log"
done

if ! cmp -s "$OUT/det_1.ppm" "$OUT/det_2.ppm"; then
    echo "DETERMINISM FAILURE: run1 != run2"; exit 1
fi
if ! cmp -s "$OUT/det_1.ppm" "$OUT/det_3.ppm"; then
    echo "DETERMINISM FAILURE: run1 != run3"; exit 1
fi

if [ -f tests/golden/cube.ppm ]; then
    if ! cmp -s "$OUT/det_1.ppm" tests/golden/cube.ppm; then
        echo "GOLDEN MISMATCH: output differs from tests/golden/cube.ppm"
        exit 1
    fi
    echo "3 separate runs byte-identical AND match tests/golden/cube.ppm"
else
    echo "3 separate runs byte-identical (no golden reference committed yet)"
fi
