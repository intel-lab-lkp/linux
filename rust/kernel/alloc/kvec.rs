// SPDX-License-Identifier: GPL-2.0

//! Implementation of [`KVec`].

use super::{allocator::Kmalloc, AllocError, Allocator, Flags};
use crate::types::Unique;
use core::{
    fmt,
    mem::{ManuallyDrop, MaybeUninit},
    ops::Deref,
    ops::DerefMut,
    ops::Index,
    ops::IndexMut,
    ptr,
    ptr::NonNull,
    slice,
    slice::SliceIndex,
};

/// Create a [`KVec`] containing the arguments.
///
/// # Examples
///
/// ```
/// use kernel::alloc::{flags::*, KVec};
///
/// let mut v = kernel::kvec![];
/// v.push(1, GFP_KERNEL).unwrap();
/// assert_eq!(v, [1]);
///
/// let mut v = kernel::kvec![1; 3]?;
/// v.push(4, GFP_KERNEL).unwrap();
/// assert_eq!(v, [1, 1, 1, 4]);
///
/// let mut v = kernel::kvec![1, 2, 3]?;
/// v.push(4, GFP_KERNEL).unwrap();
/// assert_eq!(v, [1, 2, 3, 4]);
///
/// # Ok::<(), Error>(())
/// ```
#[macro_export]
macro_rules! kvec {
    () => (
        {
            $crate::alloc::KVec::new()
        }
    );
    ($elem:expr; $n:expr) => (
        {
            $crate::alloc::KVec::from_elem(
                $elem, $n,
                $crate::alloc::allocator::Kmalloc,
                GFP_KERNEL
            )
        }
    );
    ($($x:expr),+ $(,)?) => (
        {
            match $crate::alloc::KBox::new([$($x),+], GFP_KERNEL) {
                Ok(b) => Ok($crate::alloc::KBox::into_vec(b)),
                Err(e) => Err(e),
            }
        }
    );
}

/// The kernel's `Vec` type named [`KVec`].
///
/// A contiguous growable array type with contents allocated with the kernel's allocators (e.g.
/// `Kmalloc`, `Vmalloc` or `KVmalloc`, written `KVec<T, A>`.
///
/// For non-zero-sized values, a [`KVec`] will use the given allocator `A` for its allocation. If
/// no specific `Allocator` is requested, [`KVec`] will default to `Kmalloc`.
///
/// For zero-sized types the [`KVec`]'s pointer must be `dangling_mut::<T>`; no memory is allocated.
///
/// Generally, [`KVec`] consists of a pointer that represents the vector's backing buffer, the
/// capacity of the vector (the number of elements that currently fit into the vector), it's length
/// (the number of elements that are currently stored in the vector) and the `Allocator` used to
/// allocate (and free) the backing buffer.
///
/// A [`KVec`] can be deconstructed into and (re-)constructed from it's previously named raw parts
/// and manually modified.
///
/// [`KVec`]'s backing buffer gets, if required, automatically increased (re-allocated) when
/// elements are added to the vector.
///
/// # Invariants
///
/// The [`KVec`] backing buffer's pointer always properly aligned and either points to memory
/// allocated with `A` or, for zero-sized types, is a dangling pointer.
///
/// The length of the vector always represents the exact number of elements stored in the vector.
///
/// The capacity of the vector always represents the absolute number of elements that can be stored
/// within the vector without re-allocation. However, it is legal for the backing buffer to be
/// larger than `size_of<T>` times the capacity.
///
/// The `Allocator` of the vector is the exact allocator the backing buffer was allocated with (and
/// must be freed with).
pub struct KVec<T, A: Allocator = Kmalloc> {
    ptr: Unique<T>,
    /// Never used for ZSTs; it's `capacity()`'s responsibility to return usize::MAX in that case.
    ///
    /// # Safety
    ///
    /// `cap` must be in the `0..=isize::MAX` range.
    cap: usize,
    len: usize,
    alloc: A,
}

impl<T> KVec<T> {
    /// Create a new empty `KVec` with no memory allocated yet.
    #[inline]
    pub const fn new() -> Self {
        Self::new_alloc(Kmalloc)
    }

