// SPDX-License-Identifier: GPL-2.0

//! Unique owned pointer types for objects with custom drop logic.
//!
//! These pointer types are useful for C-allocated objects which by API-contract
//! are owned by Rust, but need to be freed through the C API.

use core::{
    mem::ManuallyDrop,
    ops::{
        Deref,
        DerefMut, //
    },
    pin::Pin,
    ptr::NonNull, //
};

/// Types that specify their own way of performing allocation and destruction. Typically, this trait
/// is implemented on types from the C side.
///
/// Implementing this trait allows types to be referenced via the [`Owned<Self>`] pointer type. This
/// is useful when it is desirable to tie the lifetime of the reference to an owned object, rather
/// than pass around a bare reference. [`Ownable`] types can define custom drop logic that is
/// executed when the owned reference [`Owned<Self>`] pointing to the object is dropped.
///
/// Note: The underlying object is not required to provide internal reference counting, because it
/// represents a unique, owned reference. If reference counting (on the Rust side) is required,
/// [`RefCounted`](crate::types::RefCounted) should be implemented.
///
/// # Examples
///
/// A minimal example implementation of [`Ownable`] and its usage with [`Owned`] looks like
/// this:
///
/// ```
/// # #![expect(clippy::disallowed_names)]
/// # use core::cell::Cell;
/// # use core::ptr::NonNull;
/// # use kernel::sync::global_lock;
/// # use kernel::alloc::{flags, kbox::KBox, AllocError};
/// # use kernel::types::{Owned, Ownable};
///
/// // Let's count the allocations to see if freeing works.
/// kernel::sync::global_lock! {
///     // SAFETY: we call `init()` right below, before doing anything else.
///     unsafe(uninit) static FOO_ALLOC_COUNT: Mutex<usize> = 0;
/// }
/// // SAFETY: We call `init()` only once, here.
/// unsafe { FOO_ALLOC_COUNT.init() };
///
/// struct Foo;
///
/// impl Foo {
///     fn new() -> Result<Owned<Self>> {
///         // We are just using a `KBox` here to handle the actual allocation, as our `Foo` is
///         // not actually a C-allocated object.
///         let result = KBox::new(
///             Foo {},
///             flags::GFP_KERNEL,
///         )?;
///         let result = KBox::into_nonnull(result);
///         // Count new allocation
///         *FOO_ALLOC_COUNT.lock() += 1;
///         // SAFETY:
///         //  - We just allocated the `Self`, thus it is valid and we own it.
///         //  - We can transfer this ownership to the `from_raw` method.
///         Ok(unsafe { Owned::from_raw(result) })
///     }
/// }
///
/// impl Ownable for Foo {
///     unsafe fn release(&mut self) {
///         // SAFETY: The [`KBox<Self>`] is still alive. We can pass ownership to the [`KBox`], as
///         // by requirement on calling this function.
///         drop(unsafe { KBox::from_raw(self) });
///         // Count released allocation
///         *FOO_ALLOC_COUNT.lock() -= 1;
///     }
/// }
///
/// {
///    let foo = Foo::new()?;
///    assert!(*FOO_ALLOC_COUNT.lock() == 1);
/// }
/// // `foo` is out of scope now, so we expect no live allocations.
/// assert!(*FOO_ALLOC_COUNT.lock() == 0);
/// # Ok::<(), Error>(())
/// ```
pub trait Ownable {
    /// Tear down this `Ownable`.
    ///
    /// Implementers of `Ownable` can use this function to clean up the use of `Self`. This can
    /// include freeing the underlying object.
    ///
    /// # Safety
    ///
    /// Callers must ensure that the caller has exclusive ownership of `T`, and this ownership can
    /// be transferred to the `release` method.
    unsafe fn release(&mut self);
}

/// A mutable reference to an owned `T`.
///
/// The [`Ownable`] is automatically freed or released when an instance of [`Owned`] is
/// dropped.
///
/// # Invariants
///
/// - Until `T::release` is called, this `Owned<T>` exclusively owns the underlying `T`.
/// - The `T` value is pinned.
pub struct Owned<T: Ownable> {
    ptr: NonNull<T>,
}

impl<T: Ownable> Owned<T> {
    /// Creates a new instance of [`Owned`].
    ///
    /// This function takes over ownership of the underlying object.
    ///
    /// # Safety
    ///
    /// Callers must ensure that:
    /// - `ptr` points to a valid instance of `T`.
    /// - Until `T::release` is called, the returned `Owned<T>` exclusively owns the underlying `T`.
    pub unsafe fn from_raw(ptr: NonNull<T>) -> Self {
        // INVARIANT: By funvtion safety requirement we satisfy the first invariant of `Self`.
        // We treat `T` as pinned from now on.
        Self { ptr }
    }

    /// Consumes the [`Owned`], returning a raw pointer.
    ///
    /// This function does not drop the underlying `T`. When this function returns, ownership of the
    /// underlying `T` is with the caller.
    pub fn into_raw(me: Self) -> NonNull<T> {
        ManuallyDrop::new(me).ptr
    }

    /// Get a pinned mutable reference to the data owned by this `Owned<T>`.
    pub fn as_pin_mut(&mut self) -> Pin<&mut T> {
        // SAFETY: The type invariants guarantee that the object is valid, and that we can safely
        // return a mutable reference to it.
        let unpinned = unsafe { self.ptr.as_mut() };

        // SAFETY: By type invariant `T` is pinned.
        unsafe { Pin::new_unchecked(unpinned) }
    }
}

// SAFETY: It is safe to send an [`Owned<T>`] to another thread when the underlying `T` is [`Send`],
// because of the ownership invariant. Sending an [`Owned<T>`] is equivalent to sending the `T`.
unsafe impl<T: Ownable + Send> Send for Owned<T> {}

// SAFETY: It is safe to send [`&Owned<T>`] to another thread when the underlying `T` is [`Sync`],
// because of the ownership invariant. Sending an [`&Owned<T>`] is equivalent to sending the `&T`.
unsafe impl<T: Ownable + Sync> Sync for Owned<T> {}

impl<T: Ownable> Deref for Owned<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // SAFETY: The type invariants guarantee that the object is valid.
        unsafe { self.ptr.as_ref() }
    }
}

impl<T: Ownable + Unpin> DerefMut for Owned<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: The type invariants guarantee that the object is valid, and that we can safely
        // return a mutable reference to it.
        unsafe { self.ptr.as_mut() }
    }
}

impl<T: Ownable> Drop for Owned<T> {
    fn drop(&mut self) {
        // SAFETY: By existence of `&mut self` we exclusively own `self` and the underlying `T`. As
        // we are dropping `self`, we can transfer ownership of the `T` to the `release` method.
        unsafe { T::release(self.ptr.as_mut()) };
    }
}
