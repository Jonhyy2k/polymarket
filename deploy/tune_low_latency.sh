#!/usr/bin/env bash
# Low-latency host tuning for the arb_detector deploy box (EC2 / bare metal).
#
# Implements the README "host tuning" runbook. Our 5-min profile showed the host
# is already excellent (e2e p50 ~3 µs, no leak) and the ONLY in-process tail driver
# is involuntary preemption (~9/s) of the busy-poll threads on un-isolated cores —
# so core isolation below is the highest-value lever (and only matters once you're
# latency-competitive, i.e. colocated/Ireland; it's still µs vs the ms network).
#
#   sudo deploy/tune_low_latency.sh --cores 2,4,6 [--apply-grub] [--mitigations-off]
#
#   --cores C        CPUs to isolate for receiver/parser/sender(+logger). Reserve
#                    FULL physical cores (not hyperthread siblings of each other).
#   --apply-grub     rewrite GRUB cmdline + update-grub (NEEDS REBOOT). Without it,
#                    the recommended GRUB line is printed for you to apply.
#   --mitigations-off  also add mitigations=off (faster, but a security trade-off).
#
# Runtime settings (governor, THP, IRQ affinity, sysctls, swap, KSM) apply now.
set -euo pipefail

CORES="2,4,6"; APPLY_GRUB=0; MIT_OFF=0
while [ $# -gt 0 ]; do case "$1" in
  --cores) CORES="$2"; shift 2;;
  --apply-grub) APPLY_GRUB=1; shift;;
  --mitigations-off) MIT_OFF=1; shift;;
  *) echo "unknown arg: $1"; exit 1;;
esac; done
[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)"; exit 1; }
echo ">> isolating cores: $CORES"

echo ">> [runtime] CPU governor -> performance, turbo on"
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  [ -w "$g" ] && echo performance > "$g" || true
done
[ -w /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo || true

echo ">> [runtime] transparent hugepages -> never, KSM off, swap off"
[ -w /sys/kernel/mm/transparent_hugepage/enabled ] && echo never > /sys/kernel/mm/transparent_hugepage/enabled || true
[ -w /sys/kernel/mm/ksm/run ] && echo 0 > /sys/kernel/mm/ksm/run || true
swapoff -a || true

echo ">> [runtime] sysctl: busy-poll, no NUMA balancing, no swap pressure"
sysctl -qw net.core.busy_poll=50 net.core.busy_read=50 \
            kernel.numa_balancing=0 vm.swappiness=0 vm.stat_interval=120 || true

echo ">> [runtime] steer IRQs OFF the isolated cores (best-effort)"
# Build a hex affinity mask of all cores EXCEPT the isolated ones.
NPROC=$(nproc)
mask=0
for c in $(seq 0 $((NPROC-1))); do
  case ",$CORES," in *",$c,"*) :;; *) mask=$((mask | (1 << c)));; esac
done
hexmask=$(printf '%x' "$mask")
echo "$hexmask" > /proc/irq/default_smp_affinity 2>/dev/null || true
for irq in /proc/irq/*/smp_affinity; do echo "$hexmask" > "$irq" 2>/dev/null || true; done
echo "   (IRQs -> mask 0x$hexmask; ENA/NIC IRQs may pin themselves — verify with: grep . /proc/interrupts)"

echo ">> [limits] real-time + mlock (SCHED_FIFO / mlockall need these)"
grep -q 'arb_detector low-latency' /etc/security/limits.conf 2>/dev/null || cat >> /etc/security/limits.conf <<'LIM'
# arb_detector low-latency
*   -   rtprio    99
*   -   memlock   unlimited
*   -   nice      -20
LIM

GRUB_ADD="isolcpus=${CORES} nohz_full=${CORES} rcu_nocbs=${CORES} intel_idle.max_cstate=1 processor.max_cstate=1 transparent_hugepage=never"
[ "$MIT_OFF" -eq 1 ] && GRUB_ADD="$GRUB_ADD mitigations=off"
# nosmt is optional: doubles per-thread L1/L2 but halves cores. Decide per box.

if [ "$APPLY_GRUB" -eq 1 ]; then
  echo ">> [grub] applying boot params (REBOOT REQUIRED)"
  cp /etc/default/grub /etc/default/grub.bak.$(date +%s)
  if grep -q '^GRUB_CMDLINE_LINUX=' /etc/default/grub; then
    sed -i "s|^GRUB_CMDLINE_LINUX=\"|GRUB_CMDLINE_LINUX=\"${GRUB_ADD} |" /etc/default/grub
  else
    echo "GRUB_CMDLINE_LINUX=\"${GRUB_ADD}\"" >> /etc/default/grub
  fi
  (command -v update-grub >/dev/null && update-grub) || grub2-mkconfig -o /boot/grub2/grub.cfg || true
  echo "   GRUB updated. REBOOT, then verify."
else
  echo
  echo ">> [grub] NOT applied (no --apply-grub). Add this to GRUB_CMDLINE_LINUX and reboot:"
  echo "     ${GRUB_ADD}"
fi

cat <<EOF

>> Verify after reboot:
   cat /sys/devices/system/cpu/isolated              # -> ${CORES}
   cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor   # -> performance
   cat /sys/kernel/mm/transparent_hugepage/enabled    # -> [never]
   chrt -f 80 nproc                                   # SCHED_FIFO works
>> Then run with the cancel-sender pinned to an isolated core, e.g. config:
   "receiver_cpu":2, "parser_cpu":4, "logger_cpu":6, "sender_cpu":<isolated>,
   "sender_priority":80, "lock_memory":true, "sender_park_after_idle_us":0
EOF
echo ">> done."
