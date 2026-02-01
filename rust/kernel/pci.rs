// SPDX-License-Identifier: GPL-2.0

//! Abstractions for the PCI bus.
//!
//! C header: [`include/linux/pci.h`](srctree/include/linux/pci.h)

use crate::{
    bindings,
    container_of,
    device,
    device_id::{
        RawDeviceId,
        RawDeviceIdIndex, //
    },
    driver,
    error::{
        from_result,
        to_result, //
    },
    prelude::*,
    str::CStr,
    types::Opaque,
    ThisModule, //
};
use core::{
    marker::PhantomData,
    mem::offset_of,
    ptr::{
        addr_of_mut,
        NonNull, //
    },
};

mod id;
mod io;
mod irq;

pub use self::id::{
    Class,
    ClassMask,
    Vendor, //
};
pub use self::io::Bar;

/// A standard PCI capability ID.
///
/// This is a thin wrapper around the underlying numeric capability ID.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CapabilityId(u8);

impl CapabilityId {
    /// Creates a [`CapabilityId`] from a raw value.
    #[inline]
    pub const fn from_raw(id: u8) -> Self {
        Self(id)
    }

    /// Returns the raw value.
    #[inline]
    pub const fn as_raw(self) -> u8 {
        self.0
    }

    /// Power Management.
    pub const PM: Self = Self(bindings::PCI_CAP_ID_PM as u8);
    /// Accelerated Graphics Port.
    pub const AGP: Self = Self(bindings::PCI_CAP_ID_AGP as u8);
    /// Vital Product Data.
    pub const VPD: Self = Self(bindings::PCI_CAP_ID_VPD as u8);
    /// Slot Identification.
    pub const SLOTID: Self = Self(bindings::PCI_CAP_ID_SLOTID as u8);
    /// Message Signalled Interrupts.
    pub const MSI: Self = Self(bindings::PCI_CAP_ID_MSI as u8);
    /// CompactPCI HotSwap.
    pub const CHSWP: Self = Self(bindings::PCI_CAP_ID_CHSWP as u8);
    /// PCI-X.
    pub const PCIX: Self = Self(bindings::PCI_CAP_ID_PCIX as u8);
    /// HyperTransport.
    pub const HT: Self = Self(bindings::PCI_CAP_ID_HT as u8);
    /// Vendor-Specific.
    pub const VNDR: Self = Self(bindings::PCI_CAP_ID_VNDR as u8);
    /// Debug port.
    pub const DBG: Self = Self(bindings::PCI_CAP_ID_DBG as u8);
    /// CompactPCI Central Resource Control.
    pub const CCRC: Self = Self(bindings::PCI_CAP_ID_CCRC as u8);
    /// PCI Standard Hot-Plug Controller.
    pub const SHPC: Self = Self(bindings::PCI_CAP_ID_SHPC as u8);
    /// Bridge subsystem vendor/device ID.
    pub const SSVID: Self = Self(bindings::PCI_CAP_ID_SSVID as u8);
    /// AGP 8x.
    pub const AGP3: Self = Self(bindings::PCI_CAP_ID_AGP3 as u8);
    /// Secure Device.
    pub const SECDEV: Self = Self(bindings::PCI_CAP_ID_SECDEV as u8);
    /// PCI Express.
    pub const EXP: Self = Self(bindings::PCI_CAP_ID_EXP as u8);
    /// MSI-X.
    pub const MSIX: Self = Self(bindings::PCI_CAP_ID_MSIX as u8);
    /// Serial ATA Data/Index Configuration.
    pub const SATA: Self = Self(bindings::PCI_CAP_ID_SATA as u8);
    /// PCI Advanced Features.
    pub const AF: Self = Self(bindings::PCI_CAP_ID_AF as u8);
    /// PCI Enhanced Allocation.
    pub const EA: Self = Self(bindings::PCI_CAP_ID_EA as u8);
}

