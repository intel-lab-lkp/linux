// SPDX-License-Identifier: GPL-2.0
// Author: Manos Pitsidianakis <manos@pitsidianak.is>

//! Hardware Random Number Generators
//!
//! This module provides an abstraction for implementing a hardware random number generator and
//! using it with the kernel's `hw_random` system.
//!
//! # Example
//!
//! ```no_run
//!# fn no_run() {
//!# use kernel::hw_random::*;
//!# use kernel::str::CString;
//!# use kernel::prelude::*;
//! #[pin_data]
//! struct ExampleHwRng {}
//!
//! #[vtable]
//! impl HwRngImpl for ExampleHwRng {
//!     fn read(&self, data: &mut Buffer<'_>, can_wait: bool) -> Result<()> {
//!         // write zeroes - in your driver, this should write actual data from your hardware.
//!         data.write(&[0_u8; 8]);
//!         Ok(())
//!     }
//! }
//!
//! let name = CString::try_from(c"example_hwrng").unwrap();
//! let my_rng = KBox::pin_init(
//!                 HwRng::new(
//!                     name,
//!                     0,
//!                     try_pin_init!(ExampleHwRng {})
//!                 ),
//!                 GFP_KERNEL
//!              ).unwrap();
//! // Register `my_rng`: after this succeeds, the kernel may call our `HwRngImpl` method at any
//! // time.
//! my_rng.register().unwrap();
//!
//! // ...
//!
//! my_rng.unregister();
//!# }
//!```

use crate::{
    error::{
        from_result,          //
        to_result,            //
        VTABLE_DEFAULT_ERROR, //
    },
    prelude::*, //
    str::{
        CString, //
    },
    types::{
        Opaque, //
    },
};

use core::{
    ffi::{
        c_int,    //
        c_ushort, //
        c_void,   //
    },
    mem::{
        MaybeUninit, //
    },
    ptr::{
        slice_from_raw_parts,     //
        slice_from_raw_parts_mut, //
    },
    sync::atomic::{
        AtomicBool, //
        Ordering,   //
    },
};

use pin_init::pin_init_from_closure;

/// A buffer to write random bytes in using [`Buffer::write`] that tracks how many bytes were
/// written.
///
/// See also [`HwRngImpl::read`].
pub struct Buffer<'a> {
    inner: &'a mut [MaybeUninit<u8>],
    written: usize,
}

impl Buffer<'_> {
    /// Returns `true` if the buffer has been filled.
    #[inline]
    pub const fn is_empty(&self) -> bool {
        self.written == self.inner.len()
    }

    /// Returns the number of bytes that can be written.
    #[inline]
    pub const fn len(&self) -> usize {
        self.inner.len() - self.written
    }

    /// Writes bytes from `buf` into buffer and returns the amount of bytes written.
    #[inline]
    pub fn write(&mut self, buf: &[u8]) -> usize {
        let to_copy = self.len().min(buf.len());
        let ptr = buf.as_ptr();
        // SAFETY: u8 and MaybeUninit<u8> have the same layout
        let buf = unsafe { &*slice_from_raw_parts(ptr.cast::<MaybeUninit<u8>>(), to_copy) };
        self.inner[self.written..][..to_copy].copy_from_slice(buf);
        self.written += to_copy;
        to_copy
    }
}

/// An adapter type for the registration of hardware random number generators drivers.
///
/// [`struct hwrng`]: srctree/include/linux/hw_random.h
#[pin_data(PinnedDrop)]
pub struct HwRng<T: HwRngImpl + 'static> {
    #[pin]
    registration: Opaque<bindings::hwrng>,
    registered: AtomicBool,
    #[pin]
    name: CString,
    #[pin]
    inner: T,
}

impl<T: HwRngImpl + 'static> core::ops::Deref for HwRng<T> {
    type Target = T;

    #[inline]
    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

// SAFETY: HwRng contains a `*const u8` reference but it is opaque for us in Rust.
unsafe impl<T: HwRngImpl + 'static> Send for HwRng<T> {}

// SAFETY: `HwRng` has no interior mutability from Rust, and C manages it with the rng_mutex lock.
unsafe impl<T: HwRngImpl + 'static> Sync for HwRng<T> {}

#[pinned_drop]
impl<T: HwRngImpl> PinnedDrop for HwRng<T> {
    fn drop(self: Pin<&mut Self>) {
        self.unregister();
    }
}

