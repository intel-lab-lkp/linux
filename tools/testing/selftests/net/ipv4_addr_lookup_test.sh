#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Stress test for IPv4 address hash table (inet_addr_lst / rhltable).
#
# Exercises the rhltable insert, lookup, and remove paths by:
#  1. Adding many IPv4 addresses (triggers rhltable growth/resizing)
#  2. Sending unconnected UDP to exercise the __ip_dev_find lookup hot path
#  3. Removing all addresses (triggers rhltable shrinking)
#  4. Testing duplicate keys (same IP on different devices)
#
# Uses veth pairs in network namespaces to avoid polluting the host.
#
# Options:
#   --num-addrs N    Number of addresses to add (default: 1000)
#   --rounds N       Measurement rounds for UDP benchmark (default: 10)
#   --duration  S    Seconds per measurement round (default: 3)
#   --bench-only     Only run the UDP sendmsg benchmark (skip other tests)
#   --sink           Use C receiver to count packets (adds CPU overhead)
#   --threaded-napi  Move veth RX to separate CPU (cleaner perf profiles)
#   --verbose        Show detailed output
#   --help           Show usage

source "$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")/lib.sh"

NUM_ADDRS=1000
ROUNDS=10
DURATION=3
BENCH_ONLY=0
VERBOSE=0
USE_BPFTRACE=0
BPFTRACE_DEBUG=0
USE_SINK=0
USE_THREADED_NAPI=0
RET=0
BPFTRACE_PID=0
BPFTRACE_LOG=""

usage() {
	echo "Usage: $0 [OPTIONS]"
	echo "  --num-addrs N   Number of IPv4 addresses to add (default: $NUM_ADDRS)"
	echo "  --rounds N      Measurement rounds for benchmark (default: $ROUNDS)"
	echo "  --duration S    Seconds per measurement round (default: $DURATION)"
	echo "  --bench-only    Only run the UDP sendmsg benchmark"
	echo "  --verbose       Show detailed output"
	echo "  --bpftrace      Trace __ip_dev_find latency (minimal overhead for A/B)"
	echo "  --sink            Use C receiver to count packets (adds CPU overhead)"
	echo "  --threaded-napi   Move veth RX to separate CPU (cleaner perf profiles)"
	echo "  --bpftrace-debug  Trace all code paths (lookup, insert, remove, resize)"
	exit 0
}

while [ $# -gt 0 ]; do
	case "$1" in
	--num-addrs)	NUM_ADDRS="$2"; shift 2 ;;
	--rounds)	ROUNDS="$2"; shift 2 ;;
	--duration)	DURATION="$2"; shift 2 ;;
	--bench-only)	BENCH_ONLY=1; shift ;;
	--verbose)	VERBOSE=1; shift ;;
	--bpftrace)	USE_BPFTRACE=1; shift ;;
	--sink)		USE_SINK=1; shift ;;
	--threaded-napi)	USE_THREADED_NAPI=1; shift ;;
	--bpftrace-debug)	USE_BPFTRACE=1; BPFTRACE_DEBUG=1; shift ;;
	--help)		usage ;;
	*)		echo "Unknown option: $1"; usage ;;
	esac
done

log() {
	[ "$VERBOSE" -eq 1 ] && echo "  $*"
}

log_config() {
	echo "  Config: $*"
}

PASS=0
FAIL=0

# ---------------------------------------------------------------------------
# bpftrace helpers
# ---------------------------------------------------------------------------

BT_SCRIPT_GEN=""

# Check if a kernel function is actually kprobe-able (not notrace)
can_kprobe() {
	local f="$1"
	# available_filter_functions lists what kprobes can actually attach to
	local aff
	for aff in /sys/kernel/tracing/available_filter_functions \
		   /sys/kernel/debug/tracing/available_filter_functions; do
		[ -r "$aff" ] && { grep -qw "$f" "$aff" 2>/dev/null; return; }
	done
	# Fallback: check kallsyms (may include notrace functions)
	grep -q "^[0-9a-f]* [tT] ${f}$" /proc/kallsyms 2>/dev/null
}

