// SPDX-License-Identifier: GPL-2.0

//! Virtqueue functionality.

use crate::{
    alloc::{
        Flags, //
    },
    bindings,
    device::Bound,
    error::{
        code::{
            EINVAL,
            ENOENT, //
        },
        to_result,
        Result, //
    },
    scatterlist::SGEntry,
    str::CStr,
    types::Opaque,
    virtio::Device, //
};

use core::{
    ffi::c_uint,
    ptr::NonNull, //
};

/// Info for a virtqueue.
///
/// [`struct virtqueue_info`]: srctree/include/linux/virtio_config.h
#[doc(alias = "virtqueue_info")]
#[repr(transparent)]
pub struct VirtqueueInfo(Opaque<bindings::virtqueue_info>);

impl VirtqueueInfo {
    /// Create a new [`VirtqueueInfo`]
    pub const fn new(
        name: &'static CStr,
        ctx: bool,
        callback: Option<unsafe extern "C" fn(*mut bindings::virtqueue)>,
    ) -> Self {
        Self(Opaque::new(bindings::virtqueue_info {
            name: name.as_ptr(),
            ctx,
            callback,
        }))
    }
}

/// An opaque handler for a virtqueue.
///
/// [`struct virtqueue`]: srctree/include/linux/virtio.h
#[repr(transparent)]
pub struct Virtqueue(Opaque<bindings::virtqueue>);

