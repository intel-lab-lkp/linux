// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright 2025 Collabora ltd.

//! This module provides types like [`Registration`] and
//! [`ThreadedRegistration`], which allow users to register handlers for a given
//! IRQ line.

use core::marker::PhantomPinned;

use pin_init::pin_init_from_closure;

use crate::alloc::Allocator;
use crate::device::Bound;
use crate::device::Device;
use crate::devres::Devres;
use crate::error::to_result;
use crate::irq::flags::Flags;
use crate::prelude::*;
use crate::str::CStr;
use crate::sync::Arc;

/// The value that can be returned from an IrqHandler or a ThreadedIrqHandler.
pub enum IrqReturn {
    /// The interrupt was not from this device or was not handled.
    None,

    /// The interrupt was handled by this device.
    Handled,
}

impl IrqReturn {
    fn into_inner(self) -> u32 {
        match self {
            IrqReturn::None => bindings::irqreturn_IRQ_NONE,
            IrqReturn::Handled => bindings::irqreturn_IRQ_HANDLED,
        }
    }
}

/// Callbacks for an IRQ handler.
pub trait Handler: Sync {
    /// The actual handler function. As usual, sleeps are not allowed in IRQ
    /// context.
    fn handle_irq(&self) -> IrqReturn;
}

impl<T: ?Sized + Handler + Send> Handler for Arc<T> {
    fn handle_irq(&self) -> IrqReturn {
        T::handle_irq(self)
    }
}

impl<T: ?Sized + Handler, A: Allocator> Handler for Box<T, A> {
    fn handle_irq(&self) -> IrqReturn {
        T::handle_irq(self)
    }
}

struct RegistrationInner {
    irq: u32,
    cookie: *mut kernel::ffi::c_void,
}

impl RegistrationInner {
    fn synchronize(&self) {
        // SAFETY:
        // - `self.irq` is the same as the one passed to `request_{threaded}_irq`.
        unsafe { bindings::synchronize_irq(self.irq) };
    }
}

impl Drop for RegistrationInner {
    fn drop(&mut self) {
        // SAFETY:
        // - `self.irq` is the same as the one passed to `request_{threaded}_irq`.
        //
        // -  `cookie` was passed to `request_{threaded}_irq` as the cookie. It
        // is guaranteed to be unique by the type system, since each call to
        // `register` will return a different instance of `Registration`.
        //
        // - `&self` is `!Unpin` and was initializing using pin-init, so it
        // occupied the same memory location for the entirety of its lifetime.
        //
        // Notice that this will block until all handlers finish executing,
        // i.e.: at no point will &self be invalid while the handler is running.
        unsafe { bindings::free_irq(self.irq, self.cookie) };
    }
}

/// A registration of an IRQ handler for a given IRQ line.
///
/// # Examples
///
/// The following is an example of using `Registration`. It uses a
/// [`AtomicU32`](core::sync::AtomicU32) to provide the interior mutability.
///
/// ```
/// use core::sync::atomic::AtomicU32;
/// use core::sync::atomic::Ordering;
///
/// use kernel::prelude::*;
/// use kernel::device::Bound;
/// use kernel::irq::flags;
/// use kernel::irq::Registration;
/// use kernel::irq::IrqReturn;
/// use kernel::platform;
/// use kernel::sync::Arc;
/// use kernel::c_str;
/// use kernel::alloc::flags::GFP_KERNEL;
///
/// // Declare a struct that will be passed in when the interrupt fires. The u32
/// // merely serves as an example of some internal data.
/// struct Data(AtomicU32);
///
/// // [`handle_irq`] takes &self. This example illustrates interior
/// // mutability can be used when share the data between process context and IRQ
/// // context.
///
/// type Handler = Data;
///
/// impl kernel::irq::request::Handler for Handler {
///     // This is executing in IRQ context in some CPU. Other CPUs can still
///     // try to access to data.
///     fn handle_irq(&self) -> IrqReturn {
///         self.0.fetch_add(1, Ordering::Relaxed);
///
///         IrqReturn::Handled
///     }
/// }
///
/// // This is running in process context.
/// fn register_irq(handler: Handler, dev: &platform::Device<Bound>) -> Result<Arc<Registration<Handler>>> {
///     let registration = dev.irq_by_index(0, flags::SHARED, c_str!("my-device"), handler)?;
///
///     // You can have as many references to the registration as you want, so
///     // multiple parts of the driver can access it.
///     let registration = Arc::pin_init(registration, GFP_KERNEL)?;
///
///     // The handler may be called immediately after the function above
///     // returns, possibly in a different CPU.
///
///     {
///         // The data can be accessed from the process context too.
///         registration.handler().0.fetch_add(1, Ordering::Relaxed);
///     }
///
///     Ok(registration)
/// }
///
/// # Ok::<(), Error>(())
///```
///
/// # Invariants
///
/// * We own an irq handler using `&self` as its private data.
///
#[pin_data]
pub struct Registration<T: Handler + 'static> {
    inner: Devres<RegistrationInner>,

    #[pin]
    handler: T,

    /// Pinned because we need address stability so that we can pass a pointer
    /// to the callback.
    #[pin]
    _pin: PhantomPinned,
}

