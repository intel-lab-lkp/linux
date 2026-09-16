#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

from os import path
from devmem_lib import (setup_test, run_rx, run_tx, run_tx_chunks, run_rx_hds,
                        run_rx_large_niov, require_devmem)
from lib.py import (defer, EthtoolFamily, KperfRunner, ksft_run, SystemMonitor,
                    ksft_exit, ksft_disruptive)
from lib.py import NetDrvEpEnv


@ksft_disruptive
def check_rx(cfg) -> None:
    """Run the devmem RX test."""
    run_rx(cfg)


@ksft_disruptive
def check_tx(cfg) -> None:
    """Run the devmem TX test."""
    run_tx(cfg)


@ksft_disruptive
def check_tx_chunks(cfg) -> None:
    """Run the devmem TX chunking test."""
    run_tx_chunks(cfg)


def check_rx_hds(cfg) -> None:
    """Run the HDS test."""
    run_rx_hds(cfg)


def check_rx_large_niov(cfg) -> None:
    """Run the devmem RX test with rx-page-size = 16 KiB."""
    run_rx_large_niov(cfg)


@ksft_disruptive
def check_kperf(cfg) -> None:
    """Measure CUDA-backed devmem RX performance with kperf."""
    cfg.require_nsim(False)

    kperf = KperfRunner(cfg)

    dmabuf_size = 2048
    # once udmabuf works well with 4K:
    # dmabuf_size = 64
    # if cmd("test -c /dev/udmabuf", fail=False).ret:
    #     raise KsftSkipEx("Test requires /dev/udmabuf")
    # size_limit = cmd("cat /sys/module/udmabuf/parameters/size_limit_mb",
    #                  fail=False)
    # if size_limit.ret == 0 and int(size_limit.stdout) < dmabuf_size:
    #     raise KsftSkipEx(
    #         f"Test requires a {dmabuf_size} MiB udmabuf size limit")

    require_devmem(cfg)

    ethnl = EthtoolFamily()
    rings = ethnl.rings_get({"header": {"dev-index": cfg.ifindex}})
    defer(ethnl.rings_set, {
        "header": {"dev-index": cfg.ifindex},
        "tcp-data-split": rings.get("tcp-data-split", "unknown"),
        "rx": rings["rx"],
    })
    ethnl.rings_set({
        "header": {"dev-index": cfg.ifindex},
        "tcp-data-split": "enabled",
        "rx": rings["rx-max"],
    })

    rss = ethnl.rss_get({"header": {"dev-index": cfg.ifindex}})
    defer(ethnl.rss_set, {
        "header": {"dev-index": cfg.ifindex},
        "indir": rss["indir"],
    })

    devmem_args = [
        "--time", "15",
        "--num-connections", "4",
        "--read-size", str(64 << 20),
        "--write-size", str(64 << 20),
        "--pin-off", "4",
        "--num-rx-queues", "4",
        "--dmabuf-rx-size-mb", str(dmabuf_size),
        "--dmabuf-tx-size-mb", str(dmabuf_size),
        "--validate", "no",
        "--devmem-rx",
        "--devmem-tx",
        "--devmem-rx-memory", "cuda",
        "--devmem-tx-memory", "cuda",
        "--devmem-src-dev", "0000:4e:00",
        "--devmem-dst-dev", "0000:4e:00",
    ]

    # The remote source uses CUDA TX and the local destination uses CUDA RX.
    with kperf:
        with SystemMonitor(cfg):
            kperf.run(client_args=devmem_args)


def main() -> None:
    """Run the devmem test cases."""
    with NetDrvEpEnv(__file__) as cfg:
        setup_test(cfg, path.abspath(path.dirname(__file__) + "/ncdevmem"))
        ksft_run([check_rx, check_tx, check_tx_chunks, check_rx_hds,
                  check_rx_large_niov, check_kperf],
                 args=(cfg,))
    ksft_exit()


if __name__ == "__main__":
    main()
