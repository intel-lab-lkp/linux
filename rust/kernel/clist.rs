// SPDX-License-Identifier: GPL-2.0

//! A C doubly circular intrusive linked list interface for rust code.
//!
//! # Examples
//!
//! ```
//! use kernel::{
//!     bindings,
//!     clist_create,
//!     types::Opaque, //
//! };
//! # // Create test list with values (0, 10, 20) - normally done by C code but it is
//! # // emulated here for doctests using the C bindings.
//! # use core::mem::MaybeUninit;
//! #
//! # /// C struct with embedded `list_head` (typically will be allocated by C code).
//! # #[repr(C)]
//! # pub struct SampleItemC {
//! #     pub value: i32,
//! #     pub link: bindings::list_head,
//! # }
//! #
//! # let mut head = MaybeUninit::<bindings::list_head>::uninit();
//! #
//! # let head = head.as_mut_ptr();
//! # // SAFETY: head and all the items are test objects allocated in this scope.
//! # unsafe { bindings::INIT_LIST_HEAD(head) };
//! #
//! # let mut items = [
//! #     MaybeUninit::<SampleItemC>::uninit(),
//! #     MaybeUninit::<SampleItemC>::uninit(),
//! #     MaybeUninit::<SampleItemC>::uninit(),
//! # ];
//! #
//! # for (i, item) in items.iter_mut().enumerate() {
//! #     let ptr = item.as_mut_ptr();
//! #     // SAFETY: pointers are to allocated test objects with a list_head field.
//! #     unsafe {
//! #         (*ptr).value = i as i32 * 10;
//! #         // &raw mut computes address of link directly as link is uninitialized.
//! #         bindings::INIT_LIST_HEAD(&raw mut (*ptr).link);
//! #         bindings::list_add_tail(&mut (*ptr).link, head);
//! #     }
//! # }
//!
//! // Rust wrapper for the C struct.
//! // The list item struct in this example is defined in C code as:
//! //   struct SampleItemC {
//! //       int value;
//! //       struct list_head link;
//! //   };
//! //
//! #[repr(transparent)]
//! pub struct Item(Opaque<SampleItemC>);
//!
//! impl Item {
//!     pub fn value(&self) -> i32 {
//!         // SAFETY: [`Item`] has same layout as [`SampleItemC`].
//!         unsafe { (*self.0.get()).value }
//!     }
//! }
//!
//! // Create typed [`CList`] from sentinel head.
//! // SAFETY: head is valid, items are [`SampleItemC`] with embedded `link` field.
//! let list = unsafe { clist_create!(head, Item, SampleItemC, link) };
//!
//! // Iterate directly over typed items.
//! let mut found_0 = false;
//! let mut found_10 = false;
//! let mut found_20 = false;
//!
//! for item in list.iter() {
//!     let val = item.value();
//!     if val == 0 { found_0 = true; }
//!     if val == 10 { found_10 = true; }
//!     if val == 20 { found_20 = true; }
//! }
//!
//! assert!(found_0 && found_10 && found_20);
//! ```

use core::{
    iter::FusedIterator,
    marker::PhantomData, //
};

use crate::{
    bindings,
    types::Opaque, //
};

use pin_init::{
    pin_data,
    pin_init,
    PinInit //
};

/// Wraps a `list_head` object for use in intrusive linked lists.
///
/// # Invariants
///
/// - [`CListHead`] represents an allocated and valid `list_head` structure.
#[pin_data]
#[repr(transparent)]
pub struct CListHead {
    #[pin]
    inner: Opaque<bindings::list_head>,
}

impl CListHead {
    /// Create a `&CListHead` reference from a raw `list_head` pointer.
    ///
    /// # Safety
    ///
    /// - `ptr` must be a valid pointer to an allocated and initialized `list_head` structure.
    /// - `ptr` must remain valid and unmodified for the lifetime `'a`.
    /// - The list and all linked `list_head` nodes must not be modified by non-Rust code
    ///   for the lifetime `'a`.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::list_head) -> &'a Self {
        // SAFETY:
        // - [`CListHead`] has same layout as `list_head`.
        // - `ptr` is valid and unmodified for 'a per caller guarantees.
        unsafe { &*ptr.cast() }
    }

    /// Get the raw `list_head` pointer.
    #[inline]
    pub fn as_raw(&self) -> *mut bindings::list_head {
        self.inner.get()
    }

    /// Get the next [`CListHead`] in the list.
    #[inline]
    pub fn next(&self) -> &Self {
        let raw = self.as_raw();
        // SAFETY:
        // - `self.as_raw()` is valid per type invariants.
        // - The `next` pointer is guaranteed to be non-NULL.
        unsafe { Self::from_raw((*raw).next) }
    }

    /// Check if this node is linked in a list (not isolated).
    #[inline]
    pub fn is_linked(&self) -> bool {
        let raw = self.as_raw();
        // SAFETY: self.as_raw() is valid per type invariants.
        unsafe { (*raw).next != raw && (*raw).prev != raw }
    }

    /// Pin-initializer that initializes the list head.
    pub fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            // SAFETY: `INIT_LIST_HEAD` initializes `slot` to a valid empty list.
            inner <- Opaque::ffi_init(|slot| unsafe { bindings::INIT_LIST_HEAD(slot) }),
        })
    }
}

// SAFETY: [`CListHead`] can be sent to any thread.
unsafe impl Send for CListHead {}

