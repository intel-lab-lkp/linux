// SPDX-License-Identifier: GPL-2.0

//! Containers that are known not to be empty.

use core::ops::{
    Deref,
    DerefMut, //
};

/// A container that can report whether it holds any element.
///
/// The result must not change while the container is only reachable through a shared reference.
///
/// Unsafe code may only rely on the result for a container type whose implementation it trusts,
/// such as the ones in this module, since a safe implementation can return anything.
pub trait IsEmpty {
    /// Returns `true` if the container holds no element.
    fn is_empty(&self) -> bool;
}

impl<T> IsEmpty for [T] {
    #[inline]
    fn is_empty(&self) -> bool {
        <[T]>::is_empty(self)
    }
}

/// A container that holds at least one element.
///
/// [`NonEmpty`] dereferences to the container it wraps, so the container's API stays available.
/// Methods whose result is only optional because the container could be empty can be shadowed by
/// versions that return the result directly. They are added as users need them. For now that is
/// only [`NonEmpty::last_mut`] on a slice, which returns `&mut T` instead of `Option<&mut T>`.
///
/// # Invariants
///
/// The wrapped container is not empty. Unsafe code may rely on this only for container types
/// whose [`IsEmpty`] implementation is in this module.
///
/// # Examples
///
/// ```
/// use kernel::non_empty::NonEmpty;
///
/// let mut buf = [1u8, 2, 3];
/// let slice = NonEmpty::try_from_mut(&mut buf[..]).ok_or(EINVAL)?;
/// *slice.last_mut() = 0;
/// assert_eq!(slice.len(), 3);
/// assert_eq!(buf, [1, 2, 0]);
///
/// let mut empty: [u8; 0] = [];
/// assert!(NonEmpty::try_from_mut(&mut empty[..]).is_none());
/// # Ok::<(), Error>(())
/// ```
#[repr(transparent)]
#[derive(Debug)]
pub struct NonEmpty<C: ?Sized>(C);

impl<C: ?Sized + IsEmpty> NonEmpty<C> {
    /// Creates a [`NonEmpty`] from a mutable reference to a container, or returns [`None`] if the
    /// container is empty.
    #[inline]
    pub fn try_from_mut(container: &mut C) -> Option<&mut Self> {
        if container.is_empty() {
            return None;
        }

        // INVARIANT: `container` was checked to be non-empty above.
        // SAFETY: `NonEmpty<C>` is transparent to `C`, so the cast keeps both the layout and the
        // pointer metadata, and the pointer comes from a valid `&mut C` with the same lifetime.
        Some(unsafe { &mut *(core::ptr::from_mut(container) as *mut Self) })
    }
}

impl<T> NonEmpty<[T]> {
    /// Returns a mutable reference to the last element of the slice.
    #[inline]
    pub fn last_mut(&mut self) -> &mut T {
        // SAFETY: By the type invariant, the slice is not empty, so it has a last element.
        unsafe { self.0.last_mut().unwrap_unchecked() }
    }
}

impl<C: ?Sized> Deref for NonEmpty<C> {
    type Target = C;

    #[inline]
    fn deref(&self) -> &C {
        &self.0
    }
}

// `DerefMut` is only sound for containers whose length cannot change through a mutable
// reference. For a growable container such as `KVec`, `clear()` would empty it.
impl<T> DerefMut for NonEmpty<[T]> {
    #[inline]
    fn deref_mut(&mut self) -> &mut [T] {
        &mut self.0
    }
}
