// SPDX-License-Identifier: GPL-2.0

//! DRM buddy allocator bindings.
//!
//! C header: [`include/drm/drm_buddy.h`](srctree/include/drm/drm_buddy.h)
//!
//! This module provides Rust abstractions over the Linux kernel's DRM buddy
//! allocator, which implements a binary buddy memory allocation system.
//!
//! The buddy allocator manages a contiguous address space and allocates blocks
//! in power-of-two sizes. It's commonly used for physical memory management.
//!
//! # Examples
//!
//! ```ignore
//! use kernel::{
//!     drm::buddy::{BuddyFlags, DrmBuddy},
//!     prelude::*,
//!     sizes::*, //
//! };
//!
//! let buddy = DrmBuddy::new(SZ_1G, SZ_4K)?;
//! let allocated = buddy.alloc_blocks(
//!     0, 0, SZ_16M, SZ_4K,
//!     BuddyFlags::RANGE_ALLOCATION,
//!     GFP_KERNEL,
//! )?;
//!
//! for block in &allocated {
//!     // Use block.
//! }
//! // Blocks are automatically freed when `allocated` goes out of scope.
//! ```

use crate::{
    alloc::Flags,
    bindings,
    clist,
    container_of,
    error::{
        to_result,
        Result, //
    },
    prelude::KBox,
    types::Opaque, //
};
use core::ptr::NonNull;

/// Flags for DRM buddy allocator operations.
///
/// These flags control the allocation behavior of the buddy allocator.
#[derive(Clone, Copy, PartialEq)]
pub struct BuddyFlags(u64);

impl BuddyFlags {
    /// Range-based allocation from start to end addresses.
    pub const RANGE_ALLOCATION: BuddyFlags =
        BuddyFlags(bindings::DRM_BUDDY_RANGE_ALLOCATION as u64);

    /// Allocate from top of address space downward.
    pub const TOPDOWN_ALLOCATION: BuddyFlags =
        BuddyFlags(bindings::DRM_BUDDY_TOPDOWN_ALLOCATION as u64);

    /// Allocate physically contiguous blocks.
    pub const CONTIGUOUS_ALLOCATION: BuddyFlags =
        BuddyFlags(bindings::DRM_BUDDY_CONTIGUOUS_ALLOCATION as u64);

    /// Clear allocated blocks (zero them).
    pub const CLEAR_ALLOCATION: BuddyFlags =
        BuddyFlags(bindings::DRM_BUDDY_CLEAR_ALLOCATION as u64);

    /// Block has been cleared - internal flag.
    pub const CLEARED: BuddyFlags = BuddyFlags(bindings::DRM_BUDDY_CLEARED as u64);

    /// Disable trimming of partially used blocks.
    pub const TRIM_DISABLE: BuddyFlags = BuddyFlags(bindings::DRM_BUDDY_TRIM_DISABLE as u64);

    /// Get raw value for FFI.
    pub(crate) fn as_raw(self) -> u64 {
        self.0
    }
}

impl core::ops::BitOr for BuddyFlags {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

/// DRM buddy allocator instance.
///
/// This structure wraps the C `drm_buddy` allocator.
///
/// # Safety
///
/// Not thread-safe. Concurrent alloc/free operations require external
/// synchronization (e.g., wrapping in `Arc<Mutex<DrmBuddy>>`).
///
/// # Invariants
///
/// - `mm` is initialized via `drm_buddy_init()` and remains valid until Drop.
pub struct DrmBuddy {
    mm: Opaque<bindings::drm_buddy>,
}

impl DrmBuddy {
    /// Create a new buddy allocator.
    ///
    /// Creates a buddy allocator that manages a contiguous address space of the given
    /// size, with the specified minimum allocation unit (chunk_size must be at least 4KB).
    ///
    /// # Examples
    ///
    /// See the complete example in the documentation comments for [`AllocatedBlocks`].
    pub fn new(size: usize, chunk_size: usize) -> Result<Self> {
        // Create buddy allocator with zeroed memory.
        let buddy = Self {
            mm: Opaque::zeroed(),
        };

        // Initialize the C buddy structure.
        // SAFETY: buddy.mm points to valid, zeroed memory.
        unsafe {
            to_result(bindings::drm_buddy_init(
                buddy.mm.get(),
                size as u64,
                chunk_size as u64,
            ))?;
        }

        Ok(buddy)
    }