/// A PCIe extended capability ID.
///
/// This is a thin wrapper around the underlying numeric capability ID.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ExtendedCapabilityId(u16);

impl ExtendedCapabilityId {
    /// Creates an [`ExtendedCapabilityId`] from a raw value.
    #[inline]
    pub const fn from_raw(id: u16) -> Self {
        Self(id)
    }

    /// Returns the raw value.
    #[inline]
    pub const fn as_raw(self) -> u16 {
        self.0
    }

    /// Advanced Error Reporting.
    pub const ERR: Self = Self(bindings::PCI_EXT_CAP_ID_ERR as u16);
    /// Virtual Channel.
    pub const VC: Self = Self(bindings::PCI_EXT_CAP_ID_VC as u16);
    /// Device Serial Number.
    pub const DSN: Self = Self(bindings::PCI_EXT_CAP_ID_DSN as u16);
    /// Vendor-Specific Extended Capability.
    pub const VNDR: Self = Self(bindings::PCI_EXT_CAP_ID_VNDR as u16);
    /// Access Control Services.
    pub const ACS: Self = Self(bindings::PCI_EXT_CAP_ID_ACS as u16);
    /// Alternate Routing-ID Interpretation.
    pub const ARI: Self = Self(bindings::PCI_EXT_CAP_ID_ARI as u16);
    /// Address Translation Services.
    pub const ATS: Self = Self(bindings::PCI_EXT_CAP_ID_ATS as u16);
    /// Single Root I/O Virtualization.
    pub const SRIOV: Self = Self(bindings::PCI_EXT_CAP_ID_SRIOV as u16);
    /// Resizable BAR.
    pub const REBAR: Self = Self(bindings::PCI_EXT_CAP_ID_REBAR as u16);
    /// Latency Tolerance Reporting.
    pub const LTR: Self = Self(bindings::PCI_EXT_CAP_ID_LTR as u16);
    /// Downstream Port Containment.
    pub const DPC: Self = Self(bindings::PCI_EXT_CAP_ID_DPC as u16);
    /// L1 PM Substates.
    pub const L1SS: Self = Self(bindings::PCI_EXT_CAP_ID_L1SS as u16);
    /// Precision Time Measurement.
    pub const PTM: Self = Self(bindings::PCI_EXT_CAP_ID_PTM as u16);
    /// Designated Vendor-Specific Extended Capability.
    pub const DVSEC: Self = Self(bindings::PCI_EXT_CAP_ID_DVSEC as u16);
}

pub use self::irq::{
    IrqType,
    IrqTypes,
    IrqVector, //
};

/// An adapter for the registration of PCI drivers.
pub struct Adapter<T: Driver>(T);

// SAFETY: A call to `unregister` for a given instance of `RegType` is guaranteed to be valid if
// a preceding call to `register` has been successful.
unsafe impl<T: Driver + 'static> driver::RegistrationOps for Adapter<T> {
    type RegType = bindings::pci_driver;

    unsafe fn register(
        pdrv: &Opaque<Self::RegType>,
        name: &'static CStr,
        module: &'static ThisModule,
    ) -> Result {
        // SAFETY: It's safe to set the fields of `struct pci_driver` on initialization.
        unsafe {
            (*pdrv.get()).name = name.as_char_ptr();
            (*pdrv.get()).probe = Some(Self::probe_callback);
            (*pdrv.get()).remove = Some(Self::remove_callback);
            (*pdrv.get()).id_table = T::ID_TABLE.as_ptr();
        }

        // SAFETY: `pdrv` is guaranteed to be a valid `RegType`.
        to_result(unsafe {
            bindings::__pci_register_driver(pdrv.get(), module.0, name.as_char_ptr())
        })
    }

    unsafe fn unregister(pdrv: &Opaque<Self::RegType>) {
        // SAFETY: `pdrv` is guaranteed to be a valid `RegType`.
        unsafe { bindings::pci_unregister_driver(pdrv.get()) }
    }
}

