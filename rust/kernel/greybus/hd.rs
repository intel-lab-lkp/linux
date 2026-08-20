// SPDX-License-Identifier: GPL-2.0

//! Greybus host device abstractions.
//!
//! A driver implements the transmit path through [`HdDriver`] and feeds received data back into
//! the core with [`Device::data_rcvd`].
//!
//! A host device is created, populated with driver private data and added to the Greybus bus by
//! constructing a [`Registration`], which owns the underlying `struct gb_host_device`. Dropping
//! the registration drops the private data and removes the device from the bus, so it is normally
//! stored in the driver data of the parent device and torn down implicitly when that device goes
//! away.
//!
//! Individual references to a live host device are represented by [`ARef<Device>`], which keeps
//! the embedded `struct device` reference count balanced.
//!
//! C header: [`include/linux/greybus/hd.h`](srctree/include/linux/greybus/hd.h)
//!
//! # Examples
//!
//! ```ignore
//! use kernel::{device, prelude::*};
//!
//! struct MyHd {
//!     // transport state
//! }
//!
//! #[vtable]
//! impl HdDriver for MyHd {
//!     fn message_send(data: &Self, dest_cport_id: u16, msg: Message) -> Result {
//!         // Copy what is needed out of `msg`, queue it, and return without sleeping.
//!         // Call `HostDevice::message_sent()` once the core may release the message.
//!         Ok(())
//!     }
//!
//!     fn message_cancel(_msg: Message) {}
//! }
//!
//! fn probe(parent: &device::Device) -> Result<Registration<MyHd>> {
//!     Registration::new(parent, BUFFER_SIZE_MAX, NUM_CPORTS, try_pin_init!(MyHd {}))
//! }
//! ```

use core::ptr::addr_of_mut;
use core::{marker::PhantomData, ptr::NonNull};

use kernel::{device, prelude::*};

use crate::error::{code, from_err_ptr, to_result};
use crate::greybus::Connection;
use crate::sync::aref::{ARef, AlwaysRefCounted};
use crate::{greybus::Message, types::Opaque};

/// The set of callbacks a Greybus host driver can provide.
#[vtable]
pub trait HdDriver: Send + Sync + Sized + 'static {
    /// Transmits `msg` to `dest_cport_id`.
    ///
    /// This may be called in atomic context and therefore must not sleep; queue the message and
    /// return. Once the core is allowed to release the message, call
    /// [`HostDevice::message_sent`].
    ///
    /// The message is only borrowed for the duration of this call: copy what is needed out of it,
    /// do not stash the reference.
    fn message_send(data: &Self, dest_cport_id: u16, msg: Message) -> Result;

    /// Aborts the transmission of a message previously handed to [`HdDriver::message_send`].
    ///
    /// Always called in process context.
    fn message_cancel(msg: Message);
}

/// Builds the C callback table for a [`HdDriver`] implementation.
struct HdDriverVTable<T: HdDriver>(PhantomData<T>);

impl<T: HdDriver> HdDriverVTable<T> {
    const DRIVER: bindings::gb_hd_driver = bindings::gb_hd_driver {
        // Both are mandatory, hence no `HAS_*` check. See [`HdDriver`].
        message_send: Some(Self::message_send),
        message_cancel: Some(Self::message_cancel),
        // Every other callback is left `NULL`, which the Greybus core takes as "use the default
        // behaviour".
        ..pin_init::zeroed()
    };

