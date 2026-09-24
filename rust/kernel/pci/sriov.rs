// SPDX-License-Identifier: GPL-2.0

//! Abstractions for PCI Single Root I/O Virtualization (SR-IOV) drivers.

use super::Device as PciDevice;
use crate::{
    bindings,
    device, //
    prelude::*,
    types::{
        CovariantForLt,
        ForLt, //
    },
};
use core::{
    any::TypeId,
    marker::PhantomPinned, //
};

/// Wrapper for VF registration data stored inside a [`VfRegistration`].
///
/// Stores a [`TypeId`] header (derived from `F`) followed by the pinned data,
/// so that [`PciDevice::vf_registration_data_with()`] can verify the type at
/// runtime.
#[repr(C)]
#[pin_data]
struct VfRegistrationData<'a, F: ForLt + 'static> {
    type_id: TypeId,
    #[pin]
    data: F::Of<'a>,
}

static_assert!(
    core::mem::offset_of!(VfRegistrationData<'static, CovariantForLt!(())>, type_id) == 0
);

impl<'a, F: ForLt + 'static> VfRegistrationData<'a, F> {
    /// Pin-initializer for the registration data.
    fn new<D>(data: D) -> impl PinInit<Self, Error> + use<'a, D, F>
    where
        D: PinInit<F::Of<'a>, Error> + 'a,
    {
        try_pin_init!(Self {
            type_id: TypeId::of::<F>(),
            data <- data,
        })
    }
}

/// SR-IOV VF registration on a PF device.
///
/// Owns the registration data that VF drivers access via
/// [`PciDevice::vf_registration_data_with()`] and [`PciDevice::vf_registration_data()`].
///
/// The Rust PCI adapter removes all VFs before invoking the PF driver's unbind callback or
/// dropping its data. Drop of a published registration calls `pci_disable_sriov()` before
/// clearing the pointer and letting the data fields drop.
#[pin_data(PinnedDrop)]
pub struct VfRegistration<'a, F: ForLt + 'static> {
    pdev: &'a PciDevice<device::Bound>,
    #[pin]
    inner: VfRegistrationData<'a, F>,
    published: bool,
    #[pin]
    _pin: PhantomPinned,
}

impl<'a, F: ForLt + 'static> VfRegistration<'a, F>
where
    for<'b> F::Of<'b>: Send + Sync,
{
    /// Create a new VF registration.
    ///
    /// Returns a pin-initializer so the registration can be embedded directly
    /// in the PF driver's bus device private data.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the containing struct's field ordering drops
    /// this `VfRegistration` before any resources that the registration data
    /// borrows.
    ///
    /// The caller must invoke this during the PCI driver's probe and embed the result in the driver
    /// data. On an SR-IOV PF, no VF may be enabled before probe successfully installs the complete
    /// driver data.
    pub unsafe fn new<'core, D>(
        pdev: &'a PciDevice<device::Core<'core>>,
        data: D,
    ) -> impl PinInit<Self, Error> + use<'a, 'core, D, F>
    where
        D: PinInit<F::Of<'a>, Error> + 'a,
    {
        pin_init::pin_init_scope(move || {
            if pdev.is_virtfn() {
                return Err(ENODEV);
            }

            let published = pdev.is_physfn();
            if published {
                if pdev.num_vf() != 0 {
                    return Err(EBUSY);
                }

                if !pdev.vf_registration_data_rust().is_null() {
                    return Err(EBUSY);
                }
            }

            Ok(try_pin_init!(Self {
                pdev,
                inner <- VfRegistrationData::new(data),
                published,
                _pin: PhantomPinned,
                _: {
                    if *published {
                        // Store the pointer to the pinned `VfRegistrationData`
                        // on the PCI device so VF drivers can find it.
                        pdev.set_vf_registration_data_rust(
                            core::ptr::from_ref(inner.as_ref().get_ref()).cast_mut().cast(),
                        );
                    }
                },
            }))
        })
    }
}

#[pinned_drop]
impl<F: ForLt + 'static> PinnedDrop for VfRegistration<'_, F> {
    fn drop(self: Pin<&mut Self>) {
        if !self.published {
            return;
        }

        // SAFETY: `pci_disable_sriov()` is safe to call on any `pci_dev`; it
        // is a no-op if the device has no VFs enabled. When VFs are enabled,
        // this blocks until all VF `remove()` callbacks complete.
        unsafe { bindings::pci_disable_sriov(self.pdev.as_raw()) };

        // After `pci_disable_sriov()` all VFs are gone, so no one can read
        // the pointer anymore.
        self.pdev
            .set_vf_registration_data_rust(core::ptr::null_mut());

        // The pinned `inner` field is dropped automatically after this returns.
    }
}

// SAFETY: The inner data is `Send` (enforced by the bound), and `&PciDevice` is `Send + Sync`.
unsafe impl<F: ForLt> Send for VfRegistration<'_, F> where for<'a> F::Of<'a>: Send {}