    /// Get a raw pointer to the underlying C drm_buddy structure.
    ///
    /// # Safety
    ///
    /// Caller must ensure the returned pointer is not used after this
    /// structure is dropped.
    pub unsafe fn as_raw(&self) -> *mut bindings::drm_buddy {
        self.mm.get()
    }

    /// Get the chunk size (minimum allocation unit).
    pub fn chunk_size(&self) -> u64 {
        // SAFETY: mm is initialized and valid per struct invariant.
        unsafe { (*self.as_raw()).chunk_size }
    }

    /// Get the total managed size.
    pub fn size(&self) -> u64 {
        // SAFETY: mm is initialized and valid per struct invariant.
        unsafe { (*self.as_raw()).size }
    }

    /// Get the available (free) memory.
    pub fn avail(&self) -> u64 {
        // SAFETY: mm is initialized and valid per struct invariant.
        unsafe { (*self.as_raw()).avail }
    }

    /// Allocate blocks from the buddy allocator.
    ///
    /// Returns an [`AllocatedBlocks`] structure that owns the allocated blocks and automatically
    /// frees them when dropped. Allocation of `list_head` uses the `gfp` flags passed.
    pub fn alloc_blocks(
        &self,
        start: usize,
        end: usize,
        size: usize,
        min_block_size: usize,
        flags: BuddyFlags,
        gfp: Flags,
    ) -> Result<AllocatedBlocks<'_>> {
        // Allocate list_head on the heap.
        let mut list_head = KBox::new(bindings::list_head::default(), gfp)?;

        // SAFETY: list_head is valid and heap-allocated.
        unsafe {
            bindings::INIT_LIST_HEAD(&mut *list_head as *mut _);
        }

        // SAFETY: mm is a valid DrmBuddy object per the type's invariants.
        unsafe {
            to_result(bindings::drm_buddy_alloc_blocks(
                self.as_raw(),
                start as u64,
                end as u64,
                size as u64,
                min_block_size as u64,
                &mut *list_head as *mut _,
                flags.as_raw() as usize,
            ))?;
        }

        // `list_head` is now the head of a list that contains allocated blocks
        // from C code. The allocated blocks will be automatically freed when
        // `AllocatedBlocks` is dropped.
        Ok(AllocatedBlocks {
            list_head,
            buddy: self,
        })
    }
}

impl Drop for DrmBuddy {
    fn drop(&mut self) {
        // SAFETY: self.mm is initialized and valid. drm_buddy_fini properly
        // cleans up all resources. This is called exactly once during Drop.
        unsafe {
            bindings::drm_buddy_fini(self.as_raw());
        }
    }
}

// SAFETY: DrmBuddy can be sent between threads. Caller is responsible for
// ensuring thread-safe access if needed (e.g., via Mutex).
unsafe impl Send for DrmBuddy {}

/// Allocated blocks from the buddy allocator with automatic cleanup.
///
/// This structure owns a list of allocated blocks and ensures they are
/// automatically freed when dropped. Blocks may be iterated over and are
/// read-only after allocation (iteration via [`IntoIterator`] and
/// automatic cleanup via [`Drop`] only). To share across threads, wrap
/// in `Arc<AllocatedBlocks>`. Rust owns the head list head of the
/// allocated blocks; C allocates blocks and links them to the head
/// list head. Clean up of the allocated blocks is handled by C code.
///
/// # Invariants
///
/// - `list_head` is an owned, valid, initialized list_head.
/// - `buddy` points to a valid, initialized [`DrmBuddy`].
pub struct AllocatedBlocks<'a> {
    list_head: KBox<bindings::list_head>,
    buddy: &'a DrmBuddy,
}

