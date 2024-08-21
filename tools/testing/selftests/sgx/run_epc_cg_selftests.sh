#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-2.0
# Copyright(c) 2023, 2024 Intel Corporation.

PROCESS_SUCCESS=1
PROCESS_FAILURE=0
# Wait for a process and check for expected exit status.
#
# Arguments:
#	$1 - the pid of the process to wait and check.
#	$2 - 1 if expecting success, 0 for failure.
#
# Return:
#	0 if the exit status of the process matches the expectation.
#	1 otherwise.
wait_check_process_status() {
    pid=$1
    check_for_success=$2

    wait "$pid"
    status=$?

    if [ $check_for_success -eq $PROCESS_SUCCESS ] && [ $status -eq 0 ]; then
        echo "# Process $pid succeeded."
        return 0
    elif [ $check_for_success -eq $PROCESS_FAILURE ] && [ $status -ne 0 ]; then
        echo "# Process $pid returned failure."
        return 0
    fi
    return 1
}

# Wait for a set of processes and check for expected exit status
#
# Arguments:
#	$1 - 1 if expecting success, 0 for failure.
#	remaining args - The pids of the processes
#
# Return:
#	0 if exit status of any process matches the expectation.
#	1 otherwise.
wait_and_detect_for_any() {
    check_for_success=$1

    shift
    detected=1 # 0 for success detection

    for pid in $@; do
        if wait_check_process_status "$pid" "$check_for_success"; then
            detected=0
            # Wait for other processes to exit
        fi
    done

    return $detected
}

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4
if [ "$(id -u)" -ne 0 ]; then
    echo "SKIP: SGX cgroup tests need root privileges."
    exit $ksft_skip
fi

cg_root=/sys/fs/cgroup
if [ ! -d "$cg_root/$test_root_cg" ]; then
    echo "SKIP: SGX cgroup tests require v2 cgroups."
    exit $ksft_skip
fi
test_root_cg=sgx_kselftest
#make sure we start clean
if [ -d "$cg_root/$test_root_cg" ]; then
    echo "SKIP: Please clean up $cg_root/$test_root_cg."
    exit $ksft_skip
fi

test_cg_small_parent=$test_root_cg/sgx_test_small_parent
test_cg_large=$test_root_cg/sgx_test_large
test_cg_small=$test_cg_small_parent/sgx_test_small
test_cg_larger=$test_root_cg/sgx_test_larger

clean_up()
{
    # Wait a little for cgroups to reset counters for dead processes.
    sleep 2
    rmdir $cg_root/$test_cg_large
    rmdir $cg_root/$test_cg_small
    rmdir $cg_root/$test_cg_larger
    rmdir $cg_root/$test_cg_small_parent
    rmdir $cg_root/$test_root_cg
}

mkdir $cg_root/$test_root_cg && \
mkdir $cg_root/$test_cg_small_parent && \
mkdir $cg_root/$test_cg_large && \
mkdir $cg_root/$test_cg_small && \
mkdir $cg_root/$test_cg_larger
if [ $? -ne 0 ]; then
    echo "FAIL: Failed creating cgroups."
    exit 1
fi

# Turn on misc and memory controller in non-leaf nodes
echo "+misc" >  $cg_root/cgroup.subtree_control && \
echo "+memory" > $cg_root/cgroup.subtree_control && \
echo "+misc" >  $cg_root/$test_root_cg/cgroup.subtree_control && \
echo "+memory" > $cg_root/$test_root_cg/cgroup.subtree_control && \
echo "+misc" >  $cg_root/$test_cg_small_parent/cgroup.subtree_control
if [ $? -ne 0 ]; then
    echo "FAIL: can't set up cgroups, make sure misc and memory cgroups are enabled."
    clean_up
    exit 1
fi

epc_capacity=$(grep "sgx_epc" "$cg_root/misc.capacity" | awk '{print $2}')

# This is below number of VA pages needed for enclave of capacity size. So
# should fail oversubscribed cases
epc_small_limit=$(( epc_capacity / 512 ))

# At least load one enclave of capacity size successfully, maybe up to 4.
# But some may fail if we run more than 4 concurrent enclaves of capacity size.
epc_large_limit=$(( epc_small_limit * 4 ))

# Load lots of enclaves
epc_larger_limit=$epc_capacity
echo "# Setting up SGX cgroup limits."
echo "sgx_epc $epc_small_limit" > $cg_root/$test_cg_small_parent/misc.max && \
echo "sgx_epc $epc_large_limit" >  $cg_root/$test_cg_large/misc.max && \
echo "sgx_epc $epc_larger_limit" > $cg_root/$test_cg_larger/misc.max
if [ $? -ne 0 ]; then
    echo "# Failed setting up misc limits for sgx_epc."
    echo "SKIP: Kernel does not support SGX cgroup."
    clean_up
    exit $ksft_skip
fi

test_cmd="./test_sgx -t unclobbered_vdso_oversubscribed"