    const fn build() -> &'static bindings::gb_hd_driver {
        &Self::DRIVER
    }

    /// # Safety
    ///
    /// `hd` must point at a registered host device whose private area holds a pointer to a live
    /// `T`, and `msg` must point at a valid message.
    unsafe extern "C" fn message_send(
        hd: *mut bindings::gb_host_device,
        dest_cport_id: u16,
        msg: *mut bindings::gb_message,
        _gfp_mask: bindings::gfp_t,
    ) -> c_int {
        // SAFETY: `gb_host_device` and `HostDevice` have the same layout.
        let hd = unsafe { Device::<device::CoreInternal<'_>>::from_raw(hd) };
        // SAFETY: `message_send` is only ever called after a successful call to
        // `gb_hd_add`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { hd.as_ref().drvdata_borrow() };
        // SAFETY: The caller guarantees `msg` is valid for the duration of this call.
        let msg = unsafe { Message::from_raw(msg) };

        match T::message_send(&data, dest_cport_id, msg) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `msg` must point at a valid message of a registered host device of this driver.
    unsafe extern "C" fn message_cancel(msg: *mut bindings::gb_message) {
        // SAFETY: The caller guarantees `msg` is valid for the duration of this call.
        let msg = unsafe { Message::from_raw(msg) };

        T::message_cancel(msg);
    }
}

/// A Greybus host device.
///
/// # Invariants
///
/// The wrapped value is a valid `struct gb_host_device` created by `gb_hd_create()`, and every
/// [`ARef<HostDevice>`] owns an increment on its reference count.
#[repr(transparent)]
pub struct Device<Ctx = device::Normal> {
    ptr: Opaque<bindings::gb_host_device>,
    _ctx: PhantomData<Ctx>,
}

// SAFETY: `gb_host_device` is reference counted through its embedded `struct device`, which may be
// used from any thread.
unsafe impl<Ctx> Send for Device<Ctx> {}

// SAFETY: `gb_host_device` has its own internal locking, so it is safe to share references to it
// across threads.
unsafe impl<Ctx> Sync for Device<Ctx> {}

// SAFETY: The embedded `struct device` carries the reference count, and `gb_hd_put()` is just
// `put_device()` on it, so the object stays alive for as long as increments are outstanding.
unsafe impl<Ctx> AlwaysRefCounted for Device<Ctx> {
    #[inline]
    fn inc_ref(&self) {
        // SAFETY: By the type invariant there is a live reference to the host device, and `dev` is
        // its embedded `struct device`.
        unsafe { bindings::get_device(&raw mut (*self.ptr.get()).dev) };
    }

    #[inline]
    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The caller guarantees it owns an increment on the reference count.
        unsafe { bindings::gb_hd_put(obj.as_ptr().cast()) }
    }
}

impl<Ctx> Device<Ctx> {
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a `struct gb_host_device`.
    #[inline]
    pub(crate) const unsafe fn from_raw<'a>(ptr: *mut bindings::gb_host_device) -> &'a Self {
        // SAFETY: `Device` is a transparent wrapper of `Opaque<bindings::gb_host_device>`.
        unsafe { &*ptr.cast() }
    }

    #[inline]
    pub(crate) fn as_raw(&self) -> *mut bindings::gb_host_device {
        self.ptr.get()
    }

    /// Hands a message received on `hd_cport_id` to the Greybus core.
    #[inline]
    pub fn data_rcvd(&self, cport_id: u16, msg: &[u8]) {
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct gb_host_device`. `msg` is valid for reads of ``msg.len()` bytes for the duration
        // of the call, and the core only reads through the pointer — it copies the payload into
        // the operation before returning — so handing it a `*mut` derived from a shared reference
        // is sound.
        //
        // TODO: The C signature of this function should be changed to take const pointer for msg.
        unsafe {
            bindings::greybus_data_rcvd(self.as_raw(), cport_id, msg.as_ptr().cast_mut(), msg.len())
        }
    }

    /// Looks up the connection bound to `cport` on interface `id`.
    pub fn find_connection_by_intf(&self, id: u8, cport: u16) -> Option<ARef<Connection>> {
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct gb_host_device`.
        let ptr = NonNull::new(unsafe {
            bindings::gb_connection_hd_find_by_intf(self.as_raw(), id, cport)
        })?;

        // SAFETY: ptr is a valid gb_connection
        Some(unsafe { ARef::from_raw(ptr.cast()) })
    }
}

impl<Ctx: device::DeviceContext> AsRef<device::Device<Ctx>> for Device<Ctx> {
    #[inline]
    fn as_ref(&self) -> &device::Device<Ctx> {
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct gb_host_device`. `dev` points to a valid `struct device`.
        unsafe { device::Device::from_raw(addr_of_mut!((*self.as_raw()).dev)) }
    }
}

