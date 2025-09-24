// SPDX-License-Identifier: GPL-2.0

//! Slab bindings.
//!
//! C header: [`include/linux/slab.h`](srctree/include/linux/slab.h)

use core::{marker::PhantomData, mem, ptr::NonNull};

use crate::{
    alloc::Flags,
    bindings,
    error::{code::ENOMEM, Result},
    str::CStr,
};

/// A wrapper for kmem_cache that allocates objects of type `T`.
#[repr(transparent)]
pub struct Slab<T> {
    cache: NonNull<bindings::kmem_cache>,
    _p: PhantomData<T>,
}

impl<T> Slab<T> {
    /// Creates a cache for objects of type `T`.
    pub fn try_new(name: &CStr, flags: Flags) -> Result<Self> {
        let size = mem::size_of::<T>();
        let align = mem::align_of::<T>();
        debug_assert!(size <= usize::MAX);
        debug_assert!(align <= usize::MAX);

        // SAFETY: `flags` is a valid impl, `name` is a valid C string, and
        // other arguments are plain values.
        let cache = unsafe {
            bindings::kmem_cache_create(
                name.as_char_ptr(),
                size as u32,
                align as u32,
                flags.as_raw(),
                None,
            )
        };

        NonNull::new(cache)
            .map(|c| Slab {
                cache: c,
                _p: PhantomData,
            })
            .ok_or(ENOMEM)
    }

    /// Allocates one object from the cache with the given gfp flags.
    #[inline]
    pub fn alloc(&self, flags: Flags) -> Result<NonNull<T>> {
        // SAFETY: `self.cache` is a valid pointer obtained from
        // `kmem_cache_create` and still alive because `self` is borrowed.
        let ptr = unsafe { bindings::kmem_cache_alloc(self.cache.as_ptr(), flags.as_raw()) };
        NonNull::new(ptr.cast()).ok_or(ENOMEM)
    }

    /// Frees an object previously returned by `alloc()`.
    ///
    /// # Safety
    /// The caller must guarantee that `obj` was allocated from this cache and
    /// is no longer accessed afterwards.
    #[inline]
    pub unsafe fn free(&self, obj: NonNull<T>) {
        // SAFETY: By the safety contract the pointer is valid and unique at
        // this point.
        unsafe { bindings::kmem_cache_free(self.cache.as_ptr(), obj.cast().as_ptr()) };
    }

    /// Returns the raw mutable pointer to the cache
    #[inline]
    pub fn as_ptr(&self) -> *mut bindings::kmem_cache {
        self.cache.as_ptr()
    }
}

impl<T> Drop for Slab<T> {
    fn drop(&mut self) {
        // SAFETY: `self.cache` is valid and we are the final owner because
        // of ownership rules.
        unsafe { bindings::kmem_cache_destroy(self.cache.as_ptr()) };
    }
}