echo "# Start unclobbered_vdso_oversubscribed with small EPC limit, expecting failure..."
./ash_cgexec.sh $test_cg_small $test_cmd >/dev/null 2>&1
if [ $? -eq 0 ]; then
    echo "FAIL: Fail on small EPC limit, not expecting any test passes."
    clean_up
    exit 1
else
    echo "# Test failed as expected."
fi

echo "PASS: small EPC limit test."

echo "# Start 4 concurrent unclobbered_vdso_oversubscribed tests with large EPC limit, \
expecting at least one success...."

pids=""
for i in 1 2 3 4; do
    (
        ./ash_cgexec.sh $test_cg_large $test_cmd >/dev/null 2>&1
    ) &
    pids="$pids $!"
done

if wait_and_detect_for_any $PROCESS_SUCCESS "$pids"; then
    echo "PASS: large EPC limit positive testing."
else
    echo "FAIL: Failed on large EPC limit positive testing, no test passes."
    clean_up
    exit 1
fi

echo "# Start 5 concurrent unclobbered_vdso_oversubscribed tests with large EPC limit, \
expecting at least one failure...."
pids=""
for i in 1 2 3 4 5; do
    (
        ./ash_cgexec.sh $test_cg_large $test_cmd >/dev/null 2>&1
    ) &
    pids="$pids $!"
done

if wait_and_detect_for_any $PROCESS_FAILURE "$pids"; then
    echo "PASS: large EPC limit negative testing."
else
    echo "FAIL: Failed on large EPC limit negative testing, no test fails."
    clean_up
    exit 1
fi

echo "# Start 8 concurrent unclobbered_vdso_oversubscribed tests with larger EPC limit, \
expecting no failure...."
pids=""
for i in 1 2 3 4 5 6 7 8; do
    (
        ./ash_cgexec.sh $test_cg_larger $test_cmd >/dev/null 2>&1
    ) &
    pids="$pids $!"
done

if wait_and_detect_for_any $PROCESS_FAILURE "$pids"; then
    echo "FAIL: Failed on larger EPC limit, at least one test fails."
    clean_up
    exit 1
else
    echo "PASS: larger EPC limit tests."
fi

echo "# Start 8 concurrent unclobbered_vdso_oversubscribed tests with larger EPC limit,\
 randomly kill one, expecting no failure...."
pids=""
for i in 1 2 3 4 5 6 7 8; do
    (
        ./ash_cgexec.sh $test_cg_larger $test_cmd >/dev/null 2>&1
    ) &
    pids="$pids $!"
done
random_number=$(awk 'BEGIN{srand();print int(rand()*5)}')
sleep $((random_number + 1))

# Randomly select a process to kill
# Make sure usage counter not leaked at the end.
random_index=$(awk 'BEGIN{srand();print int(rand()*8)}')
counter=0
for pid in $pids; do
    if [ "$counter" -eq "$random_index" ]; then
        pid_to_kill=$pid
        break
    fi
    counter=$((counter + 1))
done

kill $pid_to_kill
echo "# Killed process with PID: $pid_to_kill"

any_failure=0
for pid in $pids; do
    wait "$pid"
    status=$?
    if [ "$pid" != "$pid_to_kill" ]; then
        if [ $status -ne 0 ]; then
	    echo "# Process $pid returned failure."
            any_failure=1
        fi
    fi
done

if [ $any_failure -ne 0 ]; then
    echo "FAIL: Failed on random killing, at least one test fails."
    clean_up
    exit 1
fi
echo "PASS: larger EPC limit test with a process randomly killed."

mem_limit_too_small=$((epc_capacity - 2 * epc_large_limit))

echo "$mem_limit_too_small" > $cg_root/$test_cg_large/memory.max
if [ $? -ne 0 ]; then
    echo "FAIL: Failed setting up memory controller."
    clean_up
    exit 1
fi

echo "# Start 4 concurrent unclobbered_vdso_oversubscribed tests with large EPC limit, \
and too small RAM limit, expecting all failures...."
# Ensure swapping off so the OOM killer is activated when mem_cgroup limit is hit.
swapoff -a
pids=""
for i in 1 2 3 4; do
    (
        ./ash_cgexec.sh $test_cg_large $test_cmd >/dev/null 2>&1
    ) &
    pids="$pids $!"
done

if wait_and_detect_for_any $PROCESS_SUCCESS "$pids"; then
    echo "FAIL: Failed on tests with memcontrol, some tests did not fail."
    clean_up
    swapon -a
    exit 1
else
    swapon -a
    echo "PASS: large EPC limit tests with memcontrol."
fi

sleep 2

epc_usage=$(grep '^sgx_epc' "$cg_root/$test_root_cg/misc.current" | awk '{print $2}')
if [ "$epc_usage" -ne 0 ]; then
    echo "FAIL: Final usage is $epc_usage, not 0."
else
    echo "PASS: leakage check."
    echo "PASS: ALL cgroup limit tests, cleanup cgroups..."
fi
clean_up
echo "# Done SGX cgroup tests."
