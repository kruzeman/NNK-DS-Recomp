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

# ---- 2D compositor (gx_2d_golden): 3 separate runs + golden reference ----
BIN2=build/gx_2d_golden
for i in 1 2 3; do
    "$BIN2" --out "$OUT/det2d_$i.ppm"
done
if ! cmp -s "$OUT/det2d_1.ppm" "$OUT/det2d_2.ppm"; then
    echo "DETERMINISM FAILURE (2D): run1 != run2"; exit 1
fi
if ! cmp -s "$OUT/det2d_1.ppm" "$OUT/det2d_3.ppm"; then
    echo "DETERMINISM FAILURE (2D): run1 != run3"; exit 1
fi
if [ -f tests/golden/scene2d.ppm ]; then
    if ! cmp -s "$OUT/det2d_1.ppm" tests/golden/scene2d.ppm; then
        echo "GOLDEN MISMATCH (2D): output differs from tests/golden/scene2d.ppm"
        exit 1
    fi
    echo "3 separate 2D runs byte-identical AND match tests/golden/scene2d.ppm"
else
    echo "3 separate 2D runs byte-identical (no 2D golden committed yet)"
fi