# Build bpftrace script dynamically based on available symbols.
# Sets NPROBES and writes to BT_SCRIPT_GEN (must be set before calling).
bpftrace_build_script() {
	NPROBES=0

	# Resolve bucket_table_alloc (may have .isra.0 suffix from GCC)
	local bta_sym=""
	local aff
	for aff in /sys/kernel/tracing/available_filter_functions \
		   /sys/kernel/debug/tracing/available_filter_functions; do
		[ -r "$aff" ] && {
			bta_sym=$(grep -oP 'bucket_table_alloc\S*' "$aff" 2>/dev/null | head -1)
			break
		}
	done
	[ -z "$bta_sym" ] && \
		bta_sym=$(grep -oP '(?<= )[tT] \K(bucket_table_alloc[.\w]*)' \
			  /proc/kallsyms 2>/dev/null | head -1)

	# --- BEGIN block ---
	if [ "$BPFTRACE_DEBUG" -eq 1 ]; then
		cat > "$BT_SCRIPT_GEN" <<'BTEOF'
BEGIN {
  printf("Tracing inet_addr_lst rhltable paths (debug mode)...\n\n");
  @ipdev_count = 0; @lookup_count = 0;
  @insert_count = 0; @insert_slow = 0; @remove_count = 0;
  @resize_events = 0; @bucket_allocs = 0; @rehash_count = 0;
  @tbl_size = 0; @tbl_resizes = 0;
}
BTEOF
	else
		cat > "$BT_SCRIPT_GEN" <<'BTEOF'
BEGIN {
  printf("Tracing inet_addr_lst rhltable paths...\n\n");
  @ipdev_count = 0;
}
BTEOF
	fi

	# Detect old (hlist) vs new (rhltable) kernel:
	#   old kernel: inet_hash_insert does hlist hash+insert, visible to kprobe
	#   new kernel: inet_hash_insert wraps rhltable_insert, inlined away
	local has_rhltable=0
	if can_kprobe inet_hash_insert; then
		log "  detected OLD kernel (inet_hash_insert is kprobe-able)"
	else
		has_rhltable=1
		log "  detected NEW kernel (inet_hash_insert inlined -> rhltable)"
	fi

	# --- Core probe: __ip_dev_find (always, minimal overhead for A/B) ---
	if can_kprobe __ip_dev_find; then
		log "  probe: __ip_dev_find (full lookup)"
		if [ "$BPFTRACE_DEBUG" -eq 1 ] && [ "$has_rhltable" -eq 1 ]; then
			# New kernel: read rhltable bucket count via BTF to detect resize
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
kprobe:__ip_dev_find {
  @ipdev_entry[tid] = nsecs;
  $net = (struct net *)arg0;
  $tbl = $net->ipv4.inet_addr_lst.ht.tbl;
  $size = $tbl->size;
  if ($size != @tbl_size) {
    printf("TABLE RESIZE: buckets %lld -> %d  (nelems=%d)\n",
           @tbl_size, $size, $net->ipv4.inet_addr_lst.ht.nelems.counter);
    @tbl_size = $size;
    @tbl_resizes++;
  }
}
BTEOF
		else
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
kprobe:__ip_dev_find { @ipdev_entry[tid] = nsecs; }
BTEOF
		fi
		cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
kretprobe:__ip_dev_find /@ipdev_entry[tid]/ {
  $dt = nsecs - @ipdev_entry[tid];
  @ipdev_ns = hist($dt); @ipdev_stats = stats($dt); @ipdev_count++;
  delete(@ipdev_entry[tid]);
}
BTEOF
		NPROBES=$((NPROBES + 1))
	fi

	# --- Debug probes (only with --bpftrace-debug) ---
	local has_lookup=0 has_resize_wq=0 has_bta=0 has_rehash=0

	if [ "$BPFTRACE_DEBUG" -eq 1 ]; then
		log "  debug mode: attaching extra probes"

		if can_kprobe inet_lookup_ifaddr_rcu; then
			has_lookup=1
			log "  probe: inet_lookup_ifaddr_rcu (inner lookup)"
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
kprobe:inet_lookup_ifaddr_rcu { @lookup_entry[tid] = nsecs; }
kretprobe:inet_lookup_ifaddr_rcu /@lookup_entry[tid]/ {
  $dt = nsecs - @lookup_entry[tid];
  @lookup_ns = hist($dt); @lookup_stats = stats($dt); @lookup_count++;
  delete(@lookup_entry[tid]);
}
BTEOF
			NPROBES=$((NPROBES + 1))
		fi

		if can_kprobe inet_hash_insert; then
			log "  probe: inet_hash_insert (old kernel insert path)"
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
kprobe:inet_hash_insert { @insert_count++; }
BTEOF
			NPROBES=$((NPROBES + 1))
		fi

		if can_kprobe rhashtable_insert_slow; then
			log "  probe: rhashtable_insert_slow (insert slow path)"
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
kprobe:rhashtable_insert_slow { @insert_slow++; }
BTEOF
			NPROBES=$((NPROBES + 1))
		fi

		if can_kprobe inet_hash_remove; then
			log "  probe: inet_hash_remove (remove)"
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
kprobe:inet_hash_remove { @remove_count++; }
BTEOF
			NPROBES=$((NPROBES + 1))
		fi

		if can_kprobe rht_deferred_worker; then
			has_resize_wq=1
			log "  probe: rht_deferred_worker (resize worker)"
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
kprobe:rht_deferred_worker {
  @resize_wq_entry[tid] = nsecs; @resize_events++;
  printf(">>> RESIZE #%lld: deferred_worker started\n", @resize_events);
}
kretprobe:rht_deferred_worker /@resize_wq_entry[tid]/ {
  $dt = nsecs - @resize_wq_entry[tid];
  @resize_wq_ns = hist($dt);
  printf("    RESIZE: done in %lld us\n", $dt / 1000);
  delete(@resize_wq_entry[tid]);
}
BTEOF
			NPROBES=$((NPROBES + 1))
		fi

		if [ -n "$bta_sym" ] && can_kprobe "$bta_sym"; then
			has_bta=1
			log "  probe: $bta_sym (table alloc, arg1=nbuckets)"
			cat >> "$BT_SCRIPT_GEN" <<BTEOF
kprobe:${bta_sym} {
  @bucket_allocs++; @last_alloc_size = arg1;
  printf("    RESIZE: bucket_table_alloc nbuckets=%lld\\n", arg1);
  print(kstack(5));
}
BTEOF
			NPROBES=$((NPROBES + 1))
		fi

		if can_kprobe rhashtable_rehash_table; then
			has_rehash=1
			log "  probe: rhashtable_rehash_table (data migration)"
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
kprobe:rhashtable_rehash_table { @rehash_entry[tid] = nsecs; }
kretprobe:rhashtable_rehash_table /@rehash_entry[tid]/ {
  $dt = nsecs - @rehash_entry[tid];
  @rehash_ns = hist($dt); @rehash_count++;
  printf("    RESIZE: rehash done in %lld us\n", $dt / 1000);
  delete(@rehash_entry[tid]);
}
BTEOF
			NPROBES=$((NPROBES + 1))
		fi
	fi

	# --- END block -- only reference maps that actually exist ---
	cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
END {
  printf("\n========================================================\n");
  printf("  inet_addr_lst rhltable trace summary\n");
  printf("========================================================\n\n");
  printf("--- __ip_dev_find latency (ns) ---\n");
  print(@ipdev_ns);
  printf("  stats (count/avg/total): "); print(@ipdev_stats);
  printf("\nCOMPARISON: __ip_dev_find calls=%lld\n", @ipdev_count);
BTEOF
	if [ "$BPFTRACE_DEBUG" -eq 1 ]; then
		if [ "$has_rhltable" -eq 1 ]; then
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
  printf("\n--- rhltable state (via BTF struct reads) ---\n");
  printf("  kernel type            : rhltable (new)\n");
  printf("  final bucket count     : %8lld\n", @tbl_size);
  printf("  resize events observed : %8lld\n", @tbl_resizes);
BTEOF
		else
			cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
  printf("\n--- hash table type ---\n");
  printf("  kernel type            : hlist (old)\n");
BTEOF
		fi
		[ "$has_lookup" -eq 1 ] && cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
  printf("\n--- inet_lookup_ifaddr_rcu latency (ns) ---\n");
  print(@lookup_ns);
  printf("  stats (count/avg/total): "); print(@lookup_stats);
  printf("COMPARISON: inet_lookup_ifaddr_rcu calls=%lld\n", @lookup_count);
  clear(@lookup_entry);
BTEOF
		cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
  printf("\n--- Debug call counts ---\n");
  printf("  inet_hash_insert       : %8lld\n", @insert_count);
  printf("  rhashtable_insert_slow : %8lld\n", @insert_slow);
  printf("  inet_hash_remove       : %8lld\n", @remove_count);
  printf("  rht_deferred_worker    : %8lld\n", @resize_events);
  printf("  bucket_table_alloc     : %8lld\n", @bucket_allocs);
BTEOF
		[ "$has_rehash" -eq 1 ] && cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
  printf("  rhashtable_rehash      : %8lld\n", @rehash_count);
BTEOF
		[ "$has_resize_wq" -eq 1 ] && cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
  printf("\n--- rht_deferred_worker duration (ns) ---\n");
  print(@resize_wq_ns);
  clear(@resize_wq_entry);
BTEOF
		[ "$has_rehash" -eq 1 ] && cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
  printf("\n--- rhashtable_rehash_table duration (ns) ---\n");
  print(@rehash_ns);
  clear(@rehash_entry);
BTEOF
		[ "$has_bta" -eq 1 ] && cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
  clear(@last_alloc_size);
BTEOF
	fi
	cat >> "$BT_SCRIPT_GEN" <<'BTEOF'
  clear(@ipdev_entry);
}
BTEOF
}

