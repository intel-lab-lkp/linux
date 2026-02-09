// SPDX-License-Identifier: GPL-2.0

//! GPU buddy allocator bindings.
//!
//! C header: [`include/linux/gpu_buddy.h`](srctree/include/linux/gpu_buddy.h)
//!
//! This module provides Rust abstractions over the Linux kernel's GPU buddy
//! allocator, which implements a binary buddy memory allocator.
//!
//! The buddy allocator manages a contiguous address space and allocates blocks
//! in power-of-two sizes, useful for GPU physical memory management.
//!
//! # Examples
//!
//! ```
//! use kernel::{
//!     gpu::buddy::{BuddyFlags, GpuBuddy, GpuBuddyAllocParams, GpuBuddyParams},
//!     prelude::*,
//!     sizes::*, //
//! };
//!
//! // Create a 1GB buddy allocator with 4KB minimum chunk size.
//! let buddy = GpuBuddy::new(&GpuBuddyParams {
//!     base_offset_bytes: 0,
//!     physical_memory_size_bytes: SZ_1G as u64,
//!     chunk_size_bytes: SZ_4K as u64,
//! })?;
//!
//! // Verify initial state.
//! assert_eq!(buddy.size(), SZ_1G as u64);
//! assert_eq!(buddy.chunk_size(), SZ_4K as u64);
//! let initial_free = buddy.free_memory_bytes();
//!
//! // Base allocation params - mutated between calls for field overrides.
//! let mut params = GpuBuddyAllocParams {
//!     start_range_address: 0,
//!     end_range_address: 0,   // Entire range.
//!     size_bytes: SZ_16M as u64,
//!     min_block_size_bytes: SZ_16M as u64,
//!     buddy_flags: BuddyFlags::try_new(BuddyFlags::RANGE_ALLOCATION)?,
//! };
//!
//! // Test top-down allocation (allocates from highest addresses).
//! params.buddy_flags = BuddyFlags::try_new(BuddyFlags::TOPDOWN_ALLOCATION)?;
//! let topdown = buddy.alloc_blocks(&params)?;
//! assert_eq!(buddy.free_memory_bytes(), initial_free - SZ_16M as u64);
//!
//! for block in topdown.iter() {
//!     assert_eq!(block.offset(), (SZ_1G - SZ_16M) as u64);
//!     assert_eq!(block.order(), 12); // 2^12 pages
//!     assert_eq!(block.size(), SZ_16M as u64);
//! }
//! drop(topdown);
//! assert_eq!(buddy.free_memory_bytes(), initial_free);
//!
//! // Allocate 16MB - should result in a single 16MB block at offset 0.
//! params.buddy_flags = BuddyFlags::try_new(BuddyFlags::RANGE_ALLOCATION)?;
//! let allocated = buddy.alloc_blocks(&params)?;
//! assert_eq!(buddy.free_memory_bytes(), initial_free - SZ_16M as u64);
//!
//! for block in allocated.iter() {
//!     assert_eq!(block.offset(), 0);
//!     assert_eq!(block.order(), 12); // 2^12 pages
//!     assert_eq!(block.size(), SZ_16M as u64);
//! }
//! drop(allocated);
//! assert_eq!(buddy.free_memory_bytes(), initial_free);
//!
//! // Test non-contiguous allocation with fragmented memory.
//! // Create fragmentation by allocating 4MB blocks at [0,4M) and [8M,12M).
//! params.end_range_address = SZ_4M as u64;
//! params.size_bytes = SZ_4M as u64;
//! params.min_block_size_bytes = SZ_4M as u64;
//! let frag1 = buddy.alloc_blocks(&params)?;
//! assert_eq!(buddy.free_memory_bytes(), initial_free - SZ_4M as u64);
//!
//! params.start_range_address = SZ_8M as u64;
//! params.end_range_address = (SZ_8M + SZ_4M) as u64;
//! let frag2 = buddy.alloc_blocks(&params)?;
//! assert_eq!(buddy.free_memory_bytes(), initial_free - SZ_8M as u64);
//!
//! // Allocate 8MB without CONTIGUOUS - should return 2 blocks from the holes.
//! params.start_range_address = 0;
//! params.end_range_address = SZ_16M as u64;
//! params.size_bytes = SZ_8M as u64;
//! let fragmented = buddy.alloc_blocks(&params)?;
//! assert_eq!(buddy.free_memory_bytes(), initial_free - (SZ_16M) as u64);
//!
//! let (mut count, mut total) = (0u32, 0u64);
//! for block in fragmented.iter() {
//!     // The 8MB allocation should return 2 blocks, each 4MB.
//!     assert_eq!(block.size(), SZ_4M as u64);
//!     total += block.size();
//!     count += 1;
//! }
//! assert_eq!(total, SZ_8M as u64);
//! assert_eq!(count, 2);
//! drop(fragmented);
//! drop(frag2);
//! drop(frag1);
//! assert_eq!(buddy.free_memory_bytes(), initial_free);
//!
//! // Test CONTIGUOUS failure when only fragmented space available.
//! // Create a small buddy allocator with only 16MB of memory.
//! let small = GpuBuddy::new(&GpuBuddyParams {
//!     base_offset_bytes: 0,
//!     physical_memory_size_bytes: SZ_16M as u64,
//!     chunk_size_bytes: SZ_4K as u64,
//! })?;
//!
//! // Allocate 4MB blocks at [0,4M) and [8M,12M) to create fragmented memory.
//! params.start_range_address = 0;
//! params.end_range_address = SZ_4M as u64;
//! params.size_bytes = SZ_4M as u64;
//! let hole1 = small.alloc_blocks(&params)?;
//!
//! params.start_range_address = SZ_8M as u64;
//! params.end_range_address = (SZ_8M + SZ_4M) as u64;
//! let hole2 = small.alloc_blocks(&params)?;
//!
//! // 8MB contiguous should fail - only two non-contiguous 4MB holes exist.
//! params.start_range_address = 0;
//! params.end_range_address = 0;
//! params.size_bytes = SZ_8M as u64;
//! params.buddy_flags = BuddyFlags::try_new(BuddyFlags::CONTIGUOUS_ALLOCATION)?;
//! let result = small.alloc_blocks(&params);
//! assert!(result.is_err());
//! drop(hole2);
//! drop(hole1);
//!
//! # Ok::<(), Error>(())
//! ```

