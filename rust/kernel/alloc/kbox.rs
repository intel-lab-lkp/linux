// SPDX-License-Identifier: GPL-2.0

//! Implementation of [`KBox`].

use super::{allocator::Kmalloc, AllocError, Allocator, Flags};
use core::fmt;
use core::mem::ManuallyDrop;
use core::mem::MaybeUninit;
use core::ops::{Deref, DerefMut};
use core::pin::Pin;
use core::ptr;
use core::result::Result;

use crate::types::Unique;

/// The kernel's `Box` type named [`KBox`].
///
/// `KBox` provides the simplest way to allocate memory for a generic type with one of the kernel's
/// allocators, e.g. `Kmalloc`, `Vmalloc` or `KVmalloc`.
///
/// For non-zero-sized values, a [`KBox`] will use the given allocator `A` for its allocation. If
/// no specific `Allocator` is requested, [`KBox`] will default to `Kmalloc`.
///
/// It is valid to convert both ways between a [`KBox`] and a raw pointer allocated with any
/// `Allocator`, given that the `Layout` used with the allocator is correct for the type.
///
/// For zero-sized values the [`KBox`]' pointer must be `dangling_mut::<T>`; no memory is
/// allocated.
///
/// So long as `T: Sized`, a `Box<T>` is guaranteed to be represented as a single pointer and is
/// also ABI-compatible with C pointers (i.e. the C type `T*`).
///
/// # Invariants
///
/// The [`KBox`]' pointer always properly aligned and either points to memory allocated with `A` or,
/// for zero-sized types, is a dangling pointer.
///
/// # Examples
///
/// ```
/// let b = KBox::new(24_u64, GFP_KERNEL)?;
///
/// assert_eq!(*b, 24_u64);
///
/// # Ok::<(), Error>(())
/// ```
///
/// ```
/// use kernel::alloc::allocator::KVmalloc;
///
/// struct Huge([u8; 1 << 24]);
///
/// assert!(KBox::<Huge, KVmalloc>::new_uninit_alloc(KVmalloc, GFP_KERNEL).is_ok());
/// ```
pub struct KBox<T: ?Sized, A: Allocator = Kmalloc>(Unique<T>, A);

impl<T, A> KBox<T, A>
where
    T: ?Sized,
    A: Allocator,
{
    /// Constructs a `KBox<T, A>` from a raw pointer.
    ///
    /// # Safety
    ///
    /// `raw` must point to valid memory, previously allocated with `A`, and at least the size of
    /// type `T`.
    #[inline]
    pub const unsafe fn from_raw_alloc(raw: *mut T, alloc: A) -> Self {
        // SAFETY: Safe by the requirements of this function.
        KBox(unsafe { Unique::new_unchecked(raw) }, alloc)
    }

    /// Consumes the `KBox<T, A>`, returning a wrapped raw pointer and the allocator it was
    /// allocated with.
    ///
    /// # Examples
    ///
    /// ```
    /// let x = KBox::new(24, GFP_KERNEL)?;
    /// let (ptr, alloc) = KBox::into_raw_alloc(x);
    /// let x = unsafe { KBox::from_raw_alloc(ptr, alloc) };
    ///
    /// assert_eq!(*x, 24);
    ///
    /// # Ok::<(), Error>(())
    /// ```
    pub fn into_raw_alloc(self) -> (*mut T, A) {
        let b = ManuallyDrop::new(self);
        let alloc = unsafe { ptr::read(&b.1) };
        (b.0.as_ptr(), alloc)
    }

    /// Consumes the `KBox<T>`, returning a wrapped raw pointer.
    #[inline]
    pub fn into_raw(self) -> *mut T {
        self.into_raw_alloc().0
    }

    /// Consumes and leaks the `KBox<T>`, returning a mutable reference, &'a mut T.
    #[inline]
    pub fn leak<'a>(b: Self) -> &'a mut T
    where
        T: 'a,
    {
        // SAFETY: `KBox::into_raw` always returns a properly aligned and dereferenceable pointer
        // which points to an initialized instance of `T`.
        unsafe { &mut *KBox::into_raw(b) }
    }

    /// Converts a `KBox<T>` into a `Pin<KBox<T>>`.
    #[inline]
    pub fn into_pin(b: Self) -> Pin<Self>
    where
        A: 'static,
    {
        // SAFETY: It's not possible to move or replace the insides of a `Pin<KBox<T>>` when
        // `T: !Unpin`, so it's safe to pin it directly without any additional requirements.
        unsafe { Pin::new_unchecked(b) }
    }
}

impl<T, A> KBox<MaybeUninit<T>, A>
where
    A: Allocator,
{
    /// Converts to `KBox<T, A>`.
    ///
    /// # Safety
    ///
    /// As with MaybeUninit::assume_init, it is up to the caller to guarantee that the value really
    /// is in an initialized state. Calling this when the content is not yet fully initialized
    /// causes immediate undefined behavior.
    pub unsafe fn assume_init(self) -> KBox<T, A> {
        let (raw, alloc) = KBox::into_raw_alloc(self);
        // SAFETY: Reconstruct the `KBox<MaybeUninit<T>, A>` as KBox<T, A> now that has been
        // initialized. `raw` and `alloc` are safe by the invariants of `KBox`.
        unsafe { KBox::from_raw_alloc(raw as *mut T, alloc) }
    }

    /// Writes the value and converts to `KBox<T, A>`.
    pub fn write(mut boxed: Self, value: T) -> KBox<T, A> {
        (*boxed).write(value);
        // SAFETY: We've just initialized `boxed`'s value.
        unsafe { boxed.assume_init() }
    }
}