impl<T: Handler + 'static> Registration<T> {
    /// Registers the IRQ handler with the system for the given IRQ number.
    pub(crate) fn register<'a>(
        dev: &'a Device<Bound>,
        irq: u32,
        flags: Flags,
        name: &'static CStr,
        handler: T,
    ) -> impl PinInit<Self, Error> + 'a {
        let closure = move |slot: *mut Self| {
            // SAFETY: The slot passed to pin initializer is valid for writing.
            unsafe {
                slot.write(Self {
                    inner: Devres::new(
                        dev,
                        RegistrationInner {
                            irq,
                            cookie: slot.cast(),
                        },
                        GFP_KERNEL,
                    )?,
                    handler,
                    _pin: PhantomPinned,
                })
            };

            // SAFETY:
            // - The callbacks are valid for use with request_irq.
            // - If this succeeds, the slot is guaranteed to be valid until the
            // destructor of Self runs, which will deregister the callbacks
            // before the memory location becomes invalid.
            let res = to_result(unsafe {
                bindings::request_irq(
                    irq,
                    Some(handle_irq_callback::<T>),
                    flags.into_inner() as usize,
                    name.as_char_ptr(),
                    slot.cast(),
                )
            });

            if res.is_err() {
                // SAFETY: We are returning an error, so we can destroy the slot.
                unsafe { core::ptr::drop_in_place(&raw mut (*slot).handler) };
            }

            res
        };

        // SAFETY:
        // - if this returns Ok, then every field of `slot` is fully
        // initialized.
        // - if this returns an error, then the slot does not need to remain
        // valid.
        unsafe { pin_init_from_closure(closure) }
    }

    /// Returns a reference to the handler that was registered with the system.
    pub fn handler(&self) -> &T {
        &self.handler
    }

    /// Wait for pending IRQ handlers on other CPUs.
    ///
    /// This will attempt to access the inner [`Devres`] container.
    pub fn try_synchronize(&self) -> Result {
        let inner = self.inner.try_access().ok_or(ENODEV)?;
        inner.synchronize();
        Ok(())
    }

    /// Wait for pending IRQ handlers on other CPUs.
    pub fn synchronize(&self, dev: &Device<Bound>) -> Result {
        let inner = self.inner.access(dev)?;
        inner.synchronize();
        Ok(())
    }
}

/// # Safety
///
/// This function should be only used as the callback in `request_irq`.
unsafe extern "C" fn handle_irq_callback<T: Handler>(
    _irq: i32,
    ptr: *mut core::ffi::c_void,
) -> core::ffi::c_uint {
    // SAFETY: `ptr` is a pointer to Registration<T> set in `Registration::new`
    let data = unsafe { &*(ptr as *const Registration<T>) };
    T::handle_irq(&data.handler).into_inner()
}

/// The value that can be returned from `ThreadedHandler::handle_irq`.
pub enum ThreadedIrqReturn {
    /// The interrupt was not from this device or was not handled.
    None,

    /// The interrupt was handled by this device.
    Handled,

    /// The handler wants the handler thread to wake up.
    WakeThread,
}