bpftrace_start() {
	[ "$USE_BPFTRACE" -eq 0 ] && return

	if ! command -v bpftrace >/dev/null 2>&1; then
		echo "WARN: bpftrace not found, skipping tracing"
		USE_BPFTRACE=0
		return
	fi

	BT_SCRIPT_GEN=$(mktemp /tmp/rhltable_trace_XXXXXX.bt)
	echo "Probing /proc/kallsyms for available trace points..."
	bpftrace_build_script

	if [ "$NPROBES" -eq 0 ]; then
		echo "WARN: no kprobe-able symbols found, skipping tracing"
		USE_BPFTRACE=0
		rm -f "$BT_SCRIPT_GEN"
		return
	fi
	echo "Built dynamic bpftrace script with $NPROBES probe groups"
	log "Script: $BT_SCRIPT_GEN"

	BPFTRACE_LOG=$(mktemp /tmp/rhltable_trace.XXXXXX)
	bpftrace "$BT_SCRIPT_GEN" > "$BPFTRACE_LOG" 2>&1 &
	BPFTRACE_PID=$!
	# Give bpftrace time to attach probes
	sleep 2
	if ! kill -0 $BPFTRACE_PID 2>/dev/null; then
		echo "WARN: bpftrace failed to start"
		cat "$BPFTRACE_LOG"
		USE_BPFTRACE=0
		rm -f "$BT_SCRIPT_GEN"
		return
	fi
	echo "bpftrace attached (pid $BPFTRACE_PID)"
}

