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
/// Implementing this trait allows types to be referenced via the [`Owned<Self>`] pointer type.
///  - This is useful when it is desirable to tie the lifetime of an object reference to an owned
///    object, rather than pass around a bare reference.
///  - [`Ownable`] types can define custom drop logic that is executed when the owned reference
///    of type [`Owned<_>`] pointing to the object is dropped.
///
/// Note: The underlying object is not required to provide internal reference counting, because it
/// represents a unique, owned reference. If reference counting (on the Rust side) is required,
/// [`AlwaysRefCounted`](crate::sync::aref::AlwaysRefCounted) should be implemented.
///
/// # Access model
///
/// An [`Owned<Self>`] is a unique handle: at most one [`Owned<Self>`] exists for an object at any
/// time. There are two kinds of references to the object to consider, and they are governed by
/// different rules:
///
///  - References that borrow the [`Owned<Self>`]. An [`Owned<Self>`] hands out `&Self` and
///    `Pin<&mut Self>` to safe code, and also `&mut Self` if `Self` is [`Unpin`]. These references
///    borrow the [`Owned<Self>`], so the borrow checker ensures that they do not outlive it and
///    that they follow the usual aliasing rules with respect to each other.
///  - Other references. These are references to the object that do not borrow the
///    [`Owned<Self>`], such as an `ARef<Self>`, or a `&Self` obtained from one. The borrow checker
///    does not relate these to the [`Owned<Self>`].
///
/// Whether other references to an object may exist together with the [`Owned<Self>`] is decided by
/// the implementer of this trait:
///
///  - A type may forbid all other references to an object for as long as an [`Owned<Self>`] to
///    the object exists. For such a type, an [`Owned<Self>`] is the exclusive owner of the object.
///  - A type may allow other references, such as `ARef<Self>`, to exist together with an
///    [`Owned<Self>`]. For such a type, an [`Owned<Self>`] has exclusive access only to the parts
///    of the object that the type reserves for it.
///
/// The borrow checker cannot ensure that references that borrow the [`Owned<Self>`] are sound in
/// the presence of other references. This is the responsibility of the code that creates an
/// [`Owned<Self>`], see [`Owned::from_raw`]. Note that a `&mut Self` must never alias another
/// reference, so a type that is [`Unpin`] cannot allow other references to exist together with an
/// [`Owned<Self>`].
///
/// A type that allows other references to exist together with an [`Owned<Self>`] must place all
/// state that is accessible through those references inside [`Opaque`](crate::types::Opaque) or
/// [`UnsafeCell`](core::cell::UnsafeCell). A `&Self` asserts that state outside of these wrappers
/// is not mutated while the reference is live, so such state must not be mutated through the
/// [`Owned<Self>`]. In addition, a `Pin<&mut Self>` handed out by an [`Owned<Self>`] may only
/// alias other references because the compiler does not treat a mutable reference to a type that
/// is not [`Unpin`] as unique. The intent is to express this with `UnsafePinned` instead, once it
/// is stable.
///
/// Implementers should document, as a type invariant of `Self`, which other references may exist
/// together with an [`Owned<Self>`], and why access through the [`Owned<Self>`] is sound.
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
/// // Let's count the allocations to see if freeing works.
/// kernel::sync::global_lock! {
///     // SAFETY: we call `init()` right below, before doing anything else.
///     unsafe(uninit) static FOO_ALLOC_COUNT: Mutex<usize> = 0;
/// }
/// // SAFETY: We call `init()` only once, here.
/// unsafe { FOO_ALLOC_COUNT.init() };
///
/// // An exclusively owned struct for demonstration purposes.
/// //
/// // # Invariants
/// //
/// // `Foo` does not allow other references to exist together with an `Owned<Foo>`: while an
/// // `Owned<Foo>` exists, it is the only reference to the object.
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
///         let result = KBox::into_non_null(result);
///         // Count new allocation
///         *FOO_ALLOC_COUNT.lock() += 1;
///         // SAFETY:
///         //  - We just allocated the `Self`, thus it is valid until we free it in `release`.
///         //  - No other pointer to the allocation exists, so this is the only `Owned<Foo>` and
///         //    no other reference to the `Foo` exists. Thus access through the `Owned<Foo>` is
///         //    sound, and it can be used from any thread that `Foo` can be used from.
///         //  - The `Foo` is not moved until it is freed in `release`.
///         Ok(unsafe { Owned::from_raw(result) })
///     }
/// }
///
/// impl Ownable for Foo {
///     unsafe fn release(this: NonNull<Self>) {
///         // SAFETY: The [`KBox<Self>`] is still alive. We can pass ownership to the [`KBox`], as
///         // by requirement on calling this function.
///         drop(unsafe { KBox::from_raw(this.as_ptr()) });
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
    /// include freeing the underlying object, handing it back to the subsystem that allocated it,
    /// or dropping a reference count.
    ///
    /// # Safety
    ///
    /// Callers must ensure that:
    /// - `this` points to a valid `Self`.
    /// - They hold the ownership of the `Self` pointed to by `this` that is represented by the
    ///   unique [`Owned<Self>`], and that this ownership is transferred to the `release` method.
    /// - `this` is not used after calling this method, as the underlying object may have been
    ///   freed.
    ///
    /// `this` is pinned and implementers of this method must observe this constraint.
    unsafe fn release(this: NonNull<Self>);
}

