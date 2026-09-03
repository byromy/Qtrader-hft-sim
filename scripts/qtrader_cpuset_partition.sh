#!/usr/bin/env bash
set -euo pipefail

QTRADER_CGROUP_ROOT=/sys/fs/cgroup
QTRADER_CGROUP_NAME=qtrader-isolated
QTRADER_CGROUP_PATH=${QTRADER_CGROUP_ROOT}/${QTRADER_CGROUP_NAME}
QTRADER_STATE_DIR=/run/qtrader-cpuset-partition
QTRADER_CPU_SET=${QTRADER_CPU_SET:-8-9}
QTRADER_RUN_CPU=${QTRADER_RUN_CPU:-8}
QTRADER_MEMORY_NODES=${QTRADER_MEMORY_NODES:-0}

require_root()
{
	if (( EUID != 0 )); then
		printf 'error: run this operation through sudo\n' >&2
		exit 77
	fi
}

require_cgroup_v2()
{
	if [[ $(stat -fc %T "${QTRADER_CGROUP_ROOT}") != cgroup2fs ]]; then
		printf 'error: %s is not a cgroup v2 hierarchy\n' \
			"${QTRADER_CGROUP_ROOT}" >&2
		exit 78
	fi
	if ! grep -qw cpuset "${QTRADER_CGROUP_ROOT}/cgroup.controllers"; then
		printf 'error: cgroup v2 cpuset controller is unavailable\n' >&2
		exit 79
	fi
}

setup_partition()
{
	require_root
	require_cgroup_v2
	if [[ -e "${QTRADER_CGROUP_PATH}" ]]; then
		printf 'error: %s already exists; inspect it with status first\n' \
			"${QTRADER_CGROUP_PATH}" >&2
		exit 80
	fi

	install -d -m 0700 "${QTRADER_STATE_DIR}"
	if grep -qw cpuset "${QTRADER_CGROUP_ROOT}/cgroup.subtree_control"; then
		printf 'already-enabled\n' > "${QTRADER_STATE_DIR}/cpuset-controller"
	else
		printf 'enabled-by-qtrader\n' > \
			"${QTRADER_STATE_DIR}/cpuset-controller"
		printf '+cpuset\n' > "${QTRADER_CGROUP_ROOT}/cgroup.subtree_control"
	fi

	mkdir "${QTRADER_CGROUP_PATH}"
	printf '%s\n' "${QTRADER_MEMORY_NODES}" > \
		"${QTRADER_CGROUP_PATH}/cpuset.mems"
	printf '%s\n' "${QTRADER_CPU_SET}" > \
		"${QTRADER_CGROUP_PATH}/cpuset.cpus"
	printf '%s\n' "${QTRADER_CPU_SET}" > \
		"${QTRADER_CGROUP_PATH}/cpuset.cpus.exclusive"
	printf 'isolated\n' > "${QTRADER_CGROUP_PATH}/cpuset.cpus.partition"

	local partition_state
	partition_state=$(<"${QTRADER_CGROUP_PATH}/cpuset.cpus.partition")
	if [[ "${partition_state}" != isolated ]]; then
		printf 'error: partition creation failed: %s\n' "${partition_state}" >&2
		exit 81
	fi
	printf 'created=%s cpus=%s run_cpu=%s mems=%s partition=%s\n' \
		"${QTRADER_CGROUP_PATH}" \
		"$(<"${QTRADER_CGROUP_PATH}/cpuset.cpus.effective")" \
		"${QTRADER_RUN_CPU}" \
		"$(<"${QTRADER_CGROUP_PATH}/cpuset.mems.effective")" \
		"${partition_state}"
}