use crate::{
    bindings,
    clist::CListHead,
    clist_create,
    error::to_result,
    new_mutex,
    prelude::*,
    sync::{
        lock::mutex::MutexGuard,
        Arc,
        Mutex, //
    },
    types::Opaque,
};

/// Flags for GPU buddy allocator operations.
///
/// These flags control the allocation behavior of the buddy allocator.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct BuddyFlags(usize);

impl BuddyFlags {
    /// Range-based allocation from start to end addresses.
    pub const RANGE_ALLOCATION: usize = bindings::GPU_BUDDY_RANGE_ALLOCATION;

    /// Allocate from top of address space downward.
    pub const TOPDOWN_ALLOCATION: usize = bindings::GPU_BUDDY_TOPDOWN_ALLOCATION;

    /// Allocate physically contiguous blocks.
    pub const CONTIGUOUS_ALLOCATION: usize = bindings::GPU_BUDDY_CONTIGUOUS_ALLOCATION;

    /// Request allocation from the cleared (zeroed) memory. The zero'ing is not
    /// done by the allocator, but by the caller before freeing old blocks.
    pub const CLEAR_ALLOCATION: usize = bindings::GPU_BUDDY_CLEAR_ALLOCATION;

    /// Disable trimming of partially used blocks.
    pub const TRIM_DISABLE: usize = bindings::GPU_BUDDY_TRIM_DISABLE;

    /// Mark blocks as cleared (zeroed) when freeing. When set during free,
    /// indicates that the caller has already zeroed the memory.
    pub const CLEARED: usize = bindings::GPU_BUDDY_CLEARED;

