// SPDX-License-Identifier: GPL-2.0

//! A C doubly circular intrusive linked list interface for rust code.
//!
//! # Examples
//!
//! ```
//! use kernel::{
//!     bindings,
//!     clist::init_list_head,
//!     clist_create,
//!     types::Opaque, //
//! };
//! # // Create test list with values (0, 10, 20) - normally done by C code but it is
//! # // emulated here for doctests using the C bindings.
//! # use core::mem::MaybeUninit;
//! #
//! # /// C struct with embedded `list_head` (typically will be allocated by C code).
//! # #[repr(C)]
//! # pub(crate) struct SampleItemC {
//! #     pub value: i32,
//! #     pub link: bindings::list_head,
//! # }
//! #
//! # let mut head = MaybeUninit::<bindings::list_head>::uninit();
//! #
//! # // SAFETY: head and all the items are test objects allocated in this scope.
//! # unsafe { init_list_head(head.as_mut_ptr()) };
//! # // SAFETY: head is a test object allocated in this scope.
//! # let mut head = unsafe { head.assume_init() };
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
//! #         // addr_of_mut!() computes address of link directly as link is uninitialized.
//! #         init_list_head(core::ptr::addr_of_mut!((*ptr).link));
//! #         bindings::list_add_tail(&mut (*ptr).link, &mut head);
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
//! pub(crate) struct Item(Opaque<SampleItemC>);
//!
//! impl Item {
//!     pub(crate) fn value(&self) -> i32 {
//!         // SAFETY: `Item` has same layout as `SampleItemC`.
//!         unsafe { (*self.0.get()).value }
//!     }
//! }
//!
//! // Create typed Clist from sentinel head.
//! // SAFETY: head is valid, items are `SampleItemC` with embedded `link` field.
//! let list = unsafe { clist_create!(&mut head, Item, SampleItemC, link) };
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

/// Initialize a `list_head` object to point to itself.
///
/// # Safety
///
/// `list` must be a valid pointer to a `list_head` object.
#[inline]
pub unsafe fn init_list_head(list: *mut bindings::list_head) {
    // SAFETY: Caller guarantees `list` is a valid pointer to a `list_head`.
    unsafe {
        (*list).next = list;
        (*list).prev = list;
    }
}

/// Wraps a `list_head` object for use in intrusive linked lists.
///
/// # Invariants
///
/// - `ClistHead` represents an allocated and valid `list_head` structure.
///
/// # Safety
///
/// - All `list_head` nodes must not be modified by C code for the lifetime of `ClistHead`.
#[repr(transparent)]
pub struct ClistHead(Opaque<bindings::list_head>);

impl ClistHead {
    /// Create a `&ClistHead` reference from a raw `list_head` pointer.
    ///
    /// # Safety
    ///
    /// - `ptr` must be a valid pointer to an allocated and initialized `list_head` structure.
    /// - `ptr` must remain valid and unmodified for the lifetime `'a`.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::list_head) -> &'a Self {
        // SAFETY:
        // - `ClistHead` has same layout as `list_head`.
        // - `ptr` is valid and unmodified for 'a.
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
        let raw = self.as_raw();
        // SAFETY:
        // - `self.as_raw()` is valid per type invariants.
        // - The `next` pointer is guaranteed to be non-NULL.
        unsafe { Self::from_raw((*raw).next) }
    }

    /// Get the previous `ClistHead` in the list.
    #[inline]
    pub fn prev(&self) -> &Self {
        let raw = self.as_raw();
        // SAFETY:
        // - self.as_raw() is valid per type invariants.
        // - The `prev` pointer is guaranteed to be non-NULL.
        unsafe { Self::from_raw((*raw).prev) }
    }

    /// Check if this node is linked in a list (not isolated).
    #[inline]
    pub fn is_linked(&self) -> bool {
        let raw = self.as_raw();
        // SAFETY: self.as_raw() is valid per type invariants.
        unsafe { (*raw).next != raw && (*raw).prev != raw }
    }
}

// SAFETY: `ClistHead` can be sent to any thread.
unsafe impl Send for ClistHead {}

// SAFETY: `ClistHead` can be shared among threads as it is not modified by C per type invariants.
unsafe impl Sync for ClistHead {}

impl PartialEq for ClistHead {
    fn eq(&self, other: &Self) -> bool {
        self.as_raw() == other.as_raw()
    }
}

impl Eq for ClistHead {}

/// Low-level iterator over `list_head` nodes.
///
/// An iterator used to iterate over a C intrusive linked list (`list_head`). Caller has to
/// perform conversion of returned `ClistHead` to an item (using `container_of` macro or similar).
///
/// # Invariants
///
/// `ClistHeadIter` is iterating over an allocated, initialized and valid list.
struct ClistHeadIter<'a> {
    current_head: &'a ClistHead,
    list_head: &'a ClistHead,
    exhausted: bool,
}

