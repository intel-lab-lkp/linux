// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright 2019 Collabora ltd.

//! IRQ allocation and handling

use crate::error::to_result;
use crate::prelude::*;
use crate::str::CStr;
use crate::types::Opaque;

/// Flags to be used when registering IRQ handlers.
///
/// They can be combined with the operators `|`, `&`, and `!`.
///
/// Values can be used from the [`flags`] module.
#[derive(Clone, Copy)]
pub struct Flags(u64);

impl core::ops::BitOr for Flags {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for Flags {
    type Output = Self;
    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::Not for Flags {
    type Output = Self;
    fn not(self) -> Self::Output {
        Self(!self.0)
    }
}

/// The flags that can be used when registering an IRQ handler.
pub mod flags {
    use super::Flags;

    use crate::bindings;

    /// Use the interrupt line as already configured.
    pub const TRIGGER_NONE: Flags = Flags(bindings::IRQF_TRIGGER_NONE as _);

    /// The interrupt is triggered when the signal goes from low to high.
    pub const TRIGGER_RISING: Flags = Flags(bindings::IRQF_TRIGGER_RISING as _);

    /// The interrupt is triggered when the signal goes from high to low.
    pub const TRIGGER_FALLING: Flags = Flags(bindings::IRQF_TRIGGER_FALLING as _);

    /// The interrupt is triggered while the signal is held high.
    pub const TRIGGER_HIGH: Flags = Flags(bindings::IRQF_TRIGGER_HIGH as _);

    /// The interrupt is triggered while the signal is held low.
    pub const TRIGGER_LOW: Flags = Flags(bindings::IRQF_TRIGGER_LOW as _);

    /// Allow sharing the irq among several devices.
    pub const SHARED: Flags = Flags(bindings::IRQF_SHARED as _);

    /// Set by callers when they expect sharing mismatches to occur.
    pub const PROBE_SHARED: Flags = Flags(bindings::IRQF_PROBE_SHARED as _);

    /// Flag to mark this interrupt as timer interrupt.
    pub const TIMER: Flags = Flags(bindings::IRQF_TIMER as _);

    /// Interrupt is per cpu.
    pub const PERCPU: Flags = Flags(bindings::IRQF_PERCPU as _);

    /// Flag to exclude this interrupt from irq balancing.
    pub const NOBALANCING: Flags = Flags(bindings::IRQF_NOBALANCING as _);

    /// Interrupt is used for polling (only the interrupt that is registered
    /// first in a shared interrupt is considered for performance reasons).
    pub const IRQPOLL: Flags = Flags(bindings::IRQF_IRQPOLL as _);

    /// Interrupt is not reenabled after the hardirq handler finished. Used by
    /// threaded interrupts which need to keep the irq line disabled until the
    /// threaded handler has been run.
    pub const ONESHOT: Flags = Flags(bindings::IRQF_ONESHOT as _);

    /// Do not disable this IRQ during suspend. Does not guarantee that this
    /// interrupt will wake the system from a suspended state.
    pub const NO_SUSPEND: Flags = Flags(bindings::IRQF_NO_SUSPEND as _);

    /// Force enable it on resume even if [`NO_SUSPEND`] is set.
    pub const FORCE_RESUME: Flags = Flags(bindings::IRQF_FORCE_RESUME as _);

    /// Interrupt cannot be threaded.
    pub const NO_THREAD: Flags = Flags(bindings::IRQF_NO_THREAD as _);

    /// Resume IRQ early during syscore instead of at device resume time.
    pub const EARLY_RESUME: Flags = Flags(bindings::IRQF_EARLY_RESUME as _);

    /// If the IRQ is shared with a NO_SUSPEND user, execute this interrupt
    /// handler after suspending interrupts. For system wakeup devices users
    /// need to implement wakeup detection in their interrupt handlers.
    pub const COND_SUSPEND: Flags = Flags(bindings::IRQF_COND_SUSPEND as _);

