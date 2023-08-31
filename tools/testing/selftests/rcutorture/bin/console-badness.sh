#!/bin/bash
# SPDX-License-Identifier: GPL-2.0+
#
# Scan standard input for error messages, dumping any found to standard
# output.
#
# Usage: console-badness.sh
#
# Copyright (C) 2020 Facebook, Inc.
#
# Authors: Paul E. McKenney <paulmck@kernel.org>
INPUT_DATA=$(< /dev/stdin)

# Get the line numbers for all the grep matches
GREP_LINES="$(echo "$INPUT_DATA" |
grep -n -E 'Badness|WARNING:|Warn|BUG|===========|BUG: KCSAN:|Call Trace:|Oops:|detected stalls on CPUs/tasks:|self-detected stall on CPU|Stall ended before state dump start|\?\?\? Writer stall state|rcu_.*kthread starved for|!!!' |
grep -v 'ODEBUG: ' |
grep -v 'This means that this is a DEBUG kernel and it is' |
grep -v 'Warning: unable to open an initial console' |
grep -v 'Warning: Failed to add ttynull console. No stdin, stdout, and stderr.*the init process!' |
grep -v 'NOHZ tick-stop error: Non-RCU local softirq work is pending, handler'
)"

# Exit if no grep matches
if [ ! -n "$GREP_LINES" ]; then exit 0; fi

# Go through each line of GREP_LINES, extract the line number and then
# print from that line and 20 lines after that line. Do that for each
# grep match.
issue_num=1
while IFS= read -r line; do
    # Extract the line number from the line
    num=$(echo "$line" | awk -F: '{print $1}')
    # Print 20 lines after the matched line
    echo "Issue $issue_num:"
    issue_num="$(($issue_num + 1))"
    echo "$INPUT_DATA" | sed -n "${num},$(($num + 20))p"
    echo "-------------------------------------"
done <<< "$GREP_LINES"

