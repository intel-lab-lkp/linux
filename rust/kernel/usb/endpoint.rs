// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (C) 2026 Wyliodrin SRL.

//! USB endpoints.
//!
//! C header: [`include/linux/usb.h`](srctree/include/linux/usb.h)
//!
//! An [`HostEndpoint`] is a borrowed view of a `struct usb_host_endpoint` - one of the addressable
//! sources or sinks of data on a USB device. Its accessors read the endpoint descriptor the device
//! reported: [`number()`](HostEndpoint::number) and [`address()`](HostEndpoint::address),
//! [`direction()`](HostEndpoint::direction) and [`transfer_type()`](HostEndpoint::transfer_type),
//! and the packet and interval geometry needed to size and schedule transfers.
//!
//! # Where endpoints come from
//!
//! Endpoints declared by an interface are borrowed from the alternate setting that describes them,
//! via [`AlternateSetting::endpoints()`](crate::usb::AlternateSetting::endpoints). The
//! default control endpoint - endpoint 0 - is not among them: no interface descriptor lists
//! it, because the specification excludes it from `bNumEndpoints` and devices never send a
//! descriptor for it. It is reached through [`Device::control_endpoint()`] instead.
//!
//! # Type-state
//!
//! [`HostEndpoint`] carries two marker type parameters recording what is *statically* known
//! about it. Endpoints start out fully generic, and the `as_*` accessors check the
//! descriptor once and hand back a reference that remembers the answer:
//!
//! ```text
//!                              Endpoint<Generic, Generic>
//!                                 |        |                   |
//!                       as_in() <--        v as_control()      --> as_out()
//!      Endpoint<In, Generic>   Endpoint<Bidirectional, Control>   Endpoint<Out, Generic>
//! ```
//!
//! A function taking `&Endpoint<In, Bulk>` therefore cannot be handed anything but a bulk IN
//! endpoint, and needs no run-time check of its own. The markers are [`PhantomData`], so this
//! costs nothing at run time and a typed endpoint has the same layout as an untyped one.
//!
//! # Direction and control endpoints
//!
//! The two axes are not independent. A control endpoint is bidirectional: the direction of a
//! control transfer comes from the setup packet's `bmRequestType`, and USB 2.0
//! section 9.6.6 correspondingly defines bit 7 of `bEndpointAddress` as ignored
//! for control endpoints. So the direction of a control endpoint is
//! not merely unknown - it does not exist.
//!
//! That is what [`Bidirectional`] marks. Because [`as_in()`](HostEndpoint::as_in) and
//! [`as_out()`](HostEndpoint::as_out) are defined only for [`Generic`],
//! asking a control endpoint which direction it runs in is a compile error rather than
//! an answer that would mislead whichever way it came out. See [`control`](crate::usb::control)
//! for issuing transfers on one.
//!
//! # Examples
//!
//! ```
//! use kernel::usb::{
//!     endpoint::{HostEndpoint, In, Out},
//!     AlternateSetting,
//! };
//!
//! /// Picks out the first IN and OUT endpoints of an alternate setting.
//! fn pair(alt: &AlternateSetting) -> (Option<&Endpoint<In>>, Option<&Endpoint<Out>>) {
//!     let eps = alt.endpoints();
//!
//!     (
//!         eps.iter().find_map(ep.as_in),
//!         eps.iter().find_map(ep.as_out),
//!     )
//! }
//! ```

use core::marker::PhantomData;

use crate::{device, types::Opaque, usb::Device};

