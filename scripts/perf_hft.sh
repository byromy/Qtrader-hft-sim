#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runner="${repo_root}/build/qtrader_pipeline_dense"
results="${repo_root}/results"
mkdir -p "${results}"

events="task-clock,context-switches,cpu-migrations,page-faults"
if [[ "${PERF_HARDWARE:-0}" == "1" ]]; then
  events+=",cycles,instructions,branches,branch-misses,cache-references,cache-misses"
fi

perf stat -r "${PERF_RUNS:-10}" \
  -e "${events}" \
  -o "${results}/perf-stat.txt" -- "${runner}" --generated "${EVENTS:-200000}" paced

perf record -F "${PERF_FREQ:-999}" -g \
  -o "${results}/perf.data" -- "${runner}" --generated "${EVENTS:-200000}" paced

echo "perf stat: ${results}/perf-stat.txt"
echo "perf profile: perf report -i ${results}/perf.data"
