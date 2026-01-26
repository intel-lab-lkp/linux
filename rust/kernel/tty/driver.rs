// SPDX-License-Identifier: GPL-2.0

//! TTY driver support.
//!
//! Provides [`TtyDriverBuilder`] and [`TtyDriver`] for registering TTY drivers.

use core::marker::PhantomData;

use super::{
    DriverPort,
    PortOperations,
    Tty,
};
use crate::{
    bindings,
    error::{
        Error,
        Result,
        VTABLE_DEFAULT_ERROR,
    },
    prelude::*,
    sync::Arc,
    types::Opaque,
};

/// TTY driver flags.
pub mod flags {
    use crate::bindings;

    /// Reset termios when the last process closes the device.
    pub const RESET_TERMIOS: usize = bindings::tty_driver_flag_TTY_DRIVER_RESET_TERMIOS as usize;
    /// Driver will guarantee not to set any special character handling flags.
    pub const REAL_RAW: usize = bindings::tty_driver_flag_TTY_DRIVER_REAL_RAW as usize;
    /// Do not create numbered /dev nodes (e.g., /dev/ttyprintk instead of /dev/ttyprintk0).
    pub const UNNUMBERED_NODE: usize =
        bindings::tty_driver_flag_TTY_DRIVER_UNNUMBERED_NODE as usize;
}

/// Termios output flags.
pub mod oflag {
    use crate::bindings;

    /// Post-process output.
    pub const OPOST: u32 = bindings::OPOST;
    /// Map CR to NL on output.
    pub const OCRNL: u32 = bindings::OCRNL;
    /// No CR output at column 0.
    pub const ONOCR: u32 = bindings::ONOCR;
    /// NL performs CR function.
    pub const ONLRET: u32 = bindings::ONLRET;
}

/// Major device number for TTY aux devices.
pub const TTYAUX_MAJOR: i32 = bindings::TTYAUX_MAJOR as i32;

/// TTY driver types.
#[repr(u32)]
#[derive(Copy, Clone, Debug)]
pub enum DriverType {
    /// System TTY.
    System = bindings::tty_driver_type_TTY_DRIVER_TYPE_SYSTEM,
    /// Console TTY.
    Console = bindings::tty_driver_type_TTY_DRIVER_TYPE_CONSOLE,
    /// Serial TTY.
    Serial = bindings::tty_driver_type_TTY_DRIVER_TYPE_SERIAL,
    /// PTY.
    Pty = bindings::tty_driver_type_TTY_DRIVER_TYPE_PTY,
}

/// Options for creating a TTY driver.
#[derive(Copy, Clone)]
pub struct Options {
    /// Driver name (shown in /proc/tty/drivers).
    pub driver_name: &'static CStr,
    /// Device name (used for /dev node).
    pub name: &'static CStr,
    /// Major device number.
    pub major: i32,
    /// Starting minor device number.
    pub minor_start: i32,
    /// Driver type.
    pub driver_type: DriverType,
    /// Driver flags (see [`flags`] module).
    pub flags: usize,
}

/// Trait implemented by TTY device drivers.
#[vtable]
pub trait Operations: Sized + Send + Sync {
    /// Driver-specific data type stored in `tty_struct->driver_data`.
    ///
    /// Use `Arc<T>` for shared data across multiple opens, or `()` if not needed.
    /// Access via [`Tty::driver_data`] (returns `Option` since it may not be set until `open`).
    type DriverData: Send + Sync;

    /// Driver-level state type stored in `tty_driver->driver_state`.
    ///
    /// Use `Arc<T>` for shared state across all ttys, or `()` if not needed.
    /// Access via [`Tty::driver_state`].
    type DriverState: Send + Sync;

    /// Port operations type. Must implement [`PortOperations`].
    type PortOps: PortOperations + 'static;

    /// Called when the TTY device is opened.
    fn open(
        tty: &Tty<Self::DriverData, Self::DriverState>,
        file: *mut bindings::file,
    ) -> Result<()>;

    /// Called when the TTY device is closed.
    fn close(tty: &Tty<Self::DriverData, Self::DriverState>, file: *mut bindings::file);

