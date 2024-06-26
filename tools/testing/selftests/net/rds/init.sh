#! /bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e
set -u

COLLECT_GCOV=0
LOG_DIR=/tmp
PY_CMD="/usr/bin/python3"
PLOSS=0
PCORRUPT=0
PDUP=0
while getopts "d:p:l:c:u:g" opt; do
  case ${opt} in
    d)
      LOG_DIR=${OPTARG}
      ;;
    p)
      PY_CMD=${OPTARG}
      ;;
    l)
      PLOSS=${OPTARG}
      ;;
    c)
      PCORRUPT=${OPTARG}
      ;;
    u)
      PDUP=${OPTARG}
      ;;
    g)
      COLLECT_GCOV=1
      ;;
    :)
      echo "USAGE: init.sh [-d logdir] [-p python_cmd] [-l packet_loss] [-c packet_corruption] " \
	   "[-u packet_duplcate] [-g]"
      exit 1
      ;;
    ?)
      echo "Invalid option: -${OPTARG}."
      exit 1
      ;;
  esac
done

LOG_FILE="${LOG_DIR}/rds-strace.txt"

mount -t proc none /proc
mount -t sysfs none /sys
mount -t tmpfs none /var/run
mount -t debugfs none /sys/kernel/debug

echo running RDS tests...
echo Traces will be logged to "$LOG_FILE"
rm -f "$LOG_FILE"
strace -T -tt -o "$LOG_FILE" "$PY_CMD" "$(dirname "$0")/test.py" --timeout 300 -d "$LOG_DIR" \
	-l "$PLOSS" -c "$PCORRUPT" -u "$PDUP" || true

if [ "$COLLECT_GCOV" -eq 1 ]; then
	echo saving coverage data...
	(set +x; cd /sys/kernel/debug/gcov; find ./* -name '*.gcda' | \
	while read -r f
	do
		cat < "/sys/kernel/debug/gcov/$f" > "/$f"
	done)
fi

dmesg > "${LOG_DIR}/dmesg.out"

/usr/sbin/poweroff --no-wtmp --force