/// A single endpoint of an [`AlternateSetting`](crate::usb::AlternateSetting).
///
/// `Dir` and `Type` are compile-time markers recording what is statically known about the
/// endpoint's direction and transfer type. An endpoint obtained from an
/// [`AlternateSetting`](crate::usb::AlternateSetting) starts out as `Endpoint<Generic, Generic>`,
/// i.e. nothing is known about it yet. The [`as_in()`], [`as_out()`] and [`as_control()`]
/// accessors inspect the endpoint descriptor at run time and, on success, hand back a reference
/// carrying the corresponding marker. Code that accepts only an `&Endpoint<In, Bulk>` may then
/// rely on the endpoint really being a bulk IN endpoint without re-checking it.
///
/// Direction and transfer type are not independent: a control endpoint has no direction, so
/// [`as_control()`] yields [`Bidirectional`] rather than preserving or discovering an IN/OUT
/// marker, and [`as_in()`]/[`as_out()`] are not defined on the result.
///
/// The markers are [`PhantomData`], so a typed `HostEndpoint` has the same layout as
/// an untyped one and the refinement is free at run time.
///
/// # Invariants
///
/// - The wrapped [`Opaque`] holds an initialised `struct usb_host_endpoint`. Instances are never
///   constructed by Rust code; they are only ever borrowed out of a `struct usb_host_interface`
///   owned by the C side, which guarantees that `desc` is initialised and that the endpoint
///   outlives the reference.
/// - If `Type` is [`Control`], [`Isochronous`], [`Bulk`] or [`Interrupt`], then
///   `desc.bmAttributes` really encodes that transfer type.
/// - If `Dir` is [`In`] or [`Out`], then bit 7 of `desc.bEndpointAddress` really encodes that
///   direction. If `Dir` is [`Bidirectional`], then `Type` is [`Control`] and the direction bit
///   carries no meaning at all.
/// - [`Generic`] asserts nothing in either position.
///
/// [`as_in()`]: HostEndpoint::as_in
/// [`as_out()`]: HostEndpoint::as_out
/// [`as_control()`]: HostEndpoint::as_control
#[repr(transparent)]
pub struct HostEndpoint<Dir: EndpointDirection = Generic, Type: EndpointTransferType = Generic>(
    Opaque<bindings::usb_host_endpoint>,
    PhantomData<Dir>,
    PhantomData<Type>,
);

/// A marker usable in the `Dir` position of [`HostEndpoint`].
///
/// This trait is sealed: it is implemented by [`Generic`], [`In`], [`Out`] and [`Bidirectional`]
/// only, and cannot be implemented outside of this module.
pub trait EndpointDirection: private::Sealed {}

/// A marker usable in the `Type` position of [`HostEndpoint`].
///
/// This trait is sealed: it is implemented by [`Generic`], [`Control`], [`Isochronous`],
/// [`Bulk`] and [`Interrupt`] only, and cannot be implemented outside of this module.
pub trait EndpointTransferType: private::Sealed {}

/// Marker for an [`HostEndpoint`] whose direction or transfer type is not statically known.
///
/// This is the default in both marker positions. It carries no guarantee, so the property has
/// to be queried at run time with [`HostEndpoint::direction()`] or
/// [`HostEndpoint::transfer_type()`], or established once and for all with one of
/// the `as_*` accessors.
pub struct Generic;

/// Marker for a device-to-host ("IN") [`HostEndpoint`].
pub struct In;

/// Marker for a host-to-device ("OUT") [`HostEndpoint`].
pub struct Out;

/// Marker for an [`HostEndpoint`] that carries data in both directions.
///
/// This is the direction marker of every control endpoint, and the only marker they get. A control
/// transfer takes the direction of its data stage from bit 7 of the setup
/// packet's `bmRequestType`, and USB 2.0 section 9.6.6 correspondingly defines bit 7
/// of `bEndpointAddress` as ignored for control endpoints - so "is this endpoint IN or OUT"
/// has no answer for one.
///
/// Since [`HostEndpoint::as_in()`] and [`HostEndpoint::as_out()`] are defined only
/// for [`Generic`], asking that question of a [`Bidirectional`] endpoint fails to compile rather
/// than returning an answer that would be misleading either way.
pub struct Bidirectional;

/// Marker for an [`HostEndpoint`] using control transfers.
pub struct Control;

