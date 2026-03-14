#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

lib_dir=$(dirname "$0")/../../../net
source "$lib_dir"/lib.sh

NSIM_SRV_ID=$((1024 + RANDOM % 1024))
NSIM_CLI_ID=$((2048 + RANDOM % 1024))
NSIM_SYS_LINK=/sys/bus/netdevsim/link_device
SERVER_ADDR=192.0.2.1
CLIENT_ADDR=192.0.2.2
RMEM_PORT=12345
WARM_PORT=12346
RMEM_QUEUED_LEN=65000
RMEM_INFLATED_LEN=65000
RMEM_SMALL_EXTRA=4096
RMEM_LARGE_EXTRA=65536
WARM_WARMUP_ROUNDS=16
WARM_WARMUP_LEN=65000
WARM_QUEUED_LEN=62000
WARM_INFLATED_LEN=65000
WARM_EXTRA=65536

srv_dev=
cli_dev=
srv_pid=
cli_pid=
srv_fd=
cli_fd=
stage_dir=
CASE_BASE_METRIC=
CASE_FINAL_METRIC=

cleanup()
{
	local rc=$?

	if [ -n "${srv_pid:-}" ]; then
		kill "${srv_pid}" 2>/dev/null || true
		wait "${srv_pid}" 2>/dev/null || true
	fi

	if [ -n "${cli_pid:-}" ]; then
		kill "${cli_pid}" 2>/dev/null || true
		wait "${cli_pid}" 2>/dev/null || true
	fi

	if [ -n "${srv_fd:-}" ]; then
		eval "exec ${srv_fd}<&-"
	fi

	if [ -n "${cli_fd:-}" ]; then
		eval "exec ${cli_fd}<&-"
	fi

	if [ -d "${stage_dir:-}" ]; then
		rm -rf "${stage_dir}"
	fi

	cleanup_netdevsim "${NSIM_SRV_ID}" 2>/dev/null || true
	cleanup_netdevsim "${NSIM_CLI_ID}" 2>/dev/null || true
	cleanup_ns "${SRV:-}" "${CLI:-}" 2>/dev/null || true

	exit "${rc}"
}

trap cleanup EXIT

ensure_debugfs()
{
	if mount | grep -q 'on /sys/kernel/debug type debugfs'; then
		return 0
	fi

	if ! mount -t debugfs none /sys/kernel/debug >/dev/null 2>&1; then
		echo "SKIP: failed to mount debugfs"
		exit "${ksft_skip}"
	fi
}

ensure_netdevsim()
{
	if [ -w /sys/bus/netdevsim/new_device ]; then
		return 0
	fi

	if ! modprobe netdevsim >/dev/null 2>&1; then
		echo "SKIP: no netdevsim support"
		exit "${ksft_skip}"
	fi
}

create_nsim()
{
	local id="$1"
	local ns="$2"
	local addr="$3"
	local dev

	echo "${id}" | ip netns exec "${ns}" tee /sys/bus/netdevsim/new_device >/dev/null
	udevadm settle

	dev=$(ip netns exec "${ns}" ls /sys/bus/netdevsim/devices/netdevsim"${id}"/net)
	ip -netns "${ns}" link set dev "${dev}" name "nsim${id}"
	ip -netns "${ns}" addr add "${addr}/24" dev "nsim${id}"
	ip -netns "${ns}" link set dev "nsim${id}" up

	echo "nsim${id}"
}

link_nsim_peers()
{
	local srv_ifindex
	local cli_ifindex

	eval "exec {srv_fd}</var/run/netns/${SRV}"
	eval "exec {cli_fd}</var/run/netns/${CLI}"

	srv_ifindex=$(ip netns exec "${SRV}" cat /sys/class/net/"${srv_dev}"/ifindex)
	cli_ifindex=$(ip netns exec "${CLI}" cat /sys/class/net/"${cli_dev}"/ifindex)

	echo "${srv_fd}:${srv_ifindex} ${cli_fd}:${cli_ifindex}" > "${NSIM_SYS_LINK}"
}

wait_for_file()
{
	local path="$1"
	local i

	for i in $(seq 100); do
		if [ -e "${path}" ]; then
			return 0
		fi
		sleep 0.1
	done

	return 1
}

server_python='
import array
import fcntl
import os
import socket
import struct
import sys
import time

SO_MEMINFO = 55
SK_MEMINFO_RMEM_ALLOC = 0
TCP_MAXSEG = getattr(socket, "TCP_MAXSEG", 2)
FIONREAD = 0x541B
POLL_INTERVAL = 0.01
POLL_TIMEOUT = 20.0

(mode, host, port, warmup_rounds, warmup_len, queued_len, inflated_len,
 ready_file, result_file) = sys.argv[1:]
port = int(port)
warmup_rounds = int(warmup_rounds)
warmup_len = int(warmup_len)
queued_len = int(queued_len)
inflated_len = int(inflated_len)