impl<T: Driver + 'static> Adapter<T> {
    extern "C" fn probe_callback(
        pdev: *mut bindings::pci_dev,
        id: *const bindings::pci_device_id,
    ) -> c_int {
        // SAFETY: The PCI bus only ever calls the probe callback with a valid pointer to a
        // `struct pci_dev`.
        //
        // INVARIANT: `pdev` is valid for the duration of `probe_callback()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `DeviceId` is a `#[repr(transparent)]` wrapper of `struct pci_device_id` and
        // does not add additional invariants, so it's safe to transmute.
        let id = unsafe { &*id.cast::<DeviceId>() };
        let info = T::ID_TABLE.info(id.index());

        from_result(|| {
            let data = T::probe(pdev, info);

            pdev.as_ref().set_drvdata(data)?;
            Ok(0)
        })
    }

    extern "C" fn remove_callback(pdev: *mut bindings::pci_dev) {
        // SAFETY: The PCI bus only ever calls the remove callback with a valid pointer to a
        // `struct pci_dev`.
        //
        // INVARIANT: `pdev` is valid for the duration of `remove_callback()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `remove_callback` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { pdev.as_ref().drvdata_obtain::<T>() };

        T::unbind(pdev, data.as_ref());
    }
}

/// Declares a kernel module that exposes a single PCI driver.
///
/// # Examples
///
///```ignore
/// kernel::module_pci_driver! {
///     type: MyDriver,
///     name: "Module name",
///     authors: ["Author name"],
///     description: "Description",
///     license: "GPL v2",
/// }
///```
#[macro_export]
macro_rules! module_pci_driver {
($($f:tt)*) => {
    $crate::module_driver!(<T>, $crate::pci::Adapter<T>, { $($f)* });
};
}

/// Abstraction for the PCI device ID structure ([`struct pci_device_id`]).
///
/// [`struct pci_device_id`]: https://docs.kernel.org/PCI/pci.html#c.pci_device_id
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(bindings::pci_device_id);

impl DeviceId {
    const PCI_ANY_ID: u32 = !0;

    /// Equivalent to C's `PCI_DEVICE` macro.
    ///
    /// Create a new `pci::DeviceId` from a vendor and device ID.
    #[inline]
    pub const fn from_id(vendor: Vendor, device: u32) -> Self {
        Self(bindings::pci_device_id {
            vendor: vendor.as_raw() as u32,
            device,
            subvendor: DeviceId::PCI_ANY_ID,
            subdevice: DeviceId::PCI_ANY_ID,
            class: 0,
            class_mask: 0,
            driver_data: 0,
            override_only: 0,
        })
    }

    /// Equivalent to C's `PCI_DEVICE_CLASS` macro.
    ///
    /// Create a new `pci::DeviceId` from a class number and mask.
    #[inline]
    pub const fn from_class(class: u32, class_mask: u32) -> Self {
        Self(bindings::pci_device_id {
            vendor: DeviceId::PCI_ANY_ID,
            device: DeviceId::PCI_ANY_ID,
            subvendor: DeviceId::PCI_ANY_ID,
            subdevice: DeviceId::PCI_ANY_ID,
            class,
            class_mask,
            driver_data: 0,
            override_only: 0,
        })
    }

    /// Create a new [`DeviceId`] from a class number, mask, and specific vendor.
    ///
    /// This is more targeted than [`DeviceId::from_class`]: in addition to matching by [`Vendor`],
    /// it also matches the PCI [`Class`] (up to the entire 24 bits, depending on the
    /// [`ClassMask`]).
    #[inline]
    pub const fn from_class_and_vendor(
        class: Class,
        class_mask: ClassMask,
        vendor: Vendor,
    ) -> Self {
        Self(bindings::pci_device_id {
            vendor: vendor.as_raw() as u32,
            device: DeviceId::PCI_ANY_ID,
            subvendor: DeviceId::PCI_ANY_ID,
            subdevice: DeviceId::PCI_ANY_ID,
            class: class.as_raw(),
            class_mask: class_mask.as_raw(),
            driver_data: 0,
            override_only: 0,
        })
    }
}

