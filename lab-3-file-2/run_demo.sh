#!/usr/bin/env bash
set -euo pipefail

make
# One OpenMP thread per port is set in the code, so OMP_NUM_THREADS is not needed.
mpirun -np 10 ./wsn_v2 3 3 --ports 6 --threshold 1 \
    --rounds 5 --interval-ms 1000 --window-ms 30000 \
    --scenario corner --log base_station.log