def queued_bytes(sock):
    buf = array.array("I", [0])
    fcntl.ioctl(sock.fileno(), FIONREAD, buf, True)
    return buf[0]

def wait_for_queued(sock, target):
    deadline = time.time() + POLL_TIMEOUT
    while time.time() < deadline:
        if queued_bytes(sock) >= target:
            return
        time.sleep(POLL_INTERVAL)
    raise SystemExit(f"timed out waiting for {target} queued bytes")

def meminfo(sock):
    raw = sock.getsockopt(socket.SOL_SOCKET, SO_MEMINFO, 9 * 4)
    return struct.unpack("=9I", raw)

def wait_for_growth(sock, idx, base):
    deadline = time.time() + POLL_TIMEOUT
    while time.time() < deadline:
        cur = meminfo(sock)[idx]
        if cur > base:
            return cur
        time.sleep(POLL_INTERVAL)
    raise SystemExit(f"timed out waiting for SO_MEMINFO[{idx}] growth from {base}")

def write_metric(path, value):
    with open(path, "w", encoding="ascii") as fp:
        fp.write(f"{value}\n")

def recv_all(sock, total):
    remaining = total
    while remaining:
        chunk = sock.recv(min(65536, remaining))
        if not chunk:
            raise SystemExit("unexpected EOF while draining receive data")
        remaining -= len(chunk)

listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.setsockopt(socket.IPPROTO_TCP, TCP_MAXSEG, 1000)
listener.bind((host, port))
listener.listen(1)
conn, _ = listener.accept()

for _ in range(warmup_rounds):
    recv_all(conn, warmup_len)

if mode == "rmem_alloc":
    wait_for_queued(conn, queued_len)
    base_metric = meminfo(conn)[SK_MEMINFO_RMEM_ALLOC]
    write_metric(ready_file, base_metric)

    recv_all(conn, queued_len)
    wait_for_queued(conn, inflated_len)
    grown_metric = meminfo(conn)[SK_MEMINFO_RMEM_ALLOC]
    write_metric(result_file, grown_metric)
elif mode == "rmem_alloc_warm":
    wait_for_queued(conn, queued_len)
    base_metric = meminfo(conn)[SK_MEMINFO_RMEM_ALLOC]
    write_metric(ready_file, base_metric)

    wait_for_queued(conn, queued_len + 1)
    grown_metric = wait_for_growth(conn, SK_MEMINFO_RMEM_ALLOC, base_metric)
    write_metric(result_file, grown_metric)
elif mode == "rmem_alloc_growth":
    # The growth cases compare against a live socket metric, so wait for
    # observed growth instead of trusting one instantaneous post-queue sample.
    wait_for_queued(conn, queued_len)
    base_metric = meminfo(conn)[SK_MEMINFO_RMEM_ALLOC]
    write_metric(ready_file, base_metric)

    recv_all(conn, queued_len)
    wait_for_queued(conn, inflated_len)
    grown_metric = wait_for_growth(conn, SK_MEMINFO_RMEM_ALLOC, base_metric)
    write_metric(result_file, grown_metric)
else:
    raise SystemExit(f"unknown mode: {mode}")
'

client_python='
import os
import socket
import sys
import time

POLL_INTERVAL = 0.01
POLL_TIMEOUT = 20.0

host, port, warmup_rounds, warmup_len, queued_len, inflated_len, gate_file = sys.argv[1:]
port = int(port)
warmup_rounds = int(warmup_rounds)
warmup_len = int(warmup_len)
queued_len = int(queued_len)
inflated_len = int(inflated_len)

def send_all(sock, total):
    payload = b"a" * min(total, 65536)
    left = total
    while left:
        chunk = payload[: min(len(payload), left)]
        sent = sock.send(chunk)
        if sent <= 0:
            raise SystemExit("short send")
        left -= sent

def wait_for_file(path):
    deadline = time.time() + POLL_TIMEOUT
    while time.time() < deadline:
        if os.path.exists(path):
            return
        time.sleep(POLL_INTERVAL)
    raise SystemExit(f"timed out waiting for {path}")

cli = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
cli.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, 1000)
cli.connect((host, port))
for _ in range(warmup_rounds):
    send_all(cli, warmup_len)
send_all(cli, queued_len)
wait_for_file(gate_file)
send_all(cli, inflated_len)
cli.close()
'

read_metric()
{
	local path="$1"
	local value

	if ! read -r value < "${path}"; then
		echo "FAIL: unable to read metric from ${path}"
		exit "${ksft_fail}"
	fi

	printf '%s\n' "${value}"
}