impl ThreadedIrqReturn {
    fn into_inner(self) -> u32 {
        match self {
            ThreadedIrqReturn::None => bindings::irqreturn_IRQ_NONE,
            ThreadedIrqReturn::Handled => bindings::irqreturn_IRQ_HANDLED,
            ThreadedIrqReturn::WakeThread => bindings::irqreturn_IRQ_WAKE_THREAD,
        }
    }
}

/// Callbacks for a threaded IRQ handler.
pub trait ThreadedHandler: Sync {
    /// The actual handler function. As usual, sleeps are not allowed in IRQ
    /// context.
    fn handle_irq(&self) -> ThreadedIrqReturn;

    /// The threaded handler function. This function is called from the irq
    /// handler thread, which is automatically created by the system.
    fn thread_fn(&self) -> IrqReturn;
}

impl<T: ?Sized + ThreadedHandler + Send> ThreadedHandler for Arc<T> {
    fn handle_irq(&self) -> ThreadedIrqReturn {
        T::handle_irq(self)
    }

    fn thread_fn(&self) -> IrqReturn {
        T::thread_fn(self)
    }
}

impl<T: ?Sized + ThreadedHandler, A: Allocator> ThreadedHandler for Box<T, A> {
    fn handle_irq(&self) -> ThreadedIrqReturn {
        T::handle_irq(self)
    }

    fn thread_fn(&self) -> IrqReturn {
        T::thread_fn(self)
    }
}

/// A registration of a threaded IRQ handler for a given IRQ line.
///
/// Two callbacks are required: one to handle the IRQ, and one to handle any
/// other work in a separate thread.
///
/// The thread handler is only called if the IRQ handler returns `WakeThread`.
///
/// # Examples
///
/// The following is an example of using `ThreadedRegistration`. It uses a
/// [`AtomicU32`](core::sync::AtomicU32) to provide the interior mutability.
///
/// ```
/// use core::sync::atomic::AtomicU32;
/// use core::sync::atomic::Ordering;
///
/// use kernel::prelude::*;
/// use kernel::device::Bound;
/// use kernel::irq::flags;
/// use kernel::irq::ThreadedIrqReturn;
/// use kernel::irq::ThreadedRegistration;
/// use kernel::irq::IrqReturn;
/// use kernel::platform;
/// use kernel::sync::Arc;
/// use kernel::sync::SpinLock;
/// use kernel::alloc::flags::GFP_KERNEL;
/// use kernel::c_str;
///
/// // Declare a struct that will be passed in when the interrupt fires. The u32
/// // merely serves as an example of some internal data.
/// struct Data(AtomicU32);
///
/// // [`handle_irq`] takes &self. This example illustrates interior
/// // mutability can be used when share the data between process context and IRQ
/// // context.
///
/// type Handler = Data;
///
/// impl kernel::irq::request::ThreadedHandler for Handler {
///     // This is executing in IRQ context in some CPU. Other CPUs can still
///     // try to access to data.
///     fn handle_irq(&self) -> ThreadedIrqReturn {
///         self.0.fetch_add(1, Ordering::Relaxed);
///
///         // By returning `WakeThread`, we indicate to the system that the
///         // thread function should be called. Otherwise, return
///         // ThreadedIrqReturn::Handled.
///         ThreadedIrqReturn::WakeThread
///     }
///
///     // This will run (in a separate kthread) if and only if `handle_irq`
///     // returns `WakeThread`.
///     fn thread_fn(&self) -> IrqReturn {
///         self.0.fetch_add(1, Ordering::Relaxed);
///
///         IrqReturn::Handled
///     }
/// }
///
/// // This is running in process context.
/// fn register_threaded_irq(handler: Handler, dev: &platform::Device<Bound>) -> Result<Arc<ThreadedRegistration<Handler>>> {
///     let registration = dev.threaded_irq_by_index(0, flags::SHARED, c_str!("my-device"), handler)?;
///
///     // You can have as many references to the registration as you want, so
///     // multiple parts of the driver can access it.
///     let registration = Arc::pin_init(registration, GFP_KERNEL)?;
///
///     // The handler may be called immediately after the function above
///     // returns, possibly in a different CPU.
///
///     {
///         // The data can be accessed from the process context too.
///         registration.handler().0.fetch_add(1, Ordering::Relaxed);
///     }
///
///     Ok(registration)
/// }
///
///
/// # Ok::<(), Error>(())
///```
///
/// # Invariants
///
/// * We own an irq handler using `&self` as its private data.
///
#[pin_data]
pub struct ThreadedRegistration<T: ThreadedHandler + 'static> {
    inner: Devres<RegistrationInner>,

    #[pin]
    handler: T,

    /// Pinned because we need address stability so that we can pass a pointer
    /// to the callback.
    #[pin]
    _pin: PhantomPinned,
}

