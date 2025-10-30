// SPDX-License-Identifier: GPL-2.0

//! Sample for Rust code interfacing with C linked lists (list_head).
//!
//! This sample demonstrates iteration of a C-managed linked list using the [`clist`] module.
//! C code creates and populates the list, Rust code performs iteration.

use core::ptr::NonNull;
use kernel::{
    bindings,
    clist,
    container_of,
    prelude::*, //
};

module! {
    type: RustClistModule,
    name: "rust_clist",
    authors: ["Joel Fernandes"],
    description: "Rust Clist sample",
    license: "GPL",
}

// FFI declarations for C functions
extern "C" {
    fn clist_sample_create(count: i32) -> *mut bindings::list_head;
    fn clist_sample_free(head: *mut bindings::list_head);
}

/// Sample item with embedded C list_head (This would typically be a C struct).
#[repr(C)]
struct SampleItemC {
    value: i32,
    link: bindings::list_head,
}

/// Rust wrapper for SampleItemC object pointer allocated on the C side.
///
/// # Invariants
///
/// `ptr` points to a valid [`SampleItemC`] with an initialized embedded `list_head`.
struct SampleItem {
    ptr: NonNull<SampleItemC>,
}

impl clist::FromListHead for SampleItem {
    unsafe fn from_list_head(link: *const bindings::list_head) -> Self {
        // SAFETY: Caller ensures link points to a valid, initialized list_head.
        unsafe {
            let item_ptr = container_of!(link, SampleItemC, link) as *mut _;
            SampleItem {
                ptr: NonNull::new_unchecked(item_ptr),
            }
        }
    }
}

impl SampleItem {
    /// Get the value from the sample item.
    fn value(&self) -> i32 {
        // SAFETY: self.ptr is valid as per the SampleItem invariants.
        unsafe { (*self.ptr.as_ptr()).value }
    }
}

/// Clist struct - holds the C list_head and manages its lifecycle.
#[repr(C)]
struct RustClist {
    list_head: NonNull<bindings::list_head>,
}

// SAFETY: RustClist can be sent between threads since it only holds a pointer
// which can be accessed from any thread with proper synchronization.
unsafe impl Send for RustClist {}

impl RustClist {
    /// Create a container for a pointer to a C-allocated list_head.
    fn new(list_head: *mut bindings::list_head) -> Self {
        // SAFETY: Caller ensures list_head is a valid, non-null pointer.
        Self {
            list_head: unsafe { NonNull::new_unchecked(list_head) },
        }
    }

    /// Demonstrate iteration over the list.
    fn do_iteration(&self) {
        // Wrap the C list_head with a Rust [`Clist`].
        // SAFETY: list_head is a valid, initialized, populated list_head.
        let list = unsafe { clist::Clist::<SampleItem>::new(self.list_head.as_ptr()) };
        pr_info!("Created C list with items, is_empty: {}\n", list.is_empty());

        // Iterate over the list.
        pr_info!("Iterating over C list from Rust:\n");
        for item in list.iter() {
            pr_info!("  Item value: {}\n", item.value());
        }

        pr_info!("Iteration complete\n");
    }
}

impl Drop for RustClist {
    fn drop(&mut self) {
        // Free the list and all items using C FFI.
        // SAFETY: list_head was allocated by clist_sample_create.
        // C side handles freeing both the list_head and all items.
        unsafe {
            clist_sample_free(self.list_head.as_ptr());
        }
    }
}

struct RustClistModule;

impl kernel::Module for RustClistModule {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust Clist sample (init)\n");

        // C creates and populates a list_head with items.
        // SAFETY: clist_sample_create allocates, initializes, and populates a list.
        let c_list_head = unsafe { clist_sample_create(6) };
        if c_list_head.is_null() {
            pr_err!("Failed to create and populate C list\n");
            return Err(ENOMEM);
        }

        // Create the list container (separate from module).
        let rust_clist = RustClist::new(c_list_head);

        // Demonstrate list operations.
        rust_clist.do_iteration();

        // rust_clist is dropped here, which frees the C list via Drop impl.
        pr_info!("Rust Clist sample (exit)\n");

        Ok(RustClistModule {})
    }
}
