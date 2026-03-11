// SPDX-License-Identifier: GPL-2.0

//! Rust LSM sample — reference implementation of the kernel::lsm abstractions.
//!
//! This module demonstrates how a Linux Security Module is written in Rust.
//! It registers three hooks (file_open, task_alloc, task_free), logs each
//! invocation via pr_info!(), and allows every operation.
//!
//! It is not a policy module — it enforces nothing.  Its purpose is to:
//!
//! 1. Serve as a compile-test vehicle for the kernel::lsm abstraction layer.
//! 2. Demonstrate the API surface that upstream-bound LSMs should target.
//! 3. Provide a boot-test reference: if the kernel boots with this LSM
//!    enabled and /sys/kernel/security/lsm lists "rust_lsm_sample", the
//!    hook registration machinery is working end-to-end.
//!
//! Assisted-by: Claude:claude-sonnet-4-6

// The build system injects #![no_std] for all kernel Rust objects; do not repeat it.

// Required by pr_info! and other kernel logging macros.
const __LOG_PREFIX: &[u8] = b"rust_lsm_sample\0";

use kernel::bindings;
use kernel::lsm;
use kernel::prelude::*;

/// The Rust LSM sample implementation.
struct RustLsmSample;

impl lsm::Hooks for RustLsmSample {
    fn file_open(file: &kernel::fs::File) -> Result {
        pr_info!("rust_lsm: file_open flags={:#x}\n", file.flags());
        Ok(())
    }

    fn task_alloc(task: &kernel::task::Task, clone_flags: u64) -> Result {
        pr_info!(
            "rust_lsm: task_alloc pid={} clone_flags={:#x}\n",
            task.pid(),
            clone_flags
        );
        Ok(())
    }

    fn task_free(task: &kernel::task::Task) {
        pr_info!("rust_lsm: task_free pid={}\n", task.pid());
    }
}

// Register RustLsmSample with the kernel LSM framework.
//
// This macro generates:
//   - A static `lsm_id` identifying this module as "rust_lsm_sample".
//   - A static `security_hook_list[3]` array (filled by C shims via LSM_HOOK_INIT).
//   - An `unsafe extern "C"` init function that calls security_add_hooks().
//   - A static `lsm_info` in the `.lsm_info.init` ELF section so that
//     security_init() discovers and calls the init function at boot.
//
// LSM_ID_UNDEF is used during development.  A permanent LSM_ID_* value from
// include/uapi/linux/lsm.h will be requested as part of the upstream patch series.
kernel::define_lsm!(
    RustLsmSample,
    "rust_lsm_sample\0",
    bindings::LSM_ID_UNDEF as u64
);