    /// Creates a new [`KVec`] instance with at least the given capacity.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{flags::*, KVec};
    ///
    /// let v = KVec::<u32>::with_capacity(20, GFP_KERNEL)?;
    ///
    /// assert!(v.capacity() >= 20);
    /// # Ok::<(), Error>(())
    /// ```
    #[inline]
    pub fn with_capacity(capacity: usize, flags: Flags) -> Result<Self, AllocError> {
        Self::with_capacity_alloc(capacity, Kmalloc, flags)
    }
}

impl<T, A> KVec<T, A>
where
    A: Allocator,
{
    #[inline]
    fn is_zst() -> bool {
        core::mem::size_of::<T>() == 0
    }

    fn buffer_size(capacity: usize) -> Result<usize, AllocError> {
        if Self::is_zst() {
            Ok(0)
        } else {
            capacity
                .checked_mul(core::mem::size_of::<T>())
                .ok_or(AllocError)
        }
    }

    /// Returns a reference to the underlying allocator.
    #[inline]
    pub fn allocator(&self) -> &A {
        &self.alloc
    }

    /// Returns the total number of elements the vector can hold without
    /// reallocating.
    pub fn capacity(&self) -> usize {
        if Self::is_zst() {
            usize::MAX
        } else {
            self.cap
        }
    }

    /// Returns the number of elements in the vector, also referred to
    /// as its 'length'.
    #[inline]
    pub fn len(&self) -> usize {
        self.len
    }

    /// Forces the length of the vector to new_len.
    ///
    /// # Safety
    ///
    /// - `new_len` must be less than or equal to [`Self::capacity()`].
    /// - The elements at `old_len..new_len` must be initialized.
    #[inline]
    pub unsafe fn set_len(&mut self, new_len: usize) {
        self.len = new_len;
    }

    /// Extracts a slice containing the entire vector.
    ///
    /// Equivalent to `&s[..]`.
    #[inline]
    pub fn as_slice(&self) -> &[T] {
        self
    }

    /// Extracts a mutable slice of the entire vector.
    ///
    /// Equivalent to `&mut s[..]`.
    #[inline]
    pub fn as_mut_slice(&mut self) -> &mut [T] {
        self
    }

    /// Returns an unsafe mutable pointer to the vector's buffer, or a dangling
    /// raw pointer valid for zero sized reads if the vector didn't allocate.
    #[inline]
    pub fn as_mut_ptr(&self) -> *mut T {
        self.ptr.as_ptr()
    }

    /// Returns a raw pointer to the slice's buffer.
    #[inline]
    pub fn as_ptr(&self) -> *const T {
        self.as_mut_ptr()
    }

    /// Returns `true` if the vector contains no elements.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{flags::*, KVec};
    ///
    /// let mut v = KVec::new();
    /// assert!(v.is_empty());
    ///
    /// v.push(1, GFP_KERNEL);
    /// assert!(!v.is_empty());
    /// ```
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Like `new`, but parameterized over the choice of allocator for the returned `KVec`.
    #[inline]
    pub const fn new_alloc(alloc: A) -> Self {
        Self {
            ptr: Unique::dangling(),
            cap: 0,
            len: 0,
            alloc,
        }
    }

    fn spare_capacity_mut(&mut self) -> &mut [MaybeUninit<T>] {
        // Note:
        // This method is not implemented in terms of `split_at_spare_mut`,
        // to prevent invalidation of pointers to the buffer.
        unsafe {
            slice::from_raw_parts_mut(
                self.as_mut_ptr().add(self.len) as *mut MaybeUninit<T>,
                self.capacity() - self.len,
            )
        }
    }

    /// Appends an element to the back of the [`KVec`] instance.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{flags::*, KVec};
    ///
    /// let mut v = KVec::new();
    /// v.push(1, GFP_KERNEL)?;
    /// assert_eq!(&v, &[1]);
    ///
    /// v.push(2, GFP_KERNEL)?;
    /// assert_eq!(&v, &[1, 2]);
    /// # Ok::<(), Error>(())
    /// ```
    pub fn push(&mut self, v: T, flags: Flags) -> Result<(), AllocError> {
        KVec::reserve(self, 1, flags)?;
        let s = self.spare_capacity_mut();
        s[0].write(v);

        // SAFETY: We just initialised the first spare entry, so it is safe to increase the length
        // by 1. We also know that the new length is <= capacity because of the previous call to
        // `reserve` above.
        unsafe { self.set_len(self.len() + 1) };
        Ok(())
    }

    /// Creates a new [`KVec`] instance with at least the given capacity.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{allocator::Kmalloc, flags::*, KVec};
    ///
    /// let v = KVec::<u32, _>::with_capacity_alloc(20, Kmalloc, GFP_KERNEL)?;
    ///
    /// assert!(v.capacity() >= 20);
    /// # Ok::<(), Error>(())
    /// ```
    pub fn with_capacity_alloc(
        capacity: usize,
        alloc: A,
        flags: Flags,
    ) -> Result<Self, AllocError> {
        let mut v = KVec::new_alloc(alloc);

        Self::reserve(&mut v, capacity, flags)?;

        Ok(v)
    }

    /// Pushes clones of the elements of slice into the [`KVec`] instance.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{allocator::Kmalloc, flags::*, KVec};
    ///
    /// let mut v = KVec::new_alloc(Kmalloc);
    /// v.push(1, GFP_KERNEL)?;
    ///
    /// v.extend_from_slice(&[20, 30, 40], GFP_KERNEL)?;
    /// assert_eq!(&v, &[1, 20, 30, 40]);
    ///
    /// v.extend_from_slice(&[50, 60], GFP_KERNEL)?;
    /// assert_eq!(&v, &[1, 20, 30, 40, 50, 60]);
    /// # Ok::<(), Error>(())
    /// ```
    pub fn extend_from_slice(&mut self, other: &[T], flags: Flags) -> Result<(), AllocError>
    where
        T: Clone,
    {
        self.reserve(other.len(), flags)?;
        for (slot, item) in core::iter::zip(self.spare_capacity_mut(), other) {
            slot.write(item.clone());
        }

        // SAFETY: We just initialised the `other.len()` spare entries, so it is safe to increase
        // the length by the same amount. We also know that the new length is <= capacity because
        // of the previous call to `reserve` above.
        unsafe { self.set_len(self.len() + other.len()) };
        Ok(())
    }

    /// Creates a KVec<T, A> directly from a pointer, a length, a capacity, and an allocator.
    ///
    /// # Safety
    ///
    /// This is highly unsafe, due to the number of invariants that aren’t checked:
    ///
    /// - `ptr` must be currently allocated via the given allocator `alloc`.
    /// - `T` needs to have the same alignment as what `ptr` was allocated with. (`T` having a less
    ///   strict alignment is not sufficient, the alignment really needs to be equal to satisfy the
    ///   `dealloc` requirement that memory must be allocated and deallocated with the same layout.)
    /// - The size of `T` times the `capacity` (i.e. the allocated size in bytes) needs to be
    ///   smaller or equal the size the pointer was allocated with.
    /// - `length` needs to be less than or equal to `capacity`.
    /// - The first `length` values must be properly initialized values of type `T`.
    /// - The allocated size in bytes must be no larger than `isize::MAX`. See the safety
    ///   documentation of `pointer::offset`.
    ///
    /// # Examples
    ///
    /// ```
    /// let mut v = kernel::kvec![1, 2, 3]?;
    /// v.reserve(1, GFP_KERNEL)?;
    ///
    /// let (mut ptr, mut len, cap, alloc) = v.into_raw_parts_alloc();
    ///
    /// unsafe { ptr.add(len).write(4) };
    /// len += 1;
    ///
    /// let v = unsafe { KVec::from_raw_parts_alloc(ptr, len, cap, alloc) };
    ///
    /// assert_eq!(v, [1, 2, 3, 4]);
    ///
    /// # Ok::<(), Error>(())
    /// ```
    pub unsafe fn from_raw_parts_alloc(
        ptr: *mut T,
        length: usize,
        capacity: usize,
        alloc: A,
    ) -> Self {
        let cap = if Self::is_zst() { 0 } else { capacity };

        Self {
            ptr: unsafe { Unique::new_unchecked(ptr) },
            cap,
            len: length,
            alloc,
        }
    }

    /// Decomposes a `KVec<T, A>` into its raw components: (`pointer`, `length`, `capacity`,
    /// `allocator`).
    pub fn into_raw_parts_alloc(self) -> (*mut T, usize, usize, A) {
        let me = ManuallyDrop::new(self);
        let len = me.len();
        let capacity = me.capacity();
        let ptr = me.as_mut_ptr();
        let alloc = unsafe { ptr::read(me.allocator()) };
        (ptr, len, capacity, alloc)
    }

    /// Ensures that the capacity exceeds the length by at least `additional` elements.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{allocator::Kmalloc, flags::*, KVec};
    ///
    /// let mut v = KVec::new_alloc(Kmalloc);
    /// v.push(1, GFP_KERNEL)?;
    ///
    /// v.reserve(10, GFP_KERNEL)?;
    /// let cap = v.capacity();
    /// assert!(cap >= 10);
    ///
    /// v.reserve(10, GFP_KERNEL)?;
    /// let new_cap = v.capacity();
    /// assert_eq!(new_cap, cap);
    ///
    /// # Ok::<(), Error>(())
    /// ```
    pub fn reserve(&mut self, additional: usize, flags: Flags) -> Result<(), AllocError> {
        let len = self.len();
        let cap = self.capacity();

        if cap - len >= additional {
            return Ok(());
        }

        if Self::is_zst() {
            // The capacity is already `usize::MAX` for SZTs, we can't go higher.
            return Err(AllocError);
        }

        // We know cap is <= `isize::MAX` because `Layout::array` fails if the resulting byte size
        // is greater than `isize::MAX`. So the multiplication by two won't overflow.
        let new_cap = core::cmp::max(cap * 2, len.checked_add(additional).ok_or(AllocError)?);
        let layout = core::alloc::Layout::array::<T>(new_cap).map_err(|_| AllocError)?;

        // We need to make sure that `ptr` is either NULL or comes from a previous call to
        // `realloc_flags`. A `KVec<T, A>`'s `ptr` value is not guaranteed to be NULL and might be
        // dangling after being created with `KVec::new`. Instead, we can rely on `KVec<T, A>`'s
        // capacity to be zero if no memory has been allocated yet.
        let ptr = if cap == 0 {
            ptr::null_mut()
        } else {
            self.as_mut_ptr()
        };

        // SAFETY: `ptr` is valid because it's either NULL or comes from a previous call to
        // `krealloc_aligned`. We also verified that the type is not a ZST.
        let ptr = unsafe {
            self.alloc
                .realloc(ptr.cast(), Self::buffer_size(cap)?, layout, flags)
        }?;

        self.ptr = ptr.cast().into();
        self.cap = new_cap;

        Ok(())
    }
}