    /// Don't enable IRQ or NMI automatically when users request it. Users will
    /// enable it explicitly by `enable_irq` or `enable_nmi` later.
    pub const NO_AUTOEN: Flags = Flags(bindings::IRQF_NO_AUTOEN as _);

    /// Exclude from runnaway detection for IPI and similar handlers, depends on
    /// `PERCPU`.
    pub const NO_DEBUG: Flags = Flags(bindings::IRQF_NO_DEBUG as _);
}

/// The value that can be returned from an IrqHandler;
pub enum IrqReturn {
    /// The interrupt was not from this device or was not handled.
    None = bindings::irqreturn_IRQ_NONE as _,

    /// The interrupt was handled by this device.
    Handled = bindings::irqreturn_IRQ_HANDLED as _,
}

/// Callbacks for an IRQ handler.
pub trait Handler: Sync {
    /// The actual handler function. As usual, sleeps are not allowed in IRQ
    /// context.
    fn handle_irq(&self) -> IrqReturn;
}

/// A registration of an IRQ handler for a given IRQ line.
///
/// # Invariants
///
/// * We own an irq handler using `&self` as its private data.
///
/// # Examples
///
/// The following is an example of using `Registration`:
///
/// ```
/// use kernel::prelude::*;
/// use kernel::irq;
/// use kernel::irq::Registration;
/// use kernel::sync::Arc;
/// use kernel::sync::lock::SpinLock;
///
/// // Declare a struct that will be passed in when the interrupt fires. The u32
/// // merely serves as an example of some internal data.
/// struct Data(u32);
///
/// // [`handle_irq`] returns &self. This example illustrates interior
/// // mutability can be used when share the data between process context and IRQ
/// // context.
/// //
/// // Ideally, this example would be using a version of SpinLock that is aware
/// // of `spin_lock_irqsave` and `spin_lock_irqrestore`, but that is not yet
/// // implemented.
///
/// type Handler = SpinLock<Data>;
///
/// impl kernel::irq::Handler for Handler {
///     // This is executing in IRQ context in some CPU. Other CPUs can still
///     // try to access to data.
///     fn handle_irq(&self) -> irq::IrqReturn {
///         // We now have exclusive access to the data by locking the SpinLock.
///         let mut handler = self.lock();
///         handler.0 += 1;
///
///         IrqReturn::Handled
///     }
/// }
///
/// // This is running in process context.
/// fn register_irq(irq: u32, handler: Handler) -> Result<irq::Registration<Handler>> {
///     let registration = Registration::register(irq, irq::flags::SHARED, "my-device", handler)?;
///
///     // You can have as many references to the registration as you want, so
///     // multiple parts of the driver can access it.
///     let registration = Arc::pin_init(registration)?;
///
///     // The handler may be called immediately after the function above
///     // returns, possibly in a different CPU.
///
///     // The data can be accessed from the process context too.
///     registration.handler().lock().0 = 42;
///
///     Ok(registration)
/// }
///
/// # Ok::<(), Error>(())
///```
#[pin_data(PinnedDrop)]
pub struct Registration<T: Handler> {
    irq: u32,
    #[pin]
    handler: Opaque<T>,
}

impl<T: Handler> Registration<T> {
    /// Registers the IRQ handler with the system for the given IRQ number. The
    /// handler must be able to be called as soon as this function returns.
    pub fn register(
        irq: u32,
        flags: Flags,
        name: &'static CStr,
        handler: T,
    ) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            irq,
            handler: Opaque::new(handler)
        })
        .pin_chain(move |slot| {
            // SAFETY:
            // - `handler` points to a valid function defined below.
            // - only valid flags can be constructed using the `flags` module.
            // - `devname` is a nul-terminated string with a 'static lifetime.
            // - `ptr` is a cookie used to identify the handler. The same cookie is
            // passed back when the system calls the handler.
            to_result(unsafe {
                bindings::request_irq(
                    irq,
                    Some(handle_irq_callback::<T>),
                    flags.0,
                    name.as_char_ptr(),
                    &*slot as *const _ as *mut core::ffi::c_void,
                )
            })?;

            Ok(())
        })
    }

    /// Returns a reference to the handler that was registered with the system.
    pub fn handler(&self) -> &T {
        // SAFETY: `handler` is initialized in `register`.
        unsafe { &*self.handler.get() }
    }
}

