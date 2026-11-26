#!/usr/bin/env drgn
# SPDX-License-Identifier: GPL-2.0-only
"""Show where HugeTLB reservations are held, per huge page size.

For each huge page size, compares resv_huge_pages against hugetlbfs
inodes, private owner VMAs and unused subpool reserves, and flags
subpools below their minimum.

Exit status: 0 ok, 1 mismatch, 2 incomplete scan.
"""

import argparse
from collections import defaultdict
from contextlib import contextmanager
import json
import stat
import sys

from drgn import Object, ProgramFlags, cast, container_of
from drgn.helpers.linux.fs import mount_dst, super_block_for_each_mount
from drgn.helpers.linux.hugetlb import for_each_hstate, huge_page_size
from drgn.helpers.linux.list import (
    hlist_for_each_entry,
    validate_list_for_each_entry,
)
from drgn.helpers.linux.mm import for_each_vma
from drgn.helpers.linux.pid import for_each_task

# Preprocessor macros in mm/hugetlb.c.
HPAGE_RESV_OWNER = 1
HPAGE_RESV_MASK = 3


def error_text(err):
    return f"{type(err).__name__}: {err}"


@contextmanager
def record_errors(issues, what):
    try:
        yield
    except Exception as err:
        issues.append(f"{what}: {error_text(err)}")


def file_regions(resv_map):
    if resv_map.adds_in_progress:
        raise ValueError("resv_map update in progress")
    head = resv_map.regions.address_of_()
    for entry in validate_list_for_each_entry("struct file_region", head,
                                              "link"):
        rg = entry[0].read_()
        yield int(rg.member_("from")), int(rg.to)


def read_inode(inode, hpage_size):
    info = container_of(inode, "struct hugetlbfs_inode_info", "vfs_inode")
    resv_map_pages = sum(
        to_idx - from_idx
        for from_idx, to_idx in file_regions(info.resv_map.read_())
    )
    instantiated = int(inode.i_blocks) * 512 // hpage_size
    # Internal SysV shm and memfd inodes still carry a dentry name here.
    name = None
    try:
        alias = inode.i_dentry.first.read_()
        if alias:
            dentry = container_of(alias, "struct dentry", "d_alias")
            raw = dentry.d_name.name.string_()
            name = raw.decode("utf-8", "backslashreplace")
    except Exception:
        pass
    res = {
        "inode": int(inode),
        "ino": int(inode.i_ino),
        "name": name,
        "resv_map_pages": resv_map_pages,
        "instantiated": instantiated,
        "outstanding": resv_map_pages - instantiated,
    }
    if instantiated > resv_map_pages:
        res["outstanding"] = None
        res["excess"] = instantiated - resv_map_pages
    return res


def subpool_limits(spool):
    min_hpages = rsv_hpages = 0
    max_hpages = used_hpages = min_shortfall = None
    if spool is not None:
        if spool.min_hpages != -1:
            min_hpages = int(spool.min_hpages)
            rsv_hpages = int(spool.rsv_hpages)
        if spool.max_hpages != -1:
            max_hpages = int(spool.max_hpages)
            used_hpages = int(spool.used_hpages)
            min_shortfall = max(0, min_hpages - used_hpages - rsv_hpages)
    return {
        "min_hpages": min_hpages,
        "rsv_hpages": rsv_hpages,
        "max_hpages": max_hpages,
        "used_hpages": used_hpages,
        "min_shortfall": min_shortfall,
    }


def init_ns_mount_path(sb):
    try:
        init_ns = sb.prog_["init_task"].nsproxy.mnt_ns.read_()
        for mnt in super_block_for_each_mount(sb):
            if mnt.mnt_ns == init_ns:
                return mount_dst(mnt).decode("utf-8", "backslashreplace")
    except Exception as err:
        print(f"warning: mount path for sb {int(sb):#x}: {error_text(err)}",
              file=sys.stderr)
    return None


