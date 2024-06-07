#! /bin/bash
# SPDX-License-Identifier: GPL-2.0

set -e
set -u

LOG_DIR=/tmp
PY_CMD="/usr/bin/python3"
while getopts "d:p:" opt; do
  case ${opt} in
    d)
      LOG_DIR=${OPTARG}
      ;;
    p)
      PY_CMD=${OPTARG}
      ;;
    :)
      echo "USAGE: init.sh [-d logdir] [-p python_cmd]"
      exit 1
      ;;
    ?)
      echo "Invalid option: -${OPTARG}."
      exit 1
      ;;
  esac
done

LOG_FILE=$LOG_DIR/rds-strace.txt

mount -t proc none /proc
mount -t sysfs none /sys
mount -t tmpfs none /var/run
mount -t debugfs none /sys/kernel/debug

echo running RDS tests...
echo Traces will be logged to $LOG_FILE
rm -f $LOG_FILE
strace -T -tt -o "$LOG_FILE" $PY_CMD $(dirname "$0")/test.py -d "$LOG_DIR" || true

echo saving coverage data...
(set +x; cd /sys/kernel/debug/gcov; find * -name '*.gcda' | \
while read f
do
	cat < /sys/kernel/debug/gcov/$f > /$f
done)

dmesg > $LOG_DIR/dmesg.out

/usr/sbin/poweroff --no-wtmp --force
