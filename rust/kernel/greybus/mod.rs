// SPDX-License-Identifier: GPL-2.0

//! Abstractions for the Greybus core.
//!
//! C header: [`include/linux/greybus.h`](srctree/include/linux/greybus.h)
//!
//! Greybus host drivers ("host devices") bridge the Greybus core to whatever transport actually
//! carries the traffic. A host driver creates a [`HostDevice`], registers it, and implements
//! [`HdDriver`] to transmit the messages the core hands it.

use core::ptr::NonNull;

use crate::{
    sync::aref::AlwaysRefCounted,
    transmute::FromBytes,
    types::Opaque, //
};

pub mod hd;
pub mod protocols;

/// The largest Greybus message, header included.
///
/// Bounded by the 16-bit `size` field in the operation header.
pub const GB_OPERATION_SIZE_MAX: usize = u16::MAX as usize;

/// The largest valid CPort id.
///
/// Ids above this are reserved by the protocol; a host device's `num_cports` cannot exceed
/// `CPORT_ID_MAX + 1`.
pub const CPORT_ID_MAX: usize = bindings::CPORT_ID_MAX as usize;

/// A Greybus message handed to a host driver for transmission.
///
/// # Invariants
///
/// The shared reference is only ever handed out for the duration of a [`HdDriver`] callback, during
/// which the Greybus core guarantees the message and its buffer stay alive.
#[repr(transparent)]
pub struct Message(Opaque<bindings::gb_message>);