impl<T: Clone, A: Allocator> KVec<T, A> {
    /// Extend the vector by `n` clones of value.
    pub fn extend_with(&mut self, n: usize, value: T, flags: Flags) -> Result<(), AllocError> {
        self.reserve(n, flags)?;

        unsafe {
            let mut ptr = self.as_mut_ptr().add(self.len());

            // Write all elements except the last one
            for _ in 1..n {
                ptr::write(ptr, value.clone());
                ptr = ptr.add(1);
            }

            if n > 0 {
                // We can write the last element directly without cloning needlessly
                ptr::write(ptr, value);
            }
        }

        // SAFETY: `self.reserve` not bailing out with an error guarantees that we're not
        // exceeding the capacity of this `KVec`.
        unsafe { self.set_len(self.len() + n) };

        Ok(())
    }

    /// Create a new `KVec<T, A> and extend it by `n` clones of value.
    pub fn from_elem(value: T, n: usize, alloc: A, flags: Flags) -> Result<Self, AllocError> {
        let mut v = Self::with_capacity_alloc(n, alloc, flags)?;

        v.extend_with(n, value, flags)?;

        Ok(v)
    }
}

impl<T, A> Drop for KVec<T, A>
where
    A: Allocator,
{
    fn drop(&mut self) {
        // SAFETY: We need to drop the vector's elements in place, before we free the backing
        // memory.
        unsafe {
            core::ptr::drop_in_place(core::ptr::slice_from_raw_parts_mut(
                self.as_mut_ptr(),
                self.len,
            ))
        };

        // If `cap == 0` we never allocated any memory in the first place.
        if self.cap != 0 {
            // SAFETY: `self.ptr` was previously allocated with `self.alloc`.
            unsafe { self.alloc.free(self.as_mut_ptr().cast()) };
        }
    }
}