// SAFETY: `DeviceId` is a `#[repr(transparent)]` wrapper of `pci_device_id` and does not add
// additional invariants, so it's safe to transmute to `RawType`.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::pci_device_id;
}

// SAFETY: `DRIVER_DATA_OFFSET` is the offset to the `driver_data` field.
unsafe impl RawDeviceIdIndex for DeviceId {
    const DRIVER_DATA_OFFSET: usize = core::mem::offset_of!(bindings::pci_device_id, driver_data);

    fn index(&self) -> usize {
        self.0.driver_data
    }
}

/// `IdTable` type for PCI.
pub type IdTable<T> = &'static dyn kernel::device_id::IdTable<DeviceId, T>;

/// Create a PCI `IdTable` with its alias for modpost.
#[macro_export]
macro_rules! pci_device_table {
    ($table_name:ident, $module_table_name:ident, $id_info_type: ty, $table_data: expr) => {
        const $table_name: $crate::device_id::IdArray<
            $crate::pci::DeviceId,
            $id_info_type,
            { $table_data.len() },
        > = $crate::device_id::IdArray::new($table_data);

        $crate::module_device_table!("pci", $module_table_name, $table_name);
    };
}

/// The PCI driver trait.
///
/// # Examples
///
///```
/// # use kernel::{bindings, device::Core, pci};
///
/// struct MyDriver;
///
/// kernel::pci_device_table!(
///     PCI_TABLE,
///     MODULE_PCI_TABLE,
///     <MyDriver as pci::Driver>::IdInfo,
///     [
///         (
///             pci::DeviceId::from_id(pci::Vendor::REDHAT, bindings::PCI_ANY_ID as u32),
///             (),
///         )
///     ]
/// );
///
/// impl pci::Driver for MyDriver {
///     type IdInfo = ();
///     const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;
///
///     fn probe(
///         _pdev: &pci::Device<Core>,
///         _id_info: &Self::IdInfo,
///     ) -> impl PinInit<Self, Error> {
///         Err(ENODEV)
///     }
/// }
///```
/// Drivers must implement this trait in order to get a PCI driver registered. Please refer to the
/// `Adapter` documentation for an example.
pub trait Driver: Send {
    /// The type holding information about each device id supported by the driver.
    // TODO: Use `associated_type_defaults` once stabilized:
    //
    // ```
    // type IdInfo: 'static = ();
    // ```
    type IdInfo: 'static;

    /// The table of device ids supported by the driver.
    const ID_TABLE: IdTable<Self::IdInfo>;

    /// PCI driver probe.
    ///
    /// Called when a new pci device is added or discovered. Implementers should
    /// attempt to initialize the device here.
    fn probe(dev: &Device<device::Core>, id_info: &Self::IdInfo) -> impl PinInit<Self, Error>;

    /// PCI driver unbind.
    ///
    /// Called when a [`Device`] is unbound from its bound [`Driver`]. Implementing this callback
    /// is optional.
    ///
    /// This callback serves as a place for drivers to perform teardown operations that require a
    /// `&Device<Core>` or `&Device<Bound>` reference. For instance, drivers may try to perform I/O
    /// operations to gracefully tear down the device.
    ///
    /// Otherwise, release operations for driver resources should be performed in `Self::drop`.
    fn unbind(dev: &Device<device::Core>, this: Pin<&Self>) {
        let _ = (dev, this);
    }
}

/// The PCI device representation.
///
/// This structure represents the Rust abstraction for a C `struct pci_dev`. The implementation
/// abstracts the usage of an already existing C `struct pci_dev` within Rust code that we get
/// passed from the C side.
///
/// # Invariants
///
/// A [`Device`] instance represents a valid `struct pci_dev` created by the C portion of the
/// kernel.
#[repr(transparent)]
pub struct Device<Ctx: device::DeviceContext = device::Normal>(
    Opaque<bindings::pci_dev>,
    PhantomData<Ctx>,
);

