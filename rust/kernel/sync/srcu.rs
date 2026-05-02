// SPDX-License-Identifier: GPL-2.0

//! Sleepable read-copy update (SRCU) support.
//!
//! C header: [`include/linux/srcu.h`](srctree/include/linux/srcu.h)

use crate::{
    bindings,
    error::to_result,
    prelude::*,
    sync::LockClassKey,
    types::{
        NotThreadSafe,
        Opaque, //
    },
};

use pin_init::pin_data;

/// Creates an [`Srcu`] initialiser with the given name and a newly-created lock class.
#[macro_export]
macro_rules! new_srcu {
    ($($name:literal)?) => {
        $crate::sync::Srcu::new($crate::optional_name!($($name)?), $crate::static_lock_class!())
    };
}
pub use new_srcu;

/// Sleepable read-copy update primitive.
///
/// SRCU readers may sleep while holding the read-side guard.
///
/// The destructor may sleep.
///
/// # Invariants
///
/// This represents a valid `struct srcu_struct` initialized by the C SRCU API
/// and it remains pinned and valid until the pinned destructor runs.
#[repr(transparent)]
#[pin_data(PinnedDrop)]
pub struct Srcu {
    #[pin]
    inner: Opaque<bindings::srcu_struct>,
}

impl Srcu {
    /// Creates a new SRCU instance.
    #[inline]
    pub fn new(name: &'static CStr, key: Pin<&'static LockClassKey>) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            inner <- Opaque::try_ffi_init(|ptr: *mut bindings::srcu_struct| {
                // SAFETY: `ptr` points to valid uninitialised memory for a `srcu_struct`.
                to_result(unsafe {
                    bindings::init_srcu_struct_with_key(ptr, name.as_char_ptr(), key.as_ptr())
                })
            }),
        })
    }

    /// Enters an SRCU read-side critical section.
    ///
    /// # Safety
    ///
    /// The returned [`Guard`] must not be leaked. Leaking it with [`core::mem::forget`]
    /// leaves the SRCU read-side critical section active.
    #[inline]
    pub unsafe fn read_lock(&self) -> Guard<'_> {
        // SAFETY: By the type invariants, `self` contains a valid `struct srcu_struct`.
        let idx = unsafe { bindings::srcu_read_lock(self.inner.get()) };

        // INVARIANT: `idx` was returned by `srcu_read_lock()` for this `Srcu`.
        Guard {
            srcu: self,
            idx,
            _not_send: NotThreadSafe,
        }
    }

    /// Runs `f` in an SRCU read-side critical section.
    #[inline]
    pub fn with_read_lock<T>(&self, f: impl FnOnce(&Guard<'_>) -> T) -> T {
        // SAFETY: The guard is owned within this function and is not leaked.
        let guard = unsafe { self.read_lock() };

        f(&guard)
    }

    /// Waits until all pre-existing SRCU readers have completed.
    #[inline]
    pub fn synchronize(&self) {
        // SAFETY: By the type invariants, `self` contains a valid `struct srcu_struct`.
        unsafe { bindings::synchronize_srcu(self.inner.get()) };
    }

    /// Waits until all pre-existing SRCU readers have completed, expedited.
    ///
    /// This requests a lower-latency grace period than [`Srcu::synchronize`] typically
    /// at the cost of higher system-wide overhead. Prefer [`Srcu::synchronize`] by default
    /// and use this variant only when reducing reset or teardown latency is more important
    /// than the extra cost.
    #[inline]
    pub fn synchronize_expedited(&self) {
        // SAFETY: By the type invariants, `self` contains a valid `struct srcu_struct`.
        unsafe { bindings::synchronize_srcu_expedited(self.inner.get()) };
    }
}

#[pinned_drop]
impl PinnedDrop for Srcu {
    fn drop(self: Pin<&mut Self>) {
        let ptr = self.inner.get();

        // SAFETY: By the type invariants, `self` contains a valid and pinned `struct srcu_struct`.
        unsafe { bindings::srcu_barrier(ptr) };
        // SAFETY: Same as above.
        unsafe { bindings::cleanup_srcu_struct(ptr) };
    }
}

// SAFETY: `srcu_struct` may be shared and used across threads.
unsafe impl Send for Srcu {}
// SAFETY: `srcu_struct` may be shared and used concurrently.
unsafe impl Sync for Srcu {}

/// Guard for an active SRCU read-side critical section on a particular [`Srcu`].
///
/// Leaking this guard with [`core::mem::forget`] leaves the SRCU read-side
/// critical section active.
///
/// # Invariants
///
/// `idx` is the index returned by `srcu_read_lock()` for `srcu`.
pub struct Guard<'a> {
    srcu: &'a Srcu,
    idx: i32,
    _not_send: NotThreadSafe,
}

impl Guard<'_> {
    /// Explicitly releases the SRCU read-side critical section.
    #[inline]
    pub fn unlock(self) {}
}

impl Drop for Guard<'_> {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: `Guard` is only constructible through `Srcu::read_lock()`,
        // which returns a valid index for the SRCU instance.
        unsafe { bindings::srcu_read_unlock(self.srcu.inner.get(), self.idx) };
    }
}
