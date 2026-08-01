// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (C) 2026 Wyliodrin SRL.

//! USB control transfers.
//!
//! Control transfers are the request/response mechanism every USB device must support. A transfer
//! consists of an 8-byte setup packet, an optional data stage, and a status stage;
//! the setup packet is described by [`Request`], and the direction of the data stage is chosen by
//! calling either [`Device::send_control_message()`] or [`Device::receive_control_message()`].
//!
//! Both travel over a control endpoint - usually the device's default one, from
//! [`Device::control_endpoint()`]. Because control endpoints are bidirectional, the endpoint does
//! not determine the direction: the `Direction` bit of `bmRequestType` does, and these two methods
//! set it so it cannot disagree with the buffer you passed.

use core::ptr;

use ffi::c_void;

use crate::{
    alloc::Flags,
    bindings,
    error::{code::EOVERFLOW, Error, Result},
    num::Bounded,
    usb::{
        endpoint::{Bidirectional, Control, Direction, HostEndpoint},
        Device,
    },
};

/// Position of the `Type` field within `bmRequestType` (bits 6:5).
const REQUEST_TYPE_SHIFT: u32 = 5;

/// Which part of the USB specification defines the meaning of a [`Request`].
///
/// This is the `Type` field of `bmRequestType`, occupying bits 6:5 of the setup packet's first
/// byte. It selects the namespace that [`Request::request`] is interpreted in, so the same request
/// code means different things under different types.
#[derive(Copy, Clone, PartialEq, PartialOrd)]
#[repr(u8)]
pub enum RequestType {
    /// Request is a USB standard request, defined by the specification itself and interpreted the
    /// same way by every device. Usually handled by the USB core rather than by a driver; see
    /// [`Device`].
    Standard = 0,
    /// Request is intended for a USB class.
    Class = 1,
    /// Request is vendor-specific.
    Vendor = 2,
    /// Reserved.
    Reserved = 3,
}

/// The target a [`Request`] is addressed to.
///
/// This is the `Recipient` field of `bmRequestType`, occupying bits 4:0 of the setup
/// packet's first byte. For [`INTERFACE`](Self::INTERFACE) and [`ENDPOINT`](Self::ENDPOINT)
/// the specific interface or endpoint is named by [`Request::index`]; the other recipients
/// ignore it or give it a request-specific meaning.
///
/// Although the field is five bits wide, only the values below are defined - hence
/// the bound on the wrapped [`Bounded`].
#[derive(Copy, Clone, PartialEq, PartialOrd)]
#[repr(transparent)]
pub struct Recipient(Bounded<u8, 5>);

impl Recipient {
    /// The device as a whole.
    pub const DEVICE: Self = Self(Bounded::<u8, 5>::new::<0u8>());
    /// A specific interface, named by [`Request::index`].
    pub const INTERFACE: Self = Self(Bounded::<u8, 5>::new::<1u8>());
    /// A specific endpoint, named by [`Request::index`].
    pub const ENDPOINT: Self = Self(Bounded::<u8, 5>::new::<2u8>());
    /// Some other target, defined by the request itself.
    pub const OTHER: Self = Self(Bounded::<u8, 5>::new::<3u8>());
    /// A port. Wireless USB only.
    pub const PORT: Self = Self(Bounded::<u8, 5>::new::<4u8>());
    /// An RPipe. Wireless USB only.
    pub const RPIPE: Self = Self(Bounded::<u8, 5>::new::<5u8>());

    /// Builds a recipient from a raw field value.
    ///
    /// Prefer the associated constants; this exists for values a future specification revision may
    /// define. The [`Bounded`] parameter rules out values the field cannot hold, but does not
    /// guarantee the device understands the one you pass.
    pub fn new(val: Bounded<u8, 5>) -> Recipient {
        Recipient(val)
    }
}

/// The setup packet of a control transfer, minus the direction and length.
///
/// These fields map onto the setup packet as `bmRequestType` (from
/// [`request_type`](Self::request_type) and [`recipient`](Self::recipient)), `bRequest`, `wValue`
/// and `wIndex`. The remaining two - the `Direction` bit of `bmRequestType` and `wLength` - are
/// filled in by [`Device::send_control_message()`] and [`Device::receive_control_message()`] from
/// the method you call and the buffer you hand it, which is what keeps them consistent with each
/// other.
pub struct Request {
    /// Type of the request.
    pub request_type: RequestType,
    /// Recipient of the request.
    pub recipient: Recipient,
    /// Request code. The meaning of the value depends on the previous fields.
    pub request: u8,
    /// Request value. The meaning of the value depends on the previous fields.
    pub value: u16,
    /// Request index. The meaning of the value depends on the previous fields.
    pub index: u16,
}

