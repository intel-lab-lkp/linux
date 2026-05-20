// SPDX-License-Identifier: GPL-2.0

//! RCU support.
//!
//! C header: [`include/linux/rcupdate.h`](srctree/include/linux/rcupdate.h)

use crate::{
    bindings,
    prelude::*,
    types::{
        NotThreadSafe,
        Opaque,
    },
    alloc::Flags,
};

/// Evidence that the RCU read side lock is held on the current thread/CPU.
///
/// The type is explicitly not `Send` because this property is per-thread/CPU.
///
/// # Invariants
///
/// The RCU read side lock is actually held while instances of this guard exist.
pub struct Guard(NotThreadSafe);

impl Guard {
    /// Acquires the RCU read side lock and returns a guard.
    #[inline]
    pub fn new() -> Self {
        // SAFETY: An FFI call with no additional requirements.
        unsafe { bindings::rcu_read_lock() };
        // INVARIANT: The RCU read side lock was just acquired above.
        Self(NotThreadSafe)
    }

    /// Explicitly releases the RCU read side lock.
    #[inline]
    pub fn unlock(self) {}
}

impl Default for Guard {
    #[inline]
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Guard {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: By the type invariants, the RCU read side is locked, so it is ok to unlock it.
        unsafe { bindings::rcu_read_unlock() };
    }
}

/// Acquires the RCU read side lock.
#[inline]
pub fn read_lock() -> Guard {
    Guard::new()
}


/// An RCU callback object. Carries the user's data to drop() it once a grace period ellapsed.
///
/// This object serves to implement C's `call_rcu()` method. Since it is almost
/// always used to free a resource once a grace period ellapsed, the only thing
/// this implementation does is drop the user's data. In the rare cases in which
/// the user needs more action to take place, said actions need to be implemented
/// on the user's data via the [`Drop`] trait.
///
/// # Examples
///
/// ```
/// use kernel::sync::rcu::Callback;
///
/// struct Foo {};
///
/// impl Drop for Foo {
///     fn drop(&mut self) {
///         pr_info!("rcu::Foo Dropping.\n");
///     }
/// }
///
/// let data = Foo {};
///
/// let cb = Callback::new(data, GFP_KERNEL)?;
/// cb.submit();
///
/// Ok::<(), Error>(())
/// ```
#[repr(C)]
#[pin_data]
pub struct Callback<T: Send + 'static> {
    /// The RCU head. Only used (and initialized) by the C backend.
    #[pin]
    inner: Opaque<bindings::callback_head>,
    /// The user's data. This should implement [`Drop`] if the user wants specific
    /// actions, besides mere deallocation, to happen.
    #[pin]
    data: T,
}

impl<T: Send + 'static> Callback<T> {
    /// Create a new callback.
    pub fn new(data: impl PinInit<T>, flags: Flags) -> Result<Pin<KBox<Self>>> {
        let cb = try_pin_init!(Self {
            inner: Opaque::uninit(), // Only needed for the C backend, who will initialize it.
            data <- data,
        });

        KBox::pin_init(cb, flags)
    }

    extern "C" fn callback(rcu_head: *mut bindings::callback_head) {
        let cb_ptr = rcu_head as *mut Self;

        // SAFETY: All [`Callback`] objects in this module are always created
        // as `Pin<KBox<Self>>`. `Pin` is a transparent container. The action
        // below merely serves re-creating the KBox so that it can drop properly.
        let _cb = unsafe { KBox::from_raw(cb_ptr) };

        // Self::data drops, ensuring the desired cleanup operation.
    }

    fn as_raw(&self) -> *mut bindings::callback_head {
        self.inner.get()
    }

    /// Arm a [`Callback`]. One grace period after this function was called,
    /// the callback object will be dropped.
    pub fn submit(self: Pin<KBox<Self>>) {
        // SAFETY: The memory is not moved by this code or the C backend.
        let cb = unsafe { Pin::into_inner_unchecked(self) };
        let ptr = KBox::into_raw(cb);
        // SAFETY: `ptr` was just created validly above. `Self::callback` relies
        // on the RCU module / code never being unloaded.
        unsafe { bindings::call_rcu((*ptr).as_raw(), Some(Self::callback)) };
    }
}
