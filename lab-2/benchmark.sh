#!/usr/bin/env bash
#
# Benchmark harness for lab-2/task1.c.
#
# Unlike lab-1's benchmark.c (a single C program that reimplements every
# variant in-process), MPI process count is an mpirun launch-time parameter,
# not something the program can vary internally -- so this harness drives
# ./task1 as a subprocess per trial and parses its printed timing line.
#
# Right now task1.c is still serial (no MPI, no strategy argument yet), so
# this only sweeps n with processes fixed at 1 and strategy recorded as
# "serial". Once MPI + strategy selection land (later build steps), extend
# the loops below to sweep process counts and strategies too -- the CSV
# schema (n,strategy,processes,time) is already shaped for that.
#
# Usage:
#   ./benchmark.sh
#
# Output: results.csv in the current directory.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

BINARY=./task1
OUTPUT_CSV=results.csv

if [[ ! -x "$BINARY" ]]; then
    echo "Error: $BINARY not found or not executable. Build it first (e.g. mpicc -O2 -o task1 task1.c)." >&2
    exit 1
fi

# n values: fixed step increments within ranges (inclusive of both ends),
# same shape as lab-1/benchmark.c so results are visually comparable.
N_VALUES=()
# -f "%.0f" avoids seq emitting scientific notation (e.g. "1e+06") for
# round numbers on some platforms (observed with BSD seq on macOS).
for n in $(seq -f "%.0f" 100000 50000 1000000); do N_VALUES+=("$n"); done
for n in $(seq -f "%.0f" 1500000 500000 10000000); do N_VALUES+=("$n"); done

echo "n,strategy,processes,time" > "$OUTPUT_CSV"

total=${#N_VALUES[@]}
trial=0

for n in "${N_VALUES[@]}"; do
    trial=$((trial + 1))
    output=$("$BINARY" "$n")
    time_taken=$(echo "$output" | grep "Computation time taken" | sed -E 's/[^0-9.]*([0-9.]+).*/\1/')

    if [[ -z "$time_taken" ]]; then
        echo "Warning: could not parse timing for n=$n, skipping" >&2
        continue
    fi

    echo "$n,serial,1,$time_taken" >> "$OUTPUT_CSV"
    printf "[%d/%d] n=%d -> %s s\n" "$trial" "$total" "$n" "$time_taken"
done

echo
echo "Done. Results written to $OUTPUT_CSV"
