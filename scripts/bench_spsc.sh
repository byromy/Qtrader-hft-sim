#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${repo_root}/build/qtrader_spsc_bench"
results="${repo_root}/results"
mkdir -p "${results}"

if [[ ! -x "${binary}" ]]; then
  echo "Build first: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
  exit 1
fi

for run in $(seq 1 "${RUNS:-10}"); do
  echo "run=${run}"
  "${binary}" "${SAMPLES:-1000000}" "${WARMUP:-100000}"
done | tee "${results}/spsc-comparison.txt"
