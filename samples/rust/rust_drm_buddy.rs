// SPDX-License-Identifier: GPL-2.0

//! Rust DRM buddy allocator sample.
//!
//! This sample demonstrates using the DRM buddy allocator from Rust.

use kernel::{
    drm::buddy::{
        BuddyFlags,
        DrmBuddy, //
    },
    prelude::*,
    sizes::*, //
};

module! {
    type: RustDrmBuddySample,
    name: "rust_drm_buddy",
    authors: ["Joel Fernandes"],
    description: "DRM buddy allocator sample",
    license: "GPL",
}

struct RustDrmBuddySample;

impl kernel::Module for RustDrmBuddySample {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        // Create a buddy allocator managing 1GB with 4KB chunks.
        let buddy = DrmBuddy::new(SZ_1G, SZ_4K)?;

        pr_info!("=== Test 1: Single 16MB block ===\n");
        let allocated = buddy.alloc_blocks(
            0,
            0,
            SZ_16M,
            SZ_4K,
            BuddyFlags::RANGE_ALLOCATION,
            GFP_KERNEL,
        )?;

        let mut count = 0;
        for block in &allocated {
            pr_info!(
                "  Block {}: offset=0x{:x}, order={}, size={}\n",
                count,
                block.offset(),
                block.order(),
                block.size(&buddy)
            );
            count += 1;
        }
        pr_info!("  Total: {} blocks\n", count);
        drop(allocated);

        pr_info!("=== Test 2: Three 4MB blocks ===\n");
        let allocated = buddy.alloc_blocks(
            0,
            0,
            SZ_4M * 3,
            SZ_4K,
            BuddyFlags::RANGE_ALLOCATION,
            GFP_KERNEL,
        )?;

        count = 0;
        for block in &allocated {
            pr_info!(
                "  Block {}: offset=0x{:x}, order={}, size={}\n",
                count,
                block.offset(),
                block.order(),
                block.size(&buddy)
            );
            count += 1;
        }
        pr_info!("  Total: {} blocks\n", count);
        drop(allocated);

        pr_info!("=== Test 3: Two 8MB blocks ===\n");
        let allocated = buddy.alloc_blocks(
            0,
            0,
            SZ_8M * 2,
            SZ_4K,
            BuddyFlags::RANGE_ALLOCATION,
            GFP_KERNEL,
        )?;

        count = 0;
        for block in &allocated {
            pr_info!(
                "  Block {}: offset=0x{:x}, order={}, size={}\n",
                count,
                block.offset(),
                block.order(),
                block.size(&buddy)
            );
            count += 1;
        }
        pr_info!("  Total: {} blocks\n", count);

        pr_info!("=== All tests passed! ===\n");

        Ok(RustDrmBuddySample {})
    }
}
