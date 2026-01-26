// SPDX-License-Identifier: GPL-2.0

//! Framebuffer device.
//!
//! This module provides the core abstractions for framebuffer device management.
//!
//! C header: [`include/linux/fb.h`]

use crate::{
    bindings, device,
    error::from_err_ptr,
    fb,
    fb::driver::Operations,
    fs::file,
    mm,
    prelude::*,
    sync::{
        aref::{ARef, AlwaysRefCounted},
        Refcount,
    },
    types::Opaque,
};
use core::{
    ffi::{c_int, c_uint},
    marker::PhantomData,
    mem,
    ops::Deref,
    ptr,
    ptr::NonNull,
};

/// A typed framebuffer device with a specific `fb::Driver` implementation.
///
/// # Invariants
///
/// A [`Device`] instance represents a valid `struct fb_info` created by the C portion of the kernel.
///
/// - `self.0` is a valid pointer to a `struct fb_info`.
/// - The `fb_info.par` field points to a valid `T::Data` instance.
/// - The `fb_info.fbops` field always points to `Self::FBOPS` and is never null.
///
/// Instances of this type are always reference-counted.
#[repr(transparent)]
pub struct Device<T: fb::Driver>(Opaque<bindings::fb_info>, PhantomData<T>);

impl<T: fb::Driver> Device<T> {
    /// Returns a reference to the underlying C `struct fb_info`.
    #[inline]
    fn as_ref(&self) -> &bindings::fb_info {
        // SAFETY: By the type invariant, the pointer stored in `self` is valid.
        unsafe { &*self.as_raw() }
    }

    /// Returns the variable screen info.
    pub fn var(&self) -> &fb::VarScreenInfo {
        // SAFETY: `var` is a valid field of `fb_info` and remains valid for the lifetime of `self`.
        unsafe { fb::VarScreenInfo::from_raw(&self.as_ref().var) }
    }

    /// Returns the fixed screen info.
    pub fn fix(&self) -> &fb::FixScreenInfo {
        // SAFETY: `fix` is a valid field of `fb_info` and remains valid for the lifetime of `self`.
        unsafe { fb::FixScreenInfo::from_raw(&self.as_ref().fix) }
    }

    /// Returns the screen base address.
    pub fn screen_base(&self) -> *mut u8 {
        // SAFETY: `screen_base` is a union field accessed via the generated union.
        unsafe { self.as_ref().__bindgen_anon_1.screen_base as *mut u8 }
    }

    /// Returns the screen size.
    pub fn screen_size(&self) -> usize {
        self.as_ref().screen_size as usize
    }

    /// Returns the pseudo palette pointer.
    pub fn pseudo_palette(&self) -> *mut core::ffi::c_void {
        self.as_ref().pseudo_palette
    }

    /// Returns the framebuffer device node number.
    ///
    /// This is the device node identifier assigned by the framebuffer subsystem
    /// when the device is registered (e.g., 0 for /dev/fb0, 1 for /dev/fb1, etc.).
    pub fn node(&self) -> i32 {
        self.as_ref().node
    }

    /// Configures the fixed screen information.
    ///
    /// # Safety
    ///
    /// The caller must ensure that:
    /// - The device is not yet registered with the framebuffer subsystem
    /// - The provided configuration is valid for this device
    pub unsafe fn configure_fix<F>(&self, f: F)
    where
        F: FnOnce(&mut bindings::fb_fix_screeninfo),
    {
        // SAFETY: By the safety requirements, we have exclusive access to the device during
        // initialization, before registration.
        let info_ptr = self.as_raw();
        unsafe {
            f(&mut (*info_ptr).fix);
        }
    }

    /// Configures the variable screen information.
    ///
    /// # Safety
    ///
    /// The caller must ensure that:
    /// - The device is not yet registered with the framebuffer subsystem
    /// - The provided configuration is valid for this device
    pub unsafe fn configure_var<F>(&self, f: F)
    where
        F: FnOnce(&mut bindings::fb_var_screeninfo),
    {
        // SAFETY: By the safety requirements, we have exclusive access to the device during
        // initialization, before registration.
        let info_ptr = self.as_raw();
        unsafe {
            f(&mut (*info_ptr).var);
        }
    }

