// SPDX-License-Identifier: GPL-2.0

//! Devres abstraction
//!
//! [`Devres`] represents an abstraction for the kernel devres (device resource management)
//! implementation.

use crate::{
    alloc::Flags,
    bindings,
    device::Device,
    error::{Error, Result},
    prelude::*,
    revocable::Revocable,
    types::ARef,
};

use core::ffi::c_void;
use core::ops::Deref;

#[pin_data]
struct DevresInner<T> {
    dev: ARef<Device>,
    #[pin]
    data: Revocable<T>,
}

/// This abstraction is meant to be used by subsystems to containerize [`Device`] bound resources to
/// manage their lifetime.
///
/// [`Device`] bound resources should be freed when either the resource goes out of scope or the
/// [`Device`] is unbound respectively, depending on what happens first.
///
/// To achieve that [`Devres`] registers a devres callback on creation, which is called once the
/// [`Device`] is unbound, revoking access to the encapsulated resource (see also [`Revocable`]).
///
/// After the [`Devres`] has been unbound it is not possible to access the encapsulated resource
/// anymore.
///
/// [`Devres`] users should make sure to simply free the corresponding backing resource in `T`'s
/// [`Drop`] implementation.
///
/// # Example
///
/// ```
/// use kernel::devres::Devres;
///
/// // See also [`pci::Bar`] for a real example.
/// struct IoRemap(IoMem);
///
/// impl IoRemap {
///     fn new(usize paddr, usize len) -> Result<Self>{
///         // assert success
///         let addr = unsafe { bindings::ioremap(paddr as _); };
///         let iomem = IoMem::new(addr, len)?;
///
///         Ok(IoRemap(iomem))
///     }
/// }
///
/// impl Drop for IoRemap {
///     fn drop(&mut self) {
///         unsafe { bindings::iounmap(self.0.ioptr as _); };
///     }
/// }
///
/// impl Deref for IoRemap {
///    type Target = IoMem;
///
///    fn deref(&self) -> &Self::Target {
///        &self.0
///    }
/// }
///
/// let devres = Devres::new(dev, IoRemap::new(0xBAAAAAAD, 0x4)?, GFP_KERNEL)?;
///
/// let res = devres.try_access().ok_or(ENXIO)?;
/// res.writel(0xBAD);
/// ```
///
pub struct Devres<T> {
    inner: Pin<Box<DevresInner<T>>>,
    callback: unsafe extern "C" fn(*mut c_void),
}

impl<T> DevresInner<T> {
    fn as_ptr(&self) -> *const DevresInner<T> {
        self as *const DevresInner<T>
    }

    fn as_cptr(&self) -> *mut c_void {
        self.as_ptr() as *mut c_void
    }
}

unsafe extern "C" fn devres_callback<T>(inner: *mut c_void) {
    let inner = inner as *const DevresInner<T>;
    let inner = unsafe { &*inner };

    inner.data.revoke();
}

impl<T> Devres<T> {
    /// Creates a new [`Devres`] instance of the give data.
    pub fn new(dev: ARef<Device>, data: T, flags: Flags) -> Result<Self> {
        let callback = devres_callback::<T>;

        let inner = Box::pin_init(
            pin_init!( DevresInner {
                dev: dev,
                data <- Revocable::new(data),
            }),
            flags,
        )?;

        let ret = unsafe {
            bindings::devm_add_action(inner.dev.as_raw(), Some(callback), inner.as_cptr())
        };

        if ret != 0 {
            return Err(Error::from_errno(ret));
        }

        // We have to store the exact callback function pointer used with
        // `bindings::devm_add_action` for `bindings::devm_remove_action`. There compiler might put
        // multiple definitions of `devres_callback<T>` for the same `T` in both the kernel itself
        // and modules. Hence, we might see different pointer values depending on whether we look
        // at `devres_callback<T>`'s address from `Devres::new` or `Devres::drop`.
        Ok(Devres { inner, callback })
    }
}

impl<T> Deref for Devres<T> {
    type Target = Revocable<T>;

    fn deref(&self) -> &Self::Target {
        &self.inner.data
    }
}

impl<T> Drop for Devres<T> {
    fn drop(&mut self) {
        unsafe {
            bindings::devm_remove_action(
                self.inner.dev.as_raw(),
                Some(self.callback),
                self.inner.as_cptr(),
            )
        }
    }
}