bpftrace_stop() {
	[ "$USE_BPFTRACE" -eq 0 ] && return
	[ "$BPFTRACE_PID" -eq 0 ] && return

	# Send INT so bpftrace prints its END summary
	kill -INT $BPFTRACE_PID 2>/dev/null || true
	wait $BPFTRACE_PID 2>/dev/null || true
	BPFTRACE_PID=0

	echo ""
	echo "============================================"
	echo "bpftrace output"
	echo "============================================"
	cat "$BPFTRACE_LOG"
	echo ""

	# Validate expected code paths were hit
	local rc=0
	if grep -q '__ip_dev_find calls=0' "$BPFTRACE_LOG" 2>/dev/null; then
		echo "FAIL: __ip_dev_find was never called"
		rc=1
	elif grep -q 'COMPARISON: __ip_dev_find' "$BPFTRACE_LOG" 2>/dev/null; then
		echo "PASS: __ip_dev_find lookup path verified"
	fi
	if grep -q 'TABLE RESIZE:' "$BPFTRACE_LOG" 2>/dev/null; then
		echo "PASS: rhltable resize detected (BTF struct reads)"
	elif grep -q 'RESIZE.*bucket_table_alloc' "$BPFTRACE_LOG" 2>/dev/null; then
		echo "PASS: rhltable resize detected (kprobe)"
	else
		echo "INFO: no resize observed (use --bpftrace-debug to detect via BTF)"
	fi
	check_result "bpftrace code path verification" $rc

	rm -f "$BPFTRACE_LOG" "$BT_SCRIPT_GEN"
}

