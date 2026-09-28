// SPDX-License-Identifier: GPL-2.0

//! Implementation of [`ArrayVec`].

use crate::{
    alloc::kvec::{
        impl_slice_eq,
        PushError, //
    },
    const_assert,
    error::{
        code::EINVAL,
        Error,
        Result, //
    },
    fmt, //
};

use core::{
    borrow::{
        Borrow,
        BorrowMut, //
    },
    mem::MaybeUninit,
    ops::{
        Deref,
        DerefMut, //
    },
    ptr,
    slice, //
};

use pin_init::{
    init_from_closure,
    Init,
    Zeroable, //
};

/// A fixed capacity vector that holds at most `N` elements.
///
/// # Invariants
///
/// - `len` is at most `N`.
/// - The first `len` elements of `data` are initialized.
///
/// # Examples
///
/// ```
/// use kernel::alloc::ArrayVec;
///
/// let mut v = ArrayVec::<u8, 4>::new();
/// v.extend_from_slice(b"abc")?;
/// assert_eq!(*v, *b"abc");
///
/// assert!(v.extend_from_slice(b"ab").is_err());
///
/// v.push(4u8)?;
/// assert_eq!(*v, *b"abc\x04");
/// assert!(v.push(5u8).is_err());
///
/// v.clear();
/// assert!(v.is_empty());
/// # Ok::<(), Error>(())
/// ```
#[derive(Zeroable)]
pub struct ArrayVec<T, const N: usize> {
    data: [MaybeUninit<T>; N],
    len: usize,
}

impl<T, const N: usize> ArrayVec<T, N> {
    /// Creates an empty [`ArrayVec`].
    #[inline]
    pub const fn new() -> Self {
        // Clippy triggers this even if the enclosing function is never called, so skip if clippy is
        // on.
        const_assert!(
            cfg!(clippy) || size_of::<Self>() <= 512,
            "use `init_with` instead of constructing a large ArrayVec on the stack"
        );

        // INVARIANT: An empty ArrayVec trivially has all its elements initialized.
        Self {
            data: [const { MaybeUninit::uninit() }; N],
            len: 0,
        }
    }

    /// Creates an initializer for an [`ArrayVec`] populated by `f`.
    ///
    /// `f` gets an empty [`ArrayVec`] and can fill it in place.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::ArrayVec;
    ///
    /// let v = KBox::init(
    ///     ArrayVec::<u8, 4096>::init_with(|v| v.extend_from_slice(b"abc")),
    ///     GFP_KERNEL,
    /// )?;
    /// assert_eq!(**v, *b"abc");
    /// # Ok::<(), Error>(())
    /// ```
    pub fn init_with<E>(f: impl FnOnce(&mut Self) -> Result<(), E>) -> impl Init<Self, E> {
        let init = move |slot: *mut Self| {
            // SAFETY: By the initializer contract `slot` is valid for writes. Once `len` is zero
            // the slot holds a valid empty ArrayVec, since `data` requires no initialization.
            // INVARIANT: An empty ArrayVec trivially has all its elements initialized.
            unsafe { ptr::addr_of_mut!((*slot).len).write(0) };

            // SAFETY: `slot` holds a valid ArrayVec and no other reference to it exists.
            let v = unsafe { &mut *slot };
            f(v).inspect_err(|_| {
                // SAFETY: `slot` holds a valid ArrayVec, and on failure the slot is never accessed
                // again, so the elements can't be dropped twice.
                unsafe { ptr::drop_in_place(slot) }
            })
        };

        // SAFETY: `init` fully initializes the slot on success and drops the potentially filled
        // ArrayVec on failure.
        unsafe { init_from_closure(init) }
    }

    /// Appends an element to the back of the [`ArrayVec`].
    ///
    /// Fails when the [`ArrayVec`] is full, handing the element back in [`PushError`].
    pub fn push(&mut self, v: T) -> Result<(), PushError<T>> {
        self.try_push_init(v)
            .map_err(|PushInitError::Full(v)| PushError(v))
    }

    /// Appends an element to the back of the [`ArrayVec`] by initializing it in place.
    ///
    /// Fails with [`FullError`] when the [`ArrayVec`] is full.
    pub fn push_init(&mut self, init: impl Init<T>) -> Result<(), FullError> {
        self.try_push_init(init)
            .map_err(|PushInitError::Full(_)| FullError)
    }

    /// Appends an element to the back of the [`ArrayVec`] by initializing it in place.
    ///
    /// Unlike [`ArrayVec::push_init`], the initializer may be fallible. If the [`ArrayVec`] is
    /// full, the original initializer `init` is handed back in [`PushInitError::Full`]. If the
    /// initializer itself fails, its error is returned in [`PushInitError::InitError`].
    pub fn try_push_init<I, E>(&mut self, init: I) -> Result<(), PushInitError<I, E>>
    where
        I: Init<T, E>,
    {
        let Some(slot) = self.spare_capacity_mut().first_mut() else {
            return Err(PushInitError::Full(init));
        };

        // SAFETY: `slot` refers to allocated, aligned memory valid for a write of one `T`.
        unsafe { init.__init(slot.as_mut_ptr()) }.map_err(PushInitError::InitError)?;

        // INVARIANT: The element at index `len` was just initialized, and the new `len` does not
        // exceed `N` because a spare slot existed.
        self.len += 1;

        Ok(())
    }