show_status()
{
	require_cgroup_v2
	printf 'kernel_isolated_cpus=%s\n' \
		"$(<"${QTRADER_CGROUP_ROOT}/cpuset.cpus.isolated")"
	if [[ ! -d "${QTRADER_CGROUP_PATH}" ]]; then
		printf 'partition=absent\n'
		return
	fi
	printf 'partition=%s\n' \
		"$(<"${QTRADER_CGROUP_PATH}/cpuset.cpus.partition")"
	printf 'configured_cpus=%s\n' \
		"$(<"${QTRADER_CGROUP_PATH}/cpuset.cpus")"
	printf 'effective_cpus=%s\n' \
		"$(<"${QTRADER_CGROUP_PATH}/cpuset.cpus.effective")"
	printf 'exclusive_cpus=%s\n' \
		"$(<"${QTRADER_CGROUP_PATH}/cpuset.cpus.exclusive.effective")"
	printf 'effective_mems=%s\n' \
		"$(<"${QTRADER_CGROUP_PATH}/cpuset.mems.effective")"
	printf 'member_processes='
	tr '\n' ' ' < "${QTRADER_CGROUP_PATH}/cgroup.procs"
	printf '\n'
}

run_in_partition()
{
	require_root
	require_cgroup_v2
	if [[ ! -d "${QTRADER_CGROUP_PATH}" ]] ||
		[[ $(<"${QTRADER_CGROUP_PATH}/cpuset.cpus.partition") != isolated ]]; then
		printf 'error: isolated partition is not active\n' >&2
		exit 82
	fi
	if (( $# == 0 )); then
		printf 'error: run requires a command\n' >&2
		exit 64
	fi
	if [[ -z ${SUDO_UID:-} || -z ${SUDO_GID:-} ]]; then
		printf 'error: invoke run with sudo from the benchmark user\n' >&2
		exit 83
	fi

	printf '%s\n' "$$" > "${QTRADER_CGROUP_PATH}/cgroup.procs"
	exec taskset -c "${QTRADER_RUN_CPU}" setpriv \
		--reuid "${SUDO_UID}" --regid "${SUDO_GID}" --init-groups -- "$@"
}

restore_partition()
{
	require_root
	require_cgroup_v2
	if [[ ! -d "${QTRADER_CGROUP_PATH}" ]]; then
		if [[ -f "${QTRADER_STATE_DIR}/cpuset-controller" ]] &&
			[[ $(<"${QTRADER_STATE_DIR}/cpuset-controller") == enabled-by-qtrader ]]; then
			printf '%s\n' '-cpuset' > \
				"${QTRADER_CGROUP_ROOT}/cgroup.subtree_control" || true
		fi
		rm -f "${QTRADER_STATE_DIR}/cpuset-controller"
		rmdir "${QTRADER_STATE_DIR}" 2>/dev/null || true
		printf 'partition already absent\n'
		return
	fi
	if [[ -s "${QTRADER_CGROUP_PATH}/cgroup.procs" ]]; then
		printf 'error: partition still has processes; refusing to remove it\n' >&2
		show_status >&2
		exit 84
	fi

	printf 'member\n' > "${QTRADER_CGROUP_PATH}/cpuset.cpus.partition"
	rmdir "${QTRADER_CGROUP_PATH}"
	if [[ -f "${QTRADER_STATE_DIR}/cpuset-controller" ]] &&
		[[ $(<"${QTRADER_STATE_DIR}/cpuset-controller") == enabled-by-qtrader ]]; then
		printf '%s\n' '-cpuset' > \
			"${QTRADER_CGROUP_ROOT}/cgroup.subtree_control" || true
	fi
	rmdir "${QTRADER_STATE_DIR}" 2>/dev/null || true
	printf 'restored: %s removed\n' "${QTRADER_CGROUP_PATH}"
}

case ${1:-} in
setup)
	setup_partition
	;;
status)
	show_status
	;;
run)
	shift
	run_in_partition "$@"
	;;
restore)
	restore_partition
	;;
*)
	printf 'usage: sudo %s {setup|run COMMAND...|status|restore}\n' "$0" >&2
	exit 64
	;;
esac
