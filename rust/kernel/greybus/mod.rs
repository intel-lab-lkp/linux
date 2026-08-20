// SPDX-License-Identifier: GPL-2.0

//! Abstractions for the Greybus core.
//!
//! C header: [`include/linux/greybus.h`](srctree/include/linux/greybus.h)
//!
//! Greybus host drivers ("host devices") bridge the Greybus core to whatever transport actually
//! carries the traffic. A host driver creates a [`HostDevice`], registers it, and implements
//! [`HdDriver`] to transmit the messages the core hands it.

use core::ptr::NonNull;

use kernel::sync::aref::AlwaysRefCounted;
use kernel::transmute::FromBytes;
use kernel::types::Opaque;

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
pub struct Message(NonNull<bindings::gb_message>);

impl Message {
    /// # Safety
    ///
    /// `ptr` must be non-null and point at a valid `struct gb_message` which outlives the
    /// returned `Self`.
    #[inline]
    pub(crate) const unsafe fn from_raw(ptr: *mut bindings::gb_message) -> Self {
        // SAFETY: The caller guarantees `ptr` is non-null.
        Self(unsafe { NonNull::new_unchecked(ptr) })
    }

    /// Returns the operation header at the start of the message.
    #[inline]
    pub const fn header(&self) -> &protocols::GbOperationMsgHdr {
        // SAFETY: By the type invariant the message is valid, and so is its header.
        let msg = unsafe { &*self.0.as_ptr() };
        // SAFETY: `header` points at a valid `gb_operation_msg_hdr` for as long as the message is
        // alive, and `GbOperationMsgHdr` is a transparent wrapper of it.
        unsafe { &*msg.header.cast() }
    }

    /// Tells the Greybus core the transport is done with this message.
    ///
    /// `status` is `0` on success or a negative errno describing the transmit failure.
    #[inline]
    pub fn sent(self, status: i32) {
        // SAFETY: By the type invariant the message is valid and still owned by the transport,
        // and its connection and host device are alive for as long as it is.
        unsafe {
            bindings::greybus_message_sent(
                self.operation().connection().host_device().as_raw(),
                self.0.as_ptr(),
                status,
            );
        }
    }

    /// Returns the payload, i.e. the message without its operation header.
    pub const fn payload_bytes(&self) -> &[u8] {
        // SAFETY: By the type invariant the message is valid.
        let msg = unsafe { &*self.0.as_ptr() };

        if msg.payload.is_null() || msg.payload_size == 0 {
            return &[];
        }

        // SAFETY: A non-null `payload` points at `payload_size` initialized bytes.
        unsafe { core::slice::from_raw_parts(msg.payload.cast::<u8>(), msg.payload_size) }
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
        unsafe { Operation::from_raw((*self.0.as_ptr()).operation) }
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
        // as long as the operation is.
        unsafe { Connection::from_raw((*self.0.get()).connection) }
    }
}

/// A Greybus connection.
///
/// # Invariants
///
/// The wrapped value is a valid `struct gb_connection`.
#[repr(transparent)]
pub struct Connection(Opaque<bindings::gb_connection>);

// SAFETY: `gb_connection_put()` drops the reference acquired by `gb_connection_get()`, so the
// connection stays alive for as long as increments are outstanding.
unsafe impl AlwaysRefCounted for Connection {
    #[inline]
    fn inc_ref(&self) {
        // SAFETY: By the type invariant there is a live reference to the connection.
        unsafe { bindings::gb_connection_get(self.0.get()) }
    }

    #[inline]
    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The caller guarantees it owns an increment on the reference count.
        unsafe { bindings::gb_connection_put(obj.as_ptr().cast()) }
    }
}

impl Connection {
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a `struct gb_connection`.
    #[inline]
    pub(crate) const unsafe fn from_raw<'a>(ptr: *mut bindings::gb_connection) -> &'a Self {
        // SAFETY: `Connection` is a transparent wrapper of `Opaque<bindings::gb_connection>`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the interface at the far end of the connection.  In cases like SVC connection,
    /// interface can be NULL.
    #[inline]
    pub const fn interface(&self) -> Option<&Interface> {
        // SAFETY: By the type invariant the connection is valid.
        let intf_ptr = unsafe { (*self.0.get()).intf };

        if intf_ptr.is_null() {
            None
        } else {
            // SAFETY: By the previous check,  intf_ptr is valid.
            Some(unsafe { Interface::from_raw(intf_ptr) })
        }
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

    /// Returns the host device this connection belongs to.
    #[inline]
    pub const fn host_device(&self) -> &hd::Device {
        // SAFETY: By the type invariant the connection is valid, and its `hd` is alive for as
        // long as the connection is.
        unsafe { hd::Device::from_raw((*self.0.get()).hd) }
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
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to a `struct gb_interface`.
    #[inline]
    pub(crate) const unsafe fn from_raw<'a>(ptr: *mut bindings::gb_interface) -> &'a Self {
        // SAFETY: `Interface` is a transparent wrapper of `Opaque<bindings::gb_interface>`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the interface id, unique within its host device.
    #[inline]
    pub const fn id(&self) -> u8 {
        // SAFETY: By the type invariant the interface is valid.
        unsafe { (*self.0.get()).interface_id }
    }
}