    /// Create [`BuddyFlags`] from a raw value with validation.
    ///
    /// Use `|` operator to combine flags if needed, before calling this method.
    pub fn try_new(flags: usize) -> Result<Self> {
        // Flags must not exceed u32::MAX to satisfy the GPU buddy allocator C API.
        if flags > u32::MAX as usize {
            return Err(EINVAL);
        }

        // `TOPDOWN_ALLOCATION` only works without `RANGE_ALLOCATION`. When both are
        // set, `TOPDOWN_ALLOCATION` is silently ignored by the allocator. Reject this.
        if (flags & Self::RANGE_ALLOCATION) != 0 && (flags & Self::TOPDOWN_ALLOCATION) != 0 {
            return Err(EINVAL);
        }

        Ok(Self(flags))
    }

    /// Get raw value of the flags.
    pub(crate) fn as_raw(self) -> usize {
        self.0
    }
}

/// Parameters for creating a GPU buddy allocator.
pub struct GpuBuddyParams {
    /// Base offset in bytes where the managed memory region starts.
    /// Allocations will be offset by this value.
    pub base_offset_bytes: u64,
    /// Total physical memory size managed by the allocator in bytes.
    pub physical_memory_size_bytes: u64,
    /// Minimum allocation unit / chunk size in bytes, must be >= 4KB.
    pub chunk_size_bytes: u64,
}

/// Parameters for allocating blocks from a GPU buddy allocator.
pub struct GpuBuddyAllocParams {
    /// Start of allocation range in bytes. Use 0 for beginning.
    pub start_range_address: u64,
    /// End of allocation range in bytes. Use 0 for entire range.
    pub end_range_address: u64,
    /// Total size to allocate in bytes.
    pub size_bytes: u64,
    /// Minimum block size for fragmented allocations in bytes.
    pub min_block_size_bytes: u64,
    /// Buddy allocator behavior flags.
    pub buddy_flags: BuddyFlags,
}

/// Inner structure holding the actual buddy allocator.
///
/// # Synchronization
///
/// The C `gpu_buddy` API requires synchronization (see `include/linux/gpu_buddy.h`).
/// The internal [`GpuBuddyGuard`] ensures that the lock is held for all
/// allocator and free operations, preventing races between concurrent allocations
/// and the freeing that occurs when [`AllocatedBlocks`] is dropped.
///
/// # Invariants
///
/// The inner [`Opaque`] contains a valid, initialized buddy allocator.
#[pin_data(PinnedDrop)]
struct GpuBuddyInner {
    #[pin]
    inner: Opaque<bindings::gpu_buddy>,
    #[pin]
    lock: Mutex<()>,
    /// Base offset for all allocations (does not change after init).
    base_offset: u64,
    /// Cached chunk size (does not change after init).
    chunk_size: u64,
    /// Cached total size (does not change after init).
    size: u64,
}

impl GpuBuddyInner {
    /// Create a pin-initializer for the buddy allocator.
    fn new(params: &GpuBuddyParams) -> impl PinInit<Self, Error> {
        let base_offset = params.base_offset_bytes;
        let size = params.physical_memory_size_bytes;
        let chunk_size = params.chunk_size_bytes;

        try_pin_init!(Self {
            inner <- Opaque::try_ffi_init(|ptr| {
                // SAFETY: ptr points to valid uninitialized memory from the pin-init
                // infrastructure. gpu_buddy_init will initialize the structure.
                to_result(unsafe { bindings::gpu_buddy_init(ptr, size, chunk_size) })
            }),
            lock <- new_mutex!(()),
            base_offset: base_offset,
            chunk_size: chunk_size,
            size: size,
        })
    }

    /// Lock the mutex and return a guard for accessing the allocator.
    fn lock(&self) -> GpuBuddyGuard<'_> {
        GpuBuddyGuard {
            inner: self,
            _guard: self.lock.lock(),
        }
    }
}

#[pinned_drop]
impl PinnedDrop for GpuBuddyInner {
    fn drop(self: Pin<&mut Self>) {
        let guard = self.lock();

        // SAFETY: guard provides exclusive access to the allocator.
        unsafe {
            bindings::gpu_buddy_fini(guard.as_raw());
        }
    }
}

