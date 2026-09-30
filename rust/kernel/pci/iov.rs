// SPDX-License-Identifier: GPL-2.0

//! Abstractions for PCI Single Root I/O Virtualization (SR-IOV) drivers.

use super::Device;

use crate::{
    bindings,
    device,
    error::to_result,
    prelude::*,
    types::{
        CovariantForLt,
        ForLt, //
    }, //
};
use core::{
    any::TypeId,
    marker::PhantomPinned, //
};

impl Device {
    /// Returns `true` if this device is a Physical Function (PF).
    #[inline]
    #[expect(dead_code)]
    pub(crate) fn is_physfn(&self) -> bool {
        // SAFETY: `self.as_raw` is a valid pointer to a `struct pci_dev`.
        unsafe { (*self.as_raw()).is_physfn() != 0 }
    }

    /// Returns `true` if this device is a Virtual Function (VF).
    #[inline]
    pub fn is_virtfn(&self) -> bool {
        // SAFETY: `self.as_raw` is a valid pointer to a `struct pci_dev`.
        unsafe { (*self.as_raw()).is_virtfn() != 0 }
    }
}

impl Device<device::Core<'_>> {
    /// Returns the number of Virtual Functions (VF) enabled for a Physical Function (PF).
    pub fn num_vf(&self) -> i32 {
        // SAFETY: `self.as_raw()` is valid and this call runs in the PCI core callback context.
        unsafe { bindings::pci_num_vf(self.as_raw()) }
    }
}

impl Device<device::CoreInternal<'_>> {
    /// Enable the Single Root I/O Virtualization (SR-IOV) capability for this device,
    /// where `nr_virtfn` is number of Virtual Functions (VF) to enable.
    #[expect(dead_code)]
    pub(crate) fn enable_sriov(&self, nr_virtfn: c_int) -> Result {
        // SAFETY:
        // `self.as_raw` returns a valid pointer to a `struct pci_dev`.
        //
        // `pci_enable_sriov()` checks that the enable operation is valid:
        // - the device is a Physical Function (PF),
        // - SR-IOV is currently disabled, and
        // - `nr_virtfn` does not exceed the total number of supported VFs.
        //
        // The CoreInternal device context inherits from the Bound device context,
        // which guarantees that the PF device is bound to a driver.
        to_result(unsafe { bindings::pci_enable_sriov(self.as_raw(), nr_virtfn) })
    }

    /// Disable the Single Root I/O Virtualization (SR-IOV) capability for this device.
    #[expect(dead_code)]
    pub(crate) fn disable_sriov(&self) {
        // SAFETY:
        // `self.as_raw` returns a valid pointer to a `struct pci_dev`.
        //
        // `pci_disable_sriov()` checks that the disable operation is valid:
        // - the device is a Physical Function (PF), and
        // - SR-IOV is currently enabled.
        //
        // The CoreInternal device context inherits from the Bound device context,
        // which guarantees that the PF device is bound to a driver.
        unsafe { bindings::pci_disable_sriov(self.as_raw()) };
    }
}

/// Wrapper for VF registration data stored inside a [`VfRegistration`].
///
/// Stores a [`TypeId`] header (derived from `F`) followed by the pinned data,
/// so that [`Device::vf_registration_data_with()`] can verify the type at
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
    fn new(data: impl PinInit<F::Of<'a>, Error>) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            type_id: TypeId::of::<F>(),
            data <- data,
        })
    }
}