/// Marker for an [`HostEndpoint`] using isochronous transfers.
pub struct Isochronous;

/// Marker for an [`HostEndpoint`] using bulk transfers.
pub struct Bulk;

/// Marker for an [`HostEndpoint`] using interrupt transfers.
pub struct Interrupt;

mod private {
    /// Prevents [`EndpointDirection`] and [`EndpointTransferType`] from being implemented
    /// outside of this module, so that the type invariants of [`HostEndpoint`] cannot be forged by
    /// downstream code.
    ///
    /// [`EndpointDirection`]: super::EndpointDirection
    /// [`EndpointTransferType`]: super::EndpointTransferType
    /// [`HostEndpoint`]: super::Endpoint
    pub trait Sealed {}

    impl Sealed for super::Generic {}
    impl Sealed for super::In {}
    impl Sealed for super::Out {}
    impl Sealed for super::Bidirectional {}
    impl Sealed for super::Control {}
    impl Sealed for super::Isochronous {}
    impl Sealed for super::Bulk {}
    impl Sealed for super::Interrupt {}
}

impl EndpointDirection for Generic {}
impl EndpointTransferType for Generic {}
impl EndpointDirection for In {}
impl EndpointDirection for Out {}
impl EndpointDirection for Bidirectional {}
impl EndpointTransferType for Control {}
impl EndpointTransferType for Isochronous {}
impl EndpointTransferType for Bulk {}
impl EndpointTransferType for Interrupt {}

/// The direction in which data flows over an [`HostEndpoint`].
///
/// The direction is fixed by the endpoint descriptor and is stated from the host's point of
/// view. Control endpoints are bidirectional; for those the descriptor's direction bit is
/// meaningless and this enum should be ignored.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum Direction {
    /// Data flows from the device to the host (`USB_DIR_IN`).
    In,
    /// Data flows from the host to the device (`USB_DIR_OUT`).
    Out,
}

impl From<u8> for Direction {
    /// Extracts the direction from a raw `bEndpointAddress`.
    ///
    /// Bit 7 of the endpoint address is the direction bit: set means IN, clear means OUT. All
    /// other bits are ignored, so any `u8` is a valid input.
    fn from(value: u8) -> Self {
        if (value >> 7) & 0b1 == 1 {
            Direction::In
        } else {
            Direction::Out
        }
    }
}

/// The transfer type an [`HostEndpoint`] uses.
///
/// Every endpoint is fixed to exactly one of these by its descriptor; see USB 2.0 section 5.4
/// for what each one guarantees.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum TransferType {
    /// Bidirectional, request/response transfers with guaranteed delivery. Used for device
    /// configuration; endpoint 0 is always a control endpoint.
    Control,
    /// Transfers with guaranteed bandwidth and bounded latency but no error retry, for
    /// time-sensitive streams such as audio and video.
    Isochronous,
    /// Transfers with guaranteed delivery but no bandwidth or latency guarantee, for bulk data
    /// such as mass storage.
    Bulk,
    /// Small, periodically polled transfers with bounded latency and guaranteed delivery, for
    /// devices such as keyboards and mice.
    Interrupt,
}

impl From<u8> for TransferType {
    /// Extracts the transfer type from a raw `bmAttributes`.
    ///
    /// Bits 1:0 of the endpoint attributes hold the transfer type (`USB_ENDPOINT_XFERTYPE_MASK`)
    /// and all four encodings are defined, so any `u8` is a valid input. The remaining bits,
    /// which further describe isochronous endpoints, are ignored.
    fn from(value: u8) -> Self {
        match value & 0b11 {
            0 => TransferType::Control,
            1 => TransferType::Isochronous,
            2 => TransferType::Bulk,
            _ => TransferType::Interrupt,
        }
    }
}

impl<Dir: EndpointDirection, Type: EndpointTransferType> HostEndpoint<Dir, Type> {
    /// Returns a raw pointer to the underlying `struct usb_host_endpoint`.
    ///
    /// By the type invariants the pointer is non-null and points at an initialised endpoint for
    /// at least the lifetime of `&self`.
    fn as_raw(&self) -> *mut bindings::usb_host_endpoint {
        self.0.get()
    }