    /// Sets the screen base address.
    ///
    /// # Safety
    ///
    /// The caller must ensure that:
    /// - The device is not yet registered with the framebuffer subsystem
    /// - The address points to valid, mapped framebuffer memory
    /// - The memory remains valid for the lifetime of the device
    pub unsafe fn set_screen_base(&self, addr: *mut u8) {
        // SAFETY: By the safety requirements, we have exclusive access during initialization.
        let info_ptr = self.as_raw();
        unsafe {
            (*info_ptr).__bindgen_anon_1.screen_base = addr;
        }
    }

    /// Sets the pseudo palette pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that:
    /// - The device is not yet registered with the framebuffer subsystem
    /// - The pointer points to valid memory with sufficient size
    /// - The memory remains valid for the lifetime of the device
    pub unsafe fn set_pseudo_palette(&self, palette: *mut core::ffi::c_void) {
        // SAFETY: By the safety requirements, we have exclusive access during initialization.
        let info_ptr = self.as_raw();
        unsafe {
            (*info_ptr).pseudo_palette = palette;
        }
    }

    /// Callback for reading from framebuffers with non-linear layouts.
    extern "C" fn read_callback(
        info: *mut bindings::fb_info,
        buf: *mut core::ffi::c_char,
        count: usize,
        ppos: *mut bindings::loff_t,
    ) -> isize {
        let device = unsafe { Self::from_raw(info) };
        // SAFETY: C code ensures `buf` and `ppos` are valid. `buf` is a valid buffer pointer with
        // `count` bytes, and `ppos` is a valid `file::Offset` pointer with exclusive access.
        let pos: &mut file::Offset = unsafe { &mut *ppos };
        let result = T::Ops::read(
            &device,
            unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, count) },
            pos,
        );
        match result {
            Ok(n) => n as isize,
            Err(e) => -(e.to_errno() as isize),
        }
    }

    /// Callback for writing to framebuffers with non-linear layouts.
    extern "C" fn write_callback(
        info: *mut bindings::fb_info,
        buf: *const core::ffi::c_char,
        count: usize,
        ppos: *mut bindings::loff_t,
    ) -> isize {
        let device = unsafe { Self::from_raw(info) };
        // SAFETY: C code ensures `buf` and `ppos` are valid. `buf` is a valid buffer pointer with
        // `count` bytes, and `ppos` is a valid `file::Offset` pointer with exclusive access.
        let pos: &mut file::Offset = unsafe { &mut *ppos };
        let result = T::Ops::write(
            &device,
            unsafe { core::slice::from_raw_parts(buf as *const u8, count) },
            pos,
        );
        match result {
            Ok(n) => n as isize,
            Err(e) => -(e.to_errno() as isize),
        }
    }

    /// Callback for setting color registers.
    extern "C" fn setcolreg_callback(
        regno: c_uint,
        red: c_uint,
        green: c_uint,
        blue: c_uint,
        transp: c_uint,
        info: *mut bindings::fb_info,
    ) -> c_int {
        let device = unsafe { Self::from_raw(info) };
        let result = T::Ops::setcolreg(&device, regno, red, green, blue, transp);
        match result {
            Ok(()) => 0,
            Err(e) => e.to_errno() as c_int,
        }
    }

    /// Callback for filling a rectangle.
    extern "C" fn fillrect_callback(
        info: *mut bindings::fb_info,
        rect: *const bindings::fb_fillrect,
    ) {
        let device = unsafe { Self::from_raw(info) };
        // SAFETY: C code ensures `rect` is valid and points to a properly initialized `fb_fillrect`.
        let rect = unsafe { fb::FillRect::from_raw(*rect) };
        T::Ops::fillrect(&device, &rect);
    }

    /// Callback for copying an area.
    extern "C" fn copyarea_callback(
        info: *mut bindings::fb_info,
        area: *const bindings::fb_copyarea,
    ) {
        let device = unsafe { Self::from_raw(info) };
        // SAFETY: C code ensures `area` is valid and points to a properly initialized `fb_copyarea`.
        let area = unsafe { fb::CopyArea::from_raw(*area) };
        T::Ops::copyarea(&device, &area);
    }

    /// Callback for blitting an image.
    extern "C" fn imageblit_callback(
        info: *mut bindings::fb_info,
        image: *const bindings::fb_image,
    ) {
        let device = unsafe { Self::from_raw(info) };
        // SAFETY: C code ensures `image` is valid and points to a properly initialized `fb_image`.
        let image = unsafe { fb::Image::from_raw(*image) };
        T::Ops::imageblit(&device, &image);
    }

    /// Callback for memory mapping the framebuffer.
    extern "C" fn mmap_callback(
        info: *mut bindings::fb_info,
        vma: *mut bindings::vm_area_struct,
    ) -> c_int {
        let device = unsafe { Self::from_raw(info) };
        // SAFETY: The caller provides a `vma` that is undergoing initial VMA setup.
        let area = unsafe { mm::virt::VmaNew::from_raw(vma) };
        let result = T::Ops::mmap(&device, area);
        match result {
            Ok(()) => 0,
            Err(e) => e.to_errno() as c_int,
        }
    }

    /// Callback for destroying the framebuffer device.
    ///
    /// Performs cleanup in the correct order: driver resources, driver data, and finally
    /// the fb_info structure itself.
    extern "C" fn destroy_callback(info: *mut bindings::fb_info) {
        let device = unsafe { Self::from_raw(info) };

        // First, let the driver clean up its own resources (iounmap, release_mem_region, etc.)
        T::Ops::destroy(&device);

        // Get the pointer to the driver data (stored in `info->par`).
        // SAFETY: `info` is valid and was allocated by `framebuffer_alloc` in `Device::new()`.
        let par_ptr = unsafe { (*info).par };
        if !par_ptr.is_null() {
            // Manually call `Drop` for the driver data before `framebuffer_release`, since
            // `framebuffer_release` will `kfree` the entire `fb_info` structure (including `par`),
            // and `kfree` doesn't call Rust's `Drop`.
            // SAFETY: `par_ptr` points to a valid `T::Data` instance that was initialized in
            // `Device::new()`. This is the last access to the data before it's freed.
            unsafe {
                core::ptr::drop_in_place(par_ptr.cast::<T::Data>());
            }
        }

        // Release the `fb_info` structure that was allocated by `framebuffer_alloc`.
        // SAFETY: `info` is valid and was allocated by `framebuffer_alloc` in `Device::new()`.
        unsafe {
            bindings::framebuffer_release(info);
        }
    }

    /// Static `fb_ops` table for this driver type.
    ///
    /// This table is shared by all instances of this driver type.
    const FBOPS: bindings::fb_ops = bindings::fb_ops {
        owner: core::ptr::null_mut(),
        fb_open: None,
        fb_release: None,
        fb_read: Some(Self::read_callback),
        fb_write: Some(Self::write_callback),
        fb_check_var: None,
        fb_set_par: None,
        fb_setcolreg: Some(Self::setcolreg_callback),
        fb_setcmap: None,
        fb_blank: None,
        fb_pan_display: None,
        fb_fillrect: Some(Self::fillrect_callback),
        fb_copyarea: Some(Self::copyarea_callback),
        fb_imageblit: Some(Self::imageblit_callback),
        fb_cursor: None,
        fb_sync: None,
        fb_ioctl: None,
        fb_compat_ioctl: None,
        fb_mmap: Some(Self::mmap_callback),
        fb_get_caps: None,
        fb_destroy: Some(Self::destroy_callback),
        fb_debug_enter: None,
        fb_debug_leave: None,
    };

    /// Creates a new `fb::Device` for a `fb::Driver`.
    ///
    /// The C `framebuffer_alloc` function allocates memory as:
    /// `[fb_info][padding][driver_data]`
    ///
    /// `Device<T>` is `#[repr(transparent)]` around `Opaque<fb_info>`, so a `Device<T>` pointer
    /// is actually just an `fb_info` pointer. The driver data `T::Data` is stored in `info->par`.
    pub fn new(dev: &device::Device, data: impl PinInit<T::Data, Error>) -> Result<ARef<Self>> {
        let data_size = mem::size_of::<T::Data>();

        // SAFETY: `dev.as_raw()` is valid by its type invariants.
        let raw_info = unsafe { bindings::framebuffer_alloc(data_size, dev.as_raw()) };

        let raw_info = NonNull::new(from_err_ptr(raw_info)?).ok_or(ENOMEM)?;

        // SAFETY: `raw_info` is valid and non-null.
        let par_ptr = unsafe { (*raw_info.as_ptr()).par };
        if par_ptr.is_null() && data_size > 0 {
            // SAFETY: We just allocated this, so it's safe to free.
            unsafe { bindings::framebuffer_release(raw_info.as_ptr()) };
            return Err(ENOMEM);
        }

        // Cast `par` to our data type pointer.
        // SAFETY: `framebuffer_alloc` allocated enough space for `T::Data`.
        let data_ptr = par_ptr.cast::<T::Data>();

        // Initialize the data.
        // SAFETY: `data_ptr` is a valid pointer to uninitialized memory of the correct size, and
        // will not move until it is dropped.
        if let Err(e) = unsafe { data.__pinned_init(data_ptr) } {
            // SAFETY: We just allocated this, so it's safe to free.
            unsafe { bindings::framebuffer_release(raw_info.as_ptr()) };
            return Err(e);
        }

        // Set up `fb_ops`.
        // SAFETY: `raw_info` is valid.
        unsafe {
            (*raw_info.as_ptr()).fbops = &Self::FBOPS;
        }

        // Initialize refcount to 1 for the `ARef` we're about to return.
        // SAFETY: `raw_info` is valid and points to a properly allocated `fb_info`.
        let device_ref = unsafe { &*raw_info.cast::<Self>().as_ptr() };
        // SAFETY: `device_ref` is valid and points to a properly allocated `fb_info`.
        unsafe {
            device_ref.refcount().set(1);
        }

        // SAFETY: We've initialized the refcount to 1, and we're taking ownership of it.
        // `Device<T>` is `#[repr(transparent)]` around `Opaque<fb_info>`, so this cast is valid.
        Ok(unsafe { ARef::from_raw(raw_info.cast::<Self>()) })
    }

    pub(crate) fn as_raw(&self) -> *mut bindings::fb_info {
        self.0.get()
    }

    /// Returns a reference to the refcount field of the `fb_info`.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `self.as_raw()` is valid.
    unsafe fn refcount(&self) -> &Refcount {
        // SAFETY: `Refcount` is a transparent wrapper around `refcount_t`, and `count` is the
        // first field of `fb_info`. By the safety requirements, `self.as_raw()` is valid.
        unsafe {
            let count_ptr = ptr::addr_of_mut!((*self.as_raw()).count);
            &*count_ptr.cast::<Refcount>()
        }
    }

    /// Creates a reference from a raw `fb_info` pointer.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null, and points to an `fb_info`
    /// that was created by `Device::<T>::new()`.
    pub(crate) unsafe fn from_raw<'a>(ptr: *const bindings::fb_info) -> &'a Self {
        // SAFETY: `Device<T>` is a transparent wrapper around `Opaque<fb_info>`.
        unsafe { &*ptr.cast::<Self>() }
    }

    /// Returns a reference to the driver data stored in `fb_info.par`.
    pub fn data(&self) -> &T::Data {
        // SAFETY: By the type invariant, `info->par` points to a valid `T::Data` instance.
        unsafe {
            let par = (*self.as_raw()).par;
            &*par.cast::<T::Data>()
        }
    }
}

impl<T: fb::Driver> Deref for Device<T> {
    type Target = T::Data;

    fn deref(&self) -> &Self::Target {
        self.data()
    }
}

// SAFETY: Framebuffer device objects are always reference counted and the get/put functions
// satisfy the requirements.
unsafe impl<T: fb::Driver> AlwaysRefCounted for Device<T> {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { self.refcount().inc() };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: By the safety requirements, `obj` is valid.
        let device = unsafe { &*obj.as_ptr() };

        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        if unsafe { device.refcount().dec_and_test() } {
            Self::destroy_callback(device.as_raw());
        }
    }
}

impl<T: fb::Driver> AsRef<device::Device> for Device<T> {
    fn as_ref(&self) -> &device::Device {
        // SAFETY: `fb_info::device` is valid as long as the `fb_info` itself is valid, which is
        // guaranteed by the type invariant.
        unsafe { device::Device::from_raw((*self.as_raw()).device) }
    }
}

// SAFETY: `Device<T>` can be sent to any thread.
unsafe impl<T: fb::Driver> Send for Device<T> {}

// SAFETY: `Device<T>` can be shared among threads because all immutable methods are protected by
// the synchronization in `struct fb_info`.
unsafe impl<T: fb::Driver> Sync for Device<T> {}
