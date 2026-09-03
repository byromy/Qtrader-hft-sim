#!/usr/bin/env bash
set -euo pipefail

QTRADER_GRUB_DROPIN=/etc/default/grub.d/99-qtrader-low-latency.cfg
QTRADER_BOOT_ARGS='nohz_full=8-9 rcu_nocbs=8-9 irqaffinity=0-7,10-31 isolcpus=managed_irq,8-9 nowatchdog'

require_root()
{
	if (( EUID != 0 )); then
		printf 'error: run install/remove through sudo\n' >&2
		exit 77
	fi
}

install_config()
{
	require_root
	if [[ -e ${QTRADER_GRUB_DROPIN} ]]; then
		printf 'error: %s already exists; refusing to overwrite it\n' \
			"${QTRADER_GRUB_DROPIN}" >&2
		exit 80
	fi
	if grep -RqsE \
		'(nohz_full=|rcu_nocbs=|isolcpus=|irqaffinity=|nowatchdog)' \
		/etc/default/grub /etc/default/grub.d 2>/dev/null; then
		printf 'error: an existing GRUB CPU-isolation or watchdog setting was found; reconcile it manually first\n' >&2
		exit 81
	fi
	if ! grep -q '^CONFIG_NO_HZ_FULL=y$' "/boot/config-$(uname -r)" ||
		! grep -q '^CONFIG_RCU_NOCB_CPU=y$' "/boot/config-$(uname -r)"; then
		printf 'error: running kernel lacks NO_HZ_FULL or RCU_NOCB_CPU\n' >&2
		exit 82
	fi
	install -d -m 0755 "$(dirname "${QTRADER_GRUB_DROPIN}")"
	trap 'rm -f "${QTRADER_GRUB_DROPIN}"; printf "error: boot configuration install failed and the drop-in was removed\n" >&2' ERR
	printf '# Created by Qtrader low-noise benchmark; remove with the companion script.\n' \
		> "${QTRADER_GRUB_DROPIN}"
	printf 'GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT %s"\n' \
		"${QTRADER_BOOT_ARGS}" >> "${QTRADER_GRUB_DROPIN}"
	update-grub
	trap - ERR
	printf 'installed=%s\nreboot_required=yes\n' "${QTRADER_GRUB_DROPIN}"
}

show_status()
{
	printf 'requested_boot_args=%s\n' "${QTRADER_BOOT_ARGS}"
	printf 'configured_dropin=%s\n' \
		"$([[ -e ${QTRADER_GRUB_DROPIN} ]] && printf yes || printf no)"
	if [[ -r ${QTRADER_GRUB_DROPIN} ]]; then
		cat "${QTRADER_GRUB_DROPIN}"
	fi
	printf 'running_cmdline=%s\n' "$(< /proc/cmdline)"
	printf 'running_nohz_full=%s\n' "$(< /sys/devices/system/cpu/nohz_full)"
}

remove_config()
{
	require_root
	if [[ ! -e ${QTRADER_GRUB_DROPIN} ]]; then
		printf 'configuration already absent\n'
		return
	fi
	local saved_dropin=/run/qtrader-low-latency-grub-dropin.saved
	if [[ -e ${saved_dropin} ]]; then
		printf 'error: recovery file already exists: %s\n' \
			"${saved_dropin}" >&2
		exit 83
	fi
	mv "${QTRADER_GRUB_DROPIN}" "${saved_dropin}"
	if ! update-grub; then
		mv "${saved_dropin}" "${QTRADER_GRUB_DROPIN}"
		update-grub || true
		printf 'error: removal failed; the drop-in was restored\n' >&2
		exit 84
	fi
	rm -f "${saved_dropin}"
	printf 'removed=%s\nreboot_required=yes\n' "${QTRADER_GRUB_DROPIN}"
}

case ${1:-} in
install) install_config ;;
status) show_status ;;
remove) remove_config ;;
*)
	printf 'usage: sudo %s {install|status|remove}\n' "$0" >&2
	exit 64
	;;
esac
