// SPDX-License-Identifier: GPL-2.0

//! Allocator support.

use super::{flags::*, Flags};
use core::alloc::{GlobalAlloc, Layout};
use core::ptr;
use core::ptr::NonNull;

use crate::alloc::{AllocError, Allocator};
use crate::bindings;

/// The contiguous kernel allocator.
///
/// The contiguous kernel allocator only ever allocates physically contiguous memory through
/// `bindings::krealloc`.
pub struct Kmalloc;

/// Returns a proper size to alloc a new object aligned to `new_layout`'s alignment.
fn aligned_size(new_layout: Layout) -> usize {
    // Customized layouts from `Layout::from_size_align()` can have size < align, so pad first.
    let layout = new_layout.pad_to_align();

    let mut size = layout.size();

    if layout.align() > bindings::ARCH_SLAB_MINALIGN {
        // The alignment requirement exceeds the slab guarantee, thus try to enlarge the size
        // to use the "power-of-two" size/alignment guarantee (see comments in `kmalloc()` for
        // more information).
        //
        // Note that `layout.size()` (after padding) is guaranteed to be a multiple of
        // `layout.align()`, so `next_power_of_two` gives enough alignment guarantee.
        size = size.next_power_of_two();
    }

    size
}

unsafe impl Allocator for Kmalloc {
    unsafe fn realloc(
        &self,
        old_ptr: *mut u8,
        _old_size: usize,
        layout: Layout,
        flags: Flags,
    ) -> Result<NonNull<[u8]>, AllocError> {
        let size = aligned_size(layout);

        // SAFETY: `src` is guaranteed to point to valid memory with a size of at least
        // `old_size`, which was previously allocated with this `Allocator` or NULL.
        let raw_ptr = unsafe {
            // If `size == 0` and `old_ptr != NULL` `krealloc()` frees the memory behind the
            // pointer.
            bindings::krealloc(old_ptr.cast(), size, flags.0).cast()
        };

        let ptr = if size == 0 {
            NonNull::dangling()
        } else {
            NonNull::new(raw_ptr).ok_or(AllocError)?
        };

        Ok(NonNull::slice_from_raw_parts(ptr, size))
    }
}

unsafe impl GlobalAlloc for Kmalloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let this: &dyn Allocator = self;

        match this.alloc(layout, GFP_KERNEL) {
            Ok(ptr) => ptr.as_ptr().cast(),
            Err(_) => ptr::null_mut(),
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        // SAFETY: The safety requirements of `dealloc` are a superset of the ones of
        // `Allocator::free`.
        unsafe { self.free(ptr) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        let this: &dyn Allocator = self;
        let old_size = layout.size();

        // SAFETY:
        // - `new_size`, when rounded up to the nearest multiple of `layout.align()`, will not
        //   overflow `isize` by the function safety requirement.
        // - `layout.align()` is a proper alignment (i.e. not zero and must be a power of two).
        let layout = unsafe { Layout::from_size_align_unchecked(new_size, layout.align()) };

        // SAFETY:
        // - `ptr` is either null or a pointer allocated by this allocator by the function safety
        //   requirement.
        // - the size of `layout` is not zero because `new_size` is not zero by the function safety
        //   requirement.
        // - `old_size` represents the memory that needs to be preserved.
        match unsafe { this.realloc(ptr, old_size, layout, GFP_KERNEL) } {
            Ok(ptr) => ptr.as_ptr().cast(),
            Err(_) => ptr::null_mut(),
        }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let this: &dyn Allocator = self;

        match this.alloc(layout, GFP_KERNEL | __GFP_ZERO) {
            Ok(ptr) => ptr.as_ptr().cast(),
            Err(_) => ptr::null_mut(),
        }
    }
}

#[global_allocator]
static ALLOCATOR: Kmalloc = Kmalloc;

// See <https://github.com/rust-lang/rust/pull/86844>.
#[no_mangle]
static __rust_no_alloc_shim_is_unstable: u8 = 0;
