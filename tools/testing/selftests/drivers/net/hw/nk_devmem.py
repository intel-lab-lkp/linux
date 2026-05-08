#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Test devmem TCP with netkit."""

import os
from lib.py import ksft_run, ksft_exit, ksft_disruptive
from lib.py import NetDrvContEnv
from lib.py.devmem import (setup_test, require_devmem, configure_nic,
                           cleanup_nic, run_rx, run_tx, run_tx_chunks,
                           run_rx_hds)


@ksft_disruptive
def check_nk_rx(cfg) -> None:
    """Run the devmem RX test through netkit."""
    run_rx(cfg)


@ksft_disruptive
def check_nk_tx(cfg) -> None:
    """Run the devmem TX test through netkit."""
    run_tx(cfg)


@ksft_disruptive
def check_nk_tx_chunks(cfg) -> None:
    """Run the devmem TX chunking test through netkit."""
    run_tx_chunks(cfg)


def check_nk_rx_hds(cfg) -> None:
    """Run the HDS test through netkit."""
    run_rx_hds(cfg)


def main() -> None:
    """Configure the NIC once, then run the netkit devmem test cases."""
    with NetDrvContEnv(__file__, rxqueues=2, primary_rx_redirect=True) as cfg:
        setup_test(cfg,
                   os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "ncdevmem"))

        require_devmem(cfg)
        configure_nic(cfg)
        try:
            ksft_run([check_nk_rx, check_nk_tx, check_nk_tx_chunks,
                      check_nk_rx_hds], args=(cfg,))
        finally:
            cleanup_nic(cfg)

    ksft_exit()


if __name__ == "__main__":
    main()
