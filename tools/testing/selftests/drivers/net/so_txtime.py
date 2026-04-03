#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Regression tests for the SO_TXTIME interface.

Test delivery time in FQ and ETF qdiscs.
"""

import time

from lib.py import ksft_exit, ksft_run
from lib.py import KsftNamedVariant, KsftSkipEx, ksft_variants
from lib.py import NetDrvEpEnv, bkg, cmd


def test_so_txtime(cfg, clockid, ipver, args_tx, args_rx, expect_fail):
  bin_path = cfg.net_lib_dir / "so_txtime"

  tstart = time.time_ns() + 100_000_000

  cmd_addr = f"-S {cfg.addr_v[ipver]} -D {cfg.remote_addr_v[ipver]}"
  cmd_base = f"{bin_path} -{ipver} -c {clockid} -t {tstart} {cmd_addr}"
  cmd_rx = f"{cmd_base} {args_rx} -r"
  cmd_tx = f"{cmd_base} {args_tx}"

  try:
    with bkg(cmd_rx, host=cfg.remote, exit_wait=True):
      cmd(cmd_tx)
  except:
    if not expect_fail:
      raise


def _test_variants_mono():
  for ipver in ["4", "6"]:
    for testcase in [
        ["no_delay", "a,-1", "a,-1"],
        ["zero_delay", "a,0", "a,0"],
        ["one_pkt", "a,10", "a,10"],
        ["in_order", "a,10,b,20", "a,10,b,20"],
        ["reverse_order", "a,20,b,10", "b,20,a,20"],
    ]:
      name = f"_v{ipver}_{testcase[0]}"
      yield KsftNamedVariant(name, ipver, testcase[1], testcase[2])


@ksft_variants(_test_variants_mono())
def test_so_txtime_mono(cfg, ipver, args_tx, args_rx):
  cmd(f"tc qdisc replace dev {cfg.ifname} root fq")
  test_so_txtime(cfg, "mono", ipver, args_tx, args_rx, False)


def _test_variants_etf():
  for ipver in ["4", "6"]:
    for testcase in [
        ["no_delay", "a,-1", "a,-1", True],
        ["zero_delay", "a,0", "a,0", True],
        ["one_pkt", "a,10", "a,10", False],
        ["in_order", "a,10,b,20", "a,10,b,20", False],
        ["reverse_order", "a,20,b,10", "b,10,a,20", False],
    ]:
      name = f"_v{ipver}_{testcase[0]}"
      yield KsftNamedVariant(name, ipver, testcase[1], testcase[2], testcase[3])


@ksft_variants(_test_variants_etf())
def test_so_txtime_etf(cfg, ipver, args_tx, args_rx, expect_fail):
  try:
    # ETF does not support change, so remove and re-add it instead.
    cmd(f"tc qdisc replace dev {cfg.ifname} root pfifo_fast")
    etf_args = "clockid CLOCK_TAI delta 400000"
    cmd(f"tc qdisc replace dev {cfg.ifname} root etf {etf_args}")
  except Exception as e:
    raise KsftSkipEx("tc does not support qdisc etf. skipping") from e

  test_so_txtime(cfg, "tai", ipver, args_tx, args_rx, expect_fail)


def main() -> None:
  with NetDrvEpEnv(__file__) as cfg:
    ksft_run([test_so_txtime_mono], args=(cfg,))
    ksft_run([test_so_txtime_etf], args=(cfg,))
  ksft_exit()


if __name__ == "__main__":
  main()
