#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# --- Usage function ---
usage() {
	echo "======================================"
	echo "  KStackWatch Test Script Usage"
	echo "======================================"
	echo ""
	echo "IMPORTANT: Before running, make sure you have updated the offset values!"
	echo ""
	echo "To find your offsets, use objdump:"
	echo "  objdump -S --disassemble=canary_test_write vmlinux"
	echo ""
	echo "Then search for your function names to find the instruction addresses."
	echo "- Instruction offset: address relative to function's start"
	echo "- Stack var offset: distance from stack base (%rbp) to the variable"
	echo ""
	echo "Usage: $0 [test_case_number]"
	echo ""
	echo "Available test cases:"
	echo "  0  - Canary Write Test"
	echo "  1  - Canary Overflow Test"
	echo "  2  - Silent Corruption Test"
	echo "  3  - Recursive Corruption Test"
	echo ""
	echo "======================================"
	echo ""
}

# --- Interactive menu ---
show_menu() {
	echo "Select a test case to run:"
	echo "  0) Canary Write Test"
	echo "  1) Canary Overflow Test"
	echo "  2) Silent Corruption Test"
	echo "  3) Recursive Corruption Test"
	echo "  q) Quit"
	echo ""
	echo "WARNING: Each test may cause system crash/hang!"
	echo ""
	read -p "Enter your choice [0-3/q]: " choice
	echo ""

	case "$choice" in
	0) test0 ;;
	1) test1 ;;
	2) test2 ;;
	3) test3 ;;
	q | Q)
		echo "Exiting..."
		exit 0
		;;
	*)
		echo "Invalid choice. Please try again."
		echo ""
		show_menu
		;;
	esac
}

# --- Test Case 0: Canary Write ---
test0() {
	echo "=== Running Test Case 0: Canary Write ==="
	# function+instruction_off[+depth] [local_var_offset:local_var_len]
	echo "canary_test_write+0x12" >/proc/kstackwatch
	echo "test0" >/proc/kstackwatch_test
	echo >/proc/kstackwatch
}

# --- Test Case 1: Canary Overflow ---
test1() {
	echo "=== Running Test Case 1: Canary Overflow ==="
	# function+instruction_off[+depth] [local_var_offset:local_var_len]
	echo "canary_test_overflow+0x12" >/proc/kstackwatch
	echo "test1" >/proc/kstackwatch_test
	echo >/proc/kstackwatch

}

# --- Test Case 2: Silent Corruption ---
test2() {
	echo "=== Running Test Case 2: Silent Corruption ==="
	# function+instruction_off[+depth] [local_var_offset:local_var_len]
	echo "silent_corruption_hapless+0x7f 0:8" >/proc/kstackwatch
	echo "test2" >/proc/kstackwatch_test
	echo >/proc/kstackwatch
}

# --- Test Case 3: Recursive Corruption ---
test3() {
	echo "=== Running Test Case 3: Recursive Corruption ==="
	# function+instruction_off[+depth] [local_var_offset:local_var_len]
	echo "recursive_corruption_test+0x1b+3 0:8" >/proc/kstackwatch
	echo "test3" >/proc/kstackwatch_test
	echo >/proc/kstackwatch
}

# --- Main ---
if [ -z "$1" ]; then
	usage
	echo ""
	show_menu
else
	case "$1" in
	0) test0 ;;
	1) test1 ;;
	2) test2 ;;
	3) test3 ;;
	help | --help | -h) usage ;;
	*)
		echo "Error: Invalid argument '$1'"
		echo ""
		usage
		exit 1
		;;
	esac
fi
