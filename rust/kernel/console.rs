// SPDX-License-Identifier: GPL-2.0

//! Console driver abstraction.
//!
//! This module provides safe Rust wrappers for implementing kernel console
//! drivers, which receive kernel log messages and output them to a device.
//!
//! C header: [`include/linux/console.h`](srctree/include/linux/console.h)

use crate::{bindings, error::Result, prelude::*, str::CStr, types::Opaque};
use core::marker::PhantomData;

/// Console flags from `enum cons_flags`.
pub mod flags {
    /// Used by newly registered consoles to avoid duplicate output.
    pub const CON_PRINTBUFFER: u16 = bindings::cons_flags_CON_PRINTBUFFER as u16;
    /// Indicates console is backing /dev/console.
    pub const CON_CONSDEV: u16 = bindings::cons_flags_CON_CONSDEV as u16;
    /// Console is enabled.
    pub const CON_ENABLED: u16 = bindings::cons_flags_CON_ENABLED as u16;
    /// Early boot console.
    pub const CON_BOOT: u16 = bindings::cons_flags_CON_BOOT as u16;
    /// Console can be called from any context.
    pub const CON_ANYTIME: u16 = bindings::cons_flags_CON_ANYTIME as u16;
    /// Braille device.
    pub const CON_BRL: u16 = bindings::cons_flags_CON_BRL as u16;
    /// Console supports extended output format.
    pub const CON_EXTENDED: u16 = bindings::cons_flags_CON_EXTENDED as u16;
    /// Console is suspended.
    pub const CON_SUSPENDED: u16 = bindings::cons_flags_CON_SUSPENDED as u16;
}

/// Operations that a console driver must implement.
///
/// The `write` callback is the only required operation. It will be called
/// to output kernel log messages.
#[vtable]
pub trait ConsoleOps: Sized + Send + Sync {
    /// Writes a message to the console.
    ///
    /// This is called with a buffer containing the message to output.
    /// The implementation should send the message to the console device.
    ///
    /// # Context
    ///
    /// This may be called from any context, including IRQ context.
    /// Implementations must not sleep.
    fn write(&self, msg: &[u8]);

    /// Sets up the console.
    ///
    /// This is called when the console is registered.
    /// The `options` parameter contains any boot command line options.
    fn setup(&self, _options: Option<&CStr>) -> Result {
        Ok(())
    }
}

/// Adapter for console operations vtable.
struct ConsoleOpsAdapter<T: ConsoleOps>(PhantomData<T>);

impl<T: ConsoleOps> ConsoleOpsAdapter<T> {
    /// Write callback for the console.
    ///
    /// # Safety
    ///
    /// `con` must be a valid pointer to a `bindings::console` that was
    /// created by `Console<T>` and has valid `data` pointing to `T`.
    unsafe extern "C" fn write_callback(
        con: *mut bindings::console,
        s: *const u8,
        count: core::ffi::c_uint,
    ) {
        // SAFETY: By function safety requirements, `con` is valid.
        let data = unsafe { (*con).data };
        if data.is_null() {
            return;
        }

        // SAFETY: `data` points to a valid `T` per the type invariants.
        let ops = unsafe { &*(data as *const T) };

        // SAFETY: `s` is valid for `count` bytes.
        let msg = unsafe { core::slice::from_raw_parts(s, count as usize) };

        ops.write(msg);
    }

    /// Setup callback for the console.
    ///
    /// # Safety
    ///
    /// `con` must be a valid pointer to a `bindings::console`.
    unsafe extern "C" fn setup_callback(
        con: *mut bindings::console,
        options: *mut u8,
    ) -> core::ffi::c_int {
        // SAFETY: By function safety requirements, `con` is valid.
        let data = unsafe { (*con).data };
        if data.is_null() {
            return 0;
        }

        // SAFETY: `data` points to a valid `T` per the type invariants.
        let ops = unsafe { &*(data as *const T) };

        let options_cstr = if options.is_null() {
            None
        } else {
            // SAFETY: If not null, `options` points to a null-terminated string.
            Some(unsafe { CStr::from_char_ptr(options.cast()) })
        };

        match ops.setup(options_cstr) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }
}

/// A registered kernel console.
///
/// This struct wraps the kernel's `struct console` and provides safe
/// registration and unregistration of console drivers.
///
/// # Invariants
///
/// - `inner` contains a valid `console` structure.
/// - When registered, the console's `data` field points to valid `T`.
#[pin_data(PinnedDrop)]
pub struct Console<T: ConsoleOps> {
    #[pin]
    inner: Opaque<bindings::console>,
    #[pin]
    data: T,
    registered: bool,
}

// SAFETY: Console can be sent between threads if T can.
unsafe impl<T: ConsoleOps + Send> Send for Console<T> {}

// SAFETY: Console operations are synchronized by the caller.
unsafe impl<T: ConsoleOps + Sync> Sync for Console<T> {}

impl<T: ConsoleOps> Console<T> {
    /// Creates an initializer for registering a new console.
    ///
    /// # Arguments
    ///
    /// * `name` - The name of the console (up to 15 characters).
    /// * `flags` - Console flags from the `flags` module.
    /// * `data` - The console operations implementation.
    pub fn register(
        name: &'static CStr,
        console_flags: u16,
        data: impl PinInit<T, Error>,
    ) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            inner <- Opaque::try_ffi_init(|slot: *mut bindings::console| {
                // SAFETY: `slot` is valid for writing.
                unsafe {
                    // Zero-initialize the struct.
                    core::ptr::write_bytes(slot, 0, 1);

                    // Copy the name (up to 15 chars + null).
                    let name_bytes = name.to_bytes();
                    let name_len = core::cmp::min(name_bytes.len(), 15);
                    core::ptr::copy_nonoverlapping(
                        name_bytes.as_ptr().cast(),
                        (*slot).name.as_mut_ptr(),
                        name_len,
                    );

                    // Set flags.
                    (*slot).flags = console_flags as i16;

                    // Set the write callback.
                    (*slot).write = Some(ConsoleOpsAdapter::<T>::write_callback);

                    // Set the setup callback if T implements it.
                    if T::HAS_SETUP {
                        (*slot).setup = Some(ConsoleOpsAdapter::<T>::setup_callback);
                    }
                }
                Ok::<(), Error>(())
            }),
            data <- data,
            registered: true,  // Will be set after registration in pin_chain
        })
        .pin_chain(|this| {
            // Set the data pointer to our ops.
            // SAFETY: `this` is pinned and valid.
            unsafe {
                let con = this.inner.get();
                (*con).data = &this.data as *const T as *mut core::ffi::c_void;
            }

            // Register the console.
            // SAFETY: The console structure is properly initialized.
            unsafe { bindings::register_console(this.inner.get()) };

            Ok(())
        })
    }

    /// Returns whether the console is currently registered.
    pub fn is_registered(&self) -> bool {
        self.registered
    }

    /// Returns a pointer to the underlying console struct.
    pub fn as_ptr(&self) -> *mut bindings::console {
        self.inner.get()
    }

    /// Returns a reference to the console data.
    pub fn data(&self) -> &T {
        &self.data
    }
}

#[pinned_drop]
impl<T: ConsoleOps> PinnedDrop for Console<T> {
    fn drop(self: Pin<&mut Self>) {
        if self.registered {
            // SAFETY: The console was registered during initialization.
            unsafe { bindings::unregister_console(self.inner.get()) };
        }
    }
}