    /// Returns the endpoint number, i.e. bits 3:0 of `bEndpointAddress`.
    ///
    /// The number alone does not identify an endpoint: an IN and an OUT endpoint of the same
    /// interface may share one, so pair it with [`direction()`](Self::direction), or use
    /// [`address()`](Self::address), which combines the two.
    pub fn number(&self) -> u8 {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid
        // `struct usb_host_endpoint` with an initialised `desc`.
        unsafe { (*self.as_raw()).desc.bEndpointAddress & 0x0f }
    }

    /// Returns the full `bEndpointAddress` of the endpoint descriptor.
    ///
    /// The byte packs the endpoint number in bits 3:0 and the direction in bit 7 (set for IN,
    /// clear for OUT); bits 6:4 are reserved and zero. Taken together those fields uniquely
    /// identify the endpoint within its configuration, which is why this is the value host-side
    /// APIs use to name an endpoint - for example when constructing a URB pipe.
    ///
    /// For a control endpoint bit 7 is defined to be ignored, so the byte should not be read as a
    /// direction there; see [`Bidirectional`].
    ///
    /// Use [`number()`](Self::number) or [`direction()`](Self::direction) to get the fields
    /// individually.
    pub fn address(&self) -> u8 {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid
        // `struct usb_host_endpoint` with an initialised `desc`.
        unsafe { (*self.as_raw()).desc.bEndpointAddress }
    }

    /// Returns the direction data flows in over this endpoint.
    ///
    /// Meaningless for control endpoints, which are bidirectional; what it reports for one is
    /// whatever bit 7 of the address byte happens to hold, which the specification leaves
    /// undefined.
    pub fn direction(&self) -> Direction {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid
        // `struct usb_host_endpoint` with an initialised `desc`.
        unsafe { (*self.as_raw()).desc.bEndpointAddress.into() }
    }

    /// Returns the transfer type this endpoint uses.
    pub fn transfer_type(&self) -> TransferType {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid
        // `struct usb_host_endpoint` with an initialised `desc`.
        unsafe { (*self.as_raw()).desc.bmAttributes.into() }
    }

    /// Returns the maximum payload size of a single transaction, in bytes.
    ///
    /// This is bits 10:0 of `wMaxPacketSize`. The permitted values depend on the transfer type
    /// and the speed the device is operating at; for high-speed isochronous and interrupt
    /// endpoints the total payload per microframe is this value multiplied by the number of
    /// transactions per microframe, which [`max_packet_mult()`](Self::max_packet_mult)
    /// describes.
    pub fn max_packet_size(&self) -> u16 {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid
        // `struct usb_host_endpoint` with an initialised `desc`.
        unsafe { (*self.as_raw()).desc.wMaxPacketSize & 0x07ff }
    }

    /// Returns the number of *additional* transaction opportunities per microframe.
    ///
    /// This is bits 12:11 of `wMaxPacketSize`, so the total number of transactions per microframe
    /// is one more than the returned value. The field is only defined for high-speed isochronous
    /// and interrupt endpoints and must not be used for anything else.
    ///
    /// The specification defines the encodings `0..=2`; `3` is reserved, so a conforming device
    /// never reports it, but a malformed descriptor can and this returns it unchanged.
    pub fn max_packet_multipier(&self) -> u8 {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid
        // `struct usb_host_endpoint` with an initialised `desc`.
        unsafe { (((*self.as_raw()).desc.wMaxPacketSize >> 11) & 0b11) as u8 + 1 }
    }
}