def collect_superblock(sb, sbi, mounted_spools, issues):
    spool_ptr = sbi.spool.read_()
    mounted_spools.add(int(spool_ptr))
    limits = subpool_limits(spool_ptr[0].read_() if spool_ptr else None)
    h = sbi.hstate.read_()
    hpage_size = int(huge_page_size(h))
    # The list walker can yield an inode twice before it detects a cycle.
    inodes = {}
    with record_errors(issues, f"sb {int(sb):#x} inode list"):
        head = sb.s_inodes.address_of_()
        for inode in validate_list_for_each_entry("struct inode", head,
                                                  "i_sb_list"):
            try:
                if stat.S_ISREG(int(inode.i_mode)):
                    inodes[int(inode)] = read_inode(inode, hpage_size)
            except Exception as err:
                issues.append(f"inode {int(inode):#x}: {error_text(err)}")
    return int(h), {
        "sb": int(sb),
        "path": init_ns_mount_path(sb),
        **limits,
        "inodes": list(inodes.values()),
    }


def hugetlbfs_superblocks(prog, mounted_spools, issues):
    by_hstate = defaultdict(list)
    with record_errors(issues, "hugetlbfs superblocks"):
        head = prog["hugetlbfs_fs_type"].fs_supers.address_of_()
        for sb in hlist_for_each_entry("struct super_block", head,
                                       "s_instances"):
            with record_errors(issues, f"sb {int(sb):#x}"):
                sbi = cast("struct hugetlbfs_sb_info *", sb.s_fs_info.read_())
                if not sbi:
                    continue
                hstate_addr, sb_info = collect_superblock(sb, sbi,
                                                          mounted_spools,
                                                          issues)
                by_hstate[hstate_addr].append(sb_info)
    return by_hstate


def owner_vmas(task, mm, vm_hugetlb, vm_mayshare, base_page_size):
    tgid = int(task.tgid)
    comm = task.comm.string_().decode(errors="replace")
    for vma in for_each_vma(mm):
        flags = int(vma.vm_flags)
        if not flags & vm_hugetlb or flags & vm_mayshare:
            continue
        # Fork clears the child's copy of the pointer; the parent stays owner.
        tagged = int(vma.vm_private_data)
        if not tagged & HPAGE_RESV_OWNER:
            continue
        resv_map_addr = tagged & ~HPAGE_RESV_MASK
        inode = vma.vm_file.f_inode.read_()
        sb = inode.i_sb.read_()
        h = cast("struct hugetlbfs_sb_info *", sb.s_fs_info).hstate.read_()
        order = int(h.order)
        vm_start, vm_end = int(vma.vm_start), int(vma.vm_end)
        # Half-open [from_index, to_index) in huge-page offsets.
        from_index = int(vma.vm_pgoff) >> order
        vma_pages = (vm_end - vm_start) // (base_page_size << order)
        yield int(h), {
            "tgid": tgid,
            "comm": comm,
            "mm": int(mm),
            "vm_start": vm_start,
            "vm_end": vm_end,
            "inode": int(inode),
            "sb": int(sb),
            "resv_map": resv_map_addr,
            "from_index": from_index,
            "to_index": from_index + vma_pages,
            "vma_pages": vma_pages,
        }


def private_mappings(prog, issues):
    by_resv_map, vmas_by_hstate = defaultdict(list), defaultdict(list)
    seen_mms = set()
    with record_errors(issues, "private tasks"):
        vm_hugetlb = 1 << int(prog.constant("VMA_HUGETLB_BIT"))
        vm_mayshare = 1 << int(prog.constant("VMA_MAYSHARE_BIT"))
        base_page_size = int(prog["PAGE_SIZE"])
        for task in for_each_task(prog):
            try:
                mm = task.mm.read_()
                mm_addr = int(mm)
                if not mm_addr or mm_addr in seen_mms:
                    continue
                seen_mms.add(mm_addr)
                for hstate_addr, vma in list(owner_vmas(task, mm, vm_hugetlb,
                                                        vm_mayshare,
                                                        base_page_size)):
                    by_resv_map[vma["resv_map"]].append((hstate_addr, vma))
            except Exception as err:
                issues.append(f"task {int(task):#x}: {error_text(err)}")
    # Split VMAs can share a resv_map. Count only the entries in each VMA.
    for resv_map_addr, entries in by_resv_map.items():
        with record_errors(issues, f"private resv_map {resv_map_addr:#x}"):
            resv_map = Object(prog, "struct resv_map *", value=resv_map_addr)
            ranges = list(file_regions(resv_map))
            for hstate_addr, vma in entries:
                from_idx, to_idx = vma["from_index"], vma["to_index"]
                consumed = sum(
                    max(0, min(to_idx, end_idx) - max(from_idx, start_idx))
                    for start_idx, end_idx in ranges
                )
                vma["consumed"] = consumed
                vma["outstanding"] = vma["vma_pages"] - consumed
                vmas_by_hstate[hstate_addr].append(vma)
    return vmas_by_hstate


