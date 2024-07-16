#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Test for rt group scheduling
# Date: June 27, 2024
# Author: Xavier <xavier_qy@163.com>

# Record the list of child process PIDs
PIDS=()

# File for redirected output
LOGFILE="rt_group_sched_test.log"

# Cleanup function: kill all recorded child processes and unmount the cgroup
function cleanup() {
	echo "Cleaning up..."
	for pid in "${PIDS[@]}"; do
		if kill -0 $pid 2>/dev/null; then
			kill -TERM $pid
		fi
	done

	# Sleep for a while to ensure the processes are properly killed
	sleep 2

	# Unmount the cgroup filesystem
	umount /sys/fs/cgroup/cpu 2>/dev/null
	umount /sys/fs/cgroup 2>/dev/null
	echo "Cleanup completed."

	# Ensure the LOGFILE exists and is correct
	if [ ! -f "$LOGFILE" ]; then
		echo "$LOGFILE not found!"
		exit 1
	fi

	# Initialize the total count variable
	total=0

	# Read matching lines and calculate the total sum
	while IFS= read -r line
	do
		# Use grep to match lines containing 'pid:' and 'cnt:', and extract the value of cnt
		if echo "$line" | grep -q '^pid:[[:digit:]]\+ cnt:[[:digit:]]\+'; then
			cnt=$(echo "$line" | sed -n \
			  's/^pid:[[:digit:]]\+ cnt:\([[:digit:]]\+\)/\1/p')
			total=$((total + cnt))
		fi
	done < "$LOGFILE"

	# Print the total sum
	echo "Total cnt: $total"
	echo "Finished processing."
}

# Capture actions when interrupted or terminated by a signal
trap cleanup EXIT

# Start the cgroup filesystem and create the necessary directories
function setup_cgroups() {
	mount -t tmpfs -o mode=755 cgroup_root /sys/fs/cgroup
	mkdir -p /sys/fs/cgroup/cpu
	mount -t cgroup -o cpu none /sys/fs/cgroup/cpu
}

# Create cgroup subdirectories and configure their settings
function create_child_cgroup() {
	local base_dir=$1
	local name=$2
	local rt_period=$3
	local rt_runtime=$4
	mkdir -p "$base_dir/$name"
	echo $rt_period > "$base_dir/$name/cpu.rt_period_us"
	echo $rt_runtime > "$base_dir/$name/cpu.rt_runtime_us"
}
# Launch a process and add it to the specified cgroup
function launch_process() {
	local process_name=$1

	# Three parameters representing the number of child threads, scheduling policy, and priority
	local args=$2
	local cgroup_path=$3

	# Launch the process
	exec -a $process_name ./deadloop $args &
	local pid=$!
	PIDS+=($pid)

	# Short sleep to ensure the process starts
	sleep 1

	# Check if the process started successfully
	if ! pgrep -x $process_name > /dev/null; then
		echo "Error: No process found with name $process_name."
		exit 1
	fi

	echo $pid > "$cgroup_path/cgroup.procs"
	echo "Process $process_name with PID $pid added to cgroup $cgroup_path"
}

# Main function running all tasks
function main() {
	echo "The test needs 30 seconds..."
	rm -f "$LOGFILE"
	setup_cgroups
	create_child_cgroup "/sys/fs/cgroup/cpu" "child1" 1000000 800000
	create_child_cgroup "/sys/fs/cgroup/cpu/child1" "child2" 1000000 700000
	create_child_cgroup "/sys/fs/cgroup/cpu/child1/child2" "child3" 1000000 600000
	launch_process "child1" "3 2 50" "/sys/fs/cgroup/cpu/child1"
	launch_process "child2" "3 2 50" "/sys/fs/cgroup/cpu/child1/child2"
	launch_process "child3" "1 2 50" "/sys/fs/cgroup/cpu/child1/child2/child3"
	launch_process "tg_root" "1 2 50" "/sys/fs/cgroup/cpu"

	# Run for 30 seconds
	sleep 30
}

# Execute the main function
main