impl Request {
    /// Encodes this request into a `bmRequestType` byte for a data stage flowing in `direction`.
    ///
    /// `direction` here is a property of the transfer, not of the endpoint it runs over: bit 7 of
    /// `bmRequestType` is what the host controller obeys for a control transfer, and bit 7 of the
    /// endpoint address is defined to be ignored.
    fn bm_request_type(&self, direction: Direction) -> u8 {
        let dir = match direction {
            // `USB_DIR_IN` is `0x80`; `USB_DIR_OUT` is zero.
            Direction::In => bindings::USB_DIR_IN as u8,
            Direction::Out => 0,
        };

        dir | ((self.request_type as u8) << REQUEST_TYPE_SHIFT) | self.recipient.0.get()
    }
}

/// Converts a buffer length into a `wLength` value.
///
/// The field is 16 bits, so anything larger cannot be expressed in a single control transfer.
fn transfer_length(len: usize) -> Result<u16> {
    u16::try_from(len).map_err(|_| EOVERFLOW)
}

impl Device {
    /// Performs a control transfer with an outbound data stage, i.e. host to device.
    ///
    /// The `Direction` bit of `bmRequestType` is set to `USB_DIR_OUT` for you. Pass [`None`] as
    /// `data` for a request with no data stage at all, which sets `wLength` to zero.
    ///
    /// `endpoint` selects the control endpoint to use; it must belong to this device, since only
    /// its [`number()`](Endpoint::number) is taken from it. For the default control endpoint -
    /// almost always the right choice - pass [`control_endpoint()`](Device::control_endpoint).
    ///
    /// `timeout` is in milliseconds, with zero meaning "wait forever". `memflags` are used for an
    /// internal copy of `data`, so `data` itself need not be DMA-capable.
    ///
    /// Returns `Ok(())` only if the whole request completed; unlike `usb_control_msg()` there
    /// is no partial-success case to inspect.
    ///
    /// This sleeps, so it must not be called from atomic context.
    ///
    /// # Examples
    ///
    /// A vendor-specific request with no data stage:
    ///
    /// ```
    /// use kernel::{
    ///     alloc::flags::GFP_KERNEL,
    ///     error::Result,
    ///     usb::{
    ///         transfer::{Recipient, Request, RequestType},
    ///         Device,
    ///     },
    /// };
    ///
    /// fn set_led(dev: &Device, on: bool) -> Result {
    ///     dev.send_control_message(
    ///         dev.control_endpoint(),
    ///         Request {
    ///             request_type: RequestType::Vendor,
    ///             recipient: Recipient::DEVICE,
    ///             request: 0x01,
    ///             value: on as u16,
    ///             index: 0,
    ///         },
    ///         None,
    ///         1000,
    ///         GFP_KERNEL,
    ///     )
    /// }
    /// ```
    ///
    /// A class request that carries a payload:
    ///
    /// ```
    /// use kernel::{
    ///     alloc::flags::GFP_KERNEL,
    ///     error::Result,
    ///     usb::{
    ///         transfer::{Recipient, Request, RequestType},
    ///         Device,
    ///     },
    /// };
    ///
    /// fn set_line_coding(dev: &Device, interface: u8, coding: &[u8; 7]) -> Result {
    ///     dev.send_control_message(
    ///         dev.control_endpoint(),
    ///         Request {
    ///             request_type: RequestType::Class,
    ///             recipient: Recipient::INTERFACE,
    ///             request: 0x20,
    ///             value: 0,
    ///             index: interface as u16,
    ///         },
    ///         Some(coding),
    ///         1000,
    ///         GFP_KERNEL,
    ///     )
    /// }
    /// ```
    pub fn send_control_message(
        &self,
        endpoint: &HostEndpoint<Bidirectional, Control>,
        request: Request,
        data: Option<&[u8]>,
        timeout: i32,
        memflags: Flags,
    ) -> Result {
        let (data, size) = match data {
            Some(bytes) => (
                bytes.as_ptr().cast::<c_void>(),
                transfer_length(bytes.len())?,
            ),
            None => (ptr::null(), 0),
        };

        // SAFETY: `self.as_raw()` is a valid `struct usb_device` by the type invariants of
        // [`Device`], and `endpoint` belongs to it, so its number names a control endpoint of this
        // device. `data` is either null with `size` zero, or points at `size` initialised bytes
        // that outlive the call - the callee only reads from it, and copies before submitting, so
        // no DMA is performed on the caller's buffer.
        let ret = unsafe {
            bindings::usb_control_msg_send(
                self.as_raw(),
                endpoint.number(),
                request.request,
                request.bm_request_type(Direction::Out),
                request.value,
                request.index,
                data,
                size,
                timeout,
                memflags.as_raw(),
            )
        };

        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(())
        }
    }

    /// Performs a control transfer with an inbound data stage, i.e. device to host.
    ///
    /// The `Direction` bit of `bmRequestType` is set to `USB_DIR_IN` for you. Pass [`None`] as
    /// `data` for a request with no data stage at all, which sets `wLength` to zero - note that
    /// this makes the direction bit meaningless, so such a request is indistinguishable from the
    /// [`send_control_message()`](Device::send_control_message) equivalent.
    ///
    /// `endpoint` selects the control endpoint to use; it must belong to this device, since only
    /// its [`number()`](Endpoint::number) is taken from it. For the default control endpoint -
    /// almost always the right choice - pass [`control_endpoint()`](Device::control_endpoint).
    ///
    /// `timeout` is in milliseconds, with zero meaning "wait forever". `memflags` are used for an
    /// internal DMA-capable buffer that is copied into `data` on success, so `data`
    /// itself need not be DMA-capable.
    ///
    /// The transfer must fill `data` exactly. A device that returns fewer bytes than requested
    /// fails with `EREMOTEIO` and leaves `data` untouched, so this is not the right method for
    /// requests of variable-length descriptors - size the buffer from the device's own length
    /// field, or use a lower-level transfer.
    ///
    /// This sleeps, so it must not be called from atomic context.
    ///
    /// # Examples
    ///
    /// Reading a fixed-size vendor-specific register:
    ///
    /// ```
    /// use kernel::{
    ///     alloc::flags::GFP_KERNEL,
    ///     error::Result,
    ///     usb::{
    ///         transfer::{Recipient, Request, RequestType},
    ///         Device,
    ///     },
    /// };
    ///
    /// fn firmware_version(dev: &Device) -> Result<u16> {
    ///     let mut buf = [0u8; 2];
    ///
    ///     dev.receive_control_message(
    ///         dev.control_endpoint(),
    ///         Request {
    ///             request_type: RequestType::Vendor,
    ///             recipient: Recipient::DEVICE,
    ///             request: 0x02,
    ///             value: 0,
    ///             index: 0,
    ///         },
    ///         Some(&mut buf),
    ///         1000,
    ///         GFP_KERNEL,
    ///     )?;
    ///
    ///     Ok(u16::from_le_bytes(buf))
    /// }
    /// ```
    pub fn receive_control_message(
        &self,
        endpoint: &HostEndpoint<Bidirectional, Control>,
        request: Request,
        data: Option<&mut [u8]>,
        timeout: i32,
        memflags: Flags,
    ) -> Result {
        let (data, size) = match data {
            Some(bytes) => {
                let size = transfer_length(bytes.len())?;
                (bytes.as_mut_ptr().cast::<c_void>(), size)
            }
            None => (ptr::null_mut(), 0),
        };

        // SAFETY: `self.as_raw()` is a valid `struct usb_device` by the type invariants of
        // [`Device`], and `endpoint` belongs to it, so its number names a control endpoint of this
        // device. `data` is either null with `size` zero, or points at `size` bytes of a buffer
        // uniquely borrowed for the duration of the call, which the callee may only write to.
        let ret = unsafe {
            bindings::usb_control_msg_recv(
                self.as_raw(),
                endpoint.number(),
                request.request,
                request.bm_request_type(Direction::In),
                request.value,
                request.index,
                data,
                size,
                timeout,
                memflags.as_raw(),
            )
        };

        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(())
        }
    }
}
