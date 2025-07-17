// SPDX-License-Identifier: GPL-2.0

// Copyright (c) 2025, Kylin Software

//! Rust alloc sample.

use kernel::bindings;
use kernel::prelude::*;

module! {
    type: RustAlloc,
    name: "rust_alloc",
    authors: ["Rust for Linux Contributors"],
    description: "Rust alloc sample",
    license: "GPL",
}

const VBOX_SIZE: usize = 1024;
const VBOX_LARGE_ALIGN: usize = bindings::PAGE_SIZE * 4;
const KVEC_VAL: [usize; 3] = [10, 20, 30];

#[repr(align(128))]
struct VboxBlob([u8; VBOX_SIZE]);

// This structure is used to test the allocation of alignments larger
// than PAGE_SIZE.
// Since this is not yet supported, avoid accessing the contents of
// the structure for now.
#[allow(dead_code)]
#[repr(align(8192))]
struct VboxLargeAlignBlob([u8; VBOX_LARGE_ALIGN]);

struct RustAlloc {
    vbox_blob: VBox<VboxBlob>,
    kvec_blob: KVec<usize>,
}

fn check_align(addr: usize, align: usize) -> bool {
    debug_assert!(align.is_power_of_two());
    if addr & (align - 1) != 0 {
        pr_err!("Address {:#x} is not aligned with {:#x}.\n", addr, align);
        false
    } else {
        true
    }
}

impl kernel::Module for RustAlloc {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust allocator sample (init)\n");

        let vbox_blob = VBox::<VboxBlob>::new_uninit(GFP_KERNEL)?;
        if !check_align(vbox_blob.as_ptr() as usize, 128) {
            return Err(EINVAL);
        }
        let vbox_blob = vbox_blob.write(VboxBlob([0xfeu8; VBOX_SIZE]));

        if let Ok(_) = VBox::<VboxLargeAlignBlob>::new_uninit(GFP_KERNEL) {
            pr_err!("Allocations for VBox with alignments larger than PAGE_SIZE should fail, but here it succeeded.\n");
            return Err(EINVAL);
        }

        let mut kvec_blob = KVec::new();
        kvec_blob.extend_from_slice(&KVEC_VAL, GFP_KERNEL)?;

        Ok(Self {
            vbox_blob,
            kvec_blob,
        })
    }
}

impl Drop for RustAlloc {
    fn drop(&mut self) {
        pr_info!("Rust allocator sample (exit)\n");

        // check the values
        for b in self.vbox_blob.0.as_slice().iter() {
            if *b != 0xfeu8 {
                pr_err!("vbox_blob contains wrong values\n");
            }
        }

        if self.kvec_blob.as_slice() != KVEC_VAL {
            pr_err!("kvec_blob contains wrong values\n");
        }
    }
}