#[vtable]
/// Trait for the implementation of hardware RNGs.
pub trait HwRngImpl: Send + Sync {
    #[inline]
    /// Initialization callback, can be optionally implemented.
    fn init(&self) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    #[inline]
    /// Cleanup callback, can be optionally implemented.
    fn cleanup(&self) {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Places random bytes in `data`.
    fn read(&self, data: &mut Buffer<'_>, can_wait: bool) -> Result<()>;
}

impl<T: HwRngImpl + 'static> HwRng<T> {
    /// Create a new [`HwRng`] without registering it.
    pub fn new(
        name: CString,
        quality: c_ushort,
        inner: impl PinInit<T, Error>,
    ) -> impl PinInit<Self, Error> {
        // We use pin_init_from_closure because we need to store the `slot` address as `priv` field
        // of `hwrng` struct.

        // SAFETY:
        // - when the closure returns `Ok(())`, then it has successfully initialized all fields,
        // - when it returns `Err(e)`, it does not need to perform any cleanup.
        unsafe {
            pin_init_from_closure(move |slot: *mut Self| {
                inner.__pinned_init(&raw mut (*slot).inner)?;

                let registration = (&raw mut (*slot).registration).cast::<bindings::hwrng>();
                registration.write(bindings::hwrng {
                    name: name.as_char_ptr(),
                    read: Some(Self::read_callback),
                    init: if <T as HwRngImpl>::HAS_INIT {
                        Some(Self::init_callback)
                    } else {
                        None
                    },
                    cleanup: if <T as HwRngImpl>::HAS_CLEANUP {
                        Some(Self::cleanup_callback)
                    } else {
                        None
                    },
                    quality,
                    priv_: slot as usize,
                    ..Default::default()
                });

                let name_ptr = &raw mut (*slot).name;
                name_ptr.write(name);

                let registered = &raw mut (*slot).registered;
                registered.write(AtomicBool::new(false));

                // All fields of `HwRng` have been initialized
                Ok(())
            })
        }
    }

    /// Register `self` with the `hwrng` subsystem.
    ///
    /// After this function successfully returns, the `hwrng` subsystem can start calling the
    /// [`HwRngImpl`] methods at any time.
    ///
    /// [`hwrng_register`]: srctree/include/linux/hw_random.h
    #[inline]
    #[doc(alias = "hwrng_register")]
    pub fn register(&self) -> Result {
        if self
            .registered
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::Acquire)
            .is_ok()
        {
            // SAFETY: `registration` is properly initialized.
            if let Err(err) = to_result(unsafe {
                bindings::hwrng_register(self.registration.get().cast::<bindings::hwrng>())
            }) {
                self.registered.store(false, Ordering::Release);
                return Err(err);
            }
        }
        Ok(())
    }

    /// Unregister `self` from `hwrng` subsystem.
    ///
    /// [`hwrng_unregister`]: srctree/include/linux/hw_random.h
    #[inline]
    #[doc(alias = "hwrng_unregister")]
    pub fn unregister(&self) {
        if self
            .registered
            .compare_exchange(true, false, Ordering::SeqCst, Ordering::Acquire)
            .is_ok()
        {
            // SAFETY: Since `registration` is properly initialized and registered, destroying is
            // safe.
            unsafe {
                bindings::hwrng_unregister(self.registration.get().cast::<bindings::hwrng>())
            };
        }
    }
}

impl<T: HwRngImpl + 'static> HwRng<T> {
    extern "C" fn init_callback(ptr: *mut bindings::hwrng) -> c_int {
        // SAFETY: we set `priv_` as the value of `*mut Self` when initializing.
        let priv_ = unsafe { (*ptr).priv_ };
        let this_ptr = priv_ as *mut Self;

        // SAFETY: we set `inner` to point to a valid `T` when initializing.
        let inner: &T = unsafe { &(*this_ptr).inner };
        from_result(|| {
            inner.init()?;
            Ok(0)
        })
    }

    extern "C" fn cleanup_callback(ptr: *mut bindings::hwrng) {
        // SAFETY: we set `priv_` as the value of `*mut Self` when initializing.
        let priv_ = unsafe { (*ptr).priv_ };
        let this_ptr = priv_ as *mut Self;

        // SAFETY: we set `inner` to point to a valid `T` when initializing.
        let inner: &T = unsafe { &(*this_ptr).inner };
        inner.cleanup();
    }

    extern "C" fn read_callback(
        ptr: *mut bindings::hwrng,
        data: *mut c_void,
        max: usize,
        wait: bool,
    ) -> c_int {
        if data.is_null() || max == 0 {
            return 0;
        }

        // SAFETY: we set `priv_` as the value of `*mut Self` when initializing.
        let priv_ = unsafe { (*ptr).priv_ };
        let this_ptr = priv_ as *mut Self;

        let buf_ptr = slice_from_raw_parts_mut(data.cast::<MaybeUninit<u8>>(), max);
        // SAFETY: By the hw_random API contract, data points to a bytes buffer `max` bytes long.
        let buf_ref = unsafe { &mut *buf_ptr };

        let mut buffer = Buffer {
            inner: buf_ref,
            written: 0,
        };

        // SAFETY: we set `inner` to point to a valid `T` when initializing.
        let inner: &T = unsafe { &(*this_ptr).inner };
        from_result(|| {
            inner.read(&mut buffer, wait)?;
            Ok(buffer.written.try_into().unwrap_or(c_int::MAX))
        })
    }
}