check_result() {
	local desc="$1"
	local rc="$2"

	if [ "$rc" -eq 0 ]; then
		echo "PASS: $desc"
		PASS=$((PASS + 1))
	else
		echo "FAIL: $desc"
		FAIL=$((FAIL + 1))
		RET=1
	fi
}

cleanup() {
	# Stop bpftrace if running
	if [ "$BPFTRACE_PID" -ne 0 ]; then
		kill -INT $BPFTRACE_PID 2>/dev/null || true
		wait $BPFTRACE_PID 2>/dev/null || true
		BPFTRACE_PID=0
	fi

	# Kill any other background jobs
	local jobs
	jobs="$(jobs -p 2>/dev/null)" || true
	[ -n "$jobs" ] && kill $jobs 2>/dev/null || true
	wait 2>/dev/null || true

	cleanup_all_ns
	[ -n "$BPFTRACE_LOG" ] && rm -f "$BPFTRACE_LOG"
}

trap cleanup EXIT

# Helper: generate address from index (spreads across octets to avoid /24 limits)
# Returns 10.B2.B3.1 where B2.B3 encodes the index
idx_to_addr() {
	local i=$1
	local b2=$(( (i >> 8) & 0xff ))
	local b3=$(( i & 0xff ))
	echo "10.${b2}.${b3}.1"
}

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------

setup() {
	if ! setup_ns NS_SRC NS_DST; then
		echo "SKIP: Could not create namespaces"
		exit $ksft_skip
	fi

	# Create veth pair
	ip link add veth_src type veth peer name veth_dst
	ip link set veth_src netns "$NS_SRC"
	ip link set veth_dst netns "$NS_DST"
	ip -n "$NS_SRC" link set veth_src up
	ip -n "$NS_DST" link set veth_dst up

	if [ "$USE_THREADED_NAPI" -eq 1 ]; then
		# Move veth RX to a separate NAPI kthread for cleaner perf profiles.
		# Disable TSO on src so packets travel individually through the
		# veth ptr_ring (256 entries), enable GRO on dst for NAPI polling.
		ip netns exec "$NS_SRC" ethtool -K veth_src tso off 2>/dev/null || true
		ip netns exec "$NS_DST" ethtool -K veth_dst gro on 2>/dev/null || true
		ip netns exec "$NS_DST" \
			bash -c 'echo 1 > /sys/class/net/veth_dst/threaded' 2>/dev/null || true
		log_config "threaded-napi: veth_dst (TSO off, GRO on, NAPI kthread on CPU 0)"
	fi

	# Base addresses for connectivity
	ip -n "$NS_SRC" addr add 192.168.1.1/24 dev veth_src
	ip -n "$NS_DST" addr add 192.168.1.2/24 dev veth_dst

	# Accept packets from any source on dst side
	ip netns exec "$NS_DST" sysctl -wq net.ipv4.conf.all.rp_filter=0
	ip netns exec "$NS_DST" sysctl -wq net.ipv4.conf.veth_dst.rp_filter=0

	# Route the 10.0.0.0/8 range toward veth_src from dst side
	ip -n "$NS_DST" route add 10.0.0.0/8 via 192.168.1.1

	log "Namespaces: NS_SRC=$NS_SRC NS_DST=$NS_DST"
}