// SAFETY: The inner data is `Send + Sync`. `VfRegistration` doesn't expose mutable access;
// VF drivers only read the data through an immutable pinned reference.
unsafe impl<F: ForLt> Sync for VfRegistration<'_, F> where for<'a> F::Of<'a>: Send + Sync {}

impl<Ctx: device::DeviceContext> PciDevice<Ctx> {
    /// Returns the raw `vf_registration_data_rust` pointer from this device.
    fn vf_registration_data_rust(&self) -> *mut core::ffi::c_void {
        // SAFETY: `self.as_raw()` is valid.
        unsafe { (*self.as_raw()).vf_registration_data_rust }
    }

    /// Sets the `vf_registration_data_rust` pointer on this device.
    fn set_vf_registration_data_rust(&self, ptr: *mut core::ffi::c_void) {
        // SAFETY: `self.as_raw()` is valid. PCI probe publishes the data before enabling VFs;
        // teardown removes all VFs before withdrawing it.
        unsafe { (*self.as_raw()).vf_registration_data_rust = ptr };
    }
}

impl PciDevice<device::Bound> {
    /// Returns the PF for this VF, or [`ENODEV`] if this is not a VF.
    fn physfn(&self) -> Result<&PciDevice> {
        if !self.is_virtfn() {
            return Err(ENODEV);
        }

        // SAFETY: `self.as_raw()` is valid and this VF uses the `physfn` union field.
        let pf = unsafe { (*self.as_raw()).__bindgen_anon_1.physfn };
        if pf.is_null() {
            return Err(ENODEV);
        }

        // SAFETY: PCI holds a PF reference until VF removal completes. The returned borrow
        // cannot outlive this bound VF, and `PciDevice` is a transparent wrapper of `pci_dev`.
        Ok(unsafe { &*pf.cast() })
    }

    /// Internal helper: reads the `vf_registration_data_rust` pointer from the
    /// PF, checks the `TypeId`, and returns a pinned reference.
    ///
    /// # Safety
    ///
    /// The returned borrow must be confined by a closure higher-ranked independently over its
    /// borrow and data lifetimes, or `F` must be covariant in its encoded lifetime.
    unsafe fn vf_registration_data_pinned<F: ForLt + 'static>(&self) -> Result<Pin<&F::Of<'_>>> {
        let pf = self.physfn()?;

        let ptr = pf.vf_registration_data_rust();
        if ptr.is_null() {
            return Err(ENOENT);
        }

        // SAFETY: The Rust PCI adapter keeps the PF data installed until VF removal completes.
        // `ptr` points to a `VfRegistrationData` whose first field is a `TypeId`.
        let type_id = unsafe { ptr.cast::<TypeId>().read() };
        if type_id != TypeId::of::<F>() {
            return Err(EINVAL);
        }

        // SAFETY: TypeId check confirms the stored type matches `F`. The data
        // is pinned inside the PF's driver data struct. Lifetime shortening
        // from the PF's binding scope to `'_` is layout-compatible.
        let data_ptr = unsafe {
            let vfrd = ptr.cast::<VfRegistrationData<'_, F>>();
            &raw const (*vfrd).data
        };

        // SAFETY: `data` is structurally pinned inside `VfRegistrationData`.
        Ok(unsafe { Pin::new_unchecked(&*data_ptr) })
    }

    /// Access the VF registration data through a closure with an HRTB lifetime.
    ///
    /// `F` is the [`ForLt`](trait@ForLt) encoding of the data type. Returns
    /// [`ENODEV`] if this is not a VF, [`ENOENT`] if no data was registered,
    /// or [`EINVAL`] if `F` does not match the type registered by the PF.
    ///
    /// The closure's borrow and the registration data's lifetime are independent, so a borrow of
    /// the context cannot be stored in invariant registration data.
    pub fn vf_registration_data_with<F: ForLt + 'static, R>(
        &self,
        f: impl for<'borrow, 'data> FnOnce(Pin<&'borrow F::Of<'data>>) -> R,
    ) -> Result<R> {
        // SAFETY: The higher-ranked closure prevents the borrow from escaping or being stored in
        // invariant data by keeping its lifetime independent of the erased data lifetime.
        let pinned = unsafe { self.vf_registration_data_pinned::<F>()? };
        Ok(f(pinned))
    }

    /// Returns a pinned reference to the VF registration data.
    ///
    /// Available only when `F` implements [`CovariantForLt`](trait@crate::types::CovariantForLt),
    /// guaranteeing that shortening the PF data lifetime is sound.
    ///
    /// For non-covariant types, use [`Self::vf_registration_data_with()`].
    ///
    /// It returns the same errors as [`Self::vf_registration_data_with()`].
    pub fn vf_registration_data<F: CovariantForLt + 'static>(&self) -> Result<Pin<&F::Of<'_>>> {
        // SAFETY: `CovariantForLt` permits shortening the encoded lifetime to this borrow.
        unsafe { self.vf_registration_data_pinned::<F>() }
    }
}