impl<Ctx: device::DeviceContext> Device<Ctx> {
    #[inline]
    fn as_raw(&self) -> *mut bindings::pci_dev {
        self.0.get()
    }
}

impl Device {
    /// Returns the PCI vendor ID as [`Vendor`].
    ///
    /// # Examples
    ///
    /// ```
    /// # use kernel::{device::Core, pci::{self, Vendor}, prelude::*};
    /// fn log_device_info(pdev: &pci::Device<Core>) -> Result {
    ///     // Get an instance of `Vendor`.
    ///     let vendor = pdev.vendor_id();
    ///     dev_info!(
    ///         pdev.as_ref(),
    ///         "Device: Vendor={}, Device=0x{:x}\n",
    ///         vendor,
    ///         pdev.device_id()
    ///     );
    ///     Ok(())
    /// }
    /// ```
    #[inline]
    pub fn vendor_id(&self) -> Vendor {
        // SAFETY: `self.as_raw` is a valid pointer to a `struct pci_dev`.
        let vendor_id = unsafe { (*self.as_raw()).vendor };
        Vendor::from_raw(vendor_id)
    }

    /// Returns the PCI device ID.
    #[inline]
    pub fn device_id(&self) -> u16 {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct pci_dev`.
        unsafe { (*self.as_raw()).device }
    }

    /// Returns the PCI revision ID.
    #[inline]
    pub fn revision_id(&self) -> u8 {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct pci_dev`.
        unsafe { (*self.as_raw()).revision }
    }

    /// Returns the PCI bus device/function.
    #[inline]
    pub fn dev_id(&self) -> u16 {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct pci_dev`.
        unsafe { bindings::pci_dev_id(self.as_raw()) }
    }

    /// Returns the PCI subsystem vendor ID.
    #[inline]
    pub fn subsystem_vendor_id(&self) -> u16 {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct pci_dev`.
        unsafe { (*self.as_raw()).subsystem_vendor }
    }

    /// Returns the PCI subsystem device ID.
    #[inline]
    pub fn subsystem_device_id(&self) -> u16 {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct pci_dev`.
        unsafe { (*self.as_raw()).subsystem_device }
    }

    /// Returns the start of the given PCI BAR resource.
    pub fn resource_start(&self, bar: u32) -> Result<bindings::resource_size_t> {
        if !Bar::index_is_valid(bar) {
            return Err(EINVAL);
        }

        // SAFETY:
        // - `bar` is a valid bar number, as guaranteed by the above call to `Bar::index_is_valid`,
        // - by its type invariant `self.as_raw` is always a valid pointer to a `struct pci_dev`.
        Ok(unsafe { bindings::pci_resource_start(self.as_raw(), bar.try_into()?) })
    }

    /// Returns the size of the given PCI BAR resource.
    pub fn resource_len(&self, bar: u32) -> Result<bindings::resource_size_t> {
        if !Bar::index_is_valid(bar) {
            return Err(EINVAL);
        }

        // SAFETY:
        // - `bar` is a valid bar number, as guaranteed by the above call to `Bar::index_is_valid`,
        // - by its type invariant `self.as_raw` is always a valid pointer to a `struct pci_dev`.
        Ok(unsafe { bindings::pci_resource_len(self.as_raw(), bar.try_into()?) })
    }

    /// Returns the PCI class as a `Class` struct.
    #[inline]
    pub fn pci_class(&self) -> Class {
        // SAFETY: `self.as_raw` is a valid pointer to a `struct pci_dev`.
        Class::from_raw(unsafe { (*self.as_raw()).class })
    }

    /// Finds a PCI capability by ID and returns its config-space offset.
    ///
    /// Returns `None` if the capability is not present.
    ///
    /// This is a thin wrapper around `pci_find_capability()`.
    #[inline]
    pub fn find_capability(&self, id: CapabilityId) -> Option<u8> {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct pci_dev`.
        let offset = unsafe { bindings::pci_find_capability(self.as_raw(), id.as_raw().into()) };

        if offset == 0 {
            return None;
        }

        Some(offset)
    }

