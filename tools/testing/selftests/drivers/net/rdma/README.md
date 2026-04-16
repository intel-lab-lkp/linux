# RDMA CM Selftests Usage Guide

These scripts provide baseline observability and regression checks for RDMA/CM
paths under the Linux `kselftest` framework.

Files:

- `rdma_cm_baseline.sh`
- `rdma_cm_trace_sequence.sh`
- `rdma_cm_counter_delta.sh`
- `rdma_cm_fault_injection.sh`
- `rdma_cm_review_loop.sh`
- `rdma_common.sh`

The scripts use a fixed test flow and only require workload commands from
environment variables.

## 1. Use Cases

- CM main-flow observability checks (REQ/REP/RTU)
- CM counter delta validation
- Recovery validation after fault injection (`failslab`)
- One-shot serial regression run

## 2. Requirements

- root privileges
- Reachable client/server network path
- Perftest command available on the remote side (default: `ib_send_bw -R`)
- For fault injection: kernel support for `failslab` and access to
  `/sys/kernel/debug/failslab`

## 3. Recommended Execution Order

```bash
./rdma_cm_baseline.sh
./rdma_cm_trace_sequence.sh
./rdma_cm_counter_delta.sh
./rdma_cm_fault_injection.sh
```

Or run all in sequence:

```bash
./rdma_cm_review_loop.sh
```

## 4. Quick Start (Two Hosts)

### 4.1 Server side (recommended: loop and keep listening)

```bash
while true; do
  ib_send_bw -d <server_ibdev> -i <server_port> -R
  sleep 1
done
```

### 4.2 Client side (set workload command)

```bash
export CM_WORKLOAD_CMD='ib_send_bw -d rocep1s0f0 -i 1 -R -g 3 192.168.1.22'
export CM_VALIDATE_RECOVERY_CMD="${CM_WORKLOAD_CMD}"

./rdma_cm_review_loop.sh
```

### 4.3 Run through kselftest harness

```bash
sudo -E make -C tools/testing/selftests TARGETS=drivers/net/rdma run_tests
```

`sudo -E` keeps exported workload variables for test scripts.

### 4.4 Run a single script directly

```bash
cd tools/testing/selftests/drivers/net/rdma
sudo -E ./rdma_cm_counter_delta.sh
```

## 5. Configuration Parameters

Only workload command variables are supported:

- `CM_WORKLOAD_CMD`: required; workload command used by trace/counter/fault tests
- `CM_VALIDATE_RECOVERY_CMD`: optional; command for recovery stage in fault
  injection test (falls back to `CM_WORKLOAD_CMD`)

Fixed internal settings:

- Counter pre-wait: `2s`
- Recovery pre-wait: `2s`
- Failslab path: `/sys/kernel/debug/failslab`
- Failslab knobs: `task-filter=1`, `probability=1`, `interval=100`, `times=1`
- Counter limits: `cm_rx_duplicates.* <= 10`, `cm_tx_retries.* <= 10`
- Trace log path: `/tmp/rdma_cm_trace.<timestamp>.log`

## 6. Exit Codes

- `0`: pass
- `4`: skip (environment not ready, e.g. missing tracefs/failslab/counters)
- other non-zero: fail

## 7. Result Interpretation

When running with kselftest (`make ... run_tests`), TAP output looks like:

```text
ok 1 selftests: drivers/net/rdma: rdma_cm_baseline.sh
ok 2 selftests: drivers/net/rdma: rdma_cm_trace_sequence.sh
not ok 3 selftests: drivers/net/rdma: rdma_cm_counter_delta.sh # exit=1
```

- `ok N ...`: that script passed
- `not ok N ... # exit=1`: that script failed
- `not ok N ... # exit=4`: that script was skipped by environment checks

When running `rdma_cm_review_loop.sh` directly, check the final summary block:

```text
==== summary ====
baseline=0
trace=0
counters=1
fault_injection=0
```

Each value is the corresponding script return code.

## 8. Common Issues

### 8.1 `cm counters are unavailable under /sys/class/infiniband`

The script did not find `cm_tx_msgs` (and related) counters. Check:

- whether `cm_tx_msgs` exists under any available RDMA port path

### 8.2 `missing CM send trace events (req/rep/rtu)`

This usually means workload did not create a CM handshake. Verify
`CM_WORKLOAD_CMD` and remote server readiness.

### 8.3 `Unexpected CM event ... 8`

Usually means the server was not ready for the next connection. Try:

- keep server in a listening loop
- ensure the remote server is still listening before the recovery stage

### 8.4 `failslab is unavailable`

Expected skip when failslab is not exposed by kernel/debugfs. Check:

```bash
mount | grep debugfs
ls /sys/kernel/debug/failslab
```

## 9. Minimal Regression Profile

```bash
export CM_WORKLOAD_CMD='ib_send_bw -d <client_ibdev> -i 1 -R -g <gid_idx> <server_ip>'
export CM_VALIDATE_RECOVERY_CMD="${CM_WORKLOAD_CMD}"

./rdma_cm_review_loop.sh
```
