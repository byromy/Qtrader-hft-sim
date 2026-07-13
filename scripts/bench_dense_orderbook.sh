#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${repo_root}/build/qtrader_orderbook_bench"
results="${repo_root}/results"
mkdir -p "${results}"

for run in $(seq 1 "${RUNS:-5}"); do
  echo "run=${run}"
  "${binary}" "${ORDERS:-200000}"
done | tee "${results}/dense-orderbook-comparison.txt"
