// SPDX-License-Identifier: GPL-2.0

//! VIRTIO abstraction.
//!
//! To implement a VIRTIO driver:
//!
//! - Implement the [`Driver`] trait for your driver type (use [`virtio_device_table`] macro to
//!   declare the `ID_TABLE` associated item)
//! - Use the [`module_virtio_driver`] macro to declare your module

use crate::{
    bindings,
    device_id::RawDeviceId,
    error::{
        from_result,
        to_result,
        Error,
        Result, //
    },
    ffi::c_uint,
    prelude::*,
    types::Opaque, //
};

use core::{
    marker::PhantomData,
    pin::Pin,
    ptr::NonNull, //
};

pub mod utils;
pub mod virtqueue;

/// IdTable type for virtio drivers.
pub type IdTable<T> = &'static dyn crate::device_id::IdTable<DeviceId, T>;

/// A VIRTIO device id.
///
/// [`struct virtio_device_id`]: srctree/include/linux/mod_devicetable.h
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct DeviceId(bindings::virtio_device_id);

// SAFETY: `DeviceId` is a `#[repr(transparent)]` wrapper of `struct virtio_device_id` and
// does not add additional invariants, so it's safe to transmute to `RawType`.
unsafe impl RawDeviceId for DeviceId {
    type RawType = bindings::virtio_device_id;
}

impl DeviceId {
    #[inline]
    /// Create a new device id
    pub const fn new(device: VirtioID) -> Self {
        Self::new_with_vendor(device, VIRTIO_DEV_ANY_ID)
    }

    #[inline]
    /// Create a new device id with vendor
    pub const fn new_with_vendor(device: VirtioID, vendor: u32) -> Self {
        // Replace with `bindings::virtio_device_id::default()` once stabilized for `const`.
        // SAFETY: FFI type is valid to be zero-initialized.
        let mut ret: bindings::virtio_device_id = unsafe { core::mem::zeroed() };
        ret.device = device as u32;
        ret.vendor = vendor;
        Self(ret)
    }
}

/// Create a virtio `IdTable` with its alias for modpost.
#[macro_export]
macro_rules! virtio_device_table {
    ($table_name:ident, $module_table_name:ident, $id_info_type: ty, $table_data:expr) => {
        const $table_name: $crate::device_id::IdArray<
            $crate::virtio::DeviceId,
            $id_info_type,
            { $table_data.len() },
        > = $crate::device_id::IdArray::new_without_index($table_data);

        $crate::module_device_table!("virtio", $module_table_name, $table_name);
    };
}

/// Declares a kernel module that exposes a single virtio driver.
#[macro_export]
macro_rules! module_virtio_driver {
($($f:tt)*) => {
    $crate::module_driver!(<T>, $crate::virtio::Adapter<T>, { $($f)* });
};
}

/// The Virtio driver trait.
///
/// Drivers must implement this trait in order to get a virtio driver registered.
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

    /// virtio driver probe.
    ///
    /// Called when a new virtio device is added or discovered. Implementers should
    /// attempt to initialize the device here, but should try not sleep since driver data is set
    /// after this method returns successfully.
    fn probe(dev: &Device<crate::device::Core>) -> impl PinInit<Self, Error>;

    /// virtio driver init.
    ///
    /// Called after a virtio device is probed successfully, can sleep.
    fn init(&self, dev: &Device<crate::device::Bound>) -> Result;

    /// virtio driver remove.
    ///
    /// Called when a [`Device`] is removed from its [`Driver`]. Implementing this callback
    /// is optional.
    ///
    /// This callback serves as a place for drivers to perform teardown operations that require a
    /// `&Device<Core>` or `&Device<Bound>` reference. For instance, drivers may try to perform I/O
    /// operations to gracefully tear down the device.
    ///
    /// Otherwise, release operations for driver resources should be performed in `Self::drop`.
    fn remove(dev: &Device<crate::device::Core>, this: Pin<&Self>) {
        _ = (dev, this);
    }
}