run_case()
{
	local case_id="$1"
	local mode="$2"
	local port="$3"
	local warmups="$4"
	local warmup_len="$5"
	local queued_len="$6"
	local inflated_len="$7"
	local extra="$8"
	local label="$9"
	local ready_file="${stage_dir}/${case_id}.ready"
	local result_file="${stage_dir}/${case_id}.result"
	local gate_file="${stage_dir}/${case_id}.gate"

	rm -f "${ready_file}" "${result_file}" "${gate_file}"
	echo 0 > "${dfs_file}"

	ip netns exec "${SRV}" python3 - "${mode}" "${SERVER_ADDR}" "${port}" \
		"${warmups}" "${warmup_len}" "${queued_len}" "${inflated_len}" \
		"${ready_file}" "${result_file}" <<PY &
${server_python}
PY
	srv_pid=$!

	wait_local_port_listen "${SRV}" "${port}" tcp

	ip netns exec "${CLI}" python3 - "${SERVER_ADDR}" "${port}" \
		"${warmups}" "${warmup_len}" "${queued_len}" "${inflated_len}" \
		"${gate_file}" <<PY &
${client_python}
PY
	cli_pid=$!

	if ! wait_for_file "${ready_file}"; then
		echo "FAIL: ${label}: ready marker did not appear"
		exit "${ksft_fail}"
	fi

	echo "${extra}" > "${dfs_file}"
	touch "${gate_file}"

	wait "${cli_pid}"
	cli_pid=
	wait "${srv_pid}"
	srv_pid=

	CASE_BASE_METRIC=$(read_metric "${ready_file}")
	CASE_FINAL_METRIC=$(read_metric "${result_file}")

	echo "PASS: ${label}"
}

# This test only proves that injected truesize reaches socket memory
# accounting. Packetdrill covers the sender-visible rwnd accept/drop logic.

assert_no_growth()
{
	local label="$1"

	if [ "${CASE_FINAL_METRIC}" -gt "${CASE_BASE_METRIC}" ]; then
		echo "FAIL: ${label}: metric grew unexpectedly:" \
		     "base=${CASE_BASE_METRIC}" \
		     "after=${CASE_FINAL_METRIC}"
		exit "${ksft_fail}"
	fi
}

assert_growth()
{
	local label="$1"

	if [ "${CASE_FINAL_METRIC}" -le "${CASE_BASE_METRIC}" ]; then
		echo "FAIL: ${label}: metric did not grow:" \
		     "base=${CASE_BASE_METRIC}" \
		     "after=${CASE_FINAL_METRIC}"
		exit "${ksft_fail}"
	fi
}

ensure_debugfs
ensure_netdevsim
set +u
setup_ns SRV CLI
set -u

srv_dev=$(create_nsim "${NSIM_SRV_ID}" "${SRV}" "${SERVER_ADDR}")
cli_dev=$(create_nsim "${NSIM_CLI_ID}" "${CLI}" "${CLIENT_ADDR}")
link_nsim_peers

ip netns exec "${SRV}" sysctl -wq net.ipv4.tcp_moderate_rcvbuf=0

stage_dir=$(mktemp -d)
dfs_file="/sys/kernel/debug/netdevsim/netdevsim${NSIM_SRV_ID}/ports/0/rx_extra_truesize"

run_case "rmem_noop" "rmem_alloc" "${RMEM_PORT}" 0 0 \
	"${RMEM_QUEUED_LEN}" "${RMEM_INFLATED_LEN}" 0 \
	"peer rx truesize zero no-op"
assert_no_growth "peer rx truesize zero no-op"

run_case "rmem_small" "rmem_alloc_growth" "${RMEM_PORT}" 0 0 \
	"${RMEM_QUEUED_LEN}" "${RMEM_INFLATED_LEN}" "${RMEM_SMALL_EXTRA}" \
	"peer rx truesize small rmem_alloc"
assert_growth "peer rx truesize small rmem_alloc"
small_delta=$((CASE_FINAL_METRIC - CASE_BASE_METRIC))

run_case "rmem_large" "rmem_alloc_growth" "${RMEM_PORT}" 0 0 \
	"${RMEM_QUEUED_LEN}" "${RMEM_INFLATED_LEN}" "${RMEM_LARGE_EXTRA}" \
	"peer rx truesize large rmem_alloc"
assert_growth "peer rx truesize large rmem_alloc"
large_delta=$((CASE_FINAL_METRIC - CASE_BASE_METRIC))

if [ "${large_delta}" -le "${small_delta}" ]; then
	echo "FAIL: peer rx truesize stepped rmem_alloc:" \
	     "small_delta=${small_delta}" \
	     "large_delta=${large_delta}"
	exit "${ksft_fail}"
fi

run_case "rmem_warm" "rmem_alloc_warm" "${WARM_PORT}" "${WARM_WARMUP_ROUNDS}" "${WARM_WARMUP_LEN}" \
	"${WARM_QUEUED_LEN}" "${WARM_INFLATED_LEN}" "${WARM_EXTRA}" \
	"peer rx truesize warm rmem_alloc"
assert_growth "peer rx truesize warm rmem_alloc"
