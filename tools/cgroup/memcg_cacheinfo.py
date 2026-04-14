#!/usr/bin/env drgn
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Hongfu Li <lihongfu@kylinos.cn>

import argparse
import os
import sys
from collections import defaultdict
from enum import IntEnum

from drgn import FaultError, Object, ObjectNotFoundError, Program, container_of
from drgn.helpers.linux import for_each_page

try:
    from drgn.helpers.linux.mm import PageActive, PageDirty, PageTail, compound_head
except ImportError:
    PageActive = PageDirty = PageTail = compound_head = None

DESC = """
This is a drgn script to provide cache statistics for memory cgroups.
It supports both cgroup v1 and cgroup v2.
For drgn, visit https://github.com/osandov/drgn.
"""

PAGES_FIELD_WIDTH = 8
IDX_FIELD_WIDTH = 5


class StatIdx(IntEnum):
    TOTAL = 0
    ACTIVE = 1
    INACTIVE = 2
    DIRTY = 3


def get_prog():
    if "prog" in globals():
        return globals()["prog"]
    program = Program()
    program.set_kernel()
    return program


def err(msg):
    print(f"memcg_cacheinfo.py: error: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


def format_table_header(path_type):
    cols = ["#" + "idx".rjust(IDX_FIELD_WIDTH-1)]
    cols += [s.rjust(PAGES_FIELD_WIDTH) for s in ("cache", "active", "inactive", "dirty")]
    return " ".join(cols) + f"  {path_type}"


def format_data_row(row_idx, total, active, inactive, dirty, path):
    idx_width = IDX_FIELD_WIDTH
    field_width = PAGES_FIELD_WIDTH
    idx_col = f"{int(row_idx):>{idx_width}d}"
    nums = " ".join(f"{int(value):>{field_width}d}" for value in (total, active, inactive, dirty))
    return f"{idx_col} {nums}  {path}"


def _first_present(stat_map, *keys):
    for key in keys:
        value = stat_map.get(key)
        if value is not None:
            return value
    return None


def cgroup_cache_summary_pages(stat_map, page_size, local=True):
    if not local:
        # cgroup v2 memory.stat does not have total_* fields.
        # In that case, use local counters from the same cgroup.
        total_bytes = _first_present(stat_map, "total_cache", "file")
        active_bytes = _first_present(stat_map, "total_active_file", "active_file")
        inactive_bytes = _first_present(stat_map, "total_inactive_file", "inactive_file")
        dirty_bytes = _first_present(stat_map, "total_dirty", "file_dirty")
    else:
        total_bytes = _first_present(stat_map, "file", "cache")
        active_bytes = stat_map.get("active_file")
        inactive_bytes = stat_map.get("inactive_file")
        dirty_bytes = _first_present(stat_map, "file_dirty", "dirty")
    if total_bytes is None:
        return None
    return (
        total_bytes // page_size,
        (active_bytes // page_size) if active_bytes is not None else 0,
        (inactive_bytes // page_size) if inactive_bytes is not None else 0,
        (dirty_bytes // page_size) if dirty_bytes is not None else 0,
    )


def print_cache_summary_from_memory_stat(cgroup_path, page_size, local):
    stat_path = os.path.join(cgroup_path, "memory.stat")
    try:
        _, stat_map = read_memory_stat_map(cgroup_path)
        summary = cgroup_cache_summary_pages(stat_map, page_size, local=local)
        if summary is None:
            print(f"memory.stat: no usable counters at {stat_path}")
            return
        cache_total, active_file, inactive_file, dirty = summary
        prefix = "Local" if local else "Total"
        print(f"{prefix}: cache {cache_total}, active_file {active_file}, "
            f"inactive_file {inactive_file}, dirty {dirty}\n")
    except FileNotFoundError:
        print(f"memory.stat: file not found: {stat_path}")
    except Exception as exc:
        print(f"memory.stat: could not read {stat_path} ({exc}).")


def print_ranked_rows(rows, header, row_formatter, top):
    print(header)
    ranked = sorted(rows, key=lambda row: row[1][StatIdx.TOTAL], reverse=True)[: max(top, 0)]
    for row_idx, (path, (total, active, inactive, dirty)) in enumerate(
        ranked, start=1
    ):
        print(row_formatter(row_idx, total, active, inactive, dirty, path))


def collect_cgroup_rows(root_path, page_size):
    rows = []
    for cg_path in iter_memory_cgroups_recursive(root_path):
        try:
            _, stat_map = read_memory_stat_map(cg_path)
            summary = cgroup_cache_summary_pages(stat_map, page_size, local=True)
            if summary is not None:
                rows.append((cg_path, summary))
        except Exception:
            continue
    return rows


def iter_memory_cgroups_recursive(root_path):
    def walk(path):
        if os.path.isfile(os.path.join(path, "memory.stat")):
            yield path
        try:
            names = sorted(os.listdir(path))
        except OSError as exc:
            if path == root_path:
                err(f"cannot list cgroup directory {root_path}: {exc}")
            return
        for name in names:
            child = os.path.join(path, name)
            if os.path.isdir(child):
                yield from walk(child)

    yield from walk(root_path)


def get_page_size(prog):
    try:
        return int(prog.constant("PAGE_SIZE"))
    except Exception:
        return os.sysconf("SC_PAGE_SIZE")


def read_memory_stat_map(cgroup_path):
    """Parse memory.stat into key -> value (as reported by the kernel, usually bytes)."""
    stat_path = os.path.join(cgroup_path, "memory.stat")
    out = {}
    with open(stat_path, "r", encoding="utf-8") as file:
        for line in file:
            key, _, value = line.partition(" ")
            key = key.strip()
            if not key:
                continue
            try:
                out[key] = int(value.strip())
            except ValueError:
                continue
    return stat_path, out


def detect_memcg_mountpoint():
    v2_mount = None
    with open("/proc/self/mountinfo", "r", encoding="utf-8") as file:
        for line in file:
            parts = line.strip().split()
            if len(parts) < 10:
                continue
            sep = parts.index("-")
            mount_point = parts[4]
            fs_type = parts[sep + 1]
            super_opts = parts[sep + 3] if len(parts) > sep + 3 else ""
            if fs_type == "cgroup" and "memory" in super_opts.split(","):
                return mount_point
            if fs_type == "cgroup2":
                v2_mount = mount_point
    return v2_mount or "/sys/fs/cgroup"


def resolve_cgroup_path(arg_path, mount_root):
    if arg_path is None:
        return None
    if os.path.isabs(arg_path):
        return arg_path
    return os.path.join(mount_root, arg_path.lstrip("/"))


def page_memcg_addr(page, flags_mask):
    try:
        memcg_data = int(page.memcg_data.value_())
    except Exception:
        return 0
    if memcg_data == 0:
        return 0
    return memcg_data & ~flags_mask


def page_mapping(page, prog):
    try:
        ptr = int(page.mapping.value_())
        if ptr == 0:
            return None
        if ptr & 0x1:
            return None
        return Object(prog, "struct address_space *", value=ptr & ~0x7)
    except Exception:
        return None


def dentry_path(dentry):
    names = []
    cur = dentry
    for _ in range(512):
        try:
            name = cur.d_name.name.string_().decode("utf-8", errors="replace")
        except Exception:
            name = "?"
        if name and name != "/":
            names.append(name)
        if int((parent := cur.d_parent).value_()) == int(cur.value_()):
            break
        cur = parent
    if not names:
        return "/"
    names.reverse()
    return "/" + "/".join(names)


def inode_path(inode):
    try:
        first = inode.i_dentry.first
        if int(first.value_()) == 0:
            return None
        try:
            dentry = container_of(first, "struct dentry", "d_u.d_alias")
        except Exception:
            dentry = container_of(first, "struct dentry", "d_alias")
        return dentry_path(dentry)
    except Exception:
        return None


def page_head_for_pg_flags(page):
    """Return the page whose flags field holds PG_* for this physical page (compound tail -> head)."""
    if PageTail is not None and compound_head is not None:
        try:
            if PageTail(page):
                return compound_head(page)
        except Exception:
            pass
    return page


def page_flag_is_set(prog, page, helper_fn, state):
    """Check PG_* on page/head via helper first, then by constant bit."""
    if helper_fn is not None:
        try:
            return bool(helper_fn(page))
        except Exception:
            pass
    try:
        bit = 1 << int(prog.constant(state))
        return (int(page.flags.value_()) & bit) != 0
    except Exception:
        return False


def update_file_page_stats(prog, stats, path, head):
    """stats[path] = [total, active, inactive, dirty]."""
    cnt = stats[path]
    cnt[StatIdx.TOTAL] += 1
    if page_flag_is_set(prog, head, PageActive, "PG_active"):
        cnt[StatIdx.ACTIVE] += 1
    else:
        cnt[StatIdx.INACTIVE] += 1
    if page_flag_is_set(prog, head, PageDirty, "PG_dirty"):
        cnt[StatIdx.DIRTY] += 1


def collect_file_cache_by_filename(prog, target_memcg_id):
    stats = defaultdict(lambda: [0, 0, 0, 0])
    unknown = [0, 0, 0, 0]
    try:
        flags_mask = int(prog.constant("__NR_MEMCG_DATA_FLAGS")) - 1
    except Exception:
        flags_mask = 0x3

    try:
        for page in for_each_page(prog):
            try:
                memcg_addr = page_memcg_addr(page, flags_mask)
                if memcg_addr == 0:
                    continue
                memcg = Object(prog, "struct mem_cgroup *", value=memcg_addr)
                if int(memcg.css.cgroup.kn.id.value_()) != target_memcg_id:
                    continue
                mapping = page_mapping(page, prog)
                if mapping is None:
                    continue
                inode = mapping.host
                if int(inode.value_()) == 0:
                    continue
                head = page_head_for_pg_flags(page)
                path = inode_path(inode)
                if path is None:
                    unknown[StatIdx.TOTAL] += 1
                    if page_flag_is_set(prog, head, PageActive, "PG_active"):
                        unknown[StatIdx.ACTIVE] += 1
                    else:
                        unknown[StatIdx.INACTIVE] += 1
                    if page_flag_is_set(prog, head, PageDirty, "PG_dirty"):
                        unknown[StatIdx.DIRTY] += 1
                else:
                    update_file_page_stats(prog, stats, path, head)
            except FaultError:
                continue
            except Exception:
                unknown[StatIdx.TOTAL] += 1
                try:
                    head = page_head_for_pg_flags(page)
                    if page_flag_is_set(prog, head, PageActive, "PG_active"):
                        unknown[StatIdx.ACTIVE] += 1
                    else:
                        unknown[StatIdx.INACTIVE] += 1
                    if page_flag_is_set(prog, head, PageDirty, "PG_dirty"):
                        unknown[StatIdx.DIRTY] += 1
                except Exception:
                    unknown[StatIdx.INACTIVE] += 1
    except ObjectNotFoundError as e:
        raise RuntimeError(
            f"drgn cannot iterate pages on this kernel/debug setup "
            f"(missing memory model symbols like mem_section/vmemmap/mem_map): {e}"
        ) from e

    if unknown[StatIdx.TOTAL]:
        stats["[unknown]"] = unknown
    return stats


def main():
    prog = get_prog()

    parser = argparse.ArgumentParser(
        description=DESC, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('cgroup',
        help='Target memory cgroup path (e.g. /sys/fs/cgroup/memory/mygroup)')
    parser.add_argument('--by-cgroup', action='store_true',
        help='Report file cache stats by cgroup')
    parser.add_argument('--top', type=int, default=50,
        help='Show top N rows by page cache total (default: 50)')
    args = parser.parse_args()

    target = resolve_cgroup_path(args.cgroup, detect_memcg_mountpoint())
    if not os.path.isdir(target):
        err(f"cgroup path is not a directory or does not exist: {target}")

    page_size = get_page_size(prog)

    if args.by_cgroup:
        print(f"Memcg: {target}")
        print_cache_summary_from_memory_stat(target, page_size, local=False)
        rows = collect_cgroup_rows(target, page_size)
        if not rows:
            print("no cgroups with readable memory.stat under this path.")
        cgroup_header = format_table_header("cgroup_path")
        print_ranked_rows(rows, cgroup_header, format_data_row, args.top)
        return

    try:
        cgroup_id = os.stat(target).st_ino
    except FileNotFoundError:
        err(f"cgroup path not found: {target}")

    try:
        file_stats = collect_file_cache_by_filename(prog, cgroup_id)
    except (RuntimeError, ObjectNotFoundError) as exc:
        err(str(exc))
    print(f"Memcg: {target}")
    print_cache_summary_from_memory_stat(target, page_size, local=True)
    file_header = format_table_header("file_path")
    print_ranked_rows(file_stats.items(), file_header, format_data_row, args.top)


if __name__ == "__main__":
    main()
