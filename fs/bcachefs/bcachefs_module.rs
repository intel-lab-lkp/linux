// SPDX-License-Identifier: GPL-2.0

//! bcachefs
//!
//! Rust kernel module for bcachefs.

pub mod bindings;

use kernel::prelude::*;

use crate::bindings::*;

module! {
    type: Bcachefs,
    name: "bcachefs",
    author: "Kent Overstreet <kent.overstreet@gmail.com>",
    description: "bcachefs filesystem",
    license: "GPL",
}

struct Bcachefs;

impl kernel::Module for Bcachefs {
    #[link_section = ".init.text"]
    fn init(_module: &'static ThisModule) -> Result<Self> {
        // SAFETY: this block registers the bcachefs services with the kernel. After succesful
        // registration, all such services are guaranteed by the kernel to exist as long as the
        // driver is loaded. In the event of any failure in the registration, all registered
        // services are unregistered.
        unsafe {
            bch2_bkey_pack_test();

            if bch2_kset_init() != 0
                || bch2_btree_key_cache_init() != 0
                || bch2_chardev_init() != 0
                || bch2_vfs_init() != 0
                || bch2_debug_init() != 0
            {
                __drop();
                return Err(ENOMEM);
            }
        }

        Ok(Bcachefs)
    }
}

fn __drop() {
    // SAFETY: The kernel does not allow cleanup_module() (which results in
    // drop()) to be called unless there are no users of the filesystem.
    // The *_exit() functions only free data that they confirm is allocated, so
    // this is safe to call even if the module's init() function did not finish.
    unsafe {
        bch2_debug_exit();
        bch2_vfs_exit();
        bch2_chardev_exit();
        bch2_btree_key_cache_exit();
        bch2_kset_exit();
    }
}

impl Drop for Bcachefs {
    fn drop(&mut self) {
        __drop();
    }
}
