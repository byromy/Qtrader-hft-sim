#!/usr/bin/env bash
set -euo pipefail

QTRADER_PROJECT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
QTRADER_BENCH_BIN=${BENCH_BIN:-/tmp/ParserITCHUftFormalBench}
QTRADER_ITCH_FILE=${ITCH_FILE:-/media/yu/新加卷/20190730.PSX_ITCH_50}
QTRADER_SYMBOL=${SYMBOL:-AAPL}
QTRADER_DATE=${TRADING_DATE:-20190730}
QTRADER_RUNS=${RUNS:-5}
QTRADER_CPU=${CPU:-8}
QTRADER_RUN_TAG=${RUN_TAG:-$(date +%Y%m%d-%H%M%S)}
QTRADER_WARMUP_PATH_ORDER=${WARMUP_PATH_ORDER:-QWT}
QTRADER_RESULT_DIR=${RESULT_DIR:-${QTRADER_PROJECT_DIR}/bench/results}
QTRADER_CONFIG_DIR=${CONFIG_DIR:-${QTRADER_PROJECT_DIR}/integration/wondertrader/ParserITCH/config}
QTRADER_PREFIX=${QTRADER_RESULT_DIR}/wt-qtrader-uft-formal-${QTRADER_RUN_TAG}
QTRADER_CGROUP_MEMBERSHIP=$(awk -F: '$1 == "0" {print $3}' /proc/self/cgroup)
QTRADER_ISOLATION=${ISOLATION:-none}
if [[ -z ${ISOLATION:-} ]] &&
   [[ "${QTRADER_CGROUP_MEMBERSHIP}" == /qtrader-isolated ]] &&
   [[ -r /sys/fs/cgroup/qtrader-isolated/cpuset.cpus.partition ]] &&
   [[ $(< /sys/fs/cgroup/qtrader-isolated/cpuset.cpus.partition) == isolated ]]; then
  QTRADER_ISOLATION=cgroup_v2_cpuset_isolated_8-9
fi
if [[ "${QTRADER_WARMUP_PATH_ORDER}" != WTQ &&
      "${QTRADER_WARMUP_PATH_ORDER}" != QWT ]]; then
  printf 'error: WARMUP_PATH_ORDER must be WTQ or QWT\n' >&2
  exit 64
fi

run_benchmark()
{
  local path_order=$1
  taskset -c "${QTRADER_CPU}" "${QTRADER_BENCH_BIN}" \
    "${QTRADER_ITCH_FILE}" "${QTRADER_SYMBOL}" "${QTRADER_DATE}" \
    "${QTRADER_CONFIG_DIR}/sessions.json" \
    "${QTRADER_CONFIG_DIR}/commodities.json" \
    "${QTRADER_CONFIG_DIR}/contracts.json" "${path_order}"
}

if [[ ! -x ${QTRADER_BENCH_BIN} ]]; then
	printf 'error: benchmark binary is not executable: %s\n' \
		"${QTRADER_BENCH_BIN}" >&2
	printf 'build it first or set BENCH_BIN to its persistent absolute path\n' >&2
	exit 66
fi
if [[ ! -r ${QTRADER_ITCH_FILE} ]]; then
	printf 'error: ITCH corpus is not readable: %s\n' \
		"${QTRADER_ITCH_FILE}" >&2
	exit 66
fi
for QTRADER_REQUIRED_CONFIG in sessions.json commodities.json contracts.json; do
	if [[ ! -r ${QTRADER_CONFIG_DIR}/${QTRADER_REQUIRED_CONFIG} ]]; then
		printf 'error: required configuration is not readable: %s/%s\n' \
			"${QTRADER_CONFIG_DIR}" "${QTRADER_REQUIRED_CONFIG}" >&2
		exit 66
	fi
done

mkdir -p "${QTRADER_RESULT_DIR}"
printf '%s\n' 'run,path_order,metric,scope,framework,samples_or_inputs,outputs,mean_ns,p50_ns,p99_ns,p999_ns,max_ns,cpu_migrations,input_mps,output_mps,passes,seconds,verified_callbacks' > "${QTRADER_PREFIX}.csv"
: > "${QTRADER_PREFIX}.raw.txt"

