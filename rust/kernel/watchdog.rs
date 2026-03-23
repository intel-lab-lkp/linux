// SPDX-License-Identifier: GPL-2.0

//! Watchdog device support.
//!
//! C headers: [`include/linux/watchdog.h`](srctree/include/linux/watchdog.h).

use crate::{bindings, device, error::*, prelude::*, types::Opaque};
use core::marker::PhantomData;

/// A watchdog device.
///
/// Wraps the kernel's [`struct watchdog_device`].
///
/// # Invariants
///
/// The pointer is valid and the watchdog core serialises access to the device
/// during callback execution.
///
/// [`struct watchdog_device`]: srctree/include/linux/watchdog.h
#[repr(transparent)]
pub struct Device(Opaque<bindings::watchdog_device>);

impl Device {
    /// Creates a new [`Device`] reference from a raw pointer.
    ///
    /// # Safety
    ///
    /// - `ptr` must point at a valid `watchdog_device`.
    /// - The caller must be in a callback context where the watchdog core
    ///   serialises access.
    /// - The returned reference must not outlive the callback invocation.
    unsafe fn from_raw<'a>(ptr: *mut bindings::watchdog_device) -> &'a mut Self {
        // CAST: `Self` is a `repr(transparent)` wrapper around `bindings::watchdog_device`.
        let ptr = ptr.cast::<Self>();
        // SAFETY: By the function requirements the pointer is valid and we have
        // exclusive access for the duration of the callback.
        unsafe { &mut *ptr }
    }

    /// Returns a raw pointer to the underlying `watchdog_device`.
    fn as_raw(&self) -> *mut bindings::watchdog_device {
        self.0.get()
    }

    /// Returns the current timeout in seconds.
    pub fn timeout(&self) -> u32 {
        // SAFETY: The struct invariant ensures we may access this field.
        unsafe { (*self.as_raw()).timeout }
    }

    /// Sets the current timeout in seconds.
    pub fn set_timeout(&mut self, timeout: u32) {
        // SAFETY: The struct invariant ensures exclusive access.
        unsafe { (*self.as_raw()).timeout = timeout };
    }

    /// Returns the current pretimeout in seconds.
    pub fn pretimeout(&self) -> u32 {
        // SAFETY: The struct invariant ensures we may access this field.
        unsafe { (*self.as_raw()).pretimeout }
    }

    /// Returns the minimum timeout in seconds.
    pub fn min_timeout(&self) -> u32 {
        // SAFETY: The struct invariant ensures we may access this field.
        unsafe { (*self.as_raw()).min_timeout }
    }

    /// Returns the maximum timeout in seconds.
    pub fn max_timeout(&self) -> u32 {
        // SAFETY: The struct invariant ensures we may access this field.
        unsafe { (*self.as_raw()).max_timeout }
    }

    /// Returns `true` if the watchdog is active.
    pub fn is_active(&self) -> bool {
        // SAFETY: The struct invariant ensures the pointer is valid.
        unsafe { bindings::watchdog_active(self.as_raw()) }
    }

    /// Returns `true` if the hardware watchdog is running.
    pub fn is_hw_running(&self) -> bool {
        // SAFETY: The struct invariant ensures the pointer is valid.
        unsafe { bindings::watchdog_hw_running(self.as_raw()) }
    }
}

/// An adapter for the registration of a watchdog driver.
struct Adapter<T: WatchdogOps> {
    _p: PhantomData<T>,
}

