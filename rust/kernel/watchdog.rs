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
/// The pointer is valid for the duration of a callback invocation.
///
/// # Concurrency
///
/// The watchdog core serialises most operations via its internal lock, but
/// not all of them: the first `start`/`ping` on open, the reboot notifier
/// `stop`, and the `stop` on unregistration run without it. Therefore only
/// shared references are handed to drivers, all mutation goes through
/// interior mutability, and driver data must be [`Sync`].
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
    /// - The returned reference must not outlive the callback invocation.
    unsafe fn from_raw<'a>(ptr: *mut bindings::watchdog_device) -> &'a Self {
        // CAST: `Self` is a `repr(transparent)` wrapper around
        // `bindings::watchdog_device`.
        let ptr = ptr.cast::<Self>();
        // SAFETY: By the function requirements the pointer is valid.
        unsafe { &*ptr }
    }

    /// Returns a raw pointer to the underlying `watchdog_device`.
    fn as_raw(&self) -> *mut bindings::watchdog_device {
        self.0.get()
    }

    /// Returns the current timeout in seconds.
    pub fn timeout(&self) -> u32 {
        // SAFETY: The struct invariant ensures the pointer is valid. The
        // read of this `unsigned int` field mirrors how the C core and C
        // drivers access it without synchronisation.
        unsafe { (*self.as_raw()).timeout }
    }

    /// Sets the current timeout in seconds.
    pub fn set_timeout(&self, timeout: u32) {
        // SAFETY: The struct invariant ensures the pointer is valid. The
        // field lives in an `Opaque`, so writing through a shared reference
        // is allowed; the C core accesses this field the same way.
        unsafe { (*self.as_raw()).timeout = timeout };
    }

    /// Returns the current pretimeout in seconds.
    pub fn pretimeout(&self) -> u32 {
        // SAFETY: See `timeout`.
        unsafe { (*self.as_raw()).pretimeout }
    }

    /// Returns the minimum timeout in seconds.
    pub fn min_timeout(&self) -> u32 {
        // SAFETY: See `timeout`.
        unsafe { (*self.as_raw()).min_timeout }
    }

    /// Returns the maximum timeout in seconds.
    pub fn max_timeout(&self) -> u32 {
        // SAFETY: See `timeout`.
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

/// Flags describing the capabilities of a watchdog device, for use in
/// [`super::Info::new`].
pub mod flags {
    /// The watchdog supports setting the timeout.
    pub const SETTIMEOUT: u32 = bindings::WDIOF_SETTIMEOUT;
    /// The watchdog supports keepalive pings.
    pub const KEEPALIVEPING: u32 = bindings::WDIOF_KEEPALIVEPING;
    /// The watchdog supports the magic close character.
    pub const MAGICCLOSE: u32 = bindings::WDIOF_MAGICCLOSE;
    /// The watchdog supports pretimeouts.
    pub const PRETIMEOUT: u32 = bindings::WDIOF_PRETIMEOUT;
}

/// Watchdog identity information.
///
/// Wraps the kernel's [`struct watchdog_info`].
///
/// [`struct watchdog_info`]: srctree/include/uapi/linux/watchdog.h
#[repr(transparent)]
pub struct Info(bindings::watchdog_info);

impl Info {
    /// Creates a new [`Info`] with the given option [`flags`] and identity
    /// string.
    ///
    /// The identity is truncated to 31 bytes to fit the fixed size,
    /// NUL-terminated C field.
    pub const fn new(options: u32, identity: &str) -> Self {
        let mut id = [0u8; 32];
        let bytes = identity.as_bytes();
        let mut i = 0;
        while i < bytes.len() && i < id.len() - 1 {
            id[i] = bytes[i];
            i += 1;
        }
        Self(bindings::watchdog_info {
            options,
            firmware_version: 0,
            identity: id,
        })
    }
}

/// Configuration for registering a watchdog device.
///
/// All timeouts are in seconds; `max_hw_heartbeat_ms` is in milliseconds.
///
/// The watchdog core requires either a [`WatchdogOps::stop`] implementation
/// or a non-zero `max_hw_heartbeat_ms`, otherwise registration fails with
/// `EINVAL`.
pub struct Options {
    /// The default timeout in seconds.
    pub timeout: u32,
    /// The minimum settable timeout in seconds.
    pub min_timeout: u32,
    /// The maximum settable timeout in seconds.
    pub max_timeout: u32,
    /// Hardware limit for the maximum timeout, in milliseconds.
    /// Zero means no hardware limit.
    pub max_hw_heartbeat_ms: u32,
    /// If `true`, the watchdog cannot be stopped once started.
    pub nowayout: bool,
    /// If `true`, the watchdog is stopped on system reboot. Requires a
    /// [`WatchdogOps::stop`] implementation.
    pub stop_on_reboot: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            timeout: 0,
            min_timeout: 0,
            max_timeout: 0,
            max_hw_heartbeat_ms: 0,
            nowayout: cfg!(CONFIG_WATCHDOG_NOWAYOUT),
            stop_on_reboot: false,
        }
    }
}