impl<T> Default for KVec<T> {
    #[inline]
    fn default() -> Self {
        Self::new()
    }
}

impl<T: fmt::Debug, A: Allocator> fmt::Debug for KVec<T, A> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(&**self, f)
    }
}

impl<T, A> Deref for KVec<T, A>
where
    A: Allocator,
{
    type Target = [T];

    #[inline]
    fn deref(&self) -> &[T] {
        unsafe { slice::from_raw_parts(self.as_ptr(), self.len) }
    }
}

impl<T, A> DerefMut for KVec<T, A>
where
    A: Allocator,
{
    #[inline]
    fn deref_mut(&mut self) -> &mut [T] {
        unsafe { slice::from_raw_parts_mut(self.as_mut_ptr(), self.len) }
    }
}

impl<T: Eq, A> Eq for KVec<T, A> where A: Allocator {}

impl<T, I: SliceIndex<[T]>, A> Index<I> for KVec<T, A>
where
    A: Allocator,
{
    type Output = I::Output;

    #[inline]
    fn index(&self, index: I) -> &Self::Output {
        Index::index(&**self, index)
    }
}

impl<T, I: SliceIndex<[T]>, A> IndexMut<I> for KVec<T, A>
where
    A: Allocator,
{
    #[inline]
    fn index_mut(&mut self, index: I) -> &mut Self::Output {
        IndexMut::index_mut(&mut **self, index)
    }
}