impl<T: WatchdogOps> Adapter<T> {
    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn start_callback(
        wdd: *mut bindings::watchdog_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The watchdog core serialises access to the device.
            let dev = unsafe { Device::from_raw(wdd) };
            T::start(dev)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn stop_callback(
        wdd: *mut bindings::watchdog_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The watchdog core serialises access to the device.
            let dev = unsafe { Device::from_raw(wdd) };
            T::stop(dev)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn ping_callback(
        wdd: *mut bindings::watchdog_device,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The watchdog core serialises access to the device.
            let dev = unsafe { Device::from_raw(wdd) };
            T::ping(dev)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn set_timeout_callback(
        wdd: *mut bindings::watchdog_device,
        timeout: core::ffi::c_uint,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The watchdog core serialises access to the device.
            let dev = unsafe { Device::from_raw(wdd) };
            T::set_timeout(dev, timeout)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn set_pretimeout_callback(
        wdd: *mut bindings::watchdog_device,
        pretimeout: core::ffi::c_uint,
    ) -> core::ffi::c_int {
        from_result(|| {
            // SAFETY: The watchdog core serialises access to the device.
            let dev = unsafe { Device::from_raw(wdd) };
            T::set_pretimeout(dev, pretimeout)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn get_timeleft_callback(
        wdd: *mut bindings::watchdog_device,
    ) -> core::ffi::c_uint {
        // SAFETY: The watchdog core serialises access to the device.
        // We create a shared reference since get_timeleft only needs &Device.
        let dev = unsafe { &*(wdd.cast::<Device>()) };
        T::get_timeleft(dev)
    }
}

/// Watchdog device operations.
///
/// Implement this trait to provide the callbacks for a watchdog device.
/// Only [`WatchdogOps::start`] is mandatory; all other operations are optional.
#[vtable]
pub trait WatchdogOps {
    /// Starts the watchdog device.
    ///
    /// This is the only mandatory operation.
    fn start(dev: &mut Device) -> Result;

    /// Stops the watchdog device.
    fn stop(_dev: &mut Device) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sends a keepalive ping to the watchdog device.
    fn ping(_dev: &mut Device) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sets the watchdog timeout in seconds.
    fn set_timeout(_dev: &mut Device, _timeout: u32) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sets the watchdog pretimeout in seconds.
    fn set_pretimeout(_dev: &mut Device, _pretimeout: u32) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Returns the time left before the watchdog fires (in seconds).
    fn get_timeleft(_dev: &Device) -> u32 {
        0
    }
}

/// Creates a [`bindings::watchdog_ops`] instance from [`WatchdogOps`].
///
/// This populates the C ops struct at compile time based on which trait
/// methods are implemented. The `owner` field must be set separately by
/// the caller before registration.
pub const fn create_watchdog_ops<T: WatchdogOps>() -> bindings::watchdog_ops {
    bindings::watchdog_ops {
        owner: core::ptr::null_mut(),
        start: Some(Adapter::<T>::start_callback),
        stop: if T::HAS_STOP {
            Some(Adapter::<T>::stop_callback)
        } else {
            None
        },
        ping: if T::HAS_PING {
            Some(Adapter::<T>::ping_callback)
        } else {
            None
        },
        status: None,
        set_timeout: if T::HAS_SET_TIMEOUT {
            Some(Adapter::<T>::set_timeout_callback)
        } else {
            None
        },
        set_pretimeout: if T::HAS_SET_PRETIMEOUT {
            Some(Adapter::<T>::set_pretimeout_callback)
        } else {
            None
        },
        get_timeleft: if T::HAS_GET_TIMELEFT {
            Some(Adapter::<T>::get_timeleft_callback)
        } else {
            None
        },
        restart: None,
        ioctl: None,
    }
}

/// Watchdog device registration.
///
/// Registers a watchdog device with the kernel. The device will be
/// unregistered when this instance is dropped.
///
/// The [`watchdog_device`] is heap-allocated to ensure a stable address,
/// since the watchdog core stores pointers back into it.
///
/// # Invariants
///
/// `inner` points to a heap-allocated `watchdog_device` that is registered
/// with the watchdog core via `watchdog_register_device`.
///
/// [`watchdog_device`]: srctree/include/linux/watchdog.h
pub struct Registration {
    inner: KBox<Opaque<bindings::watchdog_device>>,
}

// SAFETY: `watchdog_unregister_device` can be called from any context.
// The `KBox` ensures the `watchdog_device` has a stable heap address that
// remains valid regardless of where the `Registration` is moved.
unsafe impl Send for Registration {}

// SAFETY: The only method that returns a reference to the device (`device()`)
// returns `&Device` (shared reference), which only exposes read-only accessors.
// The watchdog core serialises access during callbacks.
unsafe impl Sync for Registration {}

impl Registration {
    /// Creates and registers a new watchdog device.
    ///
    /// The `module` parameter is used to set the `owner` field in the
    /// watchdog ops, ensuring correct module reference counting when
    /// userspace opens `/dev/watchdog`.
    pub fn register(
        module: &'static crate::ThisModule,
        parent: Option<&device::Device>,
        info: &'static bindings::watchdog_info,
        ops: &'static mut bindings::watchdog_ops,
        timeout: u32,
        min_timeout: u32,
        max_timeout: u32,
        nowayout: bool,
    ) -> Result<Self> {
        // Set the module owner so the kernel holds a reference to our
        // module while /dev/watchdog is open.
        ops.owner = module.as_ptr();

        let mut wdd: bindings::watchdog_device =
            // SAFETY: Zeroing out `watchdog_device` is valid since all fields
            // are simple types (integers, pointers) that can be zero-initialised.
            unsafe { core::mem::MaybeUninit::zeroed().assume_init() };

        wdd.info = info;
        wdd.ops = ops;
        wdd.timeout = timeout;
        wdd.min_timeout = min_timeout;
        wdd.max_timeout = max_timeout;
        wdd.parent = if let Some(p) = parent {
            p.as_raw()
        } else {
            core::ptr::null_mut()
        };

        // SAFETY: The watchdog_device is fully initialised.
        unsafe { bindings::watchdog_set_nowayout(&mut wdd, nowayout) };
        // SAFETY: The watchdog_device is fully initialised.
        unsafe { bindings::watchdog_stop_on_reboot(&mut wdd) };

        // Heap-allocate the watchdog_device so that the C core's back-pointers
        // remain valid even if this Registration is moved.
        let inner = KBox::new(Opaque::new(wdd), GFP_KERNEL)?;

        // SAFETY: `inner` points to a properly initialised watchdog_device on
        // the heap. The pointer will remain stable for the lifetime of the KBox.
        to_result(unsafe { bindings::watchdog_register_device(inner.get()) })?;

        Ok(Registration { inner })
    }

    /// Returns a reference to the underlying [`Device`].
    pub fn device(&self) -> &Device {
        // SAFETY: `inner` points to a valid `watchdog_device`.
        unsafe { &*(self.inner.get() as *const Device) }
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        // SAFETY: The type invariant guarantees that `self.inner` is registered.
        unsafe { bindings::watchdog_unregister_device(self.inner.get()) };
    }
}