// SAFETY: [`GpuBuddyInner`] can be sent between threads.
unsafe impl Send for GpuBuddyInner {}

// SAFETY: [`GpuBuddyInner`] is `Sync` because the internal [`GpuBuddyGuard`]
// serializes all access to the C allocator, preventing data races.
unsafe impl Sync for GpuBuddyInner {}

/// Guard that proves the lock is held, enabling access to the allocator.
///
/// # Invariants
///
/// The inner `_guard` holds the lock for the duration of this guard's lifetime.
pub(crate) struct GpuBuddyGuard<'a> {
    inner: &'a GpuBuddyInner,
    _guard: MutexGuard<'a, ()>,
}

impl GpuBuddyGuard<'_> {
    /// Get a raw pointer to the underlying C `gpu_buddy` structure.
    fn as_raw(&self) -> *mut bindings::gpu_buddy {
        self.inner.inner.get()
    }
}

/// GPU buddy allocator instance.
///
/// This structure wraps the C `gpu_buddy` allocator using reference counting.
/// The allocator is automatically cleaned up when all references are dropped.
///
/// # Invariants
///
/// The inner [`Arc`] points to a valid, initialized GPU buddy allocator.
pub struct GpuBuddy(Arc<GpuBuddyInner>);

impl GpuBuddy {
    /// Create a new buddy allocator.
    ///
    /// Creates a buddy allocator that manages a contiguous address space of the given
    /// size, with the specified minimum allocation unit (chunk_size must be at least 4KB).
    pub fn new(params: &GpuBuddyParams) -> Result<Self> {
        Ok(Self(Arc::pin_init(
            GpuBuddyInner::new(params),
            GFP_KERNEL,
        )?))
    }

    /// Get the base offset for allocations.
    pub fn base_offset(&self) -> u64 {
        self.0.base_offset
    }

    /// Get the chunk size (minimum allocation unit).
    pub fn chunk_size(&self) -> u64 {
        self.0.chunk_size
    }

    /// Get the total managed size.
    pub fn size(&self) -> u64 {
        self.0.size
    }

    /// Get the available (free) memory in bytes.
    pub fn free_memory_bytes(&self) -> u64 {
        let guard = self.0.lock();
        // SAFETY: guard provides exclusive access to the allocator.
        unsafe { (*guard.as_raw()).avail }
    }

    /// Allocate blocks from the buddy allocator.
    ///
    /// Returns an [`Arc<AllocatedBlocks>`] structure that owns the allocated blocks
    /// and automatically frees them when all references are dropped.
    ///
    /// Takes `&self` instead of `&mut self` because the internal [`Mutex`] provides
    /// synchronization - no external `&mut` exclusivity needed.
    pub fn alloc_blocks(&self, params: &GpuBuddyAllocParams) -> Result<Arc<AllocatedBlocks>> {
        let buddy_arc = Arc::clone(&self.0);

        // Create pin-initializer that initializes list and allocates blocks.
        let init = try_pin_init!(AllocatedBlocks {
            buddy: Arc::clone(&buddy_arc),
            list <- CListHead::new(),
            flags: params.buddy_flags,
            _: {
                // Lock while allocating to serialize with concurrent frees.
                let guard = buddy.lock();

                // SAFETY: `guard` provides exclusive access to the buddy allocator.
                to_result(unsafe {
                    bindings::gpu_buddy_alloc_blocks(
                        guard.as_raw(),
                        params.start_range_address,
                        params.end_range_address,
                        params.size_bytes,
                        params.min_block_size_bytes,
                        list.as_raw(),
                        params.buddy_flags.as_raw(),
                    )
                })?
            }
        });

        Arc::pin_init(init, GFP_KERNEL)
    }
}

/// Allocated blocks from the buddy allocator with automatic cleanup.
///
/// This structure owns a list of allocated blocks and ensures they are
/// automatically freed when dropped. Use `iter()` to iterate over all
/// allocated [`Block`] structures.
///
/// # Invariants
///
/// - `list` is an initialized, valid list head containing allocated blocks.
/// - `buddy` references a valid [`GpuBuddyInner`].
#[pin_data(PinnedDrop)]
pub struct AllocatedBlocks {
    #[pin]
    list: CListHead,
    buddy: Arc<GpuBuddyInner>,
    flags: BuddyFlags,
}