impl Drop for AllocatedBlocks<'_> {
    fn drop(&mut self) {
        // Free all blocks automatically when dropped.
        // SAFETY: list_head is a valid list of blocks per the type's invariants.
        unsafe {
            bindings::drm_buddy_free_list(self.buddy.as_raw(), &mut *self.list_head as *mut _, 0);
        }
    }
}

impl<'a> AllocatedBlocks<'a> {
    /// Check if the block list is empty.
    pub fn is_empty(&self) -> bool {
        // SAFETY: list_head is a valid list of blocks per the type's invariants.
        unsafe { clist::list_empty(&*self.list_head as *const _) }
    }

    /// Iterate over allocated blocks.
    pub fn iter(&self) -> clist::ClistIter<'_, Block> {
        // SAFETY: list_head is a valid list of blocks per the type's invariants.
        clist::iter_list_head::<Block>(&*self.list_head)
    }
}

/// Iteration support for allocated blocks.
///
/// # Examples
///
/// ```ignore
/// for block in &allocated_blocks {
///     // Use block.
/// }
/// ```
impl<'a> IntoIterator for &'a AllocatedBlocks<'_> {
    type Item = Block;
    type IntoIter = clist::ClistIter<'a, Block>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

/// A DRM buddy block.
///
/// Wraps a pointer to a C `drm_buddy_block` structure. This is returned
/// from allocation operations and used to free blocks.
///
/// # Invariants
///
/// `drm_buddy_block_ptr` points to a valid `drm_buddy_block` managed by the buddy allocator.
pub struct Block {
    drm_buddy_block_ptr: NonNull<bindings::drm_buddy_block>,
}

impl Block {
    /// Get the block's offset in the address space.
    pub fn offset(&self) -> u64 {
        // SAFETY: drm_buddy_block_ptr is valid per the type's invariants.
        unsafe { bindings::drm_buddy_block_offset(self.drm_buddy_block_ptr.as_ptr()) }
    }

    /// Get the block order (size = chunk_size << order).
    pub fn order(&self) -> u32 {
        // SAFETY: drm_buddy_block_ptr is valid per the type's invariants.
        unsafe { bindings::drm_buddy_block_order(self.drm_buddy_block_ptr.as_ptr()) }
    }

    /// Get the block's size in bytes.
    ///
    /// Requires the buddy allocator to calculate size from order.
    pub fn size(&self, buddy: &DrmBuddy) -> u64 {
        // SAFETY: Both pointers are valid per the type's invariants.
        unsafe { bindings::drm_buddy_block_size(buddy.as_raw(), self.drm_buddy_block_ptr.as_ptr()) }
    }

    /// Get a raw pointer to the underlying C block.
    ///
    /// # Safety
    ///
    /// Caller must ensure the pointer is not used after the block is freed.
    pub unsafe fn as_ptr(&self) -> *mut bindings::drm_buddy_block {
        self.drm_buddy_block_ptr.as_ptr()
    }
}

impl clist::FromListHead for Block {
    unsafe fn from_list_head(link: *const bindings::list_head) -> Self {
        // SAFETY: link points to a valid list_head embedded in drm_buddy_block.
        // The container_of macro calculates the containing struct pointer.
        // We need to account for the union field __bindgen_anon_1.link.
        //
        // The link is embedded in a union within drm_buddy_block:
        //     struct drm_buddy_block {
        //         [...]
        //         union {
        //             struct rb_node rb;
        //             struct list_head link;
        //         };
        //     }
        //
        // This is why we perform a double container_of calculation: first to get
        // to the union, then to get to the containing drm_buddy_block.
        unsafe {
            // First get to the union.
            let union_ptr = container_of!(link, bindings::drm_buddy_block__bindgen_ty_1, link);
            // Then get to the containing drm_buddy_block.
            let block_ptr =
                container_of!(union_ptr, bindings::drm_buddy_block, __bindgen_anon_1) as *mut _;
            Block {
                drm_buddy_block_ptr: NonNull::new_unchecked(block_ptr),
            }
        }
    }
}

// SAFETY: Block is just a pointer wrapper and can be safely sent between threads.
unsafe impl Send for Block {}