#[pinned_drop]
impl<T: Handler> PinnedDrop for Registration<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY:
        // - `self.irq` is the same as the one passed to `reques_irq`.
        // -  `&self` was passed to `request_irq` as the cookie. It is
        // guaranteed to be unique by the type system, since each call to
        // `register` will return a different instance of `Registration`.
        //
        // Notice that this will block until all handlers finish executing, so,
        // at no point will &self be invalid while the handler is running.
        unsafe { bindings::free_irq(self.irq, &*self as *const _ as *mut core::ffi::c_void) };
    }
}

/// The value that can be returned from `ThreadedHandler::handle_irq`.
pub enum ThreadedIrqReturn {
    /// The interrupt was not from this device or was not handled.
    None = bindings::irqreturn_IRQ_NONE as _,

    /// The interrupt was handled by this device.
    Handled = bindings::irqreturn_IRQ_HANDLED as _,

    /// The handler wants the handler thread to wake up.
    WakeThread = bindings::irqreturn_IRQ_WAKE_THREAD as _,
}

/// The value that can be returned from `ThreadedFnHandler::thread_fn`.
pub enum ThreadedFnReturn {
    /// The thread function did not make any progress.
    None = bindings::irqreturn_IRQ_NONE as _,

    /// The thread function ran successfully.
    Handled = bindings::irqreturn_IRQ_HANDLED as _,
}

/// Callbacks for a threaded IRQ handler.
pub trait ThreadedHandler: Sync {
    /// The actual handler function. As usual, sleeps are not allowed in IRQ
    /// context.
    fn handle_irq(&self) -> ThreadedIrqReturn;

    /// The threaded handler function. This function is called from the irq
    /// handler thread, which is automatically created by the system.
    fn thread_fn(&self) -> ThreadedFnReturn;
}

/// A registration of a threaded IRQ handler for a given IRQ line.
///
/// Two callbacks are required: one to handle the IRQ, and one to handle any
/// other work in a separate thread.
///
/// The thread handler is only called if the IRQ handler returns `WakeThread`.
///
/// # Invariants
///
/// * We own an irq handler using `&self` as its private data.
///
/// # Examples
///
/// The following is an example of using `ThreadedRegistration`:
///
/// ```
/// use kernel::prelude::*;
/// use kernel::irq;
/// use kernel::irq::Registration;
/// use kernel::sync::Arc;
/// use kernel::sync::lock::SpinLock;
///
/// // Declare a struct that will be passed in when the interrupt fires. The u32
/// // merely serves as an example of some internal data.
/// struct Data(u32);
///
/// // [`handle_irq`] returns &self. This example illustrates interior
/// // mutability can be used when share the data between process context and IRQ
/// // context.
/// //
/// // Ideally, this example would be using a version of SpinLock that is aware
/// // of `spin_lock_irqsave` and `spin_lock_irqrestore`, but that is not yet
/// // implemented.
///
/// type Handler = SpinLock<Data>;
///
/// impl kernel::irq::Handler for Handler {
///     // This is executing in IRQ context in some CPU. Other CPUs can still
///     // try to access to data.
///     fn handle_irq(&self) -> irq::ThreadedIrqReturn {
///         // We now have exclusive access to the data by locking the SpinLock.
///         let mut handler = self.lock();
///         handler.0 += 1;
///
///         // By returning `WakeThread`, we indicate to the system that the
///         // thread function should be called. Otherwise, return
///         // ThreadedIrqReturn::Handled.
///         ThreadedIrqReturn::WakeThread
///     }
///
///     // This will run (in a separate kthread) iff `handle_irq` returns
///     // `WakeThread`.
///     fn thread_fn(&self) -> irq::ThreadedFnReturn {
///         // We now have exclusive access to the data by locking the SpinLock.
///         let mut handler = self.lock();
///         handler.0 += 1;
///     }
///
///     // This is running in process context.
///     fn register_irq(irq: u32, handler: Handler) -> Result<irq::Registration<Handler>> {
///         let registration = ThreadedRegistration::register(irq, irq::flags::SHARED, "my-device", handler)?;
///
///         // You can have as many references to the registration as you want, so
///         // multiple parts of the driver can access it.
///         let registration = Arc::pin_init(registration)?;
///
///         // The handler may be called immediately after the function above
///         // returns, possibly in a different CPU.
///
///         // The data can be accessed from the process context too.
///         registration.handler().lock().0 = 42;
///
///         Ok(registration)
///     }
/// }
///
/// # Ok::<(), Error>(())
///```
#[pin_data(PinnedDrop)]
pub struct ThreadedRegistration<T: ThreadedHandler> {
    irq: u32,
    #[pin]
    handler: Opaque<T>,
}

