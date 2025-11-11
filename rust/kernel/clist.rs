// SPDX-License-Identifier: GPL-2.0

//! A C doubly circular intrusive linked list interface for rust code.
//!
//! TODO: Doctest example will be added in later commit in series due to dependencies.

use crate::{
    bindings,
    types::Opaque, //
};

/// A C linked list with a sentinel head
///
/// A sentinel head is one which is not embedded in an item. It represents the entire
/// linked list and can be used for add, remove, empty operations etc.
///
/// # Invariants
///
/// - `Clist` wraps an allocated and valid C list_head structure that is the sentinel of a list.
/// - All the `list_head` nodes in the list are allocated and have valid next/prev pointers.
/// - The underlying `list_head` (and entire list) is not modified by C.
#[repr(transparent)]
pub struct Clist(ClistHead);

// SAFETY: `Clist` can be sent to any thread.
unsafe impl Send for Clist {}
// SAFETY: `Clist` can be shared among threads as it is not modified by C per type invariants.
unsafe impl Sync for Clist {}

impl Clist {
    /// Create a `&Clist` from a raw sentinel `list_head` pointer.
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to an allocated and initialized `list_head` structure
    /// representing a list sentinel, and it must remain valid for the lifetime `'a`.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::list_head) -> &'a Self {
        // SAFETY:
        // - `ClistHead` has same layout as `list_head`.
        // - `ptr` is valid for 'a.
        unsafe { &*ptr.cast() }
    }

    /// Get the raw sentinel `list_head` pointer.
    #[inline]
    pub fn as_raw(&self) -> *mut bindings::list_head {
        self.0.as_raw()
    }

    /// Access the underlying `ClistHead`.
    #[inline]
    pub fn head(&self) -> &ClistHead {
        &self.0
    }

    /// Check if the list is empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// Create a low-level iterator over `ClistHead` nodes. Caller converts the returned
    /// heads into items.
    #[inline]
    pub fn iter_heads(&self) -> ClistHeadIter<'_> {
        ClistHeadIter {
            current: &self.0,
            head: &self.0,
        }
    }
}

/// Wraps a non-sentinel C `list_head` node for use in intrusive linked lists.
///
/// # Invariants
///
/// - `ClistHead` represents an allocated and valid non-sentinel `list_head` structure.
/// - The underlying `list_head` (and entire list) is not modified by C.
#[repr(transparent)]
pub struct ClistHead(Opaque<bindings::list_head>);

// SAFETY: `ClistHead` can be sent to any thread.
unsafe impl Send for ClistHead {}
// SAFETY: `ClistHead` can be shared among threads as it is not modified by C per type invariants.
unsafe impl Sync for ClistHead {}

impl ClistHead {
    /// Create a `&ClistHead` reference from a raw `list_head` pointer.
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to an allocated and initialized `list_head` structure,
    /// and it must remain valid for the lifetime `'a`.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::list_head) -> &'a Self {
        // SAFETY:
        // - `ClistHead` has same layout as `list_head`.
        // - `ptr` is valid for 'a.
        unsafe { &*ptr.cast() }
    }

    /// Get the raw `list_head` pointer.
    #[inline]
    pub fn as_raw(&self) -> *mut bindings::list_head {
        self.0.get()
    }

    /// Get the next `ClistHead` in the list.
    #[inline]
    pub fn next(&self) -> &Self {
        // SAFETY:
        // - `self.as_raw()` is valid per type invariants.
        // - The `next` pointer is guaranteed to be non-NULL.
        unsafe {
            let raw = self.as_raw();
            Self::from_raw((*raw).next)
        }
    }

    /// Get the previous `ClistHead` in the list.
    #[inline]
    pub fn prev(&self) -> &Self {
        // SAFETY:
        // - self.as_raw() is valid per type invariants.
        // - The `prev` pointer is guaranteed to be non-NULL.
        unsafe {
            let raw = self.as_raw();
            Self::from_raw((*raw).prev)
        }
    }

    /// Check if this node is linked in a list (not isolated).
    #[inline]
    pub fn is_in_list(&self) -> bool {
        // SAFETY: self.as_raw() is valid per type invariants.
        unsafe {
            let raw = self.as_raw();
            (*raw).next != raw && (*raw).prev != raw
        }
    }

    /// Check if the list is empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        // SAFETY: self.as_raw() is valid per type invariants.
        unsafe {
            let raw = self.as_raw();
            (*raw).next == raw
        }
    }
}

/// Low-level iterator over `list_head` nodes.
///
/// An iterator used to iterate over a C intrusive linked list (`list_head`). Caller has to
/// perform conversion of returned `ClistHead` to an item (typically using `container_of` macro).
///
/// # Invariants
///
/// `ClistHeadIter` is iterating over an allocated, initialized and valid `Clist`.
pub struct ClistHeadIter<'a> {
    current: &'a ClistHead,
    head: &'a ClistHead,
}

// SAFETY: ClistHeadIter gives out immutable references to ClistHead,
// which is Send.
unsafe impl Send for ClistHeadIter<'_> {}

// SAFETY: ClistHeadIter gives out immutable references to ClistHead,
// which is Sync.
unsafe impl Sync for ClistHeadIter<'_> {}

impl<'a> Iterator for ClistHeadIter<'a> {
    type Item = &'a ClistHead;

    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        // Advance to next node.
        self.current = self.current.next();

        // Check if we've circled back to HEAD.
        if self.current.as_raw() == self.head.as_raw() {
            return None;
        }

        Some(self.current)
    }
}
