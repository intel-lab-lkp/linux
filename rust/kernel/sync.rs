// SPDX-License-Identifier: GPL-2.0

//! Synchronisation primitives.
//!
//! This module contains the kernel APIs related to synchronisation that have been ported or
//! wrapped for usage by Rust code in the kernel.

use core::panic::Location;

use crate::{
    pr_cont,
    prelude::*,
    types::Opaque, //
};
use pin_init;

mod arc;
pub mod aref;
pub mod atomic;
pub mod barrier;
pub mod completion;
mod condvar;
pub mod lock;
mod locked_by;
pub mod poll;
pub mod rcu;
mod refcount;
mod set_once;

pub use arc::{Arc, ArcBorrow, UniqueArc};
pub use completion::Completion;
pub use condvar::{new_condvar, CondVar, CondVarTimeoutResult};
pub use lock::global::{global_lock, GlobalGuard, GlobalLock, GlobalLockBackend, GlobalLockedBy};
pub use lock::mutex::{new_mutex, Mutex, MutexGuard};
pub use lock::spinlock::{new_spinlock, SpinLock, SpinLockGuard};
pub use locked_by::LockedBy;
pub use refcount::Refcount;
pub use set_once::SetOnce;

/// Represents a lockdep class.
///
/// Wraps the kernel's `struct lock_class_key`.
#[repr(transparent)]
#[pin_data(PinnedDrop)]
pub struct LockClassKey {
    #[pin]
    inner: Opaque<bindings::lock_class_key>,
}

// SAFETY: Unregistering a lock class key from a different thread than where it was registered is
// allowed.
unsafe impl Send for LockClassKey {}

// SAFETY: `bindings::lock_class_key` is designed to be used concurrently from multiple threads and
// provides its own synchronization.
unsafe impl Sync for LockClassKey {}

impl LockClassKey {
    /// Initializes a statically allocated lock class key.
    ///
    /// This is usually used indirectly through the [`static_lock_class!`] macro. See its
    /// documentation for more information.
    ///
    /// # Safety
    ///
    /// * Before using the returned value, it must be pinned in a static memory location.
    /// * The destructor must never run on the returned `LockClassKey`.
    pub const unsafe fn new_static() -> Self {
        LockClassKey {
            inner: Opaque::uninit(),
        }
    }

    /// Obtain a statically allocated lock class key identified by the caller's location.
    ///
    /// Different caller locations will be guaranteed to have different lock class keys; however for
    /// the same caller location, this may return different keys, e.g. if the caller is instantiated
    /// in different object files.
    #[inline]
    #[track_caller]
    pub const fn from_caller() -> Pin<&'static Self> {
        static_assert!(size_of::<Location<'_>>() >= size_of::<LockClassKey>());

        #[cfg(CONFIG_LOCKDEP)]
        {
            let caller = Location::caller();
            // SAFETY: For static objects, lockdep does not touch the underlying memory, and only
            // the address matters. `Location::caller()` is in rodata so it meets the requirement.
            // The size check above makes sure that it does not overlap with other lock class keys.
            unsafe { Pin::new_unchecked(&*core::ptr::from_ref(caller).cast::<Self>()) }
        }

        // A separate version if lockdep is disabled to avoid unnecessary `Location` being
        // generated.
        #[cfg(not(CONFIG_LOCKDEP))]
        static_lock_class!()
    }