/// SR-IOV VF registration on a PF device.
///
/// Owns the registration data that VF drivers access via
/// [`Device::vf_registration_data_with()`] and [`Device::vf_registration_data()`].
///
/// Drop disables SR-IOV before clearing the registration pointer and releasing the data.
#[pin_data(PinnedDrop)]
pub struct VfRegistration<'a, F: ForLt + 'static> {
    pdev: &'a Device<device::Bound>,
    #[pin]
    inner: VfRegistrationData<'a, F>,
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
    /// The returned registration must be embedded in the driver's bus device private data.
    /// The initializer must run as part of the PF driver's probe.
    /// The driver's `unbind` callback and the enclosing data's destructor must keep resources
    /// accessed by VF drivers available until this registration has disabled SR-IOV.
    pub unsafe fn new(
        pdev: &'a Device<device::Core<'_>>,
        data: impl PinInit<F::Of<'a>, Error> + 'a,
    ) -> impl PinInit<Self, Error> + 'a {
        let pdev: &'a Device<device::Bound> = pdev;
        try_pin_init!(Self {
            _: {
                if pdev.is_virtfn() {
                    return Err(ENODEV);
                }
            },
            pdev,
            inner <- VfRegistrationData::new(data),
            _pin: PhantomPinned,
            _: {
                // Check after initialization in case it registered another object for this PF.
                // SAFETY: The caller runs this initializer during PF probing, before VF
                // configuration callbacks can run.
                if unsafe { bindings::pci_num_vf(pdev.as_raw()) } != 0 {
                    return Err(EBUSY);
                }

                // SAFETY: This initializer runs during PF probing, and no VFs are enabled.
                if !unsafe { (*pdev.as_raw()).vf_registration_data_rust }.is_null() {
                    return Err(EBUSY);
                }

                // SAFETY: The data is initialized and pinned, the slot is unoccupied, and
                // no VF can access it yet. No fallible work follows publication.
                unsafe {
                    (*pdev.as_raw()).vf_registration_data_rust =
                        core::ptr::from_ref(inner.as_ref().get_ref()).cast_mut().cast();
                }
            },
        })
    }
}

#[pinned_drop]
impl<F: ForLt + 'static> PinnedDrop for VfRegistration<'_, F> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: `pci_disable_sriov()` is safe to call on any `pci_dev`; it
        // is a no-op if the device has no VFs enabled. When VFs are enabled,
        // this blocks until all VF `remove()` callbacks complete.
        unsafe { bindings::pci_disable_sriov(self.pdev.as_raw()) };

        // SAFETY: The device is valid and all VF remove callbacks have completed, so no VF
        // can access the registration pointer. The data remains alive until this returns.
        unsafe { (*self.pdev.as_raw()).vf_registration_data_rust = core::ptr::null_mut() };
    }
}

// SAFETY: The inner data is `Send` (enforced by the bound), and `&Device` is `Send + Sync`.
unsafe impl<F: ForLt> Send for VfRegistration<'_, F> where for<'a> F::Of<'a>: Send {}

// SAFETY: The inner data is `Send + Sync`. `VfRegistration` doesn't expose mutable access;
// VF drivers only read the data through an immutable pinned reference.
unsafe impl<F: ForLt> Sync for VfRegistration<'_, F> where for<'a> F::Of<'a>: Send + Sync {}

impl Device<device::Bound> {
    /// Internal helper: reads the `vf_registration_data_rust` pointer from the
    /// PF, checks the `TypeId`, and returns a pinned reference.
    ///
    /// # Safety
    ///
    /// The data lifetime must be hidden behind a higher-ranked closure independently of the
    /// reference lifetime, or `F` must be covariant in its encoded lifetime.
    unsafe fn vf_registration_data_pinned<F: ForLt + 'static>(&self) -> Result<Pin<&F::Of<'_>>> {
        if !self.is_virtfn() {
            return Err(ENODEV);
        }

        // SAFETY: This bound VF uses the `physfn` union field. PCI retains its PF until
        // VF removal completes, and the PF keeps the registration installed until then.
        let ptr = unsafe {
            let pf = (*self.as_raw()).__bindgen_anon_1.physfn;
            (*pf).vf_registration_data_rust
        };

        if ptr.is_null() {
            return Err(ENOENT);
        }

        // SAFETY: The registration keeps its data installed until VF removal completes,
        // including when probe initialization rolls back.
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
    /// The reference is borrowed from this VF, while the registration data's lifetime remains
    /// abstract so the closure cannot store shorter-lived references in invariant data.
    pub fn vf_registration_data_with<'this, F: ForLt + 'static, R>(
        &'this self,
        f: impl for<'a> FnOnce(Pin<&'this F::Of<'a>>) -> R,
    ) -> Result<R> {
        // SAFETY: The closure is higher-ranked over the data lifetime, independently of `'this`.
        // It cannot insert shorter-lived references, and the outer borrow cannot outlive this VF.
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
