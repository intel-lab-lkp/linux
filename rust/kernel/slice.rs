// SPDX-License-Identifier: GPL-2.0

//! Slice abstractions to ease development.

use core::ops::{Deref, DerefMut};

/// A slice that is guaranteed to contain at least one element.
///
/// # Invariants
///
/// `self.0` is not empty.
#[repr(transparent)]
pub struct NonEmptySlice<T>([T]);

impl<T> NonEmptySlice<T> {
    /// Returns `None` if `s` is empty.
    pub fn new(s: &mut [T]) -> Option<&Self> {
        if s.is_empty() {
            return None;
        }
        // SAFETY: `Self` is `repr(transparent)` over `[T]`, so the cast keeps the slice length.
        // INVARIANT: `s` is not empty, checked above.
        Some(unsafe { &*(core::ptr::from_mut(s) as *const Self) })
    }

    /// Returns `None` if `s` is empty.
    pub fn new_mut(s: &mut [T]) -> Option<&mut Self> {
        if s.is_empty() {
            return None;
        }
        // SAFETY: `Self` is `repr(transparent)` over `[T]`, so the cast keeps the slice length.
        // INVARIANT: `s` is not empty, checked above.
        Some(unsafe { &mut *(core::ptr::from_mut(s) as *mut Self) })
    }

    /// Returns a reference to the first element.
    pub fn first(&mut self) -> &T {
        // SAFETY: By the type invariant, `len >= 1`, so `0` is in bounds.
        unsafe { self.0.get_unchecked(0) }
    }

    /// Returns a mutable reference to the first element.
    pub fn first_mut(&mut self) -> &mut T {
        // SAFETY: By the type invariant, `len >= 1`, so `0` is in bounds.
        unsafe { self.0.get_unchecked_mut(0) }
    }

    /// Returns a reference to the last element.
    pub fn last(&mut self) -> &T {
        let last = self.0.len() - 1;
        // SAFETY: By the type invariant, `len >= 1`, so `last` is in bounds.
        unsafe { self.0.get_unchecked(last) }
    }

    /// Returns a mutable reference to the last element.
    pub fn last_mut(&mut self) -> &mut T {
        let last = self.0.len() - 1;
        // SAFETY: By the type invariant, `len >= 1`, so `last` is in bounds.
        unsafe { self.0.get_unchecked_mut(last) }
    }
}

impl<T> Deref for NonEmptySlice<T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        &self.0
    }
}

impl<T> DerefMut for NonEmptySlice<T> {
    // Mutating elements through `&mut [T]` can't change the length, so the invariant holds.
    fn deref_mut(&mut self) -> &mut [T] {
        &mut self.0
    }
}