impl Message {
    /// # Safety
    ///
    /// `ptr` must be non-null and point at a valid `struct gb_message` which outlives the
    /// returned `Self`.
    #[inline]
    pub(crate) const unsafe fn from_raw<'a>(ptr: *mut bindings::gb_message) -> &'a Self {
        // SAFETY: `Message` is a transparent wrapper of `Opaque<bindings::gb_message>`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the operation header at the start of the message.
    #[inline]
    pub const fn header(&self) -> &protocols::GbOperationMsgHdr {
        // SAFETY: `header` points at a valid `gb_operation_msg_hdr` for as long as the message is
        // alive, and `GbOperationMsgHdr` is a transparent wrapper of it.
        unsafe { &*(*self.0.get()).header.cast() }
    }

    /// # Safety
    ///
    /// `Message` is not valid after this call.
    #[inline]
    pub(crate) unsafe fn sent<Ctx>(&self, hd: &hd::Device<Ctx>, status: i32) {
        // SAFETY: By the type invariant the message is valid and still owned by the transport,
        // and its connection and host device are alive for as long as it is.
        unsafe {
            bindings::greybus_message_sent(hd.as_raw(), self.0.get(), status);
        }
    }

    /// Returns the payload, i.e. the message without its operation header.
    #[inline]
    pub const fn payload_bytes(&self) -> &[u8] {
        // SAFETY: By the type invariant the message is valid.
        let payload = unsafe { (*self.0.get()).payload };
        // SAFETY: By the type invariant the message is valid.
        let payload_size = unsafe { (*self.0.get()).payload_size };

        if payload.is_null() || payload_size == 0 {
            return &[];
        }

        // SAFETY: A non-null `payload` points at `payload_size` initialized bytes.
        unsafe { core::slice::from_raw_parts(payload.cast::<u8>(), payload_size) }
    }

    /// Interprets the message payload as a `T`.
    ///
    /// Returns `None` if the payload is too short or misaligned for `T`.
    #[inline]
    pub fn payload<T: FromBytes>(&self) -> Option<&T> {
        T::from_bytes(self.payload_bytes())
    }

    /// Returns the operation this message belongs to.
    #[inline]
    pub const fn operation(&self) -> &Operation {
        // SAFETY: By the type invariant the message is valid, and its `operation` is set for as
        // long as the message is alive.
        unsafe { Operation::from_raw((*self.0.get()).operation) }
    }
}

/// A Greybus operation.
///
/// # Invariants
///
/// The wrapped value is a valid `struct gb_operation`.
#[repr(transparent)]
pub struct Operation(Opaque<bindings::gb_operation>);

impl Operation {
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a `struct gb_operation`.
    #[inline]
    pub(crate) const unsafe fn from_raw<'a>(ptr: *mut bindings::gb_operation) -> &'a Self {
        // SAFETY: `Operation` is a transparent wrapper of `Opaque<bindings::gb_operation>`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the connection this operation travels on.
    #[inline]
    pub const fn connection(&self) -> &Connection {
        // SAFETY: By the type invariant the operation is valid, and its `connection` is alive for
        // as long as the operation is. The returned borrow is tied to `&self`, so it cannot
        // outlive the host-device callback that handed out the operation, as `Connection`'s
        // invariant requires.
        unsafe { Connection::from_raw((*self.0.get()).connection) }
    }
}

/// A Greybus connection.
///
/// # Invariants
///
/// The wrapped value is a valid `struct gb_connection`.
///
/// References to a `Connection` are only handed out for the duration of a host-device callback on
/// a message travelling over it. During that time the connection is enabled, and its interface,
/// if any, is alive: the core disables and destroys a bundle's connections before it tears down
/// their interface.
#[repr(transparent)]
pub struct Connection(Opaque<bindings::gb_connection>);

impl Connection {
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a `struct gb_connection`.
    #[inline]
    pub(crate) const unsafe fn from_raw<'a>(ptr: *mut bindings::gb_connection) -> &'a Self {
        // SAFETY: `Connection` is a transparent wrapper of `Opaque<bindings::gb_connection>`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the interface at the far end of the connection. In cases like SVC connection,
    /// interface can be NULL.
    #[inline]
    pub fn interface(&self) -> Option<&Interface> {
        // SAFETY: By the type invariant the connection is valid.
        let intf_ptr = NonNull::new(unsafe { (*self.0.get()).intf })?;

        // SAFETY: `intf_ptr` is non-NULL, and by the type invariant the interface is alive for as
        // long as `&self` is. `Interface` is a transparent wrapper of
        // `Opaque<bindings::gb_interface>`.
        Some(unsafe { intf_ptr.cast().as_ref() })
    }

    /// Returns the CPort id this connection uses on the interface.
    #[inline]
    pub const fn intf_cport_id(&self) -> u16 {
        // SAFETY: By the type invariant the connection is valid.
        unsafe { (*self.0.get()).intf_cport_id }
    }

    /// Returns the CPort id this connection uses on the host device.
    #[inline]
    pub const fn hd_cport_id(&self) -> u16 {
        // SAFETY: By the type invariant the connection is valid.
        unsafe { (*self.0.get()).hd_cport_id }
    }
}

/// A Greybus interface.
///
/// # Invariants
///
/// The wrapped value is a valid `struct gb_interface`.
#[repr(transparent)]
pub struct Interface(Opaque<bindings::gb_interface>);

impl Interface {
    /// Returns the interface id, unique within its host device.
    #[inline]
    pub const fn id(&self) -> u8 {
        // SAFETY: By the type invariant the interface is valid.
        unsafe { (*self.0.get()).interface_id }
    }
}

// SAFETY: The embedded `struct device` carries the reference count, which `get_device()` and
// `put_device()` manipulate, so the object stays alive for as long as increments are outstanding.
unsafe impl AlwaysRefCounted for Interface {
    #[inline]
    fn inc_ref(&self) {
        // SAFETY: By the type invariant there is a live reference to the interface, and `dev` is
        // its embedded `struct device`.
        unsafe { bindings::get_device(&raw mut (*self.0.get()).dev) };
    }

    #[inline]
    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The caller guarantees it owns an increment on the reference count.
        unsafe {
            bindings::put_device(&raw mut (*obj.as_ptr().cast::<bindings::gb_interface>()).dev)
        }
    }
}
