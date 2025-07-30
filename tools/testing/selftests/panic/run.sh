# tools/testing/selftests/panic/run.sh

#!/bin/sh
set -e

MOD_NAME="panic_trigger_test.ko"
LOG_FILE="panic_log.txt"

echo "[*] Clearing dmesg..."
dmesg -c

echo "[*] Inserting module: $MOD_NAME"
insmod ./$MOD_NAME

echo "[*] Capturing dmesg..."
dmesg > "$LOG_FILE"