/// An adapter for the registration of a watchdog driver.
struct Adapter<T: WatchdogOps> {
    _p: PhantomData<T>,
}

impl<T: WatchdogOps> Adapter<T> {
    /// Returns a reference to the driver data of a registered device.
    ///
    /// # Safety
    ///
    /// - `wdd` must be a `watchdog_device` registered via [`Registration`],
    ///   so that its driver data points at a valid `T::Data`.
    /// - The returned reference must not outlive the callback invocation.
    unsafe fn data_from_raw<'a>(wdd: *mut bindings::watchdog_device) -> &'a T::Data {
        // SAFETY: `Registration::register` stored a pointer to the heap
        // allocated `T::Data` as driver data of this device, valid until
        // unregistration tears down the paths that invoke callbacks.
        unsafe { &*bindings::watchdog_get_drvdata(wdd).cast::<T::Data>() }
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn start_callback(wdd: *mut bindings::watchdog_device) -> c_int {
        from_result(|| {
            // SAFETY: The pointer is valid for the duration of the callback.
            let dev = unsafe { Device::from_raw(wdd) };
            // SAFETY: `wdd` was registered by `Registration`.
            let data = unsafe { Self::data_from_raw(wdd) };
            T::start(dev, data)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn stop_callback(wdd: *mut bindings::watchdog_device) -> c_int {
        from_result(|| {
            // SAFETY: The pointer is valid for the duration of the callback.
            let dev = unsafe { Device::from_raw(wdd) };
            // SAFETY: `wdd` was registered by `Registration`.
            let data = unsafe { Self::data_from_raw(wdd) };
            T::stop(dev, data)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn ping_callback(wdd: *mut bindings::watchdog_device) -> c_int {
        from_result(|| {
            // SAFETY: The pointer is valid for the duration of the callback.
            let dev = unsafe { Device::from_raw(wdd) };
            // SAFETY: `wdd` was registered by `Registration`.
            let data = unsafe { Self::data_from_raw(wdd) };
            T::ping(dev, data)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn set_timeout_callback(
        wdd: *mut bindings::watchdog_device,
        timeout: c_uint,
    ) -> c_int {
        from_result(|| {
            // SAFETY: The pointer is valid for the duration of the callback.
            let dev = unsafe { Device::from_raw(wdd) };
            // SAFETY: `wdd` was registered by `Registration`.
            let data = unsafe { Self::data_from_raw(wdd) };
            T::set_timeout(dev, data, timeout)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn set_pretimeout_callback(
        wdd: *mut bindings::watchdog_device,
        pretimeout: c_uint,
    ) -> c_int {
        from_result(|| {
            // SAFETY: The pointer is valid for the duration of the callback.
            let dev = unsafe { Device::from_raw(wdd) };
            // SAFETY: `wdd` was registered by `Registration`.
            let data = unsafe { Self::data_from_raw(wdd) };
            T::set_pretimeout(dev, data, pretimeout)?;
            Ok(0)
        })
    }

    /// # Safety
    ///
    /// `wdd` must be passed by the corresponding callback in `watchdog_ops`.
    unsafe extern "C" fn get_timeleft_callback(wdd: *mut bindings::watchdog_device) -> c_uint {
        // SAFETY: The pointer is valid for the duration of the callback.
        let dev = unsafe { Device::from_raw(wdd) };
        // SAFETY: `wdd` was registered by `Registration`.
        let data = unsafe { Self::data_from_raw(wdd) };
        T::get_timeleft(dev, data)
    }
}

/// Watchdog device operations.
///
/// Implement this trait to provide the callbacks for a watchdog device.
/// Only [`WatchdogOps::start`] is mandatory; all other operations are
/// optional. Note that the watchdog core requires either a
/// [`WatchdogOps::stop`] implementation or a non-zero
/// [`Options::max_hw_heartbeat_ms`].
///
/// Every callback receives a reference to the driver's private data of type
/// [`WatchdogOps::Data`], which is passed to [`Registration::register`] and
/// freed when the [`Registration`] is dropped.
///
/// Callbacks may be invoked concurrently (the watchdog core does not hold
/// its lock on every path), so the data must be [`Sync`] and any mutable
/// driver state needs its own synchronisation.
#[vtable]
pub trait WatchdogOps {
    /// Driver private data, accessible from all callbacks.
    type Data: Send + Sync;

    /// Starts the watchdog device.
    ///
    /// This is the only mandatory operation.
    fn start(dev: &Device, data: &Self::Data) -> Result;

    /// Stops the watchdog device.
    fn stop(_dev: &Device, _data: &Self::Data) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sends a keepalive ping to the watchdog device.
    fn ping(_dev: &Device, _data: &Self::Data) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sets the watchdog timeout in seconds.
    fn set_timeout(_dev: &Device, _data: &Self::Data, _timeout: u32) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Sets the watchdog pretimeout in seconds.
    fn set_pretimeout(_dev: &Device, _data: &Self::Data, _pretimeout: u32) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Returns the time left before the watchdog fires (in seconds).
    fn get_timeleft(_dev: &Device, _data: &Self::Data) -> u32 {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

/// Creates a [`bindings::watchdog_ops`] instance from [`WatchdogOps`].
///
/// The `owner` field is filled in by [`Registration::register`].
const fn create_watchdog_ops<T: WatchdogOps>() -> bindings::watchdog_ops {
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

/// The heap allocated backing storage of a registration.
///
/// The watchdog core stores pointers into this structure (the device `ops`
/// pointer points at `ops`, the device driver data points at `data`), so it
/// is heap allocated and only reclaimed after unregistration.
struct Inner<T: WatchdogOps> {
    wdd: Opaque<bindings::watchdog_device>,
    ops: bindings::watchdog_ops,
    data: T::Data,
}

/// Watchdog device registration.
///
/// Registers a watchdog device with the kernel. The device is unregistered
/// and the driver data dropped when this instance is dropped.
///
/// # Invariants
///
/// - `inner` points at a live [`Inner`] obtained from [`KBox::into_raw`],
///   registered with the watchdog core via `watchdog_register_device`.
/// - The registered device's `ops` pointer and driver data pointer point at
///   the `ops` and `data` fields of that [`Inner`].
pub struct Registration<T: WatchdogOps> {
    inner: *mut Inner<T>,
}

// SAFETY: `bindings::watchdog_device` contains raw pointers managed
// exclusively by the watchdog core, which is thread safe. `T::Data` is
// `Send` per the trait bound, so the heap allocation may be dropped from
// any thread.
unsafe impl<T: WatchdogOps> Send for Registration<T> {}

// SAFETY: `Registration` has no `&self` methods, so sharing references to
// it across threads gives access to nothing.
unsafe impl<T: WatchdogOps> Sync for Registration<T> {}

impl<T: WatchdogOps> Registration<T> {
    /// Creates and registers a new watchdog device.
    ///
    /// `module` is used to set the `owner` field of the watchdog operations,
    /// ensuring correct module reference counting while `/dev/watchdog` is
    /// open. `data` is the driver's private data, passed to every callback.
    ///
    /// The watchdog is configured to stop on unregistration. Note that the
    /// core skips that stop under `nowayout`, and that for drivers without
    /// a [`WatchdogOps::stop`] implementation an active hardware watchdog
    /// keeps running after unregistration with nobody pinging it, which
    /// leads to a system reset. In all cases the core tears down its
    /// character device and keepalive worker before unregistration
    /// returns, so no callback can run after the driver data has been
    /// freed.
    pub fn register(
        module: &'static crate::ThisModule,
        parent: Option<&device::Device>,
        info: &'static Info,
        options: &Options,
        data: T::Data,
    ) -> Result<Self> {
        // The core silently ignores the stop-on-reboot notifier for devices
        // without a stop operation; reject the combination instead.
        if options.stop_on_reboot && !T::HAS_STOP {
            return Err(EINVAL);
        }

        let mut ops = create_watchdog_ops::<T>();
        ops.owner = module.as_ptr();

        let wdd = bindings::watchdog_device {
            // CAST: `Info` is a `repr(transparent)` wrapper around
            // `bindings::watchdog_info`.
            info: core::ptr::from_ref(info).cast(),
            timeout: options.timeout,
            min_timeout: options.min_timeout,
            max_timeout: options.max_timeout,
            max_hw_heartbeat_ms: options.max_hw_heartbeat_ms,
            parent: parent.map_or(core::ptr::null_mut(), |p| p.as_raw()),
            ..pin_init::zeroed()
        };

        let inner = KBox::new(
            Inner {
                wdd: Opaque::new(wdd),
                ops,
                data,
            },
            GFP_KERNEL,
        )?;

        // Transfer the allocation to a raw pointer so that no reference to
        // `Inner` exists while the C core holds pointers into it. It is
        // reclaimed with `KBox::from_raw` in `Drop` (or below on failure).
        let inner = KBox::into_raw(inner);

        // The pointers below are derived from `inner` and point into the
        // heap allocation, so they remain valid until the allocation is
        // reclaimed.

        // SAFETY: `inner` came from `KBox::into_raw`, so it points at a live
        // allocation and the field projection stays in bounds.
        let wdd_ptr = Opaque::cast_into(unsafe { &raw const (*inner).wdd });
        // SAFETY: As above.
        let ops_ptr: *const bindings::watchdog_ops = unsafe { &raw const (*inner).ops };
        // SAFETY: As above.
        let data_ptr: *const T::Data = unsafe { &raw const (*inner).data };

        // SAFETY: `wdd_ptr` points at a valid `watchdog_device` that is not
        // yet registered, so we have exclusive access.
        unsafe {
            (*wdd_ptr).ops = ops_ptr;
            bindings::watchdog_set_drvdata(wdd_ptr, data_ptr.cast_mut().cast());
            bindings::watchdog_set_nowayout(wdd_ptr, options.nowayout);
            // Stop the watchdog on unregistration so that no callback can
            // run after `Inner` has been freed.
            bindings::watchdog_stop_on_unregister(wdd_ptr);
            if options.stop_on_reboot {
                bindings::watchdog_stop_on_reboot(wdd_ptr);
            }
        }

        // SAFETY: `wdd_ptr` points at a fully initialised `watchdog_device`.
        let ret = unsafe { bindings::watchdog_register_device(wdd_ptr) };
        if ret != 0 {
            // SAFETY: `inner` came from `KBox::into_raw` above and was not
            // registered, so we have exclusive ownership of it.
            drop(unsafe { KBox::from_raw(inner) });
            return Err(Error::from_errno(ret));
        }

        // INVARIANT: `inner` is registered and its `ops`/`data` fields are
        // referenced by the registered device.
        Ok(Registration { inner })
    }
}

impl<T: WatchdogOps> Drop for Registration<T> {
    fn drop(&mut self) {
        // SAFETY: The type invariant guarantees that `self.inner` is live
        // and registered. Unregistration tears down the character device
        // and the core's keepalive worker before returning, so no callback
        // can run afterwards.
        unsafe {
            bindings::watchdog_unregister_device(Opaque::cast_into(&raw const (*self.inner).wdd))
        };
        // SAFETY: `self.inner` came from `KBox::into_raw` and the device is
        // now unregistered, so we have exclusive ownership of the
        // allocation and can reclaim and drop it.
        drop(unsafe { KBox::from_raw(self.inner) });
    }
}
