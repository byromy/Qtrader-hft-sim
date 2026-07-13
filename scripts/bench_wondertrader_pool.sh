#!/usr/bin/env bash
set -euo pipefail

qtrader_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
wt_root="${WONDERTRADER_ROOT:-${qtrader_root}/../wondertrader-master}"
bin_dir="${wt_root}/src/build_all/build_x64/Release/bin/WtLatencyHFTBench"
work_dir="${wt_root}/dist/WtLatencyHFTBench"
results="${qtrader_root}/results"
mkdir -p "${results}"

for binary in WtLatencyHFTBench WtLatencyHFTBenchPool; do
  if [[ ! -x "${bin_dir}/${binary}" ]]; then
    echo "找不到 ${bin_dir}/${binary}" >&2
    echo "请先在完整 WonderTrader 工程中构建 WtLatencyHFTBench。" >&2
    exit 1
  fi
done

if [[ ! -d "${work_dir}" ]]; then
  echo "找不到 WonderTrader 运行目录：${work_dir}" >&2
  exit 1
fi

cd "${work_dir}"
ln -sfn ../WtRunnerHft/actpolicy.yaml actpolicy.yaml
for run in $(seq 1 "${RUNS:-5}"); do
  echo "run=${run} variant=original_spinlock"
  "${bin_dir}/WtLatencyHFTBench"
  echo "run=${run} variant=single_thread_unlocked"
  "${bin_dir}/WtLatencyHFTBenchPool"
done 2>&1 | tee "${results}/wondertrader-pool-comparison.txt"