impl<'a> Iterator for ClistHeadIter<'a> {
    type Item = &'a ClistHead;

    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        if self.exhausted {
            return None;
        }

        // Advance to next node.
        self.current_head = self.current_head.next();

        // Check if we've circled back to the sentinel head.
        if self.current_head == self.list_head {
            self.exhausted = true;
            return None;
        }

        Some(self.current_head)
    }
}

impl<'a> FusedIterator for ClistHeadIter<'a> {}

/// A typed C linked list with a sentinel head.
///
/// A sentinel head represents the entire linked list and can be used for
/// iteration over items of type `T`, it is not associated with a specific item.
///
/// # Invariants
///
/// - `head` is an allocated and valid C `list_head` structure that is the list's sentinel.
/// - `offset` is the byte offset of the `list_head` field within the C struct that `T` wraps.
///
/// # Safety
///
/// - All the list's `list_head` nodes must be allocated and have valid next/prev pointers.
/// - The underlying `list_head` (and entire list) must not be modified by C for the
///   lifetime 'a of `Clist`.
pub struct Clist<'a, T> {
    head: &'a ClistHead,
    offset: usize,
    _phantom: PhantomData<&'a T>,
}

impl<'a, T> Clist<'a, T> {
    /// Create a typed `Clist` from a raw sentinel `list_head` pointer.
    ///
    /// The const generic `OFFSET` specifies the byte offset of the `list_head` field within
    /// the C struct that `T` wraps.
    ///
    /// # Safety
    ///
    /// - `ptr` must be a valid pointer to an allocated and initialized `list_head` structure
    ///   representing a list sentinel.
    /// - `ptr` must remain valid and unmodified for the lifetime `'a`.
    /// - The list must contain items where the `list_head` field is at byte offset `OFFSET`.
    /// - `T` must be `#[repr(transparent)]` over the C struct.
    #[inline]
    pub unsafe fn from_raw_and_offset<const OFFSET: usize>(ptr: *mut bindings::list_head) -> Self {
        Self {
            // SAFETY: Caller guarantees `ptr` is a valid, sentinel `list_head` object.
            head: unsafe { ClistHead::from_raw(ptr) },
            offset: OFFSET,
            _phantom: PhantomData,
        }
    }

    /// Get the raw sentinel `list_head` pointer.
    #[inline]
    pub fn as_raw(&self) -> *mut bindings::list_head {
        self.head.as_raw()
    }

    /// Check if the list is empty.
    #[inline]
    pub fn is_empty(&self) -> bool {
        let raw = self.as_raw();
        // SAFETY: self.as_raw() is valid per type invariants.
        unsafe { (*raw).next == raw }
    }

    /// Create an iterator over typed items.
    #[inline]
    pub fn iter(&self) -> ClistIter<'a, T> {
        ClistIter {
            head_iter: ClistHeadIter {
                current_head: self.head,
                list_head: self.head,
                exhausted: false,
            },
            offset: self.offset,
            _phantom: PhantomData,
        }
    }
}

/// High-level iterator over typed list items.
pub struct ClistIter<'a, T> {
    head_iter: ClistHeadIter<'a>,
    offset: usize,
    _phantom: PhantomData<&'a T>,
}

impl<'a, T> Iterator for ClistIter<'a, T> {
    type Item = &'a T;

    fn next(&mut self) -> Option<Self::Item> {
        let head = self.head_iter.next()?;

        // Convert to item using offset.
        // SAFETY:
        // - `item_ptr` calculation from `offset` (calculated using offset_of!)
        //    is valid per invariants.
        Some(unsafe { &*head.as_raw().cast::<u8>().sub(self.offset).cast::<T>() })
    }
}

impl<'a, T> FusedIterator for ClistIter<'a, T> {}

/// Create a C doubly-circular linked list interface `Clist` from a raw `list_head` pointer.
///
/// This macro creates a `Clist<T>` that can iterate over items of type `$rust_type` linked
/// via the `$field` field in the underlying C struct `$c_type`.
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
/// The caller must ensure:
/// - `$head` is a valid, initialized sentinel `list_head` pointing to a list that remains
///   unmodified for the lifetime of the rust `Clist`.
/// - The list contains items of type `$c_type` linked via an embedded `$field`.
/// - `$rust_type` is `#[repr(transparent)]` over `$c_type` or has compatible layout.
/// - The macro is called from an unsafe block.
///
/// # Examples
///
/// Refer to the examples in the [crate::clist] module documentation.
#[macro_export]
macro_rules! clist_create {
    ($head:expr, $rust_type:ty, $c_type:ty, $field:ident) => {
        $crate::clist::Clist::<$rust_type>::from_raw_and_offset::<
            { ::core::mem::offset_of!($c_type, $field) },
        >($head)
    };
}
