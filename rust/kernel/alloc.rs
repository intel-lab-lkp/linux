// SPDX-License-Identifier: GPL-2.0

//! Extensions to the [`alloc`] crate.

#[cfg(not(any(test, testlib)))]
pub mod allocator;
pub mod kbox;
pub mod kvec;

#[cfg(any(test, testlib))]
pub mod allocator_test;

#[cfg(any(test, testlib))]
pub use self::allocator_test as allocator;

pub use self::kbox::KBox;
pub use self::kvec::IntoIter;
pub use self::kvec::KVec;

/// Indicates an allocation error.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct AllocError;
use core::{alloc::Layout, ptr, ptr::NonNull};

/// Flags to be used when allocating memory.
///
/// They can be combined with the operators `|`, `&`, and `!`.
///
/// Values can be used from the [`flags`] module.
#[derive(Clone, Copy)]
pub struct Flags(u32);

impl core::ops::BitOr for Flags {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for Flags {
    type Output = Self;
    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::Not for Flags {
    type Output = Self;
    fn not(self) -> Self::Output {
        Self(!self.0)
    }
}

/// Allocation flags.
///
/// These are meant to be used in functions that can allocate memory.
pub mod flags {
    use super::Flags;

    /// Zeroes out the allocated memory.
    ///
    /// This is normally or'd with other flags.
    pub const __GFP_ZERO: Flags = Flags(bindings::__GFP_ZERO);

    /// Users can not sleep and need the allocation to succeed.
    ///
    /// A lower watermark is applied to allow access to "atomic reserves". The current
    /// implementation doesn't support NMI and few other strict non-preemptive contexts (e.g.
    /// raw_spin_lock). The same applies to [`GFP_NOWAIT`].
    pub const GFP_ATOMIC: Flags = Flags(bindings::GFP_ATOMIC);

    /// Typical for kernel-internal allocations. The caller requires ZONE_NORMAL or a lower zone
    /// for direct access but can direct reclaim.
    pub const GFP_KERNEL: Flags = Flags(bindings::GFP_KERNEL);

    /// The same as [`GFP_KERNEL`], except the allocation is accounted to kmemcg.
    pub const GFP_KERNEL_ACCOUNT: Flags = Flags(bindings::GFP_KERNEL_ACCOUNT);

    /// Ror kernel allocations that should not stall for direct reclaim, start physical IO or
    /// use any filesystem callback.  It is very likely to fail to allocate memory, even for very
    /// small allocations.
    pub const GFP_NOWAIT: Flags = Flags(bindings::GFP_NOWAIT);
}

/// The kernel's [`Allocator`] trait.
///
/// An implementation of [`Allocator`] can allocate, re-allocate and free memory buffer described
/// via [`Layout`].
///
/// [`Allocator`] is designed to be implemented on ZSTs; its safety requirements to not allow for
/// keeping a state throughout an instance.
///
/// # Safety
///
/// Memory returned from an allocator must point to a valid memory buffer and remain valid until
/// its explicitly freed.
///
/// Copying, cloning, or moving the allocator must not invalidate memory blocks returned from this
/// allocator. A copied, cloned or even new allocator of the same type must behave like the same
/// allocator, and any pointer to a memory buffer which is currently allocated may be passed to any
/// other method of the allocator.
pub unsafe trait Allocator {
    /// Allocate memory based on `layout` and `flags`.
    ///
    /// On success, returns a buffer represented as `NonNull<[u8]>` that satisfies the size an
    /// alignment requirements of layout, but may exceed the requested size.
    ///
    /// This function is equivalent to `realloc` when called with a NULL pointer and an `old_size`
    /// of `0`.
    fn alloc(&self, layout: Layout, flags: Flags) -> Result<NonNull<[u8]>, AllocError> {
        // SAFETY: Passing a NULL pointer to `realloc` is valid by it's safety requirements and asks
        // for a new memory allocation.
        unsafe { self.realloc(ptr::null_mut(), 0, layout, flags) }
    }

    /// Re-allocate an existing memory allocation to satisfy the requested `layout`. If the
    /// requested size is zero, `realloc` behaves equivalent to `free`.
    ///
    /// If the requested size is larger than `old_size`, a successful call to `realloc` guarantees
    /// that the new or grown buffer has at least `Layout::size` bytes, but may also be larger.
    ///
    /// If the requested size is smaller than `old_size`, `realloc` may or may not shrink the
    /// buffer; this is implementation specific to the allocator.
    ///
    /// On allocation failure, the existing buffer, if any, remains valid.
    ///
    /// The buffer is represented as `NonNull<[u8]>`.
    ///
    /// # Safety
    ///
    /// `ptr` must point to an existing and valid memory allocation created by this allocator
    /// instance of a size of at least `old_size`.
    ///
    /// Additionally, `ptr` is allowed to be a NULL pointer; in this case a new memory allocation is
    /// created.
    unsafe fn realloc(
        &self,
        ptr: *mut u8,
        old_size: usize,
        layout: Layout,
        flags: Flags,
    ) -> Result<NonNull<[u8]>, AllocError>;

    /// Free an existing memory allocation.
    ///
    /// # Safety
    ///
    /// `ptr` must point to an existing and valid memory allocation created by this `Allocator`
    /// instance.
    unsafe fn free(&self, ptr: *mut u8) {
        // SAFETY: `ptr` is guaranteed to be previously allocated with this `Allocator` or NULL.
        // Calling `realloc` with a buffer size of zero, frees the buffer `ptr` points to.
        let _ = unsafe { self.realloc(ptr, 0, Layout::new::<()>(), Flags(0)) };
    }
}