/// A unique owning handle to a `T`.
///
/// The [`Ownable`] is automatically freed or released when an instance of [`Owned`] is
/// dropped.
///
/// Whether other references to the underlying `T` may exist together with an [`Owned<T>`] is
/// decided by `T`, see the *Access model* section of [`Ownable`].
///
/// # Invariants
///
/// Until `T::release` is called:
/// - `ptr` points to a valid `T`.
/// - This is the only `Owned<T>` for the underlying `T`.
/// - The `T` value is pinned.
/// - It is sound to create a `&T` or a `Pin<&mut T>` to the underlying `T` and to hand it to safe
///   code, for the duration of a shared or mutable borrow of this `Owned<T>` respectively. If `T`
///   is [`Unpin`], the same applies to a `&mut T`. This holds in the presence of all the references
///   that `T` allows to exist together with an `Owned<T>`.
/// - If `T` is [`Send`], it is sound to use this `Owned<T>` from, and to drop it on, another thread
///   than the one it was created on. If `T` is [`Sync`], it is sound to use shared references to
///   this `Owned<T>` from multiple threads concurrently.
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
    /// Callers must ensure that, until `T::release` is called:
    /// - `ptr` points to a valid instance of `T`.
    /// - The returned `Owned<T>` is the only `Owned<T>` for the underlying `T`.
    /// - The `T` pointed to by `ptr` is treated as pinned from this call on: unless `T: Unpin`, it
    ///   must not be moved for the rest of its lifetime, including after the pointer is recovered
    ///   with [`Owned::into_raw`].
    /// - No reference to the underlying `T` exists, or is created, other than the references that
    ///   `T` allows to exist together with an `Owned<T>`. In the presence of those references, it
    ///   must be sound to create a `&T` or a `Pin<&mut T>` to the underlying `T` and to hand it to
    ///   safe code, for the duration of a shared or mutable borrow of the returned `Owned<T>`
    ///   respectively. If `T` is [`Unpin`], the same applies to a `&mut T`.
    /// - If `T` is [`Send`], it is sound to use the returned `Owned<T>` from, and to drop it on,
    ///   another thread than the current one. If `T` is [`Sync`], it is sound to use shared
    ///   references to the returned `Owned<T>` from multiple threads concurrently.
    #[inline]
    pub unsafe fn from_raw(ptr: NonNull<T>) -> Self {
        // INVARIANT: The function safety requirements match the invariants of `Self`.
        Self { ptr }
    }

    /// Consumes the [`Owned`], returning a raw pointer.
    ///
    /// This function does not drop the underlying `T`. When this function returns, ownership of the
    /// underlying `T` is with the caller.
    ///
    /// Note that the returned pointer is pinned.
    #[inline]
    pub fn into_raw(me: Self) -> NonNull<T> {
        ManuallyDrop::new(me).ptr
    }

    /// Get a pinned mutable reference to the data owned by this `Owned<T>`.
    #[inline]
    pub fn as_pin_mut(&mut self) -> Pin<&mut T> {
        // SAFETY: By the type invariants, `ptr` points to a valid `T` and it is sound to create a
        // mutable reference to the `T` for the duration of the mutable borrow of `self`, given that
        // we only hand it out as a `Pin<&mut T>` below.
        let unpinned = unsafe { self.ptr.as_mut() };

        // SAFETY: By type invariant `T` is pinned.
        unsafe { Pin::new_unchecked(unpinned) }
    }
}

// SAFETY: By the type invariants, it is sound to use and drop an [`Owned<T>`] on another thread
// when the underlying `T` is [`Send`]. For a `T` that does not allow other references to exist
// together with an [`Owned<T>`], sending an [`Owned<T>`] is equivalent to sending the `T`.
unsafe impl<T: Ownable + Send> Send for Owned<T> {}

// SAFETY: By the type invariants, it is sound to use shared references to an [`Owned<T>`] from
// multiple threads when the underlying `T` is [`Sync`]. A [`&Owned<T>`] only gives access to a
// `&T`, so sending a [`&Owned<T>`] is equivalent to sending the `&T`.
unsafe impl<T: Ownable + Sync> Sync for Owned<T> {}

impl<T: Ownable> Deref for Owned<T> {
    type Target = T;

    #[inline]
    fn deref(&self) -> &Self::Target {
        // SAFETY: By the type invariants, `ptr` points to a valid `T` and it is sound to create a
        // shared reference to the `T` for the duration of the borrow of `self`.
        unsafe { self.ptr.as_ref() }
    }
}

impl<T: Ownable + Unpin> DerefMut for Owned<T> {
    #[inline]
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: By the type invariants, `ptr` points to a valid `T` and, as `T` is `Unpin`, it is
        // sound to create a mutable reference to the `T` for the duration of the mutable borrow of
        // `self`.
        unsafe { self.ptr.as_mut() }
    }
}

impl<T: Ownable> Drop for Owned<T> {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: By the type invariants, `ptr` points to a valid `T` and `self` is the only
        // `Owned<T>` for it. As we are dropping `self`, we can transfer the ownership it represents
        // to the `release` method, and we do not use `ptr` afterwards.
        unsafe { T::release(self.ptr) };
    }
}