/// Abstraction for the virtio device structure (`struct virtio_device`).
///
/// [`struct virtio_device`]: srctree/include/linux/virtio.h
#[repr(transparent)]
pub struct Device<Ctx: crate::device::DeviceContext = crate::device::Normal>(
    Opaque<bindings::virtio_device>,
    PhantomData<Ctx>,
);

impl<Ctx: crate::device::DeviceContext> Device<Ctx> {
    #[inline]
    fn as_raw(&self) -> *mut bindings::virtio_device {
        self.0.get()
    }
}

// SAFETY: `virtio::Device` is a transparent wrapper of `struct virtio_device`.
// The offset is guaranteed to point to a valid device field inside `virtio::Device`.
unsafe impl<Ctx: crate::device::DeviceContext> crate::device::AsBusDevice<Ctx> for Device<Ctx> {
    const OFFSET: usize = core::mem::offset_of!(bindings::virtio_device, dev);
}

// SAFETY: `Device` is a transparent wrapper of a type that doesn't depend on `Device`'s generic
// argument.
kernel::impl_device_context_deref!(unsafe { Device });

impl<Ctx: crate::device::DeviceContext> Device<Ctx> {
    // TODO: return VirtioID
    /// Returns the virtio device ID.
    #[inline]
    pub fn device_id(&self) -> u32 {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct virtio_device`.
        unsafe { (*self.as_raw()).id.device }
    }

    /// Returns the virtio vendor ID.
    #[inline]
    pub fn vendor_id(&self) -> u32 {
        // SAFETY: `self.as_raw` is a valid pointer to a `struct virtio_device`.
        unsafe { (*self.as_raw()).id.vendor }
    }

    /// Reset device.
    #[doc(alias = "virtio_reset_device")]
    #[inline]
    pub fn reset(&self) {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct virtio_device`.
        unsafe { bindings::virtio_reset_device(self.as_raw()) }
    }

    /// Mark device as ready.
    #[doc(alias = "virtio_device_ready")]
    #[inline]
    pub fn ready(&self) {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct virtio_device`.
        unsafe { bindings::virtio_device_ready(self.as_raw()) }
    }

    /// Return virtqueues for this device.
    #[doc(alias = "virtio_find_vqs")]
    pub fn find_vqs(&self, info: &[virtqueue::VirtqueueInfo]) -> Result<virtqueue::Virtqueues> {
        let mut vqs = KVec::with_capacity(info.len(), GFP_KERNEL)?;
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct virtio_device`.
        to_result(unsafe {
            bindings::virtio_find_vqs(
                self.as_raw(),
                info.len().try_into()?,
                vqs.spare_capacity_mut().as_mut_ptr().cast(),
                info.as_ptr().cast_mut().cast(),
                core::ptr::null_mut(),
            )
        })?;
        // SAFETY: virtio_find_vqs returned successfully so `vqs` must be populated.
        unsafe { vqs.inc_len(info.len()) };
        let mut inner = KVec::with_capacity(vqs.len(), GFP_KERNEL)?;
        for vq in vqs {
            inner.push(NonNull::new(vq).ok_or(EINVAL)?, GFP_KERNEL)?;
        }
        Ok(virtqueue::Virtqueues { inner })
    }

