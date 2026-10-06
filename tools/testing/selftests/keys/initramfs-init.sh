#!/bin/busybox sh
# SPDX-License-Identifier: GPL-2.0
# shellcheck shell=dash

/bin/busybox mount -t proc procfs /proc
/keys/persistent_keyring_expiry
status=$?
/bin/busybox echo "keyring expiry selftest exit status: $status"
/bin/busybox poweroff -f
exit "$status"