impl HostEndpoint<Generic, Generic> {
    /// Refines this endpoint into a control endpoint, if it is one.
    ///
    /// Returns [`None`] if [`transfer_type()`](Self::transfer_type) is not
    /// [`TransferType::Control`].
    ///
    /// The result is [`Bidirectional`], not the direction the descriptor happens to record: bit 7
    /// of a control endpoint's address is defined to be ignored, so there is nothing to preserve.
    /// That is also why this is only available on a fully generic endpoint - refining direction
    /// first and transfer type second would otherwise produce an `Endpoint<In, Control>`, a
    /// combination that has no meaning.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::usb::{
    ///     Hostendpoint::{Bidirectional, Control, Endpoint},
    ///     AlternateSetting,
    /// };
    ///
    /// /// Finds an interface's own control endpoint, if it declares one.
    /// ///
    /// /// This never finds endpoint 0, which no interface descriptor lists; reach that one
    /// /// through `Device::control_endpoint()` instead.
    /// fn extra_control(alt: &AlternateSetting) -> Option<&Endpoint<Bidirectional, Control>> {
    ///     alt.endpoints().iter().find_map(|ep| ep.as_control())
    /// }
    /// ```
    pub fn as_control(&self) -> Option<&HostEndpoint<Bidirectional, Control>> {
        matches!(self.transfer_type(), TransferType::Control).then(|| {
            // SAFETY: The transfer type was just checked, so the [`Control`] invariant holds, and
            // [`Bidirectional`] is exactly what a control endpoint warrants.
            // `Endpoint<Bidirectional, Control>` is a `#[repr(transparent)]` wrapper around the
            // same `struct usb_host_endpoint` and differs only in its `PhantomData` markers, which
            // are 1-ZSTs, so the two types have identical layout and the reference stays valid for
            // the same lifetime.
            unsafe { &*core::ptr::from_ref(self).cast() }
        })
    }
}

impl<Type: EndpointTransferType> HostEndpoint<Generic, Type> {
    /// Refines this endpoint into a device-to-host endpoint, if it is one.
    ///
    /// Returns [`None`] if [`direction()`](Self::direction) is not [`Direction::In`]. The known
    /// transfer type, if any, is preserved.
    pub fn as_in(&self) -> Option<&HostEndpoint<In, Type>> {
        if self.direction() == Direction::In {
            // SAFETY: The direction was just checked, so the [`In`] invariant holds.
            // `Endpoint<In, Type>` is a `#[repr(transparent)]` wrapper around the same
            // `struct usb_host_endpoint` and differs only in its `PhantomData` markers, which
            // are 1-ZSTs, so the two types have identical layout and the reference stays valid
            // for the same lifetime.
            unsafe { Some(&*core::ptr::from_ref(self).cast()) }
        } else {
            None
        }
    }

    /// Refines this endpoint into a host-to-device endpoint, if it is one.
    ///
    /// Returns [`None`] if [`direction()`](Self::direction) is not [`Direction::Out`]. The known
    /// transfer type, if any, is preserved.
    pub fn as_out(&self) -> Option<&HostEndpoint<Out, Type>> {
        if self.direction() == Direction::Out {
            // SAFETY: The direction was just checked, so the [`Out`] invariant holds.
            // `Endpoint<Out, Type>` is a `#[repr(transparent)]` wrapper around the same
            // `struct usb_host_endpoint` and differs only in its `PhantomData` markers, which
            // are 1-ZSTs, so the two types have identical layout and the reference stays valid
            // for the same lifetime.
            unsafe { Some(&*core::ptr::from_ref(self).cast()) }
        } else {
            None
        }
    }
}

impl<Generic: EndpointDirection> HostEndpoint<Generic, Interrupt> {
    /// Returns the raw `bInterval` value of the endpoint descriptor.
    ///
    /// How this encodes a service interval depends on the transfer type and the device's
    /// operating speed:
    ///
    /// - Full-/low-speed interrupt endpoints: the interval in frames (1 ms), `1..=255`.
    /// - High-speed interrupt endpoints and all isochronous endpoints: an exponent, giving an
    ///   interval of `2^(bInterval - 1)` microframes (125 micros), with `bInterval` in `1..=16`.
    pub fn interval(&self) -> u8 {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid
        // `struct usb_host_endpoint` with an initialised `desc`.
        unsafe { (*self.as_raw()).desc.bInterval }
    }
}