    /// Delete virtqueues from this device.
    pub(crate) fn del_vqs(&self) {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct virtio_device`.
        let config = unsafe { (*self.as_raw()).config };
        // SAFETY: `config` points to a valid virtqueue config struct.
        if let Some(del_vqs) = unsafe { (*config).del_vqs } {
            // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
            // `struct virtio_device`.
            unsafe { del_vqs(self.as_raw()) }
        }
    }

    /// Checks if the device has a feature bit.
    #[inline]
    pub fn has_feature(&self, fbit: c_uint) -> bool {
        // SAFETY: By its type invariant `self.as_raw` is always a valid pointer to a
        // `struct virtio_device`.
        unsafe { bindings::virtio_has_feature(self.as_raw(), fbit) }
    }
}

impl<Ctx: crate::device::DeviceContext> AsRef<crate::device::Device<Ctx>> for Device<Ctx> {
    #[inline]
    fn as_ref(&self) -> &crate::device::Device<Ctx> {
        // SAFETY: By the type invariant of `Self`, `self.as_raw()` is a pointer to a valid
        // `struct virtio_device`.
        let dev = unsafe { core::ptr::addr_of_mut!((*self.as_raw()).dev) };

        // SAFETY: `dev` points to a valid `struct device`.
        unsafe { crate::device::Device::from_raw(dev) }
    }
}

/// An adapter for the registration of virtio drivers.
pub struct Adapter<T: Driver>(T);

// SAFETY:
// - `bindings::virtio_driver` is a C type declared as `repr(C)`.
// - `T` is the type of the driver's device private data.
// - `struct virtio_driver` embeds a `struct device_driver`.
// - `DEVICE_DRIVER_OFFSET` is the correct byte offset to the embedded `struct device_driver`.
unsafe impl<T: Driver + 'static> crate::driver::DriverLayout for Adapter<T> {
    type DriverType = bindings::virtio_driver;
    type DriverData = T;
    const DEVICE_DRIVER_OFFSET: usize = core::mem::offset_of!(Self::DriverType, driver);
}

// SAFETY: A call to `unregister` for a given instance of `DriverType` is guaranteed to be valid if
// a preceding call to `register` has been successful.
unsafe impl<T: Driver + 'static> crate::driver::RegistrationOps for Adapter<T> {
    unsafe fn register(
        vdrv: &Opaque<Self::DriverType>,
        name: &'static CStr,
        module: &'static ThisModule,
    ) -> Result {
        // SAFETY: It's safe to set the fields of `struct virtio_driver` on initialization.
        unsafe {
            (*vdrv.get()).driver.name = name.as_char_ptr();
            (*vdrv.get()).id_table = T::ID_TABLE.as_ptr();
            (*vdrv.get()).probe = Some(Self::probe_callback);
            (*vdrv.get()).remove = Some(Self::remove_callback);
        }

        // SAFETY: `vdrv` is guaranteed to be a valid `DriverType`.
        to_result(unsafe { bindings::__register_virtio_driver(vdrv.get(), module.0) })
    }

    unsafe fn unregister(vdrv: &Opaque<Self::DriverType>) {
        // SAFETY: `vdrv` is guaranteed to be a valid `DriverType`.
        unsafe { bindings::unregister_virtio_driver(vdrv.get()) }
    }
}

impl<T: Driver + 'static> Adapter<T> {
    extern "C" fn probe_callback(vdev: *mut bindings::virtio_device) -> c_int {
        // SAFETY: The kernel only ever calls the probe callback with a valid pointer to a `struct
        // virtio_device`.
        //
        // INVARIANT: `vdev` is valid for the duration of `probe_callback()`.
        let dev = unsafe { &*vdev.cast::<Device<crate::device::CoreInternal>>() };
        from_result(|| {
            let data = T::probe(dev);

            dev.as_ref().set_drvdata(data)?;
            // SAFETY: `Device::set_drvdata()` was just called so it's safe to borrow the data.
            let data = unsafe { dev.as_ref().drvdata_borrow::<T>() };
            dev.ready();
            if let Err(err) = T::init(&data, dev) {
                // SAFETY: `Device::set_drvdata()` was just called so it's safe to re-obtain the
                // data.
                let data = unsafe { dev.as_ref().drvdata_obtain::<T>() }.unwrap();
                T::remove(dev, data.as_ref());
                drop(data);
                return Err(err);
            }
            Ok(0)
        })
    }

    extern "C" fn remove_callback(vdev: *mut bindings::virtio_device) {
        // SAFETY: The kernel only ever calls the remove callback with a valid pointer to a `struct
        // virtio_device`.
        //
        // INVARIANT: `vdev` is valid for the duration of `remove_callback()`.
        let dev = unsafe { &*vdev.cast::<Device<crate::device::CoreInternal>>() };

        // SAFETY: `remove_callback` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { dev.as_ref().drvdata_borrow::<T>() };

        T::remove(dev, data);
        dev.reset();
    }
}

/// Any vendor
pub const VIRTIO_DEV_ANY_ID: u32 = 0xffffffff;

