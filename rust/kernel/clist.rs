// SPDX-License-Identifier: GPL-2.0

//! List processing module for C list_head linked lists.
//!
//! C header: [`include/linux/list.h`](srctree/include/linux/list.h)
//!
//! Provides a safe interface for iterating over C's intrusive `list_head` structures.
//! At the moment, supports only read-only iteration.
//!
//! # Examples
//!
//! ```ignore
//! use core::ptr::NonNull;
//! use kernel::{
//!     bindings,
//!     clist,
//!     container_of,
//!     prelude::*, //
//! };
//!
//! // Example C-side struct (typically from C bindings):
//! // struct c_item {
//! //     u64 offset;
//! //     struct list_head link;
//! //     /* ... other fields ... */
//! // };
//!
//! // Rust-side struct to hold pointer to C-side struct.
//! struct Item {
//!     ptr: NonNull<bindings::c_item>,
//! }
//!
//! impl clist::FromListHead for Item {
//!     unsafe fn from_list_head(link: *const bindings::list_head) -> Self {
//!         let item_ptr = container_of!(link, bindings::c_item, link) as *mut _;
//!         Item { ptr: NonNull::new_unchecked(item_ptr) }
//!     }
//! }
//!
//! impl Item {
//!     fn offset(&self) -> u64 {
//!         unsafe { (*self.ptr.as_ptr()).offset }
//!     }
//! }
//!
//! // Get the list head from C code.
//! let c_list_head = unsafe { bindings::get_item_list() };
//!
//! // Rust wraps and iterates over the list.
//! let list = unsafe { clist::Clist::<Item>::new(c_list_head) };
//!
//! // Check if empty.
//! if list.is_empty() {
//!     pr_info!("List is empty\n");
//! }
//!
//! // Iterate over items.
//! for item in list.iter() {
//!     pr_info!("Item offset: {}\n", item.offset());
//! }
//! ```

use crate::bindings;
use core::marker::PhantomData;

/// Trait for types that can be constructed from a C list_head pointer.
///
/// This typically encapsulates `container_of` logic, allowing iterators to
/// work with high-level types without knowing about C struct layout details.
///
/// # Safety
///
/// Implementors must ensure that `from_list_head` correctly converts the
/// `list_head` pointer to the containing struct pointer using proper offset
/// calculations (typically via container_of! macro).
///
/// # Examples
///
/// ```ignore
/// impl FromListHead for MyItem {
///     unsafe fn from_list_head(link: *const bindings::list_head) -> Self {
///         let item_ptr = container_of!(link, bindings::my_c_struct, link_field) as *mut _;
///         MyItem { ptr: NonNull::new_unchecked(item_ptr) }
///     }
/// }
/// ```
pub trait FromListHead: Sized {
    /// Create instance from list_head pointer embedded in containing struct.
    ///
    /// # Safety
    ///
    /// Caller must ensure `link` points to a valid list_head embedded in the
    /// containing struct, and that the containing struct is valid for the
    /// lifetime of the returned instance.
    unsafe fn from_list_head(link: *const bindings::list_head) -> Self;
}

/// Iterator over C list items.
///
/// Uses the [`FromListHead`] trait to convert list_head pointers to
/// Rust types and iterate over them.
///
/// # Invariants
/// - `head` points to a valid, initialized list_head.
/// - `current` points to a valid node in the list.
/// - The list is not modified during iteration.
///
/// # Examples
///
/// ```ignore
/// // Iterate over blocks in a C list_head
/// for block in clist::iter_list_head::<Block>(&list_head) {
///     // Process block
/// }
/// ```
pub struct ClistIter<'a, T: FromListHead> {
    current: *const bindings::list_head,
    head: *const bindings::list_head,
    _phantom: PhantomData<&'a T>,
}

// SAFETY: Safe to send iterator if T is Send.
unsafe impl<'a, T: FromListHead + Send> Send for ClistIter<'a, T> {}

impl<'a, T: FromListHead> Iterator for ClistIter<'a, T> {
    type Item = T;

    fn next(&mut self) -> Option<Self::Item> {
        // SAFETY: Pointers are valid per the type's invariants. The list
        // structure is valid and we traverse according to kernel list semantics.
        unsafe {
            self.current = (*self.current).next;

            if self.current == self.head {
                return None;
            }

            // Use the trait to convert list_head to T.
            Some(T::from_list_head(self.current))
        }
    }
}

