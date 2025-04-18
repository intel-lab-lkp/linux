// SPDX-License-Identifier: GPL-2.0

//! This file provides a implementation of a subset of the upstream rust `UnsafePinned` type
//! for rust versions that don't include this type.

use core::{cell::UnsafeCell, marker::PhantomPinned};

/// This type provides a way to opt-out of typical aliasing rules;
/// specifically, `&mut UnsafePinned<T>` is not guaranteed to be a unique pointer.
///
/// However, even if you define your type like `pub struct Wrapper(UnsafePinned<...>)`, it is still
/// very risky to have an `&mut Wrapper` that aliases anything else. Many functions that work
/// generically on `&mut T` assume that the memory that stores `T` is uniquely owned (such as
/// `mem::swap`). In other words, while having aliasing with `&mut Wrapper` is not immediate
/// Undefined Behavior, it is still unsound to expose such a mutable reference to code you do not
/// control! Techniques such as pinning via [`Pin`](core::pin::Pin) are needed to ensure soundness.
///
/// Similar to [`UnsafeCell`], [`UnsafePinned`] will not usually show up in
/// the public API of a library. It is an internal implementation detail of libraries that need to
/// support aliasing mutable references.
///
/// Further note that this does *not* lift the requirement that shared references must be read-only!
/// Use [`UnsafeCell`] for that.
///
/// This type blocks niches the same way [`UnsafeCell`] does.
//
// As opposed to the upstream Rust type this contains a `PhantomPinned`` and `UnsafeCell<T>`
// - `PhantomPinned` to avoid needing a `impl<T> !Unpin for UnsafePinned<T>`
// - `UnsafeCell<T>` instead of T to disallow niche optimizations,
//     which is handled in the compiler in upstream Rust
#[repr(transparent)]
pub struct UnsafePinned<T: ?Sized> {
    _ph: PhantomPinned,
    value: UnsafeCell<T>,
}

impl<T> UnsafePinned<T> {
    /// Constructs a new instance of [`UnsafePinned`] which will wrap the specified value.
    ///
    /// All access to the inner value through `&UnsafePinned<T>` or `&mut UnsafePinned<T>` or
    /// `Pin<&mut UnsafePinned<T>>` requires `unsafe` code.
    #[inline(always)]
    #[must_use]
    pub const fn new(value: T) -> Self {
        UnsafePinned {
            value: UnsafeCell::new(value),
            _ph: PhantomPinned,
        }
    }
}
impl<T: ?Sized> UnsafePinned<T> {
    /// Get read-only access to the contents of a shared `UnsafePinned`.
    ///
    /// Note that `&UnsafePinned<T>` is read-only if `&T` is read-only. This means that if there is
    /// mutation of the `T`, future reads from the `*const T` returned here are UB! Use
    /// [`UnsafeCell`] if you also need interior mutability.
    ///
    /// [`UnsafeCell`]: core::cell::UnsafeCell
    ///
    /// ```rust,no_build
    /// use kernel::types::UnsafePinned;
    ///
    /// unsafe {
    ///     let mut x = UnsafePinned::new(0);
    ///     let ptr = x.get(); // read-only pointer, assumes immutability
    ///     x.get_mut_unchecked().write(1);
    ///     ptr.read(); // UB!
    /// }
    /// ```
    ///
    /// Note that the `get_mut_unchecked` function used by this example is
    /// currently not implemented in the kernel implementation.
    #[inline(always)]
    #[must_use]
    pub const fn get(&self) -> *const T {
        self.value.get()
    }

    /// Gets a mutable pointer to the wrapped value.
    ///
    /// The difference from `get_mut_pinned` and `get_mut_unchecked` is that this function
    /// accepts a raw pointer, which is useful to avoid the creation of temporary references.
    ///
    /// These functions mentioned here are currently not implemented in the kernel.
    #[inline(always)]
    #[must_use]
    pub const fn raw_get_mut(this: *mut Self) -> *mut T {
        this as *mut T
    }
}
