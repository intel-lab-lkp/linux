// SPDX-License-Identifier: GPL-2.0

//! A C doubly circular intrusive linked list interface for rust code.
//!
//! # Examples
//!
//! ```
//! use kernel::{bindings, clist::Clist, clist_iterate, impl_from_list_head, types::Opaque};
//! # // Create test list with values (0, 10, 20) - normally done by C code but it is
//! # // emulated here for doctests using the C bindings.
//! # use core::mem::MaybeUninit;
//! #
//! # /// C struct with embedded list_head (typically will be allocated by C code).
//! # #[repr(C)]
//! # pub(crate) struct SampleItemC {
//! #     pub value: i32,
//! #     pub link: bindings::list_head,
//! # }
//! #
//! # let mut head = MaybeUninit::<bindings::list_head>::uninit();
//! #
//! # // SAFETY: head and all the items are test objects allocated in this scope.
//! # unsafe { bindings::INIT_LIST_HEAD(head.as_mut_ptr()) };
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
//! #         bindings::INIT_LIST_HEAD(&mut (*ptr).link);
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
//! // Generate the link type.
//! impl_from_list_head!(pub(crate), Item, SampleItemC, link);
//!
//! impl Item {
//!     pub(crate) fn value(&self) -> i32 {
//!         // SAFETY: Item has same layout as SampleItemC.
//!         unsafe { (*self.0.get()).value }
//!     }
//! }
//!
//! // Create Clist (from a sentinel head).
//! // SAFETY: head is allocated by test code and Clist has the same layout.
//! let list = unsafe { Clist::from_raw(&mut head) };
//!
//! // Now iterate using clist_iterate! macro.
//! let mut found_0 = false;
//! let mut found_10 = false;
//! let mut found_20 = false;
//!
//! for item in clist_iterate!(list, Item, link) {
//!     let val = item.value();
//!     if val == 0 { found_0 = true; }
//!     if val == 10 { found_10 = true; }
//!     if val == 20 { found_20 = true; }
//! }
//!
//! assert!(found_0 && found_10 && found_20);
//! ```

use crate::{
    bindings,
    types::Opaque, //
};
use core::marker::PhantomData;

/// Trait for associating a link type with its container item type.
///
/// Implemented by "field link types" that are `list_head` links embedded in intrusive
/// C linked lists. Each link type is unique to a specific item type and its `list_head` field,
/// making it possible for an item to be added to multiple lists.
pub trait ClistLink {
    /// The item type that contains the `list_head` field linking to other items in the list.
    type Item: FromListHead<Self>
    where
        Self: Sized;
}

/// A C linked list with a sentinel head
///
/// Represents the entire linked list and can be used for add, remove, empty operations etc.
/// A sentinel head is one which is not embedded in an item.
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

    /// Create a high-level iterator over typed items.
    #[inline]
    pub fn iter<L: ClistLink>(&self) -> ClistIter<'_, L> {
        ClistIter {
            head_iter: self.iter_heads(),
            _phantom: PhantomData,
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

/// High-level iterator over typed list items.
pub struct ClistIter<'a, L: ClistLink> {
    head_iter: ClistHeadIter<'a>,

    /// The iterator yields immutable references to `L::Item`.
    _phantom: PhantomData<&'a L::Item>,
}

// SAFETY: ClistIter yields `&L::Item`, which is Send when `L::Item: Send`.
unsafe impl<L: ClistLink> Send for ClistIter<'_, L> where L::Item: Send {}

// SAFETY: ClistIter yields &L::Item, which is Sync when `L::Item: Sync`.
unsafe impl<L: ClistLink> Sync for ClistIter<'_, L> where L::Item: Sync {}

impl<'a, L: ClistLink> Iterator for ClistIter<'a, L>
where
    L::Item: 'a,
{
    type Item = &'a L::Item;

    fn next(&mut self) -> Option<Self::Item> {
        // Get next ClistHead.
        let head = self.head_iter.next()?;

        // Convert to item using trait.
        // SAFETY: FromListHead impl guarantees valid conversion.
        Some(unsafe { L::Item::from_list_head(head) })
    }
}

/// Trait for converting a `ClistHead` to an item reference.
pub trait FromListHead<Link>: Sized {
    /// Convert a `ClistHead` node reference to an item reference.
    ///
    /// # Safety
    ///
    /// `head` must be a valid reference to an allocated and initialized `ClistHead` structure
    /// valid for the lifetime `'a`.
    unsafe fn from_list_head<'a>(head: &'a ClistHead) -> &'a Self;
}

/// Macro to generate `FromListHead` implementations for C list integration.
///
/// `FromListHead` trait is required to iterate over a C linked list using the `clist_iterate!`
/// macro which yields immutable references to the Rust item wrapper type.
///
/// Also generates a link type named `Clist<ItemType><field>` implementing the `ClistLink` trait
/// that associates the list node with the item. The link type is used to iterate over the list
/// using the `clist_iterate!` macro.
///
/// # Arguments
///
/// - `$vis`: The visibility of the generated link type (e.g., `pub`, `pub(crate)`).
/// - `$item_type`: The Rust wrapper type for items in the list.
/// - `$c_type`: The C struct type that contains the embedded `list_head`.
/// - `$field`: The name of the `list_head` field within the C struct that links items.
///
/// # Examples
///
/// Refer to the comprehensive example in the [crate::clist] module documentation.
#[macro_export]
macro_rules! impl_from_list_head {
    ($vis:vis, $item_type:ident, $c_type:ty, $field:ident) => {
        $crate::macros::paste! {
            /// Link type for associating list nodes with items.
            $vis struct [<Clist $item_type $field>];

            // Implement ClistLink trait to associate the link with its item type.
            impl $crate::clist::ClistLink for [<Clist $item_type $field>] {
                type Item = $item_type;
            }

            impl $crate::clist::FromListHead<[<Clist $item_type $field>]> for $item_type {
                unsafe fn from_list_head<'a>(
                    head: &'a $crate::clist::ClistHead,
                ) -> &'a Self {
                    let ptr = $crate::container_of!(head.as_raw(), $c_type, $field);
                    // SAFETY: repr(transparent) makes item_type have same layout as c_type.
                    // Caller guarantees the container_of calculation is correct.
                    unsafe { &*ptr.cast::<Self>() }
                }
            }
        }
    };
}

/// Macro to assist with iterating over a C linked list.
///
/// Returns a `ClistIter` iterator which yields immutable references to the `item_type` type.
///
/// # Arguments
///
/// - `$list`: The `Clist` instance to iterate over.
/// - `$item_type`: The Rust type of the item in the list with list_head embedded.
/// - `$field`: The name of the field in the `item_type` that links it to the list.
///
/// # Examples
///
/// Refer to the comprehensive example in the [crate::clist] module documentation.
#[macro_export]
macro_rules! clist_iterate {
    ($list:expr, $item_type:ident, $field:ident) => {
        $crate::macros::paste! {
            $list.iter::<[<Clist $item_type $field>]>()
        }
    };
}
