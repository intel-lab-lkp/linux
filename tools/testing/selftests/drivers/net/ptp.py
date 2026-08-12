#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# By Maciek Machnikowski <maciek@machnikowski.net> (c) 2026,

"""
Test suite for PTP sync using ptp4l.

Start a ptp4l leader and follower and check that the follower locks onto the
leader (state s2)
"""

import time

from lib.py import (
    NetDrvEpEnv,
    bkg,
    fd_read_timeout,
    ksft_exit,
    ksft_pr,
    ksft_run,
    ksft_true,
)

PTP4L_SYNC_TIMEOUT = 40


def _poll_follower_sync(follower, timeout):
    """Read the follower stdout pipe until ptp4l reports sync state s2.

    Returns a tuple (synced, output) where output is the text read so far.
    """
    fd_file = follower.proc.stdout
    fd = fd_file.fileno()
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if b" s2 " in buf:
            break
        if follower.proc.poll() is not None:
            chunk = fd_file.read()
            if chunk:
                buf += chunk
            break
        try:
            remaining = deadline - time.monotonic()
            buf += fd_read_timeout(fd, min(1, remaining))
        except TimeoutError:
            continue
    return b" s2 " in buf, buf.decode("utf-8", "replace")


def ptp_sync_test(cfg):
    """Verify ptp4l leader/follower synchronization reaches state s2."""
    cfg.require_cmd("ptp4l", remote=True)

    leader_cmd = f"ptp4l -i {cfg.remote_ifname} -m -2"
    follower_cmd = f"ptp4l -i {cfg.ifname} -m -s -2"

    with bkg(leader_cmd, host=cfg.remote), \
         bkg(follower_cmd) as follower:
        synced, output = _poll_follower_sync(follower, PTP4L_SYNC_TIMEOUT)

    if synced:
        return

    ksft_pr(f"ptp4l follower did not reach locked state (s2) within "
            f"{PTP4L_SYNC_TIMEOUT}s")
    tail = output.strip().split("\n")[-10:]
    ksft_pr("Follower log (last 10 lines): " + " | ".join(tail))
    ksft_true(False, "PTP sync timeout")


def main():
    """Run ksft tests."""
    with NetDrvEpEnv(__file__) as cfg:
        ksft_run([ptp_sync_test], args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()