macro_rules! __impl_slice_eq {
    ([$($vars:tt)*] $lhs:ty, $rhs:ty $(where $ty:ty: $bound:ident)?) => {
        impl<T, U, $($vars)*> PartialEq<$rhs> for $lhs
        where
            T: PartialEq<U>,
            $($ty: $bound)?
        {
            #[inline]
            fn eq(&self, other: &$rhs) -> bool { self[..] == other[..] }
        }
    }
}

__impl_slice_eq! { [A1: Allocator, A2: Allocator] KVec<T, A1>, KVec<U, A2> }
__impl_slice_eq! { [A: Allocator] KVec<T, A>, &[U] }
__impl_slice_eq! { [A: Allocator] KVec<T, A>, &mut [U] }
__impl_slice_eq! { [A: Allocator] &[T], KVec<U, A> }
__impl_slice_eq! { [A: Allocator] &mut [T], KVec<U, A> }
__impl_slice_eq! { [A: Allocator] KVec<T, A>, [U] }
__impl_slice_eq! { [A: Allocator] [T], KVec<U, A> }
__impl_slice_eq! { [A: Allocator, const N: usize] KVec<T, A>, [U; N] }
__impl_slice_eq! { [A: Allocator, const N: usize] KVec<T, A>, &[U; N] }

impl<'a, T, A> IntoIterator for &'a KVec<T, A>
where
    A: Allocator,
{
    type Item = &'a T;
    type IntoIter = slice::Iter<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a, T, A: Allocator> IntoIterator for &'a mut KVec<T, A>
where
    A: Allocator,
{
    type Item = &'a mut T;
    type IntoIter = slice::IterMut<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter_mut()
    }
}

