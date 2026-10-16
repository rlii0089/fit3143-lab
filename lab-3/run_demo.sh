#!/usr/bin/env bash
set -euo pipefail

make
export OMP_NUM_THREADS="${OMP_NUM_THREADS:-2}"
mpirun -np 10 ./lab3_wsn 3 3 --ports 6 --threshold 1 \
    --rounds 5 --interval-ms 1000 --window-sec 30 \
    --scenario demo --log base_station.log
