// SPDX-License-Identifier: GPL-2.0

// Copyright (c) 2025, Kylin Software

//! Rust allocator sample.

use core::{alloc::Layout, ptr::NonNull};
use kernel::alloc::allocator;
use kernel::alloc::Allocator;
use kernel::bindings;
use kernel::prelude::*;

module! {
    type: RustAllocator,
    name: "rust_allocator",
    authors: ["Rust for Linux Contributors"],
    description: "Rust allocator sample",
    license: "GPL",
}

const VMALLOC_ARG: [(usize, usize); 2] = [
    (bindings::PAGE_SIZE * 4, bindings::PAGE_SIZE * 2),
    (1024, 128),
];

struct RustAllocator {
    vmalloc_vec: KVec<(usize, Layout)>,
}

fn vmalloc_align(size: usize, align: usize) -> Result<(NonNull<[u8]>, Layout)> {
    let layout = Layout::from_size_align(size, align).map_err(|_| EINVAL)?;

    Ok((
        <allocator::Vmalloc as Allocator>::alloc(layout, GFP_KERNEL).map_err(|_| EINVAL)?,
        layout,
    ))
}

fn vfree(addr: usize, layout: Layout) {
    let vmalloc_ptr = NonNull::new(addr as *mut u8);
    if let Some(ptr) = vmalloc_ptr {
        unsafe {
            <allocator::Vmalloc as Allocator>::free(ptr, layout);
        }
    } else {
        pr_err!("Failed to vfree: pointer is null\n");
    }
}

fn check_ptr(ptr: NonNull<[u8]>, size: usize, align: usize) -> (usize, bool) {
    let current_size = unsafe { ptr.as_ref().len() };
    if current_size != size {
        pr_err!(
            "The length to be allocated is {}, and the actually allocated memory length is {}.\n",
            size,
            current_size
        );
        return (0, false);
    }

    let addr = ptr.cast::<u8>().as_ptr() as usize;
    debug_assert!(align.is_power_of_two());
    if addr & (align - 1) != 0 {
        pr_err!("Address {:#x} is not aligned with {:#x}.\n", addr, align);
        return (0, false);
    }

    (addr, true)
}

fn clear_vmalloc_vec(v: &KVec<(usize, Layout)>) {
    for (addr, layout) in v {
        vfree(*addr, *layout);
    }
}

impl kernel::Module for RustAllocator {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust allocator sample (init)\n");

        let mut vmalloc_vec = KVec::new();
        for (size, align) in VMALLOC_ARG {
            let (ptr, layout) = vmalloc_align(size, align)?;

            let (addr, is_ok) = check_ptr(ptr, size, align);
            if !is_ok {
                clear_vmalloc_vec(&vmalloc_vec);
                return Err(EINVAL);
            }

            vmalloc_vec.push((addr, layout), GFP_KERNEL)?;
        }

        Ok(RustAllocator { vmalloc_vec })
    }
}

impl Drop for RustAllocator {
    fn drop(&mut self) {
        pr_info!("Rust allocator sample (exit)\n");

        clear_vmalloc_vec(&self.vmalloc_vec);
    }
}