impl HostEndpoint<Out, Control> {
    /// Returns the raw `bInterval` value of the endpoint descriptor.
    ///
    /// This applies only to high-speed bulk/control OUT endpoints and
    /// represents the maximum NAK rate, or zero for no limit.
    pub fn max_nak_rate(&self) -> u8 {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid
        // `struct usb_host_endpoint` with an initialised `desc`.
        unsafe { (*self.as_raw()).desc.bInterval }
    }
}

impl HostEndpoint<Out, Bulk> {
    /// Returns the raw `bInterval` value of the endpoint descriptor.
    ///
    /// This applies only to high-speed bulk/control OUT endpoints and
    /// represents the maximum NAK rate, or zero for no limit.
    pub fn max_nak_rate(&self) -> u8 {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid
        // `struct usb_host_endpoint` with an initialised `desc`.
        unsafe { (*self.as_raw()).desc.bInterval }
    }
}

impl<Ctx: device::DeviceContext> Device<Ctx> {
    /// Returns the default control endpoint (endpoint 0) of this device.
    ///
    /// Every USB device has exactly one, and it is the endpoint all enumeration and standard
    /// requests travel over. Unlike the endpoints of an
    /// [`AlternateSetting`](crate::usb::AlternateSetting), it is not described by any
    /// descriptor the device sends: the USB core synthesizes its descriptor
    /// in `usb_alloc_dev()` and fills in the packet size from `bMaxPacketSize0` of the device
    /// descriptor during enumeration. It therefore cannot be found by searching
    /// [`AlternateSetting::endpoints()`](crate::usb::AlternateSetting::endpoints), and
    /// this accessor is the only way to reach it.
    ///
    /// Neither marker needs a run-time check. [`Control`] holds because the core sets
    /// `bmAttributes` to `USB_ENDPOINT_XFER_CONTROL` itself and the specification fixes endpoint 0
    /// as a control endpoint; [`Bidirectional`] holds because control endpoints have no direction.
    /// Endpoint 0's address byte is `0x00`, so [`direction()`](HostEndpoint::direction)
    /// would report [`Direction::Out`] - a meaningless answer, which is why the direction
    /// refinements are not available on the returned type. Route control transfers
    /// on `bmRequestType` instead.
    ///
    /// No device-state bound is required: a `struct usb_device` has a valid `ep0` from allocation
    /// onwards, so this is available wherever a [`Device`] is.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::usb::Device;
    ///
    /// fn ep0_packet_size(dev: &Device) -> u16 {
    ///     dev.control_endpoint().max_packet_size()
    /// }
    /// ```
    pub fn control_endpoint(&self) -> &HostEndpoint<Bidirectional, Control> {
        // SAFETY: By the type invariants, `self.as_raw()` points at a valid `struct usb_device`.
        // `ep0` is an embedded field rather than a pointer, so it is live for as long as the
        // device is, and `usb_alloc_dev()` has initialised its descriptor.
        let ep0 = unsafe { core::ptr::addr_of!((*self.as_raw()).ep0) };

        // SAFETY: `HostEndpoint` is a `#[repr(transparent)]` wrapper around
        // `Opaque<bindings::usb_host_endpoint>`, which is layout-compatible with
        // `struct usb_host_endpoint`, so the cast preserves size and alignment. The [`Control`]
        // invariant holds because the core fixes `ep0.desc.bmAttributes` to
        // `USB_ENDPOINT_XFER_CONTROL`, and [`Bidirectional`] holds for any control endpoint. The
        // `Opaque` accounts for the C side mutating the endpoint behind this shared reference, and
        // the borrow lasts no longer than `&self`.
        unsafe { &*ep0.cast() }
    }
}
