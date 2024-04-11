#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import time
from lib.py import ksft_run, ksft_pr, ksft_eq, ksft_ge, NetdevFamily, NetdevSimDev, ip


def empty_check(nf) -> None:
    devs = nf.dev_get({}, dump=True)
    ksft_ge(len(devs), 1)


def lo_check(nf) -> None:
    lo_info = nf.dev_get({"ifindex": 1})
    ksft_eq(len(lo_info['xdp-features']), 0)
    ksft_eq(len(lo_info['xdp-rx-metadata-features']), 0)


def page_pool_check(nf) -> None:
    with NetdevSimDev() as nsimdev:
        nsim = nsimdev.nsims[0]

        # No page pools when down
        nsim.down()
        pp_list = nf.page_pool_get({}, dump=True)
        pp_list = [pp for pp in pp_list if pp.get("ifindex") == nsim.ifindex]
        ksft_eq(len(pp_list), 0)

        # Up, empty page pool appears
        nsim.up()
        pp_list = nf.page_pool_get({}, dump=True)
        pp_list = [pp for pp in pp_list if pp.get("ifindex") == nsim.ifindex]
        ksft_ge(len(pp_list), 0)
        refs = sum([pp["inflight"] for pp in pp_list])
        ksft_eq(refs, 0)

        # Down, it disappears, again
        nsim.down()
        pp_list = nf.page_pool_get({}, dump=True)
        pp_list = [pp for pp in pp_list if pp.get("ifindex") == nsim.ifindex]
        ksft_eq(len(pp_list), 0)

        # Up, allocate a page
        nsim.up()
        nsim.dfs_write("pp_hold", "y")
        pp_list = nf.page_pool_get({}, dump=True)
        refs = sum([pp["inflight"] for pp in pp_list if pp.get("ifindex") == nsim.ifindex])
        ksft_ge(refs, 1)

        # Now let's leak a page
        nsim.down()
        pp_list = nf.page_pool_get({}, dump=True)
        pp_list = [pp for pp in pp_list if pp.get("ifindex") == nsim.ifindex]
        ksft_eq(len(pp_list), 1)
        refs = sum([pp["inflight"] for pp in pp_list if pp.get("ifindex") == nsim.ifindex])
        ksft_eq(refs, 1)
        undetached = [pp for pp in pp_list if "detach-time" not in pp]
        ksft_eq(len(undetached), 0)

        # New pp can get created, and we'll have two
        nsim.up()
        pp_list = nf.page_pool_get({}, dump=True)
        pp_list = [pp for pp in pp_list if pp.get("ifindex") == nsim.ifindex]
        attached = [pp for pp in pp_list if "detach-time" not in pp]
        undetached = [pp for pp in pp_list if "detach-time" in pp]
        ksft_eq(len(attached), 1)
        ksft_eq(len(undetached), 1)

        # Free the old page and the old pp is gone
        nsim.dfs_write("pp_hold", "n")
        # Freeing check is once a second so we may need to retry
        for i in range(50):
            pp_list = nf.page_pool_get({}, dump=True)
            pp_list = [pp for pp in pp_list if pp.get("ifindex") == nsim.ifindex]
            if len(pp_list) == 1:
                break
            time.sleep(0.05)
        ksft_eq(len(pp_list), 1)

        # And down...
        nsim.down()
        pp_list = nf.page_pool_get({}, dump=True)
        pp_list = [pp for pp in pp_list if pp.get("ifindex") == nsim.ifindex]
        ksft_eq(len(pp_list), 0)

        # Last, leave the page hanging for destroy, nothing to check
        # we're trying to exercise the orphaning path in the kernel
        nsim.up()
        nsim.dfs_write("pp_hold", "y")


def main() -> None:
    nf = NetdevFamily()
    ksft_run([empty_check, lo_check, page_pool_check],
             args=(nf, ))


if __name__ == "__main__":
    main()