    /// Called to write data to the device.
    fn write(tty: &Tty<Self::DriverData, Self::DriverState>, buf: &[u8]) -> Result<usize>;

    /// Returns the number of bytes that can be written.
    fn write_room(_tty: &Tty<Self::DriverData, Self::DriverState>) -> u32 {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Called on hangup.
    fn hangup(_tty: &Tty<Self::DriverData, Self::DriverState>) {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

/// A vtable for the TTY operations.
struct OperationsVTable<T: Operations>(PhantomData<T>);

/// Type alias for the TTY type used in operations callbacks.
type OpsTty<T> = Tty<<T as Operations>::DriverData, <T as Operations>::DriverState>;

impl<T: Operations> OperationsVTable<T> {
    /// # Safety
    ///
    /// `tty` and `filp` must be valid pointers.
    unsafe extern "C" fn open(
        tty: *mut bindings::tty_struct,
        filp: *mut bindings::file,
    ) -> core::ffi::c_int {
        // SAFETY: tty is valid, driver_data starts as null.
        let tty_ref = unsafe { OpsTty::<T>::from_raw(tty) };

        match T::open(&tty_ref, filp) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `tty` and `filp` must be valid pointers.
    unsafe extern "C" fn close(tty: *mut bindings::tty_struct, filp: *mut bindings::file) {
        // SAFETY: tty is valid, driver_data was set by driver in open.
        let tty_ref = unsafe { OpsTty::<T>::from_raw(tty) };
        T::close(&tty_ref, filp);
    }

    /// # Safety
    ///
    /// `tty` must be valid, `buf` must be valid for `count` bytes.
    unsafe extern "C" fn write(
        tty: *mut bindings::tty_struct,
        buf: *const u8,
        count: usize,
    ) -> isize {
        if buf.is_null() || count == 0 {
            return 0;
        }

        // SAFETY: Kernel guarantees buf is valid for count bytes.
        let slice = unsafe { core::slice::from_raw_parts(buf, count) };

        // SAFETY: tty is valid, driver_data was set by driver in open.
        let tty_ref = unsafe { OpsTty::<T>::from_raw(tty) };

        match T::write(&tty_ref, slice) {
            Ok(n) => n as isize,
            Err(e) => e.to_errno() as isize,
        }
    }

    /// # Safety
    ///
    /// `tty` must be a valid pointer.
    unsafe extern "C" fn write_room(tty: *mut bindings::tty_struct) -> core::ffi::c_uint {
        // SAFETY: tty is valid, driver_data was set by driver in open.
        let tty_ref = unsafe { OpsTty::<T>::from_raw(tty) };
        T::write_room(&tty_ref)
    }

    /// # Safety
    ///
    /// `tty` must be a valid pointer.
    unsafe extern "C" fn hangup(tty: *mut bindings::tty_struct) {
        // SAFETY: tty is valid, driver_data was set by driver in open.
        let tty_ref = unsafe { OpsTty::<T>::from_raw(tty) };
        T::hangup(&tty_ref);
    }

    const VTABLE: bindings::tty_operations = bindings::tty_operations {
        open: Some(Self::open),
        close: Some(Self::close),
        write: Some(Self::write),
        write_room: if T::HAS_WRITE_ROOM {
            Some(Self::write_room)
        } else {
            None
        },
        hangup: if T::HAS_HANGUP {
            Some(Self::hangup)
        } else {
            None
        },
        // All other operations are NULL.
        lookup: None,
        install: None,
        remove: None,
        shutdown: None,
        cleanup: None,
        put_char: None,
        flush_chars: None,
        chars_in_buffer: None,
        ioctl: None,
        compat_ioctl: None,
        set_termios: None,
        throttle: None,
        unthrottle: None,
        stop: None,
        start: None,
        break_ctl: None,
        flush_buffer: None,
        ldisc_ok: None,
        set_ldisc: None,
        wait_until_sent: None,
        send_xchar: None,
        tiocmget: None,
        tiocmset: None,
        resize: None,
        get_icount: None,
        get_serial: None,
        set_serial: None,
        show_fdinfo: None,
        #[cfg(CONFIG_CONSOLE_POLL)]
        poll_init: None,
        #[cfg(CONFIG_CONSOLE_POLL)]
        poll_get_char: None,
        #[cfg(CONFIG_CONSOLE_POLL)]
        poll_put_char: None,
        proc_show: None,
    };

    const fn build() -> &'static bindings::tty_operations {
        &Self::VTABLE
    }
}

/// Builder for creating and configuring a TTY driver before registration.
///
/// Use [`TtyDriverBuilder::new`] to create a builder, optionally link ports
/// with [`link_port`](Self::link_port), then call [`build`](Self::build) to
/// register and obtain a [`TtyDriver`].
///
/// # Example
///
/// ```ignore
/// let driver = KBox::pin_init(
///     TtyDriverBuilder::<MyOps>::new(opts, module)?
///         .link_port(&port, 0)
///         .build(),
///     GFP_KERNEL,
/// )?;
/// ```
pub struct TtyDriverBuilder<T: Operations> {
    driver_ptr: *mut bindings::tty_driver,
    _t: PhantomData<T>,
}

impl<T: Operations> TtyDriverBuilder<T> {
    /// Creates a new TTY driver builder.
    pub fn new(opts: Options, module: &'static crate::ThisModule) -> Result<Self> {
        // SAFETY: FFI call with valid arguments.
        let driver_ptr = unsafe { bindings::__tty_alloc_driver(1, module.as_ptr(), opts.flags) };

        if driver_ptr.is_null() || (driver_ptr as isize) < 0 && (driver_ptr as isize) > -4096 {
            if driver_ptr.is_null() {
                return Err(ENOMEM);
            }
            return Err(Error::from_errno(driver_ptr as i32));
        }

        // Configure the driver.
        // SAFETY: driver_ptr is valid.
        unsafe {
            (*driver_ptr).driver_name = opts.driver_name.as_char_ptr();
            (*driver_ptr).name = opts.name.as_char_ptr();
            (*driver_ptr).major = opts.major;
            (*driver_ptr).minor_start = opts.minor_start;
            (*driver_ptr).type_ = opts.driver_type as u32;

            // Set termios.
            let mut termios = bindings::tty_std_termios;
            termios.c_oflag = oflag::OPOST | oflag::OCRNL | oflag::ONOCR | oflag::ONLRET;
            (*driver_ptr).init_termios = termios;

            // Set operations vtable.
            (*driver_ptr).ops = OperationsVTable::<T>::build();
        }

        Ok(Self {
            driver_ptr,
            _t: PhantomData,
        })
    }

    /// Links a port to this driver at the specified line index.
    ///
    /// For fixed-device drivers (e.g., ttyprintk), call this before [`build`](Self::build).
    pub fn link_port(self, port: &DriverPort<T::PortOps>, line: u32) -> Self {
        // SAFETY: Both port and driver are valid.
        unsafe {
            bindings::tty_port_link_device(port.as_raw(), self.driver_ptr, line);
        }
        self
    }

    /// Registers the driver and returns a pin-initializer for [`TtyDriver`].
    ///
    /// The actual registration happens during pin-initialization.
    pub fn build(self) -> impl PinInit<TtyDriver<T>, Error> {
        let driver_ptr = self.driver_ptr;
        // Prevent Drop from freeing the driver_ptr; TtyDriver takes ownership.
        core::mem::forget(self);

        try_pin_init!(TtyDriver::<T> {
            inner <- Opaque::try_ffi_init(move |slot: *mut *mut bindings::tty_driver| {
                // SAFETY: driver_ptr is valid.
                let ret = unsafe { bindings::tty_register_driver(driver_ptr) };
                if ret != 0 {
                    // SAFETY: driver_ptr is valid, registration failed.
                    unsafe { bindings::tty_driver_kref_put(driver_ptr) };
                    return Err(Error::from_errno(ret));
                }
                // SAFETY: slot is valid for write.
                unsafe { slot.write(driver_ptr) };
                Ok(())
            }),
            _t: PhantomData,
        }? Error)
    }
}

impl<T: Operations> Drop for TtyDriverBuilder<T> {
    fn drop(&mut self) {
        // SAFETY: driver_ptr is valid, not yet registered.
        unsafe { bindings::tty_driver_kref_put(self.driver_ptr) };
    }
}

impl<T, S> TtyDriverBuilder<T>
where
    T: Operations<DriverState = Arc<S>>,
    S: Send + Sync,
{
    /// Sets the driver-level state, taking ownership of the Arc.
    ///
    /// The state can be accessed via [`Tty::driver_state`] in TTY operation callbacks.
    ///
    /// # Note
    ///
    /// The caller must call [`TtyDriver::take_driver_state`] before the driver is
    /// dropped to reclaim the state's memory. Failure to do so will result in a
    /// memory leak.
    pub fn set_driver_state(self, state: Arc<S>) -> Self {
        // SAFETY: driver_ptr is valid.
        unsafe {
            (*self.driver_ptr).driver_state = Arc::into_raw(state) as *mut _;
        }
        self
    }
}

/// A registered TTY driver.
///
/// Created via [`TtyDriverBuilder::build`]. The driver is automatically
/// unregistered when dropped.
///
/// For probe-based drivers, ports can be linked after creation using
/// [`link_port`](Self::link_port).
///
/// # Invariants
///
/// - `inner` contains a valid pointer to a registered `tty_driver`.
/// - Deregistration occurs exactly once in [`Drop`].
#[pin_data(PinnedDrop)]
pub struct TtyDriver<T: Operations> {
    #[pin]
    inner: Opaque<*mut bindings::tty_driver>,
    _t: PhantomData<T>,
}

// SAFETY: It is allowed to call `tty_unregister_driver` on a different thread.
unsafe impl<T: Operations> Send for TtyDriver<T> {}
// SAFETY: All `&self` methods are safe to call in parallel.
unsafe impl<T: Operations> Sync for TtyDriver<T> {}

impl<T: Operations> TtyDriver<T> {
    /// Returns the driver pointer.
    fn driver_ptr(&self) -> *mut bindings::tty_driver {
        // SAFETY: inner is initialized.
        unsafe { *self.inner.get() }
    }

    /// Links a port to this driver at the specified line index.
    ///
    /// For probe-based drivers (e.g., serial), call this at device probe time.
    pub fn link_port<O: PortOperations + 'static>(&self, port: &DriverPort<O>, line: u32) {
        // SAFETY: Both port and driver are valid.
        unsafe {
            bindings::tty_port_link_device(port.as_raw(), self.driver_ptr(), line);
        }
    }

    /// Returns a raw pointer to the TTY driver.
    pub fn as_raw(&self) -> *mut bindings::tty_driver {
        self.driver_ptr()
    }
}

impl<T, S> TtyDriver<T>
where
    T: Operations<DriverState = Arc<S>>,
    S: Send + Sync,
{
    /// Takes the driver state, returning ownership of the Arc.
    ///
    /// Returns `None` if no state was set. This should be called before the driver
    /// is dropped to reclaim the state's memory.
    pub fn take_driver_state(&self) -> Option<Arc<S>> {
        // SAFETY: driver_ptr is valid.
        let ptr = unsafe { (*self.driver_ptr()).driver_state };
        if ptr.is_null() {
            return None;
        }
        // SAFETY: driver_ptr is valid.
        unsafe {
            (*self.driver_ptr()).driver_state = core::ptr::null_mut();
        }
        // SAFETY: ptr was set via set_driver_state from an Arc<S>.
        Some(unsafe { Arc::from_raw(ptr.cast()) })
    }

    /// Returns a reference to the driver state.
    ///
    /// Returns `None` if no state was set.
    pub fn driver_state(&self) -> Option<&S> {
        // SAFETY: driver_ptr is valid.
        let ptr = unsafe { (*self.driver_ptr()).driver_state };
        if ptr.is_null() {
            return None;
        }
        // SAFETY: ptr was set via set_driver_state from an Arc<S>.
        Some(unsafe { &*ptr.cast::<S>() })
    }
}

#[pinned_drop]
impl<T: Operations> PinnedDrop for TtyDriver<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: inner contains a valid registered driver.
        unsafe {
            let ptr = *self.inner.get();
            bindings::tty_unregister_driver(ptr);
            bindings::tty_driver_kref_put(ptr);
        }
    }
}