/// Virtio IDs
///
/// C header: [`include/uapi/linux/virtio_ids.h`](srctree/include/uapi/linux/virtio_ids.h)
#[repr(u32)]
pub enum VirtioID {
    /// virtio net
    Net = bindings::VIRTIO_ID_NET,
    /// virtio block
    Block = bindings::VIRTIO_ID_BLOCK,
    /// virtio console
    Console = bindings::VIRTIO_ID_CONSOLE,
    /// virtio rng
    Rng = bindings::VIRTIO_ID_RNG,
    /// virtio balloon
    Balloon = bindings::VIRTIO_ID_BALLOON,
    /// virtio ioMemory
    IOMem = bindings::VIRTIO_ID_IOMEM,
    /// virtio remote processor messaging
    RPMSG = bindings::VIRTIO_ID_RPMSG,
    /// virtio scsi
    Scsi = bindings::VIRTIO_ID_SCSI,
    /// 9p virtio console
    NineP = bindings::VIRTIO_ID_9P,
    /// virtio WLAN MAC
    Mac80211Wlan = bindings::VIRTIO_ID_MAC80211_WLAN,
    /// virtio remoteproc serial link
    RPROCSerial = bindings::VIRTIO_ID_RPROC_SERIAL,
    /// Virtio caif
    CAIF = bindings::VIRTIO_ID_CAIF,
    /// virtio memory balloon
    MemoryBalloon = bindings::VIRTIO_ID_MEMORY_BALLOON,
    /// virtio GPU
    GPU = bindings::VIRTIO_ID_GPU,
    /// virtio clock/timer
    Clock = bindings::VIRTIO_ID_CLOCK,
    /// virtio input
    Input = bindings::VIRTIO_ID_INPUT,
    /// virtio vsock transport
    VSock = bindings::VIRTIO_ID_VSOCK,
    /// virtio crypto
    Crypto = bindings::VIRTIO_ID_CRYPTO,
    /// virtio signal distribution device
    SignalDist = bindings::VIRTIO_ID_SIGNAL_DIST,
    /// virtio pstore device
    Pstore = bindings::VIRTIO_ID_PSTORE,
    /// virtio IOMMU
    Iommu = bindings::VIRTIO_ID_IOMMU,
    /// virtio mem
    Mem = bindings::VIRTIO_ID_MEM,
    /// virtio sound
    Sound = bindings::VIRTIO_ID_SOUND,
    /// virtio filesystem
    FS = bindings::VIRTIO_ID_FS,
    /// virtio pmem
    PMem = bindings::VIRTIO_ID_PMEM,
    /// virtio rpmb
    RPMB = bindings::VIRTIO_ID_RPMB,
    /// virtio mac80211-hwsim
    Mac80211Hwsim = bindings::VIRTIO_ID_MAC80211_HWSIM,
    /// virtio video encoder
    VideoEncoder = bindings::VIRTIO_ID_VIDEO_ENCODER,
    /// virtio video decoder
    VideoDecoder = bindings::VIRTIO_ID_VIDEO_DECODER,
    /// virtio SCMI
    SCMI = bindings::VIRTIO_ID_SCMI,
    /// virtio nitro secure module
    NitroSecMod = bindings::VIRTIO_ID_NITRO_SEC_MOD,
    /// virtio i2c adapter
    I2CAdapter = bindings::VIRTIO_ID_I2C_ADAPTER,
    /// virtio watchdog
    Watchdog = bindings::VIRTIO_ID_WATCHDOG,
    /// virtio can
    CAN = bindings::VIRTIO_ID_CAN,
    /// virtio dmabuf
    DMABuf = bindings::VIRTIO_ID_DMABUF,
    /// virtio parameter server
    ParamServ = bindings::VIRTIO_ID_PARAM_SERV,
    /// virtio audio policy
    AudioPolicy = bindings::VIRTIO_ID_AUDIO_POLICY,
    /// virtio bluetooth
    BT = bindings::VIRTIO_ID_BT,
    /// virtio gpio
    GPIO = bindings::VIRTIO_ID_GPIO,
    /// virtio spi
    SPI = bindings::VIRTIO_ID_SPI,
}
