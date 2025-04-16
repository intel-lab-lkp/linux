// SPDX-License-Identifier: GPL-2.0

//! Rust example LSM.
//!
//! Rust-based example LSM to demonstrate an initial version of bindings to C.

mod bindings;

use kernel::{fs::LocalFile, prelude::*};

/// Prefix to appear before log messages printed from within this crate.
const __LOG_PREFIX: &[u8] = b"rust_lsm\0";

/// LSM init function.
///
/// Called exactly once from `init()` in [`bindings`] when the LSM is registered.
fn init() -> Result {
    // Custom init logic could go here
    Ok(())
}

/// `file_permission` hook implementation.
///
/// Gets called for every file access that needs to be checked.
fn file_permission(_file: &LocalFile, _mask: i32) -> Result<bool> {
    // Arbitrary policy logic could go here
    let resolution = true;

    // Uncomment this for lots of spam in dmesg
    // pr_info!("file_permission hook called");

    Ok(resolution)
}
