#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# A version of timeout in pure bash that doesn't require the timeout command.
bash_timeout() {
    # Check if the native 'timeout' command is available
    if command -v timeout >/dev/null 2>&1; then
        timeout "$@"
        return $?
    fi

    local timeout_secs="$1"
    shift
    local command=("$@")

    # Execute the target command in the background
    "${command[@]}" &
    local cmd_pid=$!

    # Start a watcher process in the background
    (
        sleep "$timeout_secs"
        kill -TERM "$cmd_pid" 2>/dev/null || true
    ) &
    local watcher_pid=$!

    # Wait for the main command to complete (or be killed)
    wait "$cmd_pid" 2>/dev/null
    local exit_code=$?

    # Clean up: Kill the watcher process if the main command finished early
    kill "$watcher_pid" 2>/dev/null || true

    # Return the exit status of the target command
    return $exit_code
}
