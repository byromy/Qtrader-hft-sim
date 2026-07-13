#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${repo_root}/build/qtrader_avx2_bench"
results="${repo_root}/results"
mkdir -p "${results}"

if [[ ! -x "${binary}" ]]; then
  echo "找不到 AVX2 基准程序：${binary}" >&2
  echo "请先以 QTRADER_ENABLE_AVX2=ON 构建 Qtrader。" >&2
  exit 1
fi

if ! grep -qw avx2 /proc/cpuinfo; then
  echo "当前机器没有暴露 AVX2 指令集" >&2
  exit 1
fi

for run in $(seq 1 "${RUNS:-10}"); do
  echo "run=${run}"
  "${binary}" "${SAMPLES:-1048576}" "${ROUNDS:-20}"
done | tee "${results}/layout-avx2-comparison.txt"
