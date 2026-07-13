#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runner="${repo_root}/build/qtrader_pipeline_dense"
results="${repo_root}/results"
benchmark_log="${results}/ftrace-benchmark.log"
mkdir -p "${results}"

if ! command -v trace-cmd >/dev/null 2>&1; then
  echo "trace-cmd is required: sudo apt-get install trace-cmd" >&2
  exit 1
fi

sudo trace-cmd record \
  -o "${results}/hft-ftrace.dat" \
  -e sched:sched_switch \
  -e sched:sched_wakeup \
  -e irq:irq_handler_entry \
  -e irq:irq_handler_exit \
  -- sudo -u "$(id -un)" "${runner}" --generated "${EVENTS:-200000}" paced \
  2>&1 | tee "${benchmark_log}"

if ! grep -q '^completed=' "${benchmark_log}"; then
  echo "benchmark did not complete during ftrace capture" >&2
  exit 1
fi

sudo trace-cmd report -i "${results}/hft-ftrace.dat" > "${results}/hft-ftrace.txt"
sudo chown "$(id -u):$(id -g)" \
  "${results}/hft-ftrace.dat" \
  "${results}/hft-ftrace.txt"
echo "ftrace report: ${results}/hft-ftrace.txt"