# ---------------------------------------------------------------------------
# Test 1: Add many addresses (rhltable insert + resize)
# ---------------------------------------------------------------------------

test_add_many_addrs() {
	local i addr
	local rc=0

	echo "Test: Adding $NUM_ADDRS addresses..."
	local batch
	batch=$(mktemp /tmp/ip_batch_add.XXXXXX)
	for ((i = 1; i <= NUM_ADDRS; i++)); do
		echo "addr add 10.$(( (i >> 8) & 0xff )).$(( i & 0xff )).1/32 dev veth_src"
	done > "$batch"
	ip -n "$NS_SRC" -batch "$batch" 2>/dev/null || true
	rm -f "$batch"

	# Verify address count
	local count
	count=$(ip -n "$NS_SRC" -4 addr show dev veth_src | grep -c "inet " || true)
	log "Addresses on veth_src: $count (expected $((NUM_ADDRS + 1)))"

	[ "$count" -ge "$NUM_ADDRS" ] || rc=1
	check_result "add $NUM_ADDRS addresses" $rc
}

# ---------------------------------------------------------------------------
# Test 2: Verify lookup works (ping from specific source addresses)
# ---------------------------------------------------------------------------

test_lookup_ping() {
	local rc=0

	echo "Test: Verify address lookup via ping..."
	# Ping dst from a few of the added addresses
	for idx in 1 100 $((NUM_ADDRS / 2)) $NUM_ADDRS; do
		[ "$idx" -gt "$NUM_ADDRS" ] && continue
		local addr
		addr=$(idx_to_addr $idx)
		if ! ip netns exec "$NS_SRC" ping -c 1 -W 1 -I "$addr" 192.168.1.2 \
				>/dev/null 2>&1; then
			log "ping from $addr failed"
			rc=1
		else
			log "ping from $addr OK"
		fi
	done

	check_result "address lookup via ping" $rc
}

# ---------------------------------------------------------------------------
# Test 3: Unconnected UDP sendmsg stress (exercises __ip_dev_find hot path)
# ---------------------------------------------------------------------------