// SAFETY: [`CListHead`] can be shared among threads as it is not modified
// by non-Rust code per safety requirements of [`CListHead::from_raw`].
unsafe impl Sync for CListHead {}

impl PartialEq for CListHead {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        core::ptr::eq(self, other)
    }
}

impl Eq for CListHead {}

/// Low-level iterator over `list_head` nodes.
///
/// An iterator used to iterate over a C intrusive linked list (`list_head`). Caller has to
/// perform conversion of returned [`CListHead`] to an item (using `container_of` macro or similar).
///
/// # Invariants
///
/// [`CListHeadIter`] is iterating over an allocated, initialized and valid list.
struct CListHeadIter<'a> {
    /// Current position in the list.
    current: &'a CListHead,
    /// The sentinel head (used to detect end of iteration).
    sentinel: &'a CListHead,
}

impl<'a> Iterator for CListHeadIter<'a> {
    type Item = &'a CListHead;

    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        // Check if we've reached the sentinel (end of list).
        if self.current == self.sentinel {
            return None;
        }

        let item = self.current;
        self.current = item.next();
        Some(item)
    }
}

impl<'a> FusedIterator for CListHeadIter<'a> {}

/// A typed C linked list with a sentinel head.
///
/// A sentinel head represents the entire linked list and can be used for
/// iteration over items of type `T`, it is not associated with a specific item.
///
/// The const generic `OFFSET` specifies the byte offset of the `list_head` field within
/// the struct that `T` wraps.
///
/// # Invariants
///
/// - The [`CListHead`] is an allocated and valid sentinel C `list_head` structure.
/// - `OFFSET` is the byte offset of the `list_head` field within the struct that `T` wraps.
/// - All the list's `list_head` nodes are allocated and have valid next/prev pointers.
#[repr(transparent)]
pub struct CList<T, const OFFSET: usize>(CListHead, PhantomData<T>);

impl<T, const OFFSET: usize> CList<T, OFFSET> {
    /// Create a typed [`CList`] reference from a raw sentinel `list_head` pointer.
    ///
    /// # Safety
    ///
    /// - `ptr` must be a valid pointer to an allocated and initialized `list_head` structure
    ///   representing a list sentinel.
    /// - `ptr` must remain valid and unmodified for the lifetime `'a`.
    /// - The list must contain items where the `list_head` field is at byte offset `OFFSET`.
    /// - `T` must be `#[repr(transparent)]` over the C struct.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::list_head) -> &'a Self {
        // SAFETY:
        // - [`CList`] has same layout as [`CListHead`] due to repr(transparent).
        // - Caller guarantees `ptr` is a valid, sentinel `list_head` object.
        unsafe { &*ptr.cast() }
    }

    /// Check if the list is empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        !self.0.is_linked()
    }

    /// Create an iterator over typed items.
    #[inline]
    pub fn iter(&self) -> CListIter<'_, T, OFFSET> {
        let head = &self.0;
        CListIter {
            head_iter: CListHeadIter {
                current: head.next(),
                sentinel: head,
            },
            _phantom: PhantomData,
        }
    }
}

/// High-level iterator over typed list items.
pub struct CListIter<'a, T, const OFFSET: usize> {
    head_iter: CListHeadIter<'a>,
    _phantom: PhantomData<&'a T>,
}

impl<'a, T, const OFFSET: usize> Iterator for CListIter<'a, T, OFFSET> {
    type Item = &'a T;

    fn next(&mut self) -> Option<Self::Item> {
        let head = self.head_iter.next()?;

        // Convert to item using OFFSET.
        // SAFETY: `item_ptr` calculation from `OFFSET` (calculated using offset_of!)
        // is valid per invariants.
        Some(unsafe { &*head.as_raw().byte_sub(OFFSET).cast::<T>() })
    }
}

impl<'a, T, const OFFSET: usize> FusedIterator for CListIter<'a, T, OFFSET> {}

/// Create a C doubly-circular linked list interface `CList` from a raw `list_head` pointer.
///
/// This macro creates a `CList<T, OFFSET>` that can iterate over items of type `$rust_type`
/// linked via the `$field` field in the underlying C struct `$c_type`.
///
/// # Arguments
///
/// - `$head`: Raw pointer to the sentinel `list_head` object (`*mut bindings::list_head`).
/// - `$rust_type`: Each item's rust wrapper type.
/// - `$c_type`: Each item's C struct type that contains the embedded `list_head`.
/// - `$field`: The name of the `list_head` field within the C struct.
///
/// # Safety
///
/// This is an unsafe macro. The caller must ensure:
///
/// - `$head` is a valid, initialized sentinel `list_head` pointing to a list that remains
///   unmodified for the lifetime of the rust `CList`.
/// - The list contains items of type `$c_type` linked via an embedded `$field`.
/// - `$rust_type` is `#[repr(transparent)]` over `$c_type` or has compatible layout.
///
/// # Examples
///
/// Refer to the examples in this module's documentation.
#[macro_export]
macro_rules! clist_create {
    ($head:expr, $rust_type:ty, $c_type:ty, $($field:tt).+) => {{
        // Compile-time check that field path is a list_head.
        let _: fn(*const $c_type) -> *const $crate::bindings::list_head =
            |p| &raw const (*p).$($field).+;

        // Calculate offset and create `CList`.
        const OFFSET: usize = ::core::mem::offset_of!($c_type, $($field).+);
        $crate::clist::CList::<$rust_type, OFFSET>::from_raw($head)
    }};
}