/// An iterator that moves out of a vector.
///
/// This `struct` is created by the `into_iter` method on [`KVec`] (provided by the [`IntoIterator`]
/// trait).
///
/// # Examples
///
/// ```
/// let v = kernel::kvec![0, 1, 2]?;
/// let iter: kernel::alloc::IntoIter<_> = v.into_iter();
///
/// # Ok::<(), Error>(())
/// ```
pub struct IntoIter<T, A: Allocator = Kmalloc> {
    ptr: *mut T,
    buf: NonNull<T>,
    len: usize,
    cap: usize,
    alloc: A,
}

impl<T, A> IntoIter<T, A>
where
    A: Allocator,
{
    fn as_raw_mut_slice(&mut self) -> *mut [T] {
        ptr::slice_from_raw_parts_mut(self.ptr, self.len)
    }
}

impl<T, A> Iterator for IntoIter<T, A>
where
    A: Allocator,
{
    type Item = T;

    /// # Examples
    ///
    /// ```
    /// let v = kernel::kvec![1, 2, 3]?;
    /// let mut it = v.into_iter();
    ///
    /// assert_eq!(it.next(), Some(1));
    /// assert_eq!(it.next(), Some(2));
    /// assert_eq!(it.next(), Some(3));
    /// assert_eq!(it.next(), None);
    ///
    /// # Ok::<(), Error>(())
    /// ```
    fn next(&mut self) -> Option<T> {
        if self.len == 0 {
            return None;
        }

        let ptr = self.ptr;
        if !KVec::<T>::is_zst() {
            // SAFETY: We can't overflow; `end` is guaranteed to mark the end of the buffer.
            unsafe { self.ptr = self.ptr.add(1) };
        } else {
            // For ZST `ptr` has to stay where it is to remain aligned, so we just reduce `self.len`
            // by 1.
        }
        self.len -= 1;

        // SAFETY: `ptr` is guaranteed to point at a valid element within the buffer.
        Some(unsafe { ptr.read() })
    }

    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::KVec;
    ///
    /// let v: KVec<u32, _> = kernel::kvec![1, 2, 3]?;
    /// let mut iter = v.into_iter();
    /// let size = iter.size_hint().0;
    ///
    /// iter.next();
    /// assert_eq!(iter.size_hint().0, size - 1);
    ///
    /// iter.next();
    /// assert_eq!(iter.size_hint().0, size - 2);
    ///
    /// iter.next();
    /// assert_eq!(iter.size_hint().0, size - 3);
    ///
    /// # Ok::<(), Error>(())
    /// ```
    fn size_hint(&self) -> (usize, Option<usize>) {
        (self.len, Some(self.len))
    }
}

impl<T, A> Drop for IntoIter<T, A>
where
    A: Allocator,
{
    fn drop(&mut self) {
        // SAFETY: Drop the remaining vector's elements in place, before we free the backing
        // memory.
        unsafe { ptr::drop_in_place(self.as_raw_mut_slice()) };

        // If `cap == 0` we never allocated any memory in the first place.
        if self.cap != 0 {
            // SAFETY: `self.buf` was previously allocated with `self.alloc`.
            unsafe { self.alloc.free(self.buf.as_ptr().cast()) };
        }
    }
}

impl<T, A> IntoIterator for KVec<T, A>
where
    A: Allocator,
{
    type Item = T;
    type IntoIter = IntoIter<T, A>;

    /// Creates a consuming iterator, that is, one that moves each value out of
    /// the vector (from start to end). The vector cannot be used after calling
    /// this.
    ///
    /// # Examples
    ///
    /// ```
    /// let v = kernel::kvec![1, 2]?;
    /// let mut v_iter = v.into_iter();
    ///
    /// let first_element: Option<u32> = v_iter.next();
    ///
    /// assert_eq!(first_element, Some(1));
    /// assert_eq!(v_iter.next(), Some(2));
    /// assert_eq!(v_iter.next(), None);
    ///
    /// # Ok::<(), Error>(())
    /// ```
    #[inline]
    fn into_iter(self) -> Self::IntoIter {
        let (ptr, len, cap, alloc) = self.into_raw_parts_alloc();

        IntoIter {
            ptr,
            buf: unsafe { NonNull::new_unchecked(ptr) },
            len,
            cap,
            alloc,
        }
    }
}