    /// Appends a clone of each element in `slice` to the back of the [`ArrayVec`].
    ///
    /// Fails with [`EINVAL`] if `slice` is longer than the remaining capacity.
    pub fn extend_from_slice(&mut self, slice: &[T]) -> Result
    where
        T: Clone,
    {
        let Some(dst) = self.spare_capacity_mut().get_mut(..slice.len()) else {
            return Err(EINVAL);
        };

        for (d, s) in dst.iter_mut().zip(slice) {
            d.write(s.clone());
        }
        // INVARIANT: The next `slice.len()` elements after `len` were just initialized, and the
        // new `len` does not exceed `N` because the spare capacity was enough.
        self.len += slice.len();

        Ok(())
    }

    /// Removes all elements.
    #[inline]
    pub fn clear(&mut self) {
        let elems: *mut [T] = self.as_mut_slice();
        // INVARIANT: An empty ArrayVec trivially has all its elements initialized.
        self.len = 0;
        // SAFETY: There are no references to the elements since we hold `&mut self`. The elements
        // can't be dropped again because `len` is already 0.
        unsafe { ptr::drop_in_place(elems) };
    }

    /// Returns the initialized elements as a slice.
    #[inline]
    pub fn as_slice(&self) -> &[T] {
        let ptr = self.data.as_ptr().cast::<T>();
        // SAFETY: `MaybeUninit<T>` has the same layout as `T`, and by the type invariants the first
        // `len` elements of `data` are initialized.
        unsafe { slice::from_raw_parts(ptr, self.len) }
    }

    /// Returns the initialized elements as a mutable slice.
    #[inline]
    pub fn as_mut_slice(&mut self) -> &mut [T] {
        let ptr = self.data.as_mut_ptr().cast::<T>();
        // SAFETY: `MaybeUninit<T>` has the same layout as `T`, and by the type invariants the first
        // `len` elements of `data` are initialized.
        unsafe { slice::from_raw_parts_mut(ptr, self.len) }
    }

    /// Returns a slice of `MaybeUninit<T>` for the remaining spare capacity of the [`ArrayVec`].
    fn spare_capacity_mut(&mut self) -> &mut [MaybeUninit<T>] {
        // PANIC: `len` never exceeds `N` by the type invariants.
        &mut self.data[self.len..]
    }
}

/// Error type for [`ArrayVec::try_push_init`].
pub enum PushInitError<I, E> {
    /// The [`ArrayVec`] is full. Hand the initializer back.
    Full(I),
    /// The initializer failed.
    InitError(E),
}

impl<I, E> fmt::Debug for PushInitError<I, E> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PushInitError::Full(_) => write!(f, "Not enough capacity"),
            PushInitError::InitError(_) => write!(f, "Initializer failed"),
        }
    }
}

impl<I, E> From<PushInitError<I, E>> for Error
where
    Error: From<E>,
{
    #[inline]
    fn from(e: PushInitError<I, E>) -> Error {
        match e {
            PushInitError::Full(_) => EINVAL,
            PushInitError::InitError(e) => Error::from(e),
        }
    }
}

/// Error type for [`ArrayVec::push_init`].
pub struct FullError;

impl fmt::Debug for FullError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Not enough capacity")
    }
}

impl From<FullError> for Error {
    #[inline]
    fn from(_: FullError) -> Error {
        EINVAL
    }
}

impl<T, const N: usize> Default for ArrayVec<T, N> {
    #[inline]
    fn default() -> Self {
        Self::new()
    }
}

impl<T, const N: usize> Drop for ArrayVec<T, N> {
    fn drop(&mut self) {
        // SAFETY: The slice holds initialized elements that are never accessed again after this
        // point.
        unsafe { ptr::drop_in_place(self.as_mut_slice()) };
    }
}

impl<T, const N: usize> Deref for ArrayVec<T, N> {
    type Target = [T];

    #[inline]
    fn deref(&self) -> &Self::Target {
        self.as_slice()
    }
}

impl<T, const N: usize> DerefMut for ArrayVec<T, N> {
    #[inline]
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.as_mut_slice()
    }
}

impl<T, const N: usize> Borrow<[T]> for ArrayVec<T, N> {
    fn borrow(&self) -> &[T] {
        self.as_slice()
    }
}

impl<T, const N: usize> BorrowMut<[T]> for ArrayVec<T, N> {
    fn borrow_mut(&mut self) -> &mut [T] {
        self.as_mut_slice()
    }
}

impl<T: Eq, const N: usize> Eq for ArrayVec<T, N> {}

impl_slice_eq! {
    [const N: usize, const M: usize] ArrayVec<T, N>, ArrayVec<U, M>,
    [const N: usize] ArrayVec<T, N>, &[U],
    [const N: usize] ArrayVec<T, N>, &mut [U],
    [const N: usize] &[T], ArrayVec<U, N>,
    [const N: usize] &mut [T], ArrayVec<U, N>,
    [const N: usize] ArrayVec<T, N>, [U],
    [const N: usize] [T], ArrayVec<U, N>,
    [const N: usize, const M: usize] ArrayVec<T, N>, [U; M],
    [const N: usize, const M: usize] ArrayVec<T, N>, &[U; M],
}

impl<'a, T, const N: usize> IntoIterator for &'a ArrayVec<T, N> {
    type Item = &'a T;
    type IntoIter = slice::Iter<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a, T, const N: usize> IntoIterator for &'a mut ArrayVec<T, N> {
    type Item = &'a mut T;
    type IntoIter = slice::IterMut<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter_mut()
    }
}

impl<T: fmt::Debug, const N: usize> fmt::Debug for ArrayVec<T, N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(self.as_slice(), f)
    }
}