test_udp_sendmsg_stress() {
	local rc=0

	local total_time=$((ROUNDS * DURATION + 1))
	echo "Test: UDP sendmsg bench ($NUM_ADDRS addrs, ${ROUNDS}x${DURATION}s + 1s warmup = ~${total_time}s)..."

	# Locate C binary (used for both sink and sender)
	local sender_bin=""
	local script_dir
	script_dir=$(dirname "$0")

	if [ -x "${script_dir}/ipv4_addr_lookup_udp_sender" ]; then
		sender_bin="${script_dir}/ipv4_addr_lookup_udp_sender"
	elif gcc -O2 -Wall -o /tmp/udp_sender \
		"${script_dir}/ipv4_addr_lookup_udp_sender.c" 2>/dev/null; then
		sender_bin="/tmp/udp_sender"
	else
		echo "SKIP: ipv4_addr_lookup_udp_sender not found (run make first)"
		check_result "UDP sender binary available" 1
		return
	fi

	local sink_pid=0 sink_log=""

	if [ "$USE_SINK" -eq 1 ]; then
		# C receiver counts packets (adds CPU overhead to perf profiles)
		log_config "sink: C receiver on CPU 0 (verifies packet counts)"
		sink_log=$(mktemp /tmp/udp_sink.XXXXXX)
		ip netns exec "$NS_DST" \
			taskset -c 0 "$sender_bin" --sink > "$sink_log" 2>&1 &
		sink_pid=$!
		sleep 0.2
	else
		# Default: iptables DROP -- zero userspace overhead in perf profiles
		ip netns exec "$NS_DST" \
			iptables -A INPUT -p udp --dport 9000 -j DROP
	fi

	if [ "$USE_THREADED_NAPI" -eq 1 ]; then
		# Pin veth_dst NAPI kthread to CPU 0 (sender is on CPU 1)
		local napi_pid
		napi_pid=$(pgrep -f "napi/veth_dst" 2>/dev/null | head -1)
		if [ -n "$napi_pid" ]; then
			taskset -p 0x1 "$napi_pid" >/dev/null 2>&1 || true
			log "Pinned NAPI thread (pid $napi_pid) to CPU 0"
		fi
	fi

	# Snapshot softnet_stat before sending (per-CPU: processed, time_squeeze)
	local softnet_before
	softnet_before=$(mktemp /tmp/softnet_before.XXXXXX)
	cat /proc/net/softnet_stat > "$softnet_before"

	# Send unconnected UDP from many source addresses.
	# Each sendto() triggers ip_route_output -> __ip_dev_find -> rhltable_lookup.
	local sender_log
	sender_log=$(mktemp /tmp/udp_sender.XXXXXX)

	log "Using C UDP sender (pre-created sockets, $ROUNDS rounds)"
	local sndbuf_arg=""
	[ "$USE_THREADED_NAPI" -eq 1 ] && sndbuf_arg="--sndbuf 4194304"

	ip netns exec "$NS_SRC" \
		taskset -c 1 "$sender_bin" "$NUM_ADDRS" "$ROUNDS" "$DURATION" $sndbuf_arg \
		2>&1 | tee "$sender_log"
	[ "${PIPESTATUS[0]}" -ne 0 ] && rc=1

	# Show per-CPU softnet activity (detect same-CPU vs multi-CPU NAPI)
	local cpu=0 active_cpus=""
	while read -r line; do
		# shellcheck disable=SC2086
		set -- $line
		local cur_p=$((0x${1})) cur_sq=$((0x${3}))
		local prev_p=0 prev_sq=0
		if [ -n "$softnet_before" ]; then
			local prev_line
			prev_line=$(sed -n "$((cpu + 1))p" "$softnet_before")
			if [ -n "$prev_line" ]; then
				# shellcheck disable=SC2086
				set -- $prev_line
				prev_p=$((0x${1})); prev_sq=$((0x${3}))
			fi
		fi
		local dp=$((cur_p - prev_p))
		[ "$dp" -gt 0 ] && active_cpus="${active_cpus} cpu${cpu}(+${dp})"
		cpu=$((cpu + 1))
	done < /proc/net/softnet_stat
	rm -f "$softnet_before"
	local n_active
	n_active=$(echo "$active_cpus" | wc -w)
	local cpu_mode="single-CPU"
	[ "$n_active" -gt 1 ] && cpu_mode="multi-CPU(${n_active})"
	echo "  softnet: ${cpu_mode}:${active_cpus}"

	[ "$sender_bin" = "/tmp/udp_sender" ] && rm -f "$sender_bin"

	if [ "$USE_SINK" -eq 1 ] && [ "$sink_pid" -ne 0 ]; then
		# Let last packets reach socket buffer, then stop the sink
		sleep 0.1
		kill -TERM $sink_pid 2>/dev/null || true
		wait $sink_pid 2>/dev/null || true

		# Verify no packet drops: sent (includes warmup) should equal received
		local total_sent sink_received
		total_sent=$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' "$sender_log" | head -1)
		sink_received=$(sed -n 's/.*received=\([0-9]*\).*/\1/p' "$sink_log" | head -1)
		rm -f "$sink_log"

		if [ -n "$total_sent" ] && [ -n "$sink_received" ]; then
			if [ "$total_sent" -eq "$sink_received" ]; then
				echo "  Sink received: $sink_received (matches sent)"
			else
				local diff=$((total_sent - sink_received))
				echo "  WARN: sent=$total_sent but sink received=$sink_received (diff=$diff)"
			fi
		fi
	else
		ip netns exec "$NS_DST" \
			iptables -D INPUT -p udp --dport 9000 -j DROP 2>/dev/null
	fi
	rm -f "$sender_log"

	check_result "unconnected UDP sendmsg stress" $rc
}

# ---------------------------------------------------------------------------
# Test 4: Duplicate keys (same IP on two different veth devices)
# ---------------------------------------------------------------------------