def detached_subpools(prog, hstate_addr, mounted_spools, issues):
    found = []
    seen = mounted_spools | {0}
    with record_errors(issues, f"hstate {hstate_addr:#x} active folios"):
        h = Object(prog, "struct hstate *", value=hstate_addr)
        head = h.hugepage_activelist.address_of_()
        for folio in validate_list_for_each_entry("struct folio", head, "lru"):
            try:
                spool_ptr = int(folio._hugetlb_subpool)
                if spool_ptr in seen:
                    continue
                seen.add(spool_ptr)
                spool = Object(prog, "struct hugepage_subpool *",
                               value=spool_ptr)[0].read_()
                if spool.count == 0 and spool.min_hpages != -1:
                    found.append({
                        "subpool": spool_ptr,
                        "folio": int(folio),
                        **subpool_limits(spool),
                    })
            except Exception as err:
                issues.append(f"folio {int(folio):#x}: {error_text(err)}")
    return found


def read_hstate_counters(prog):
    counters = {}
    for h in for_each_hstate(prog):
        counters[int(h)] = {
            "hstate": int(h),
            "size_kib": int(huge_page_size(h)) // 1024,
            "nr_huge_pages": int(h.nr_huge_pages),
            "free_huge_pages": int(h.free_huge_pages),
            "resv_huge_pages": int(h.resv_huge_pages),
        }
    return counters


def reconcile_hstate(prog, hstate_addr, counters, superblocks, mappings,
                     mounted_spools, issues):
    inodes = [inode for sb in superblocks for inode in sb["inodes"]]
    inode_resv = sum(inode["outstanding"] or 0 for inode in inodes)
    private_resv = sum(vma["outstanding"] for vma in mappings)
    mounted_resv = sum(sb["rsv_hpages"] for sb in superblocks)

    active = counters["nr_huge_pages"] - counters["free_huge_pages"]
    detached = []
    # Pinned folios can keep a subpool alive after unmount.
    if active > 0:
        detached = detached_subpools(prog, hstate_addr, mounted_spools, issues)
    detached_resv = sum(sp["rsv_hpages"] for sp in detached)
    attributed = inode_resv + private_resv + mounted_resv + detached_resv
    unattributed = counters["resv_huge_pages"] - attributed

    subpools = superblocks + detached
    # Lower bound: assume every in-use page counts toward a subpool minimum.
    resv_floor = max(0, sum(sp["min_hpages"] for sp in subpools) - active)
    floor_shortfall = max(0, resv_floor - counters["resv_huge_pages"])

    result = {
        **counters,
        "inode_resv": inode_resv,
        "private_resv": private_resv,
        "mounted_resv": mounted_resv,
    }
    if detached:
        result["detached_resv"] = detached_resv
    result.update({
        "attributed": attributed,
        "unattributed": unattributed,
        "resv_floor": resv_floor,
        "floor_shortfall": floor_shortfall,
        "superblocks": superblocks,
        "private_mappings": mappings,
    })
    if detached:
        result["detached_subpools"] = detached

    mismatch = (unattributed != 0 or floor_shortfall > 0
                or any(sp["min_shortfall"] for sp in subpools)
                or any("excess" in inode for inode in inodes))
    return result, mismatch