impl<T: ThreadedHandler> ThreadedRegistration<T> {
    /// Registers the IRQ handler with the system for the given IRQ number. The
    /// handler must be able to be called as soon as this function returns.
    pub fn register(
        irq: u32,
        flags: Flags,
        name: &'static CStr,
        handler: T,
    ) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            irq,
            handler: Opaque::new(handler)
        })
        .pin_chain(move |slot| {
            // SAFETY:
            // - `handler` points to a valid function defined below.
            // - only valid flags can be constructed using the `flags` module.
            // - `devname` is a nul-terminated string with a 'static lifetime.
            // - `ptr` is a cookie used to identify the handler. The same cookie is
            // passed back when the system calls the handler.
            to_result(unsafe {
                bindings::request_threaded_irq(
                    irq,
                    Some(handle_threaded_irq_callback::<T>),
                    Some(thread_fn_callback::<T>),
                    flags.0,
                    name.as_char_ptr(),
                    &*slot as *const _ as *mut core::ffi::c_void,
                )
            })?;

            Ok(())
        })
    }

    /// Returns a reference to the handler that was registered with the system.
    pub fn handler(&self) -> &T {
        // SAFETY: `handler` is initialized in `register`.
        unsafe { &*self.handler.get() }
    }
}

#[pinned_drop]
impl<T: ThreadedHandler> PinnedDrop for ThreadedRegistration<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY:
        // - `self.irq` is the same as the one passed to `request_threaded_irq`.
        // -  `&self` was passed to `request_threaded_irq` as the cookie. It is
        // guaranteed to be unique by the type system, since each call to
        // `register` will return a different instance of
        // `ThreadedRegistration`.
        //
        // Notice that this will block until all handlers finish executing, so,
        // at no point will &self be invalid while the handler is running.
        unsafe { bindings::free_irq(self.irq, &*self as *const _ as *mut core::ffi::c_void) };
    }
}

unsafe extern "C" fn handle_irq_callback<T: Handler>(
    _irq: i32,
    ptr: *mut core::ffi::c_void,
) -> core::ffi::c_uint {
    let data = unsafe { &*(ptr as *const T) };
    T::handle_irq(data) as _
}

unsafe extern "C" fn handle_threaded_irq_callback<T: ThreadedHandler>(
    _irq: i32,
    ptr: *mut core::ffi::c_void,
) -> core::ffi::c_uint {
    let data = unsafe { &*(ptr as *const T) };
    T::handle_irq(data) as _
}

unsafe extern "C" fn thread_fn_callback<T: ThreadedHandler>(
    _irq: i32,
    ptr: *mut core::ffi::c_void,
) -> core::ffi::c_uint {
    let data = unsafe { &*(ptr as *const T) };
    T::thread_fn(data) as _
}