/// Create a read-only iterator over a C list_head.
///
/// This is a convenience function for creating iterators directly
/// from a list_head reference.
///
/// # Safety
///
/// Caller must ensure:
/// - `head` is a valid, initialized list_head.
/// - All items in the list are valid instances that can be converted via [`FromListHead`].
/// - The list is not modified during iteration.
///
/// # Examples
///
/// ```ignore
/// // Iterate over items in a C list.
/// for item in clist::iter_list_head::<Item>(&c_list_head) {
///     // Process item
/// }
/// ```
pub fn iter_list_head<'a, T: FromListHead>(head: &'a bindings::list_head) -> ClistIter<'a, T> {
    ClistIter {
        current: head as *const _,
        head: head as *const _,
        _phantom: PhantomData,
    }
}

/// Check if a C list is empty.
///
/// # Safety
///
/// Caller must ensure `head` points to a valid, initialized list_head.
#[inline]
pub unsafe fn list_empty(head: *const bindings::list_head) -> bool {
    // SAFETY: Caller ensures head is valid and initialized.
    unsafe { bindings::list_empty(head) }
}

/// Initialize a C list_head to an empty list.
///
/// # Safety
///
/// Caller must ensure `head` points to valid memory for a list_head.
#[inline]
pub unsafe fn init_list_head(head: *mut bindings::list_head) {
    // SAFETY: Caller ensures head points to valid memory for a list_head.
    unsafe { bindings::INIT_LIST_HEAD(head) }
}

/// An interface to C list_head structures.
///
/// This provides an iterator-based interface for traversing C's intrusive
/// linked lists. Items must implement the [`FromListHead`] trait.
///
/// Designed for iterating over lists created and managed by C code (e.g.,
/// drm_buddy block lists). [`Clist`] does not own the `list_head` or the items.
/// It's a non-owning view for iteration.
///
/// # Invariants
/// - `head` points to a valid, initialized list_head.
/// - All items in the list are valid instances of `T`.
/// - The list is not modified while iterating.
///
/// # Thread Safety
/// [`Clist`] can have its ownership transferred between threads ([`Send`]) if `T: Send`.
/// But its never [`Sync`] - concurrent Rust threads with `&Clist` could call C FFI unsafely.
/// For concurrent access, wrap in a `Mutex` or other synchronization primitive.
///
/// # Examples
///
/// ```ignore
/// use kernel::clist::Clist;
///
/// // C code provides the populated list_head.
/// let list = unsafe { Clist::<Item>::new(c_list_head) };
///
/// // Iterate over items.
/// for item in list.iter() {
///     // Process item.
/// }
/// ```
pub struct Clist<T: FromListHead> {
    head: *mut bindings::list_head,
    _phantom: PhantomData<T>,
}

// SAFETY: Safe to send Clist if T is Send (pointer moves, C data stays in place).
unsafe impl<T: FromListHead + Send> Send for Clist<T> {}

impl<T: FromListHead> Clist<T> {
    /// Wrap an existing C list_head pointer without taking ownership.
    ///
    /// # Safety
    ///
    /// Caller must ensure:
    /// - `head` points to a valid, initialized list_head.
    /// - `head` remains valid for the lifetime of the returned [`Clist`].
    /// - The list is not modified by C code while [`Clist`] exists. Caller must
    ///   implement mutual exclusion algorithms if required, to coordinate with C.
    /// - Caller is responsible for requesting or working with C to free `head` if needed.
    pub unsafe fn new(head: *mut bindings::list_head) -> Self {
        // SAFETY: Caller ensures head is valid and initialized
        Self {
            head,
            _phantom: PhantomData,
        }
    }

    /// Check if the list is empty.
    ///
    /// # Examples
    ///
    /// ```ignore
    /// let list = Clist::<Block>::new()?;
    /// assert!(list.is_empty());
    /// ```
    #[inline]
    pub fn is_empty(&self) -> bool {
        // SAFETY: self.head points to valid list_head per invariant.
        unsafe { list_empty(self.head) }
    }

    /// Iterate over items in the list.
    ///
    /// # Examples
    ///
    /// ```ignore
    /// for item in list.iter() {
    ///     // Process item
    /// }
    /// ```
    pub fn iter(&self) -> ClistIter<'_, T> {
        // SAFETY: self.head points to valid list_head per invariant.
        unsafe { iter_list_head::<T>(&*self.head) }
    }

    /// Get the raw list_head pointer.
    ///
    /// This is useful when interfacing with C APIs that need the list_head pointer.
    pub fn as_raw(&self) -> *mut bindings::list_head {
        self.head
    }
}

impl<'a, T: FromListHead> IntoIterator for &'a Clist<T> {
    type Item = T;
    type IntoIter = ClistIter<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}
