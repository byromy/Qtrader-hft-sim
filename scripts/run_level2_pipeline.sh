#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${repo_root}/build/qtrader_pipeline_reference"
dense_binary="${repo_root}/build/qtrader_pipeline_dense"
results="${repo_root}/results"
mkdir -p "${results}"

if [[ ! -x "${binary}" ]]; then
  echo "Reference pipeline not found: ${binary}" >&2
  exit 1
fi
if [[ ! -x "${dense_binary}" ]]; then
  echo "Dense pipeline not found: ${dense_binary}" >&2
  exit 1
fi

if [[ $# -gt 0 ]]; then
  input="$1"
else
  input="--generated"
fi

{
  for run in $(seq 1 "${RUNS:-1}"); do
    echo "run=${run}"
    echo "=== reference saturated throughput ==="
    "${binary}" "${input}" "${EVENTS:-200000}" saturated
    echo "=== reference paced latency ==="
    "${binary}" "${input}" "${EVENTS:-200000}" paced
    echo "=== dense saturated throughput ==="
    "${dense_binary}" "${input}" "${EVENTS:-200000}" saturated
    echo "=== dense paced latency ==="
    "${dense_binary}" "${input}" "${EVENTS:-200000}" paced
  done
} | tee "${results}/level2-pipeline.txt"