def collect(prog):
    live = bool(prog.flags & ProgramFlags.IS_LIVE)
    issues, results = [], []
    counters = {}
    with record_errors(issues, "hstates"):
        counters = read_hstate_counters(prog)
    mounted_spools = set()
    mismatch = False
    if counters:
        superblocks = hugetlbfs_superblocks(prog, mounted_spools, issues)
        private = private_mappings(prog, issues)
        for hstate_addr, state in sorted(counters.items(),
                                         key=lambda item: item[1]["size_kib"]):
            result, has_diff = reconcile_hstate(prog, hstate_addr, state,
                                                superblocks[hstate_addr],
                                                private[hstate_addr],
                                                mounted_spools, issues)
            results.append(result)
            mismatch |= has_diff
        if live:
            with record_errors(issues, "hstates recheck"):
                if counters != read_hstate_counters(prog):
                    issues.append("pool counters changed during scan")
    elif not issues:
        issues.append("no hstates (CONFIG_HUGETLBFS / debuginfo?)")
    status = "mismatch" if mismatch else "ok"
    return {
        "source": "live" if live else "dump",
        "status": "incomplete" if issues else status,
        "issues": issues,
        "hstates": results,
    }


def print_report(report):
    note = ", non-atomic" if report["source"] == "live" else ""
    print("source={source} status={status} "
          "(counts in huge pages{note})".format(note=note, **report))
    for hstate in report["hstates"]:
        print("\n{size_kib} KiB: nr_huge_pages={nr_huge_pages} "
              "free_huge_pages={free_huge_pages} "
              "resv_huge_pages={resv_huge_pages}".format(**hstate))
        extra = (" detached_resv={detached_resv}".format(**hstate)
                 if "detached_resv" in hstate else "")
        print("  inode_resv={inode_resv} private_resv={private_resv} "
              "mounted_resv={mounted_resv}{extra}".format(extra=extra,
                                                          **hstate))
        print("  attributed={attributed} unattributed={unattributed:+d} "
              "resv_floor={resv_floor} "
              "floor_shortfall={floor_shortfall}".format(**hstate))
        for sb in hstate["superblocks"]:
            inodes = [inode for inode in sb["inodes"]
                      if inode["outstanding"] != 0]
            if not (inodes or sb["min_hpages"] or sb["rsv_hpages"]
                    or sb["min_shortfall"]):
                continue
            path_str = (" path={!r}".format(sb["path"])
                        if sb["path"] is not None else "")
            print("  sb={sb:#x}{path_str} min_hpages={min_hpages} "
                  "rsv_hpages={rsv_hpages}".format(path_str=path_str, **sb))
            if sb["min_shortfall"]:
                print("    min_shortfall={min_shortfall} "
                      "used_hpages={used_hpages}".format(**sb))
            for inode in inodes:
                tail = ("excess={excess}".format(**inode)
                        if "excess" in inode
                        else "outstanding={outstanding}".format(**inode))
                print("    ino={ino} name={name!r} "
                      "resv_map_pages={resv_map_pages} "
                      "instantiated={instantiated} "
                      "{tail}".format(tail=tail, **inode))
        for vma in hstate["private_mappings"]:
            if vma["outstanding"]:
                print("  tgid={tgid} comm={comm!r} "
                      "vma={vm_start:#x}-{vm_end:#x} consumed={consumed} "
                      "outstanding={outstanding}".format(**vma))
        for sp in hstate.get("detached_subpools", ()):
            print("  detached subpool={subpool:#x} min_hpages={min_hpages} "
                  "rsv_hpages={rsv_hpages}".format(**sp))
            short = (" min_shortfall={min_shortfall} "
                     "used_hpages={used_hpages}".format(**sp)
                     if sp["min_shortfall"] else "")
            print("    folio={folio:#x}{short}".format(short=short, **sp))
    for issue in report["issues"]:
        print("  incomplete: {}".format(issue))


def main():
    parser = argparse.ArgumentParser(
        usage="drgn -k %(prog)s [--json]\n"
              "       drgn -c vmcore -s vmlinux %(prog)s [--json]",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--json", action="store_true",
                        help="print the full report as JSON")
    args = parser.parse_args()
    report = collect(prog)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_report(report)
    return {"ok": 0, "mismatch": 1, "incomplete": 2}[report["status"]]


if __name__ == "__main__":
    sys.exit(main())