/// A host device owned by its driver, together with the driver's private data `T`.
///
/// Created and added to the Greybus bus on construction, removed on drop.
///
/// # Invariants
///
/// `ptr` points at a valid `struct gb_host_device` obtained from `gb_hd_create()`, whose driver
/// data holds a live `T`
#[repr(transparent)]
pub struct Registration<T> {
    ptr: NonNull<bindings::gb_host_device>,
    _data: PhantomData<T>,
}

impl<T: HdDriver> Registration<T> {
    /// Creates a host device under `parent` and adds it to the Greybus bus.
    ///
    /// `buffer_size_max` is the largest message the transport can carry in one go, header
    /// included. `num_cports` is the number of cports that the greybus host device can connect to.
    ///
    /// `data` is initialised in place before the device is added, so callbacks may run against it
    /// from the moment `gb_hd_add()` succeeds.
    pub fn new(
        parent: &device::Device,
        buffer_size_max: usize,
        num_cports: usize,
        data: impl PinInit<T, Error>,
    ) -> Result<Self> {
        // SAFETY: `parent` is a valid device, and the driver table is `'static`. The core only
        // ever reads through the driver pointer, so casting away `const` is fine.
        let hd = from_err_ptr(unsafe {
            bindings::gb_hd_create(
                core::ptr::from_ref(HdDriverVTable::<T>::build()).cast_mut(),
                parent.as_raw(),
                buffer_size_max,
                num_cports,
            )
        })?;

        // SAFETY: `gb_host_device` and `HostDevice` have the same layout.
        let hd_dev = unsafe { &*hd.cast::<Device<device::CoreInternal<'_>>>() };
        hd_dev.as_ref().set_drvdata(data)?;

        let res = Self {
            ptr: NonNull::new(hd).ok_or(code::ENOMEM)?,
            _data: PhantomData,
        };

        res.add()?;

        Ok(res)
    }
}

impl<T> Registration<T> {
    fn add(&self) -> Result<()> {
        // SAFETY: By the type invariant the host device is valid, and it has not been added yet.
        to_result(unsafe { bindings::gb_hd_add(self.as_raw()) })
    }

    fn as_raw(&self) -> *mut bindings::gb_host_device {
        self.ptr.as_ptr()
    }
}

impl<T> AsRef<Device> for Registration<T> {
    #[inline]
    fn as_ref(&self) -> &Device {
        // SAFETY: By the type invariant the host device is valid.
        unsafe { Device::from_raw(self.as_raw()) }
    }
}

impl<T> Drop for Registration<T> {
    fn drop<'a>(&'a mut self) {
        {
            let hd = self.as_raw();
            // SAFETY: By the type invariant `hd` points at a valid host device, and
            // `gb_host_device` and `Device` have the same layout.
            let hd_dev = unsafe { &*hd.cast::<Device<device::CoreInternal<'a>>>() };
            // SAFETY: The driver data was set to a `T` in `Registration::new()` and has not been
            // taken since, and this is the only place that takes it.
            let data = unsafe { hd_dev.as_ref().drvdata_obtain::<T>() };
            drop(data);
        }

        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct gb_host_device`.
        unsafe { bindings::gb_hd_del(self.as_raw()) }
    }
}

// SAFETY: The greybus host device API is thread-safe as guaranteed by the device core, as long as
// gb_hd_del() is guaranteed to only be called once - which is guaranteed by our type not
// having Copy/Clone.
unsafe impl<T> Send for Registration<T> {}

// SAFETY: The greybus device API is thread-safe as guaranteed by the device core, as long as
// gb_hd_del() is guaranteed to only be called once - which is guaranteed by our type not
// having Copy/Clone.
unsafe impl<T> Sync for Registration<T> {}
