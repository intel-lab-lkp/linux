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
use kernel::{
    sync::aref::ARef,
    types::RefCounted, //
};

use kernel::types::ForeignOwnable;

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
/// [`RefCounted`] should be implemented. [`OwnableIntoShared`] should be implemented if conversion
/// from unique to shared (reference counted) ownership is needed, and [`OwnableTryFromShared`] if
/// conversion from shared back to unique ownership is needed.
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

// SAFETY: We derive the pointer to `T` from a valid `T`, so the returned
// pointer satisfy alignment requirements of `T`.
unsafe impl<T: Ownable> ForeignOwnable for Owned<T> {
    const FOREIGN_ALIGN: usize = core::mem::align_of::<T>();

    type Borrowed<'a>
        = &'a T
    where
        Self: 'a;
    type BorrowedMut<'a>
        = Pin<&'a mut T>
    where
        Self: 'a;

    #[inline]
    fn into_foreign(self) -> *mut kernel::ffi::c_void {
        Owned::into_raw(self).as_ptr().cast()
    }

    #[inline]
    unsafe fn from_foreign(ptr: *mut kernel::ffi::c_void) -> Self {
        // SAFETY: By function safety contract, `ptr` came from `into_foreign` and cannot be null.
        let ptr = unsafe { NonNull::new_unchecked(ptr.cast()) };

        // SAFETY: By the function safety contract, `ptr` was returned by `into_foreign`, which
        // consumed an `Owned<T>` without calling `T::release`. The requirements that were upheld
        // for that `Owned<T>` are still upheld, as the ownership it represented was only held by
        // the foreign pointer in the meantime. We retake that ownership here.
        unsafe { Owned::from_raw(ptr) }
    }

    #[inline]
    unsafe fn borrow<'a>(ptr: *mut kernel::ffi::c_void) -> Self::Borrowed<'a> {
        // SAFETY: By function safety requirements, `ptr` came from `into_foreign` and the
        // `Owned<T>` it was created from is not reconstructed or mutably borrowed during `'a`. By
        // the type invariants of that `Owned<T>`, `ptr` points to a valid `T` and it is sound to
        // create a shared reference to the `T` for `'a`.
        unsafe { &*ptr.cast() }
    }

    #[inline]
    unsafe fn borrow_mut<'a>(ptr: *mut kernel::ffi::c_void) -> Self::BorrowedMut<'a> {
        // SAFETY: By function safety requirements, `ptr` came from `into_foreign` and the
        // `Owned<T>` it was created from is not reconstructed or otherwise borrowed during `'a`. By
        // the type invariants of that `Owned<T>`, `ptr` points to a valid `T` and it is sound to
        // create a mutable reference to the `T` for `'a`, given that we only hand it out as a
        // `Pin<&mut T>` below.
        let inner = unsafe { &mut *ptr.cast() };

        // SAFETY: We never move out of inner, and we do not hand out mutable
        // references when `T: !Unpin`.
        unsafe { Pin::new_unchecked(inner) }
    }
}

