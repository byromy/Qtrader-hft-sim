#!/usr/bin/env bash
set -euo pipefail

QTRADER_SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
QTRADER_CPU_SET=${QTRADER_CPU_SET:-8-9}
QTRADER_HOUSEKEEPING_CPUS=${QTRADER_HOUSEKEEPING_CPUS:-0-7,10-31}
QTRADER_STATE_DIR=/run/qtrader-low-noise-runtime
QTRADER_CGROUP_PATH=/sys/fs/cgroup/qtrader-isolated
QTRADER_SETUP_COMPLETE=0

require_root()
{
	if (( EUID != 0 )); then
		printf 'error: run this operation through sudo\n' >&2
		exit 77
	fi
}

cpu_list_contains()
{
	local list=$1 target=$2 part first last
	local old_ifs=${IFS}
	IFS=,
	for part in ${list}; do
		if [[ ${part} == *-* ]]; then
			first=${part%-*}
			last=${part#*-}
		else
			first=${part}
			last=${part}
		fi
		if [[ ${first} =~ ^[0-9]+$ && ${last} =~ ^[0-9]+$ ]] &&
			(( target >= first && target <= last )); then
			IFS=${old_ifs}
			return 0
		fi
	done
	IFS=${old_ifs}
	return 1
}

affinity_targets_isolated_cpu()
{
	cpu_list_contains "$1" 8 || cpu_list_contains "$1" 9
}

setup_runtime()
{
	require_root
	if [[ -e ${QTRADER_STATE_DIR} ]]; then
		printf 'error: runtime state already exists; inspect status first\n' >&2
		exit 80
	fi
	install -d -m 0700 "${QTRADER_STATE_DIR}"
	trap 'if (( QTRADER_SETUP_COMPLETE == 0 )); then printf "error: setup was incomplete; run sudo %s restore before retrying\n" "$0" >&2; fi' EXIT

	if [[ -d ${QTRADER_CGROUP_PATH} ]]; then
		printf 'existing\n' > "${QTRADER_STATE_DIR}/cpuset-origin"
	else
		printf 'creating-by-runtime\n' > \
			"${QTRADER_STATE_DIR}/cpuset-origin"
		QTRADER_CPU_SET=${QTRADER_CPU_SET} \
			"${QTRADER_SCRIPT_DIR}/qtrader_cpuset_partition.sh" setup
		printf 'created-by-runtime\n' > "${QTRADER_STATE_DIR}/cpuset-origin"
	fi
	if [[ $(<"${QTRADER_CGROUP_PATH}/cpuset.cpus.partition") != isolated ]]; then
		printf 'error: cpuset partition is not isolated\n' >&2
		exit 81
	fi

	: > "${QTRADER_STATE_DIR}/governors.tsv"
	local cpu governor_file old_governor
	for cpu in 8 9; do
		governor_file=/sys/devices/system/cpu/cpufreq/policy${cpu}/scaling_governor
		if [[ ! -w ${governor_file} ]]; then
			printf 'error: governor file is not writable: %s\n' \
				"${governor_file}" >&2
			exit 82
		fi
		old_governor=$(<"${governor_file}")
		printf '%s\t%s\n' "${governor_file}" "${old_governor}" >> \
			"${QTRADER_STATE_DIR}/governors.tsv"
		printf 'performance\n' > "${governor_file}"
	done

	printf '%s\n' "$(< /proc/sys/kernel/watchdog)" > \
		"${QTRADER_STATE_DIR}/watchdog"
	printf '%s\n' "$(< /proc/sys/kernel/nmi_watchdog)" > \
		"${QTRADER_STATE_DIR}/nmi_watchdog"
	printf '0\n' > /proc/sys/kernel/watchdog
	printf '0\n' > /proc/sys/kernel/nmi_watchdog

	if systemctl is-active --quiet irqbalance.service; then
		printf 'active\n' > "${QTRADER_STATE_DIR}/irqbalance"
		systemctl stop irqbalance.service
	else
		printf 'inactive\n' > "${QTRADER_STATE_DIR}/irqbalance"
	fi

	: > "${QTRADER_STATE_DIR}/irq-affinity.tsv"
	: > "${QTRADER_STATE_DIR}/irq-unmoved.tsv"
	local affinity_file old_affinity irq
	for affinity_file in /proc/irq/[0-9]*/smp_affinity_list; do
		[[ -e ${affinity_file} ]] || continue
		old_affinity=$(<"${affinity_file}")
		affinity_targets_isolated_cpu "${old_affinity}" || continue
		irq=${affinity_file#/proc/irq/}
		irq=${irq%/smp_affinity_list}
		printf '%s\t%s\n' "${affinity_file}" "${old_affinity}" >> \
			"${QTRADER_STATE_DIR}/irq-affinity.tsv"
		if ! printf '%s\n' "${QTRADER_HOUSEKEEPING_CPUS}" > \
			"${affinity_file}" 2>/dev/null; then
			printf '%s\t%s\n' "${irq}" "write-rejected" >> \
				"${QTRADER_STATE_DIR}/irq-unmoved.tsv"
		fi
	done

	printf 'runtime low-noise setup complete\n'
	QTRADER_SETUP_COMPLETE=1
	trap - EXIT
	show_status
}

show_status()
{
	printf 'boot_nohz_full=%s\n' "$(< /sys/devices/system/cpu/nohz_full)"
	printf 'kernel_watchdog=%s\n' "$(< /proc/sys/kernel/watchdog)"
	printf 'nmi_watchdog=%s\n' "$(< /proc/sys/kernel/nmi_watchdog)"
	printf 'cpu8_governor=%s\n' \
		"$(< /sys/devices/system/cpu/cpufreq/policy8/scaling_governor)"
	printf 'cpu9_governor=%s\n' \
		"$(< /sys/devices/system/cpu/cpufreq/policy9/scaling_governor)"
	"${QTRADER_SCRIPT_DIR}/qtrader_cpuset_partition.sh" status
	local affinity_file effective count=0
	for affinity_file in /proc/irq/[0-9]*/effective_affinity_list; do
		[[ -e ${affinity_file} ]] || continue
		effective=$(<"${affinity_file}")
		if affinity_targets_isolated_cpu "${effective}"; then
			printf 'irq_still_on_8_9=%s:%s\n' \
				"${affinity_file#/proc/irq/}" "${effective}"
			((++count))
		fi
	done
	printf 'irqs_effective_on_8_9=%s\n' "${count}"
	if [[ -s ${QTRADER_STATE_DIR}/irq-unmoved.tsv ]]; then
		printf 'unmoved_irq_writes:\n'
		cat "${QTRADER_STATE_DIR}/irq-unmoved.tsv"
	fi
}

restore_runtime()
{
	require_root
	if [[ ! -d ${QTRADER_STATE_DIR} ]]; then
		printf 'runtime state absent\n'
		return
	fi
	if [[ -d ${QTRADER_CGROUP_PATH} &&
		-s ${QTRADER_CGROUP_PATH}/cgroup.procs ]]; then
		printf 'error: benchmark partition still has processes\n' >&2
		exit 83
	fi

	local affinity_file old_affinity
	if [[ -f ${QTRADER_STATE_DIR}/irq-affinity.tsv ]]; then
		while IFS=$'\t' read -r affinity_file old_affinity; do
			[[ -e ${affinity_file} ]] || continue
			printf '%s\n' "${old_affinity}" > "${affinity_file}" \
				2>/dev/null || true
		done < "${QTRADER_STATE_DIR}/irq-affinity.tsv"
	fi

	local governor_file old_governor
	if [[ -f ${QTRADER_STATE_DIR}/governors.tsv ]]; then
		while IFS=$'\t' read -r governor_file old_governor; do
			[[ -e ${governor_file} ]] || continue
			printf '%s\n' "${old_governor}" > "${governor_file}"
		done < "${QTRADER_STATE_DIR}/governors.tsv"
	fi
	if [[ -f ${QTRADER_STATE_DIR}/watchdog ]]; then
		printf '%s\n' "$(<"${QTRADER_STATE_DIR}/watchdog")" > \
			/proc/sys/kernel/watchdog
	fi
	if [[ -f ${QTRADER_STATE_DIR}/nmi_watchdog ]]; then
		printf '%s\n' "$(<"${QTRADER_STATE_DIR}/nmi_watchdog")" > \
			/proc/sys/kernel/nmi_watchdog
	fi
	if [[ -f ${QTRADER_STATE_DIR}/irqbalance ]] &&
		[[ $(<"${QTRADER_STATE_DIR}/irqbalance") == active ]]; then
		systemctl start irqbalance.service
	fi
	if [[ -f ${QTRADER_STATE_DIR}/cpuset-origin ]]; then
		case $(<"${QTRADER_STATE_DIR}/cpuset-origin") in
		creating-by-runtime|created-by-runtime)
			"${QTRADER_SCRIPT_DIR}/qtrader_cpuset_partition.sh" restore
			;;
		esac
	fi

	rm -f "${QTRADER_STATE_DIR}/cpuset-origin" \
		"${QTRADER_STATE_DIR}/governors.tsv" \
		"${QTRADER_STATE_DIR}/watchdog" \
		"${QTRADER_STATE_DIR}/nmi_watchdog" \
		"${QTRADER_STATE_DIR}/irqbalance" \
		"${QTRADER_STATE_DIR}/irq-affinity.tsv" \
		"${QTRADER_STATE_DIR}/irq-unmoved.tsv"
	rmdir "${QTRADER_STATE_DIR}"
	printf 'runtime low-noise settings restored\n'
}

case ${1:-} in
setup) setup_runtime ;;
status) show_status ;;
restore) restore_runtime ;;
*)
	printf 'usage: sudo %s {setup|status|restore}\n' "$0" >&2
	exit 64
	;;
esac
