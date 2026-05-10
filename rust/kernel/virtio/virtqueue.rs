// SPDX-License-Identifier: GPL-2.0

//! Virtqueue functionality.
//!
//! # Discovering virtqueues
//!
//! Inside your driver's [`kernel::virtio::Driver::probe`] method, call
//! [`kernel::virtio::Device::find_vqs`] method with your [`VirtqueueInfo`] struct.
//!
//! # Passing data to virtqueues
//!
//! Create your data as owned [`SGTable`] with:
//!
//! - [`Virtqueue::new_readable_sgtable`] for data that can be read from the device, and
//! - [`Virtqueue::new_writable_sgtable`] for data that can be written from the device
//!
//! These methods will make sure to create the scatter-gather tables and DMA map them to the
//! appropriate VIRTIO transport.
//!
//! To add the tables to the virtqueue, call [`Virtqueue::add_sgs`].

use crate::{
    alloc::{
        allocator::VmallocPageIter,
        Flags, //
    },
    bindings,
    device::Bound,
    dma::DataDirection,
    error::{
        code::{
            EINVAL,
            ENOENT, //
        },
        to_result,
        Error,
        Result, //
    },
    page::AsPageIter,
    prelude::*,
    scatterlist::{
        Owned,
        SGTable, //
    },
    str::{
        self,
        CStr, //
    },
    types::Opaque,
    virtio::Device, //
};

use core::{
    ptr::NonNull, //
};

/// Info for a virtqueue.
///
/// [`struct virtqueue_info`]: srctree/include/linux/virtio_config.h
#[doc(alias = "virtqueue_info")]
#[repr(transparent)]
pub struct VirtqueueInfo(Opaque<bindings::virtqueue_info>);

impl VirtqueueInfo {
    #[inline]
    /// Create a new [`VirtqueueInfo`]
    pub const fn new(
        name: &'static CStr,
        ctx: bool,
        callback: Option<unsafe extern "C" fn(*mut bindings::virtqueue)>,
    ) -> Self {
        Self(Opaque::new(bindings::virtqueue_info {
            name: str::as_char_ptr_in_const_context(name),
            ctx,
            callback,
        }))
    }
}

/// A container for discovered virtqueues returned by [`Device::find_vqs`] method.
///
/// This type dereferences to a `NonNull<Virtqueue>` slice.
///
/// It deletes the virtqueues when dropped.
pub struct Virtqueues {
    pub(crate) inner: KVec<NonNull<Virtqueue>>,
}

impl Drop for Virtqueues {
    fn drop(&mut self) {
        let inner = core::mem::take(&mut self.inner);
        let Some(first) = inner.into_iter().next() else {
            return;
        };
        let first_ref = unsafe { first.as_ref() };
        let Ok(vdev) = first_ref.dev() else {
            return;
        };
        vdev.del_vqs();
    }
}

impl core::ops::Deref for Virtqueues {
    type Target = [NonNull<Virtqueue>];

    #[inline]
    fn deref(&self) -> &Self::Target {
        &self.inner
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
    ///
    /// This method returns a pointer to the `token` value passed in [`Virtqueue::add_sgs`] method
    /// and the amount of bytes that were written by the device.
    #[inline]
    #[doc(alias = "virtqueue_get_buf")]
    pub fn get_buf(&'_ self) -> Option<(NonNull<u8>, u32)> {
        let mut len = 0;
        // SAFETY: the pointer has been promised to be valid when self was created
        let ptr = unsafe { bindings::virtqueue_get_buf(self.as_raw(), &mut len) };
        Some((NonNull::new(ptr.cast())?, len))
    }

    /// Add a list of scatter-gather lists to virtqueue.
    #[inline]
    #[doc(alias = "virtqueue_add_sgs")]
    pub fn add_sgs<'token, PIn, POut, Token>(
        &'_ self,
        out_sgs: &'token SGTableReadable<POut>,
        in_sgs: &'token SGTableWritable<PIn>,
        token: Pin<&'token Token>,
        gfp: Flags,
    ) -> Result
    where
        for<'a> PIn: AsPageIter<Iter<'a> = VmallocPageIter<'a>> + 'static,
        for<'a> POut: AsPageIter<Iter<'a> = VmallocPageIter<'a>> + 'static,
    {
        let out_sgs_num = u32::try_from(out_sgs.inner.iter().count())?;
        let in_sgs_num = u32::try_from(in_sgs.inner.iter().count())?;

        let Some(total_size) = out_sgs_num.checked_add(in_sgs_num) else {
            return Err(EINVAL);
        };

        let mut sgs = KVec::with_capacity(2, GFP_KERNEL)?;

        for entry in out_sgs.inner.iter() {
            sgs.push(entry, GFP_KERNEL)?;
        }
        for entry in in_sgs.inner.iter() {
            sgs.push(entry, GFP_KERNEL)?;
        }

        if usize::try_from(total_size) != Ok(sgs.len()) {
            return Err(EINVAL);
        }
        // SAFETY: `self` has been promised to be valid when self was created
        to_result(unsafe {
            bindings::virtqueue_add_sgs(
                self.as_raw(),
                sgs.as_ptr().cast_mut().cast(),
                out_sgs_num,
                in_sgs_num,
                NonNull::new(core::ptr::from_ref::<Token>(&*token.as_ref()).cast_mut())
                    .unwrap()
                    .as_ptr()
                    .cast(),
                gfp.as_raw(),
            )
        })
    }

    /// Create a scatter-gather table readable by the device.
    pub fn new_readable_sgtable<P>(
        &self,
        pages: P,
        flags: Flags,
    ) -> impl PinInit<SGTableReadable<P>, Error> + '_
    where
        for<'a> P: AsPageIter<Iter<'a> = VmallocPageIter<'a>> + 'static,
    {
        pin_init!(SGTableReadable {
            inner <- SGTable::new(
                self.dev().unwrap().as_ref().parent().unwrap(),
                pages,
                DataDirection::ToDevice,
                flags,
            ),
        }? Error)
    }

    /// Create a scatter-gather table writable by the device.
    pub fn new_writable_sgtable<P>(
        &self,
        pages: P,
        flags: Flags,
    ) -> impl PinInit<SGTableWritable<P>, Error> + '_
    where
        for<'a> P: AsPageIter<Iter<'a> = VmallocPageIter<'a>> + 'static,
    {
        pin_init!(SGTableWritable {
            inner <- SGTable::new(
                self.dev().unwrap().as_ref().parent().unwrap(),
                pages,
                DataDirection::FromDevice,
                flags,
            ),
        }? Error)
    }
}

/// An [`SGTable<Owned<P>>`] that is guaranteed to have been DMA-mapped as device-readable.
///
/// Created by [`Virtqueue::new_readable_sgtable`].
#[pin_data]
pub struct SGTableReadable<P> {
    #[pin]
    inner: SGTable<Owned<P>>,
}

/// An [`SGTable<Owned<P>>`] that is guaranteed to have been DMA-mapped as device-writable.
///
/// Created by [`Virtqueue::new_writable_sgtable`].
#[pin_data]
pub struct SGTableWritable<P> {
    #[pin]
    inner: SGTable<Owned<P>>,
}
