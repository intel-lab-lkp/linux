#!/usr/bin/env drgn
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2025 Ye Liu <liuye@kylinos.cn>

import argparse
from drgn import Object
from drgn.helpers.linux import find_task, follow_page, page_size
from drgn.helpers.linux.mm import (
    decode_page_flags, page_to_pfn, page_to_phys, page_to_virt, vma_find,
    PageSlab, PageCompound, PageHead, PageTail, compound_head, compound_order, compound_nr
)
from drgn.helpers.linux.cgroup import cgroup_name, cgroup_path

DESC = """
This is a drgn script to show the page state.
For more info on drgn, visit https://github.com/osandov/drgn.
"""

MEMCG_DATA_OBJEXTS = 1 << 0
MEMCG_DATA_KMEM = 1 << 1
__NR_MEMCG_DATA_FLAGS = 1 << 2

def format_page_data(data):
    """Format raw page data into a readable hex dump."""
    chunks = [data[i:i+8] for i in range(0, len(data), 8)]
    hex_chunks = ["".join(f"{b:02x}" for b in chunk[::-1]) for chunk in chunks]
    lines = [" ".join(hex_chunks[i:i+4]) for i in range(0, len(hex_chunks), 4)]
    return "\n".join(f"raw: {line}" for line in lines)

def get_memcg_info(page):
    """Retrieve memory cgroup information for a page."""
    memcg_data = page.memcg_data.value_()
    if memcg_data & MEMCG_DATA_OBJEXTS:
        memcg_value = 0
    elif memcg_data & MEMCG_DATA_KMEM:
        objcg = Object(prog, "struct obj_cgroup *", address=memcg_data & ~__NR_MEMCG_DATA_FLAGS)
        memcg_value = objcg.memcg.value_()
    else:
        memcg_value = memcg_data & ~__NR_MEMCG_DATA_FLAGS

    memcg = Object(prog, "struct mem_cgroup *", address=memcg_value)
    cgrp = memcg.css.cgroup
    return cgroup_name(cgrp).decode(), f"/sys/fs/cgroup/memory{cgroup_path(cgrp).decode()}"

def show_page_state(page, addr, mm, pid, task):
    """Display detailed information about a page."""
    print(f'PID : {pid} Comm : {task.comm.string_().decode()} mm : {hex(mm)}')
    print(f'User Virtual Address : {hex(addr)}')
    print(f'Page Address    : {hex(page.value_())}')

    print(format_page_data(prog.read(page.value_(), 64)))

    print(f'Page Flags      : {decode_page_flags(page)}')
    print(f'Page Size       : {page_size(page).value_()}')
    print(f'Page PFN        : {hex(page_to_pfn(page).value_())}')
    print(f'Page Physical   : {hex(page_to_phys(page).value_())}')
    print(f'Page Virtual    : {hex(page_to_virt(page).value_())}')
    print(f'Page Refcount   : {page._refcount.counter.value_()}')
    print(f'Page Mapcount   : {page._mapcount.counter.value_()}')
    print(f'Page Index      : {hex(page.index.value_())}')
    print(f'Page Memcg Data : {hex(page.memcg_data.value_())}')

    memcg_name, memcg_path = get_memcg_info(page)
    print(f'Memcg Name      : {memcg_name}')
    print(f'Memcg Path      : {memcg_path}')

    print(f'Page Mapping    : {hex(page.mapping.value_())}')
    print(f'Page Anon/File  : {"Anon" if page.mapping.value_() & 0x1 else "File"}')

    vma = vma_find(mm, addr)
    print(f'Page VMA        : {hex(vma.value_())}')
    print(f'VMA Start       : {hex(vma.vm_start.value_())}')
    print(f'VMA End         : {hex(vma.vm_end.value_())}')

    if PageSlab(page):
        print("This page belongs to the slab allocator.")

    if PageCompound(page):
        print("This page is part of a compound page.")
        if PageHead(page):
            print("This page is the head page of a compound page.")
        if PageTail(page):
            print("This page is the tail page of a compound page.")
        print(f'Head Page       : {hex(compound_head(page).value_())}')
        print(f'Compound Order  : {compound_order(page).value_()}')
        print(f'Number of Pages : {compound_nr(page).value_()}')
    else:
        print("This page is not part of a compound page.")

def main():
    """Main function to parse arguments and display page state."""
    parser = argparse.ArgumentParser(description=DESC, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('pid', metavar='PID', type=int, help='Target process ID (PID)')
    parser.add_argument('vaddr', metavar='VADDR', type=str, help='Target virtual address in hexadecimal format (e.g., 0x7fff1234abcd)')
    args = parser.parse_args()

    try:
        vaddr = int(args.vaddr, 16)
    except ValueError:
        print(f"Error: Invalid virtual address format: {args.vaddr}")
        return

    task = find_task(args.pid)
    mm = task.mm
    page = follow_page(mm, vaddr)

    if page:
        show_page_state(page, vaddr, mm, args.pid, task)
    else:
        print(f"Address {hex(vaddr)} is not mapped.")

if __name__ == "__main__":
    main()