impl AllocatedBlocks {
    /// Check if the block list is empty.
    pub fn is_empty(&self) -> bool {
        // An empty list head points to itself.
        !self.list.is_linked()
    }

    /// Iterate over allocated blocks.
    ///
    /// Returns an iterator yielding [`AllocatedBlock`] references. The blocks
    /// are only valid for the duration of the borrow of `self`.
    pub fn iter(&self) -> impl Iterator<Item = AllocatedBlock<'_>> + '_ {
        // SAFETY: list contains gpu_buddy_block items linked via __bindgen_anon_1.link.
        let clist = unsafe {
            clist_create!(
                self.list.as_raw(),
                Block,
                bindings::gpu_buddy_block,
                __bindgen_anon_1.link
            )
        };

        clist
            .iter()
            .map(|block| AllocatedBlock { block, alloc: self })
    }
}

#[pinned_drop]
impl PinnedDrop for AllocatedBlocks {
    fn drop(self: Pin<&mut Self>) {
        let guard = self.buddy.lock();

        // SAFETY:
        // - list is valid per the type's invariants.
        // - guard provides exclusive access to the allocator.
        // CAST: BuddyFlags were validated to fit in u32 at construction.
        unsafe {
            bindings::gpu_buddy_free_list(
                guard.as_raw(),
                self.list.as_raw(),
                self.flags.as_raw() as u32,
            );
        }
    }
}

/// A GPU buddy block.
///
/// Transparent wrapper over C `gpu_buddy_block` structure. This type is returned
/// as references from [`CListIter`] during iteration over [`AllocatedBlocks`].
///
/// # Invariants
///
/// The inner [`Opaque`] contains a valid, allocated `gpu_buddy_block`.
#[repr(transparent)]
pub struct Block(Opaque<bindings::gpu_buddy_block>);

impl Block {
    /// Get a raw pointer to the underlying C block.
    fn as_raw(&self) -> *mut bindings::gpu_buddy_block {
        self.0.get()
    }

    /// Get the block's offset in the address space.
    pub(crate) fn offset(&self) -> u64 {
        // SAFETY: self.as_raw() is valid per the type's invariants.
        unsafe { bindings::gpu_buddy_block_offset(self.as_raw()) }
    }

    /// Get the block order.
    pub(crate) fn order(&self) -> u32 {
        // SAFETY: self.as_raw() is valid per the type's invariants.
        unsafe { bindings::gpu_buddy_block_order(self.as_raw()) }
    }
}

// SAFETY: `Block` is a transparent wrapper over `gpu_buddy_block` which is not
// modified after allocation. It can be safely sent between threads.
unsafe impl Send for Block {}

// SAFETY: `Block` is a transparent wrapper over `gpu_buddy_block` which is not
// modified after allocation. It can be safely shared among threads.
unsafe impl Sync for Block {}

/// An allocated block with access to the allocation list.
///
/// # Invariants
///
/// - `block` is a valid reference to an allocated [`Block`].
/// - `alloc` is a valid reference to the [`AllocatedBlocks`] that owns this block.
pub struct AllocatedBlock<'a> {
    block: &'a Block,
    alloc: &'a AllocatedBlocks,
}

impl AllocatedBlock<'_> {
    /// Get the block's offset in the address space.
    ///
    /// Returns the absolute offset including the allocator's base offset.
    /// This is the actual address to use for accessing the allocated memory.
    pub fn offset(&self) -> u64 {
        self.alloc.buddy.base_offset + self.block.offset()
    }

    /// Get the block order (size = chunk_size << order).
    pub fn order(&self) -> u32 {
        self.block.order()
    }

    /// Get the block's size in bytes.
    pub fn size(&self) -> u64 {
        self.alloc.buddy.chunk_size << self.block.order()
    }
}
