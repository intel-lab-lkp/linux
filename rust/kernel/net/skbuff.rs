// SPDX-License-Identifier: GPL-2.0

//! Socket-buffer ownership wrappers.
//!
//! C header: [`include/linux/skbuff.h`](srctree/include/linux/skbuff.h)

use crate::bindings;

use core::ptr::NonNull;

/// Owns a single `struct sk_buff` passed into Rust from `ndo_start_xmit`.
///
/// # Invariants
///
/// - `ptr` is either `None` after ownership was handed back to the networking core, or it points
///   to a valid `struct sk_buff` exclusively owned by this value.
/// - Dropping this wrapper releases the skb exactly once via `dev_kfree_skb`.
pub struct SkBuff {
    ptr: Option<NonNull<bindings::sk_buff>>,
}

impl SkBuff {
    /// Creates an owned skb wrapper from the callback argument.
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid non-null skb pointer whose ownership is transferred to Rust for the
    /// duration of the callback.
    pub(crate) unsafe fn from_raw_owned(ptr: *mut bindings::sk_buff) -> Self {
        Self {
            // SAFETY: The caller guarantees ownership of a valid non-null skb pointer.
            ptr: Some(unsafe { NonNull::new_unchecked(ptr) }),
        }
    }

    fn ptr(&self) -> *mut bindings::sk_buff {
        self.ptr
            .expect("SkBuff ownership already transferred back to the core")
            .as_ptr()
    }

    /// Returns `skb->len`.
    pub fn len(&self) -> u32 {
        // SAFETY: `Self` owns a valid skb until ownership is explicitly returned with `into_raw`.
        unsafe { (*self.ptr()).len }
    }

    /// Hands ownership back to the networking core without freeing the skb.
    pub(crate) fn into_raw(mut self) -> *mut bindings::sk_buff {
        self.ptr
            .take()
            .expect("SkBuff ownership already transferred back to the core")
            .as_ptr()
    }
}

impl Drop for SkBuff {
    fn drop(&mut self) {
        let Some(ptr) = self.ptr.take() else {
            return;
        };

        // SAFETY: `Self` owns the skb exactly once unless ownership has been transferred back to
        // the networking core via `into_raw`.
        unsafe { bindings::dev_kfree_skb(ptr.as_ptr()) };
    }
}