    /// Finds an extended capability by ID and returns its config-space offset.
    ///
    /// Returns `None` if the capability is not present.
    ///
    /// This is a thin wrapper around `pci_find_ext_capability()`.
    #[inline]
    pub fn find_ext_capability(&self, id: ExtendedCapabilityId) -> Option<u16> {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct pci_dev`.
        let offset =
            unsafe { bindings::pci_find_ext_capability(self.as_raw(), id.as_raw().into()) };

        if offset == 0 {
            return None;
        }

        Some(offset)
    }
}

impl Device<device::Core> {
    /// Enable memory resources for this device.
    pub fn enable_device_mem(&self) -> Result {
        // SAFETY: `self.as_raw` is guaranteed to be a pointer to a valid `struct pci_dev`.
        to_result(unsafe { bindings::pci_enable_device_mem(self.as_raw()) })
    }

    /// Enable bus-mastering for this device.
    #[inline]
    pub fn set_master(&self) {
        // SAFETY: `self.as_raw` is guaranteed to be a pointer to a valid `struct pci_dev`.
        unsafe { bindings::pci_set_master(self.as_raw()) };
    }
}

// SAFETY: `pci::Device` is a transparent wrapper of `struct pci_dev`.
// The offset is guaranteed to point to a valid device field inside `pci::Device`.
unsafe impl<Ctx: device::DeviceContext> device::AsBusDevice<Ctx> for Device<Ctx> {
    const OFFSET: usize = offset_of!(bindings::pci_dev, dev);
}

// SAFETY: `Device` is a transparent wrapper of a type that doesn't depend on `Device`'s generic
// argument.
kernel::impl_device_context_deref!(unsafe { Device });
kernel::impl_device_context_into_aref!(Device);

impl crate::dma::Device for Device<device::Core> {}

// SAFETY: Instances of `Device` are always reference-counted.
unsafe impl crate::sync::aref::AlwaysRefCounted for Device {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::pci_dev_get(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::pci_dev_put(obj.cast().as_ptr()) }
    }
}

impl<Ctx: device::DeviceContext> AsRef<device::Device<Ctx>> for Device<Ctx> {
    fn as_ref(&self) -> &device::Device<Ctx> {
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct pci_dev`.
        let dev = unsafe { addr_of_mut!((*self.as_raw()).dev) };

        // SAFETY: `dev` points to a valid `struct device`.
        unsafe { device::Device::from_raw(dev) }
    }
}

impl<Ctx: device::DeviceContext> TryFrom<&device::Device<Ctx>> for &Device<Ctx> {
    type Error = kernel::error::Error;

    fn try_from(dev: &device::Device<Ctx>) -> Result<Self, Self::Error> {
        // SAFETY: By the type invariant of `Device`, `dev.as_raw()` is a valid pointer to a
        // `struct device`.
        if !unsafe { bindings::dev_is_pci(dev.as_raw()) } {
            return Err(EINVAL);
        }

        // SAFETY: We've just verified that the bus type of `dev` equals `bindings::pci_bus_type`,
        // hence `dev` must be embedded in a valid `struct pci_dev` as guaranteed by the
        // corresponding C code.
        let pdev = unsafe { container_of!(dev.as_raw(), bindings::pci_dev, dev) };

        // SAFETY: `pdev` is a valid pointer to a `struct pci_dev`.
        Ok(unsafe { &*pdev.cast() })
    }
}

// SAFETY: A `Device` is always reference-counted and can be released from any thread.
unsafe impl Send for Device {}

// SAFETY: `Device` can be shared among threads because all methods of `Device`
// (i.e. `Device<Normal>) are thread safe.
unsafe impl Sync for Device {}