impl Virtqueue {
    /// Create a [`Virtqueue`] from a raw pointer.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is a properly initialized valid `virtqueue` pointer.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::virtqueue) -> &'a Self {
        // SAFETY: The safety requirements of this function guarantee that `ptr` is a valid
        // pointer to a `struct virtqueue` for the duration of `'a`.
        unsafe { &*ptr.cast() }
    }

    /// Obtain the raw `struct virtqueue *`.
    #[inline]
    pub(crate) fn as_raw(&self) -> *mut bindings::virtqueue {
        self.0.get()
    }

    /// Get the [`Device`] associated with this virtqueue.
    #[inline]
    pub fn dev(&self) -> Result<&Device<Bound>> {
        // SAFETY: By the type invariants, `self.as_raw()` is a valid pointer to a `struct
        // virtqueue`.
        if unsafe { (*self.as_raw()).vdev }.is_null() {
            return Err(ENOENT);
        }
        // SAFETY: the pointer has been promised to be valid when self was created
        Ok(unsafe { &*(&*self.as_raw()).vdev.cast::<Device<Bound>>() })
    }

    /// Get the vring size.
    #[inline]
    #[doc(alias = "virtqueue_get_vring_size")]
    pub fn vring_size(&self) -> u32 {
        // SAFETY: the pointer has been promised to be valid when self was created
        unsafe { bindings::virtqueue_get_vring_size(self.as_raw()) }
    }

    /// Notify virtqueue.
    #[inline]
    #[doc(alias = "virtqueue_notify")]
    pub fn notify(&self) -> bool {
        // SAFETY: the pointer has been promised to be valid when self was created
        unsafe { bindings::virtqueue_notify(self.as_raw()) }
    }

    /// Kick and prepare virtqueue.
    #[inline]
    #[doc(alias = "virtqueue_kick_prepare")]
    pub fn kick_prepare(&self) -> bool {
        // SAFETY: the pointer has been promised to be valid when self was created
        unsafe { bindings::virtqueue_kick_prepare(self.as_raw()) }
    }

    /// Kick virtqueue.
    #[inline]
    #[doc(alias = "virtqueue_kick")]
    pub fn kick(&self) -> bool {
        // SAFETY: the pointer has been promised to be valid when self was created
        unsafe { bindings::virtqueue_kick(self.as_raw()) }
    }

    /// Enable virtqueue's callback.
    #[inline]
    #[doc(alias = "virtqueue_enable_cb")]
    pub fn enable_cb(&self) -> bool {
        // SAFETY: the pointer has been promised to be valid when self was created
        unsafe { bindings::virtqueue_enable_cb(self.as_raw()) }
    }

    /// Disable virtqueue's callback.
    #[inline]
    #[doc(alias = "virtqueue_disable_cb")]
    pub fn disable_cb(&self) {
        // SAFETY: the pointer has been promised to be valid when self was created
        unsafe { bindings::virtqueue_disable_cb(self.as_raw()) }
    }

    /// Get a buffer from the virtqueue, if available.
    #[inline]
    #[doc(alias = "virtqueue_get_buf")]
    pub fn get_buf(&'_ self) -> Option<(NonNull<u8>, u32)> {
        let mut len = 0;
        // SAFETY: the pointer has been promised to be valid when self was created
        let ptr = unsafe { bindings::virtqueue_get_buf(self.as_raw(), &mut len) };
        Some((NonNull::new(ptr.cast())?, len))
    }

    /// Make a device write-only buffer available.
    ///
    /// # Safety
    ///
    /// Callers must ensure:
    ///
    /// - the data pointed to by `sg` and `token` will live as long as the buffer lives in the
    ///   virtqueue
    /// - the driver deallocates any data pointed to by `sg` and `token` on error
    /// - the driver deallocates any data pointed to by `sg` and `token` when it receives a
    ///   virtqueue response
    /// - the driver deallocates any data pointed to by `sg` and `token` when it resets the device
    #[inline]
    #[doc(alias = "virtqueue_add_inbuf")]
    pub unsafe fn add_inbuf(&'_ self, sg: &SGEntry, token: NonNull<u8>, gfp: Flags) -> Result {
        // SAFETY: the pointer has been promised to be valid when self was created
        to_result(unsafe {
            bindings::virtqueue_add_inbuf(
                self.as_raw(),
                sg.as_raw(),
                1,
                token.as_ptr().cast(),
                gfp.as_raw(),
            )
        })
    }

    /// Make a device read-only buffer available.
    ///
    /// # Safety
    ///
    /// Callers must ensure:
    ///
    /// - the data pointed to by `sg` and `token` will live as long as the buffer lives in the
    ///   virtqueue
    /// - the driver deallocates any data pointed to by `sg` and `token` on error
    /// - the driver deallocates any data pointed to by `sg` and `token` when it receives a
    ///   virtqueue response
    /// - the driver deallocates any data pointed to by `sg` and `token` when it resets the device
    #[inline]
    #[doc(alias = "virtqueue_add_outbuf")]
    pub unsafe fn add_outbuf(&'_ self, sg: &SGEntry, token: NonNull<u8>, gfp: Flags) -> Result {
        // SAFETY: the pointer has been promised to be valid when self was created
        to_result(unsafe {
            bindings::virtqueue_add_outbuf(
                self.as_raw(),
                sg.as_raw(),
                1,
                token.as_ptr().cast(),
                gfp.as_raw(),
            )
        })
    }

    /// Add a list of scatter-gather lists to virtqueue.
    ///
    /// # Safety
    ///
    /// Callers must ensure:
    ///
    /// - the data pointed to by `sgs` and `token` will live as long as the buffer lives in the
    ///   virtqueue
    /// - the driver deallocates any data pointed to by `sgs` and `token` on error
    /// - the driver deallocates any data pointed to by `sgs` and `token` when it receives a
    ///   virtqueue response
    /// - the driver deallocates any data pointed to by `sgs` and `token` when it resets the device
    #[inline]
    #[doc(alias = "virtqueue_add_sgs")]
    pub unsafe fn add_sgs(
        &'_ self,
        sgs: &[&SGEntry],
        out_sgs: c_uint,
        in_sgs: c_uint,
        token: NonNull<u8>,
        gfp: Flags,
    ) -> Result {
        let Some(total_size) = out_sgs.checked_add(in_sgs) else {
            return Err(EINVAL);
        };
        if usize::try_from(total_size) != Ok(sgs.len()) {
            return Err(EINVAL);
        }
        // SAFETY: the pointer has been promised to be valid when self was created
        to_result(unsafe {
            bindings::virtqueue_add_sgs(
                self.as_raw(),
                sgs.as_ptr().cast_mut().cast(),
                out_sgs,
                in_sgs,
                token.as_ptr().cast(),
                gfp.as_raw(),
            )
        })
    }
}