impl<T: ThreadedHandler + 'static> ThreadedRegistration<T> {
    /// Registers the IRQ handler with the system for the given IRQ number.
    pub(crate) fn register<'a>(
        dev: &'a Device<Bound>,
        irq: u32,
        flags: Flags,
        name: &'static CStr,
        handler: T,
    ) -> impl PinInit<Self, Error> + 'a {
        let closure = move |slot: *mut Self| {
            // SAFETY: The slot passed to pin initializer is valid for writing.
            unsafe {
                slot.write(Self {
                    inner: Devres::new(
                        dev,
                        RegistrationInner {
                            irq,
                            cookie: slot.cast(),
                        },
                        GFP_KERNEL,
                    )?,
                    handler,
                    _pin: PhantomPinned,
                })
            };

            // SAFETY:
            // - The callbacks are valid for use with request_threaded_irq.
            // - If this succeeds, the slot is guaranteed to be valid until the
            // destructor of Self runs, which will deregister the callbacks
            // before the memory location becomes invalid.
            let res = to_result(unsafe {
                bindings::request_threaded_irq(
                    irq,
                    Some(handle_threaded_irq_callback::<T>),
                    Some(thread_fn_callback::<T>),
                    flags.into_inner() as usize,
                    name.as_char_ptr(),
                    slot.cast(),
                )
            });

            if res.is_err() {
                // SAFETY: We are returning an error, so we can destroy the slot.
                unsafe { core::ptr::drop_in_place(&raw mut (*slot).handler) };
            }

            res
        };

        // SAFETY:
        // - if this returns Ok(()), then every field of `slot` is fully
        // initialized.
        // - if this returns an error, then the slot does not need to remain
        // valid.
        unsafe { pin_init_from_closure(closure) }
    }

    /// Returns a reference to the handler that was registered with the system.
    pub fn handler(&self) -> &T {
        &self.handler
    }

    /// Wait for pending IRQ handlers on other CPUs.
    ///
    /// This will attempt to access the inner [`Devres`] container.
    pub fn try_synchronize(&self) -> Result {
        let inner = self.inner.try_access().ok_or(ENODEV)?;
        inner.synchronize();
        Ok(())
    }

    /// Wait for pending IRQ handlers on other CPUs.
    pub fn synchronize(&self, dev: &Device<Bound>) -> Result {
        let inner = self.inner.access(dev)?;
        inner.synchronize();
        Ok(())
    }
}

/// # Safety
///
/// This function should be only used as the callback in `request_threaded_irq`.
unsafe extern "C" fn handle_threaded_irq_callback<T: ThreadedHandler>(
    _irq: i32,
    ptr: *mut core::ffi::c_void,
) -> core::ffi::c_uint {
    // SAFETY: `ptr` is a pointer to ThreadedRegistration<T> set in `ThreadedRegistration::new`
    let data = unsafe { &*(ptr as *const ThreadedRegistration<T>) };
    T::handle_irq(&data.handler).into_inner()
}

/// # Safety
///
/// This function should be only used as the callback in `request_threaded_irq`.
unsafe extern "C" fn thread_fn_callback<T: ThreadedHandler>(
    _irq: i32,
    ptr: *mut core::ffi::c_void,
) -> core::ffi::c_uint {
    // SAFETY: `ptr` is a pointer to ThreadedRegistration<T> set in `ThreadedRegistration::new`
    let data = unsafe { &*(ptr as *const ThreadedRegistration<T>) };
    T::thread_fn(&data.handler).into_inner()
}