/// A trait for types that support converting a unique reference [`Owned<Self>`] into a shared
/// reference [`ARef<Self>`].
///
/// The conversion consumes the [`Owned`], so it cannot fail. Implementing this trait does not make
/// the opposite conversion available. Types that can become unique again after having been shared
/// can implement [`OwnableTryFromShared`] in addition.
///
/// # Examples
///
/// A minimal example implementation of [`OwnableIntoShared`], [`OwnableTryFromShared`],
/// [`Ownable`] and its usage with [`ARef`] and [`Owned`] looks like this:
///
/// ```
/// # #![expect(clippy::disallowed_names)]
/// # use core::ptr::NonNull;
/// # use kernel::alloc::{flags, kbox::KBox, AllocError};
/// # use kernel::sync::aref::{ARef, RefCounted};
/// # use kernel::sync::atomic::Acquire;
/// # use kernel::sync::Refcount;
/// # use kernel::types::{Owned, Ownable, OwnableIntoShared, OwnableTryFromShared};
/// // An internally refcounted struct for demonstration purposes.
/// //
/// // # Invariants
/// //
/// // - `refcount` counts the live references to the object, so the object is valid while
/// //   `refcount` is non-zero.
/// // - `Foo` does not allow other references to exist together with an `Owned<Foo>`: while an
/// //   `Owned<Foo>` exists, `refcount` is 1 and no `ARef<Foo>` exists.
/// struct Foo {
///     refcount: Refcount,
/// }
///
/// impl Foo {
///     fn new() -> Result<Owned<Self>> {
///         // We are just using a `KBox` here to handle the actual allocation, as our `Foo` is
///         // not actually a C-allocated object.
///         // INVARIANT: We initialize `refcount` to 1, counting the reference held by the
///         // returned `Owned<Foo>`. No `ARef<Foo>` exists for the new object.
///         let result = KBox::new(
///             Foo {
///                 refcount: Refcount::new(1),
///             },
///             flags::GFP_KERNEL,
///         )?;
///         let result = KBox::into_non_null(result);
///         // SAFETY:
///         //  - We just allocated the `Self`, thus it is valid until the refcount drops to zero.
///         //  - No other pointer to the allocation exists, so this is the only `Owned<Foo>` and
///         //    no other reference to the `Foo` exists. Thus access through the `Owned<Foo>` is
///         //    sound, and it can be used from any thread that `Foo` can be used from.
///         //  - The `Foo` is not moved until it is freed in `dec_ref`.
///         Ok(unsafe { Owned::from_raw(result) })
///     }
/// }
///
/// // SAFETY: We increment and decrement the refcount each time the respective function is
/// // called, and only free the `Foo` when the refcount reaches zero.
/// unsafe impl RefCounted for Foo {
///     fn inc_ref(&self) {
///         self.refcount.inc();
///     }
///
///     unsafe fn dec_ref(this: NonNull<Self>) {
///         // SAFETY: By requirement on calling this function, the refcount is non-zero,
///         // implying the underlying object is valid.
///         let refcount = unsafe { &this.as_ref().refcount };
///         if refcount.dec_and_test() {
///             // SAFETY: The refcount reached zero, so by requirement on calling this function
///             // no reference to the object remains and it will no longer be used. We can
///             // reclaim the allocation by passing ownership back to the [`KBox`], which frees
///             // the `Foo` when dropped.
///             drop(unsafe { KBox::from_raw(this.as_ptr()) });
///         }
///     }
/// }
///
/// impl OwnableIntoShared for Foo {
///     fn into_shared(this: Owned<Self>) -> ARef<Self> {
///         // SAFETY: By the type invariants of `Foo`, an `Owned<Foo>` holds the only reference
///         // (refcount 1). We consume the `Owned<Foo>` and transfer the reference to the new
///         // `ARef`.
///         unsafe { ARef::from_raw(Owned::into_raw(this)) }
///     }
/// }
///
/// impl OwnableTryFromShared for Foo {
///     fn try_from_shared(this: ARef<Self>) -> Result<Owned<Self>, ARef<Self>> {
///         // `this` is a live reference, so the refcount cannot drop below 1, and it can only
///         // grow through an existing reference. Thus observing 1 means that `this` is the only
///         // reference to the object. The `Acquire` ordering synchronizes with the release
///         // decrements of references dropped on other threads.
///         if this.refcount.as_atomic().load(Acquire) == 1 {
///             // SAFETY: The `Foo` is valid and `this` holds the only reference to it, so we
///             // can transfer this last reference to the returned `Owned<Foo>`. No other
///             // reference to the `Foo` exists, so this is the only `Owned<Foo>` and access
///             // through it is sound. The `Foo` is not moved until it is freed in `dec_ref`.
///             // INVARIANT: `refcount` is 1 and we consumed the only `ARef<Foo>`.
///             Ok(unsafe { Owned::from_raw(ARef::into_raw(this)) })
///         } else {
///             Err(this)
///         }
///     }
/// }
///
/// impl Ownable for Foo {
///     unsafe fn release(this: NonNull<Self>) {
///         // SAFETY: Using `dec_ref()` from [`RefCounted`] to release is okay, as the refcount is
///         // always 1 for an [`Owned<Foo>`].
///         unsafe { Foo::dec_ref(this) };
///     }
/// }
///
/// let foo = Foo::new()?;
/// let foo = ARef::from(foo);
/// {
///     let bar = foo.clone();
///     assert!(Owned::try_from(bar).is_err());
/// }
/// assert!(Owned::try_from(foo).is_ok());
/// # Ok::<(), Error>(())
/// ```
pub trait OwnableIntoShared: RefCounted + Ownable + Sized {
    /// Converts the [`Owned`] into an [`ARef`].
    fn into_shared(this: Owned<Self>) -> ARef<Self>;
}

/// A trait for types that support converting a shared reference [`ARef<Self>`] back into a unique
/// reference [`Owned<Self>`].
///
/// The conversion can only succeed when no [`Owned`] exists for the object, and when the
/// requirements for creating one are satisfied, so it is fallible. For a type that does not allow
/// other references to exist together with an [`Owned`], this means that the [`ARef`] must be the
/// only reference to the object. See the *Access model* section of [`Ownable`].
///
/// Not all types that implement [`OwnableIntoShared`] are able to become unique again after having
/// been shared, so this capability is provided by a separate trait.
///
/// # Examples
///
/// See the example for [`OwnableIntoShared`].
pub trait OwnableTryFromShared: RefCounted + Ownable + Sized {
    /// Checks if the [`ARef`] can be converted to an [`Owned`] and converts it if that is the case.
    /// Otherwise it returns again an [`ARef`] to the same underlying object.
    fn try_from_shared(this: ARef<Self>) -> Result<Owned<Self>, ARef<Self>>;
}

impl<T: OwnableTryFromShared> TryFrom<ARef<T>> for Owned<T> {
    type Error = ARef<T>;
    /// Tries to convert the [`ARef`] to an [`Owned`] by calling
    /// [`try_from_shared()`](OwnableTryFromShared::try_from_shared). In case the conversion is not
    /// possible, it returns again an [`ARef`] to the same underlying object.
    #[inline]
    fn try_from(b: ARef<T>) -> Result<Owned<T>, Self::Error> {
        T::try_from_shared(b)
    }
}