    /// Initializes a dynamically allocated lock class key.
    ///
    /// In the common case of using a statically allocated lock class key, the
    /// [`static_lock_class!`] macro should be used instead.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::KBox;
    /// use kernel::types::ForeignOwnable;
    /// use kernel::sync::{LockClassKey, SpinLock};
    /// use pin_init::stack_pin_init;
    ///
    /// let key = KBox::pin_init(LockClassKey::new_dynamic(), GFP_KERNEL)?;
    /// let key_ptr = key.into_foreign();
    ///
    /// {
    ///     stack_pin_init!(let num: SpinLock<u32> = SpinLock::new_with_lock_class(
    ///         0,
    ///         c"my_spinlock",
    ///         // SAFETY: `key_ptr` is returned by the above `into_foreign()`, whose
    ///         // `from_foreign()` has not yet been called.
    ///         unsafe { <Pin<KBox<LockClassKey>> as ForeignOwnable>::borrow(key_ptr) }
    ///     ));
    /// }
    ///
    /// // SAFETY: We dropped `num`, the only use of the key, so the result of the previous
    /// // `borrow` has also been dropped. Thus, it's safe to use from_foreign.
    /// unsafe { drop(<Pin<KBox<LockClassKey>> as ForeignOwnable>::from_foreign(key_ptr)) };
    /// # Ok::<(), Error>(())
    /// ```
    pub fn new_dynamic() -> impl PinInit<Self> {
        pin_init!(Self {
            // SAFETY: lockdep_register_key expects an uninitialized block of memory
            inner <- Opaque::ffi_init(|slot| unsafe { bindings::lockdep_register_key(slot) })
        })
    }

    /// Returns a raw pointer to the inner C struct.
    ///
    /// It is up to the caller to use the raw pointer correctly.
    pub fn as_ptr(&self) -> *mut bindings::lock_class_key {
        self.inner.get()
    }
}

#[pinned_drop]
impl PinnedDrop for LockClassKey {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: `self.as_ptr()` was registered with lockdep and `self` is pinned, so the address
        // hasn't changed. Thus, it's safe to pass it to unregister.
        unsafe { bindings::lockdep_unregister_key(self.as_ptr()) }
    }
}

// We want this to be completely unique so lockdep can identify when lock classes are generated with
// `new_static` mechanism, hence making this `static` and attach a `link_section` so it cannot be
// merged with other constants.
#[export_name = "lockdep_rust_name"]
#[link_section = ".rodata"]
static LOCKDEP_RUST_NAME_BYTES: [u8; 7] = *b"(rust)\0";

// This should only be used in conjunction of `LockClassKey::from_caller`.
const LOCKDEP_RUST_NAME: &CStr = match CStr::from_bytes_with_nul(&LOCKDEP_RUST_NAME_BYTES) {
    Ok(v) => v,
    Err(_) => unreachable!(),
};

#[cfg(CONFIG_LOCKDEP)]
#[expect(clippy::missing_safety_doc)]
#[no_mangle]
unsafe extern "C" fn lockdep_print_rust_name(key: *mut bindings::lock_class_key) {
    // SAFETY: `rust_print_lockdep_name` is called when the lock name is a reserved value indicating
    // that the lock_class_key is static and backed by a `Location`.
    let location = unsafe { &*key.cast::<Location<'_>>() };
    pr_cont!("{}:{}", location.file(), location.line());
}

/// Defines a new static lock class and returns a pointer to it.
///
/// # Examples
///
/// ```
/// use kernel::sync::{static_lock_class, Arc, SpinLock};
///
/// fn new_locked_int() -> Result<Arc<SpinLock<u32>>> {
///     Arc::pin_init(SpinLock::new_with_lock_class(
///         42,
///         c"new_locked_int",
///         static_lock_class!(),
///     ), GFP_KERNEL)
/// }
/// ```
#[macro_export]
macro_rules! static_lock_class {
    () => {{
        static CLASS: $crate::sync::LockClassKey =
            // SAFETY: The returned `LockClassKey` is stored in static memory and we pin it. Drop
            // never runs on a static global.
            unsafe { $crate::sync::LockClassKey::new_static() };
        $crate::prelude::Pin::static_ref(&CLASS)
    }};
}
pub use static_lock_class;

/// Returns the given string, if one is provided, otherwise generates one based on the source code
/// location.
#[doc(hidden)]
#[macro_export]
macro_rules! optional_name {
    () => {
        $crate::c_str!(::core::concat!(::core::file!(), ":", ::core::line!()))
    };
    ($name:literal) => {
        $crate::c_str!($name)
    };
}