impl<T> KBox<T> {
    /// Allocates memory with `Kmalloc` and then places `x` into it.
    ///
    /// This doesn’t actually allocate if T is zero-sized.
    pub fn new(x: T, flags: Flags) -> Result<Self, AllocError> {
        let b = Self::new_uninit(flags)?;
        Ok(KBox::write(b, x))
    }

    /// Constructs a new `KBox<T>` with uninitialized contents.
    #[inline]
    pub fn new_uninit(flags: Flags) -> Result<KBox<MaybeUninit<T>>, AllocError> {
        Self::new_uninit_alloc(Kmalloc, flags)
    }

    /// Constructs a new `Pin<KBox<T>>`. If `T` does not implement [`Unpin`], then `x` will be
    /// pinned in memory and unable to be moved.
    #[inline]
    pub fn pin(x: T, flags: Flags) -> Result<Pin<KBox<T>>, AllocError> {
        Ok(KBox::new(x, flags)?.into())
    }
}

impl<T> KBox<T>
where
    T: ?Sized,
{
    /// Constructs a `KBox<T>` from a raw pointer.
    ///
    /// # Safety
    ///
    /// `raw` must point to valid memory, previously allocated with the `Kmalloc`, and at least the
    /// size of type `T`.
    #[inline]
    pub const unsafe fn from_raw(raw: *mut T) -> Self {
        // SAFETY: Validity of `raw` is guaranteed by the safety preconditions of this function.
        KBox(unsafe { Unique::new_unchecked(raw) }, Kmalloc)
    }
}

impl<T, A> KBox<T, A>
where
    A: Allocator,
{
    fn is_zst() -> bool {
        core::mem::size_of::<T>() == 0
    }

    /// Allocates memory with the allocator `A` and then places `x` into it.
    ///
    /// This doesn’t actually allocate if T is zero-sized.
    pub fn new_alloc(x: T, alloc: A, flags: Flags) -> Result<Self, AllocError> {
        let b = Self::new_uninit_alloc(alloc, flags)?;
        Ok(KBox::write(b, x))
    }

    /// Constructs a new `KBox<T, A>` with uninitialized contents.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::allocator::Kmalloc;
    ///
    /// let b = KBox::<u64>::new_uninit_alloc(Kmalloc, GFP_KERNEL)?;
    /// let b = KBox::write(b, 24);
    ///
    /// assert_eq!(*b, 24_u64);
    ///
    /// # Ok::<(), Error>(())
    /// ```
    pub fn new_uninit_alloc(alloc: A, flags: Flags) -> Result<KBox<MaybeUninit<T>, A>, AllocError> {
        let ptr = if Self::is_zst() {
            Unique::dangling()
        } else {
            let layout = core::alloc::Layout::new::<MaybeUninit<T>>();
            let ptr = alloc.alloc(layout, flags)?;

            ptr.cast().into()
        };

        Ok(KBox(ptr, alloc))
    }

    /// Constructs a new `Pin<KBox<T, A>>`. If `T` does not implement [`Unpin`], then `x` will be
    /// pinned in memory and unable to be moved.
    #[inline]
    pub fn pin_alloc(x: T, alloc: A, flags: Flags) -> Result<Pin<KBox<T, A>>, AllocError>
    where
        A: 'static,
    {
        Ok(Self::new_alloc(x, alloc, flags)?.into())
    }
}

impl<T, A> From<KBox<T, A>> for Pin<KBox<T, A>>
where
    T: ?Sized,
    A: Allocator,
    A: 'static,
{
    /// Converts a `KBox<T>` into a `Pin<KBox<T>>`. If `T` does not implement [`Unpin`], then
    /// `*boxed` will be pinned in memory and unable to be moved.
    ///
    /// This conversion does not allocate on the heap and happens in place.
    ///
    /// This is also available via [`KBox::into_pin`].
    ///
    /// Constructing and pinning a `KBox` with <code><Pin<KBox\<T>>>::from([KBox::new]\(x))</code>
    /// can also be written more concisely using <code>[KBox::pin]\(x)</code>.
    /// This `From` implementation is useful if you already have a `KBox<T>`, or you are
    /// constructing a (pinned) `KBox` in a different way than with [`KBox::new`].
    fn from(b: KBox<T, A>) -> Self {
        KBox::into_pin(b)
    }
}

impl<T, A> Deref for KBox<T, A>
where
    T: ?Sized,
    A: Allocator,
{
    type Target = T;

    fn deref(&self) -> &T {
        // SAFETY: `self.0` is always properly aligned, dereferenceable and points to an initialized
        // instance of `T`.
        unsafe { self.0.as_ref() }
    }
}

impl<T, A> DerefMut for KBox<T, A>
where
    T: ?Sized,
    A: Allocator,
{
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: `self.0` is always properly aligned, dereferenceable and points to an initialized
        // instance of `T`.
        unsafe { self.0.as_mut() }
    }
}

impl<T, A> fmt::Debug for KBox<T, A>
where
    T: ?Sized + fmt::Debug,
    A: Allocator,
{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(&**self, f)
    }
}

impl<T, A> Drop for KBox<T, A>
where
    T: ?Sized,
    A: Allocator,
{
    fn drop(&mut self) {
        let ptr = self.0.as_ptr();

        // SAFETY: We need to drop `self.0` in place, before we free the backing memory.
        unsafe { core::ptr::drop_in_place(ptr) };

        // SAFETY: `ptr` is always properly aligned, dereferenceable and points to an initialized
        // instance of `T`.
        if unsafe { core::mem::size_of_val(&*ptr) } != 0 {
            // SAFETY: `ptr` was previously allocated with `self.1`.
            unsafe { self.1.free(ptr.cast()) };
        }
    }
}