{
  printf 'benchmark_version=uft-production-v3\n'
  printf 'benchmark_scope=raw_ITCH_message_to_first_UFT_callback_arrival_and_input_round_trip\n'
  printf 'correctness_audit=separate_untimed_full_field_order_sensitive_fingerprint\n'
  printf 'latency_callback=arrival_timestamp_then_minimal_observable_consume_and_count\n'
  printf 'throughput_validation=per_pass_exact_callback_counts_and_Q_delivery_counters\n'
  printf 'isolation=%s\n' "${QTRADER_ISOLATION}"
  printf 'affinity=taskset_cpu_%s\n' "${QTRADER_CPU}"
  printf 'throughput_minimum_measured_seconds_per_path=5.0\n'
  printf 'process_level_warmup_runs=1\n'
  printf 'process_level_warmup_path_order=%s\n' \
    "${QTRADER_WARMUP_PATH_ORDER}"
  printf 'process_level_warmup_included_in_csv=no\n'
  printf 'statistics_population=measured_runs_only\n'
  printf 'measured_runs=%s\n' "${QTRADER_RUNS}"
  printf 'timestamp=%s\n' "$(date --iso-8601=seconds)"
  sha256sum "${QTRADER_BENCH_BIN}"
  sha256sum "${QTRADER_ITCH_FILE}"
  c++ --version | head -1
  uname -a
  lscpu
  printf 'kernel_cmdline='
  tr '\n' ' ' < /proc/cmdline
  printf '\n'
  printf 'cgroup_membership=%s\n' "${QTRADER_CGROUP_MEMBERSHIP}"
  if [[ -r "/sys/devices/system/cpu/cpu${QTRADER_CPU}/cpufreq/scaling_governor" ]]; then
    printf 'cpu_governor='
    tr '\n' ' ' < "/sys/devices/system/cpu/cpu${QTRADER_CPU}/cpufreq/scaling_governor"
    printf '\n'
  fi
  if [[ -r "/sys/devices/system/cpu/cpu${QTRADER_CPU}/topology/thread_siblings_list" ]]; then
    printf 'thread_siblings='
    tr '\n' ' ' < "/sys/devices/system/cpu/cpu${QTRADER_CPU}/topology/thread_siblings_list"
    printf '\n'
  fi
  printf 'low_noise_status_begin\n'
  "${QTRADER_PROJECT_DIR}/scripts/qtrader_low_noise_runtime.sh" status
  printf 'low_noise_status_end\n'
} > "${QTRADER_PREFIX}.meta.txt"

QTRADER_ASM=$(objdump -d -C \
  --disassemble='quft_message(QUftOpaque*, unsigned char const*, unsigned long)' \
  "${QTRADER_BENCH_BIN}")
if ! grep -q 'cmp.*0x3fff' <<< "${QTRADER_ASM}" ||
   ! grep -q 'call.*\*' <<< "${QTRADER_ASM}"; then
  printf 'error: Qtrader UFT disassembly guard failed\n' >&2
  exit 7
fi
printf 'uft_dispatch_disassembly_guard=PASS\n' | tee -a "${QTRADER_PREFIX}.raw.txt"

# Prime shared libraries, file/page cache, allocator paths and package frequency
# with one complete invocation of the exact benchmark binary. Its output remains
# in the raw audit trail, but every line is prefixed and none enters the CSV.
printf 'phase=process_warmup begin included_in_statistics=no path_order=%s\n' \
  "${QTRADER_WARMUP_PATH_ORDER}" | tee -a "${QTRADER_PREFIX}.raw.txt"
QTRADER_WARMUP_OUTPUT=$(run_benchmark "${QTRADER_WARMUP_PATH_ORDER}")
printf '%s\n' "${QTRADER_WARMUP_OUTPUT}" | \
  sed 's/^/WARMUP_NOT_SAMPLED,/' | tee -a "${QTRADER_PREFIX}.raw.txt"
if ! grep -q '^validation=PASS .*audit=PASS throughput_delivery=PASS' <<< \
  "${QTRADER_WARMUP_OUTPUT}"; then
  printf 'error: process-level warm-up validation failed\n' >&2
  exit 8
fi
printf 'phase=process_warmup end validation=PASS included_in_statistics=no\n' | \
  tee -a "${QTRADER_PREFIX}.raw.txt"

for QTRADER_RUN in $(seq 1 "${QTRADER_RUNS}"); do
  if (( QTRADER_RUN % 2 == 1 )); then
    QTRADER_PATH_ORDER=WTQ
  else
    QTRADER_PATH_ORDER=QWT
  fi
  printf 'phase=measured run=%s/%s cpu=%s path_order=%s isolation=%s\n' \
    "${QTRADER_RUN}" "${QTRADER_RUNS}" "${QTRADER_CPU}" \
    "${QTRADER_PATH_ORDER}" "${QTRADER_ISOLATION}" | \
    tee -a "${QTRADER_PREFIX}.raw.txt"
  QTRADER_OUTPUT=$(run_benchmark "${QTRADER_PATH_ORDER}")
  printf '%s\n' "${QTRADER_OUTPUT}" | tee -a "${QTRADER_PREFIX}.raw.txt"
  printf '%s\n' "${QTRADER_OUTPUT}" | awk -F, -v run="${QTRADER_RUN}" \
    -v order="${QTRADER_PATH_ORDER}" '
    $1 == "CSV" && $2 == "uft_latency" {
      print run "," order ",latency," $3 "," $4 "," $5 ",," $6 "," \
        $7 "," $8 "," $9 "," $10 "," $11 ",,,,,"
    }
    $1 == "CSV" && $2 == "uft_throughput" {
      print run "," order ",throughput,wall_sustained," $3 "," $4 "," \
        $5 ",,,,,,," $6 "," $7 "," $8 "," $9 "," $10
    }' >> "${QTRADER_PREFIX}.csv"
done

printf 'raw=%s\ncsv=%s\nmeta=%s\n' "${QTRADER_PREFIX}.raw.txt" \
  "${QTRADER_PREFIX}.csv" "${QTRADER_PREFIX}.meta.txt"