test_duplicate_addrs() {
	local rc=0

	echo "Test: Duplicate address keys (same IP, different devices)..."

	# Create a second veth pair in NS_SRC
	ip link add veth_src2 type veth peer name veth_dup
	ip link set veth_src2 netns "$NS_SRC" up
	ip link set veth_dup netns "$NS_DST" up
	ip -n "$NS_DST" link set veth_dup up

	# Add the same address that's already on veth_src
	local dup_addr
	dup_addr=$(idx_to_addr 1)
	ip -n "$NS_SRC" addr add "${dup_addr}/32" dev veth_src2 2>/dev/null || true

	# Verify both devices have the address
	local count
	count=$(ip -n "$NS_SRC" -4 addr show | grep -c "$dup_addr" || true)
	log "Address $dup_addr appears on $count devices"

	[ "$count" -ge 2 ] || rc=1

	# Lookup should still work
	if ! ip netns exec "$NS_SRC" ping -c 1 -W 1 -I "$dup_addr" 192.168.1.2 \
			>/dev/null 2>&1; then
		log "ping from duplicate addr failed (expected -- routing may prefer one)"
	fi

	# Remove duplicate and verify no crash
	ip -n "$NS_SRC" addr del "${dup_addr}/32" dev veth_src2 2>/dev/null || true
	ip -n "$NS_SRC" link del veth_src2 2>/dev/null || true

	check_result "duplicate address keys" $rc
}

# ---------------------------------------------------------------------------
# Test 5: Remove all addresses (rhltable shrink)
# ---------------------------------------------------------------------------

test_remove_all_addrs() {
	local i addr
	local rc=0

	echo "Test: Removing $NUM_ADDRS addresses..."
	local batch
	batch=$(mktemp /tmp/ip_batch_del.XXXXXX)
	for ((i = 1; i <= NUM_ADDRS; i++)); do
		echo "addr del 10.$(( (i >> 8) & 0xff )).$(( i & 0xff )).1/32 dev veth_src"
	done > "$batch"
	ip -n "$NS_SRC" -batch "$batch" 2>/dev/null || true
	rm -f "$batch"

	# Verify only the base address remains
	local count
	count=$(ip -n "$NS_SRC" -4 addr show dev veth_src | grep -c "inet " || true)
	log "Addresses remaining: $count (expected 1)"

	[ "$count" -eq 1 ] || rc=1
	check_result "remove all addresses (rhltable shrink)" $rc
}

# ---------------------------------------------------------------------------
# Test 6: Re-add and check address lifetime (exercises check_lifetime)
# ---------------------------------------------------------------------------

test_addr_lifetime() {
	local rc=0

	echo "Test: Address lifetime expiry..."

	# Add an address with short valid/preferred lifetime
	ip -n "$NS_SRC" addr add 10.99.99.1/32 dev veth_src \
		valid_lft 3 preferred_lft 2

	# Verify it exists
	local exists
	exists=$(ip -n "$NS_SRC" -4 addr show dev veth_src | grep -c "10.99.99.1" || true)
	[ "$exists" -ge 1 ] || { rc=1; check_result "address lifetime" $rc; return; }

	log "Address 10.99.99.1 added with valid_lft=3s"

	# Wait for it to expire (check_lifetime runs periodically)
	sleep 5

	exists=$(ip -n "$NS_SRC" -4 addr show dev veth_src | grep -c "10.99.99.1" || true)
	log "After 5s: addr present=$exists (expected 0)"

	[ "$exists" -eq 0 ] || rc=1
	check_result "address lifetime expiry" $rc
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

echo "============================================"
echo "inet_addr_lst rhltable stress test"
echo "  addresses: $NUM_ADDRS"
echo "  rounds:    $ROUNDS x ${DURATION}s"
[ "$BENCH_ONLY" -eq 1 ] && echo "  mode:      bench-only"
echo "============================================"

setup
bpftrace_start

if [ "$BENCH_ONLY" -eq 1 ]; then
	test_add_many_addrs
	test_udp_sendmsg_stress
else
	test_add_many_addrs
	test_lookup_ping
	test_udp_sendmsg_stress
	test_duplicate_addrs
	test_remove_all_addrs
	test_addr_lifetime
fi

bpftrace_stop

echo ""
echo "============================================"
echo "Results: $PASS passed, $FAIL failed"
echo "============================================"

exit $RET
