// SPDX-License-Identifier: GPL-2.0

//! Character device support.
//!
//! C headers: [`include/linux/cdev.h`](srctree/include/linux/cdev.h) and
//! [`include/linux/fs.h`](srctree/include/linux/fs.h)

use crate::{
    bindings, container_of,
    error::{to_result, VTABLE_DEFAULT_ERROR},
    fs::{File, LocalFile},
    ioctl::IoctlCommand,
    prelude::*,
    types::{ForeignOwnable, Opaque},
    uaccess::{UserPtr, UserSlice, UserSliceReader, UserSliceWriter},
};
use core::{ffi, mem, ops::Deref};

/// Character device ID.
///
/// This is a wrapper around the kernel's `dev_t` type and identifies a
/// character device by its major and minor numbers.
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(transparent)]
pub struct CharDeviceID(bindings::dev_t); // u32

impl CharDeviceID {
    /// Creates a new device ID from the given major and minor numbers.
    ///
    /// This corresponds to the kernel's `MKDEV` macro.
    pub fn new(major: u32, minor: u32) -> Self {
        // SAFETY: The kernel's `MKDEV` macro is safe to call with any values.
        Self(unsafe { bindings::MKDEV(major, minor) })
    }

    /// Returns the major number of the device ID.
    ///
    /// This corresponds to the kernel's `MAJOR` macro.
    pub fn major(&self) -> u32 {
        // SAFETY: The kernel's `MAJOR` macro is safe to call with any value.
        unsafe { bindings::MAJOR(self.0) }
    }

    /// Returns the minor number of the device ID.
    ///
    /// This corresponds to the kernel's `MINOR` macro.
    pub fn minor(&self) -> u32 {
        // SAFETY: The kernel's `MINOR` macro is safe to call with any value.
        unsafe { bindings::MINOR(self.0) }
    }
}

/// Seek mode for the `llseek` method.
///
/// This enum corresponds to the `SEEK_*` constants in the kernel.
#[repr(u32)]
pub enum Whence {
    /// Set the file position to `offset`.
    Set = bindings::SEEK_SET,
    /// Set the file position to the current position plus `offset`.
    Cur = bindings::SEEK_CUR,
    /// Set the file position to the end of the file plus `offset`.
    End = bindings::SEEK_END,
    /// Set the file position to the next location in the file greater than or
    /// equal to `offset` containing data.
    Data = bindings::SEEK_DATA,
    /// Set the file position to the next hole in the file greater than or
    /// equal to `offset`.
    Hole = bindings::SEEK_HOLE,
}

// Make sure at compile time that the `Whence` enum can be safely converted
// from integers up to `SEEK_MAX`.
const _: () = assert!(Whence::Hole as u32 == bindings::SEEK_MAX);

/// Trait implemented by a registered character device.
///
/// A registered character device just handles the `open` operation on the
/// device file and returns an open device type (which implements the
/// [`OpenCharDevice`] trait) that handles the actual I/O operations on the
/// device file. Optionally, the `release` operation can be implemented to
/// handle the final close of the device file, but simple cleanup can also be
/// done in the `Drop` implementation of the open device type.
///
/// # Example
///
/// ```
/// # use kernel::{
/// #     char_dev::{CharDevice, CharDeviceID, OpenCharDevice},
/// #     fs::{File, LocalFile},
/// #     prelude::*,
/// #     uaccess::UserSliceWriter,
/// # };
/// #
/// struct MyCharDev;
/// struct MyOpenCharDev;
///
/// #[vtable]
/// impl CharDevice for MyCharDev {
///     type OpenPtr = Box<MyOpenCharDev>;
///     type Err = Error;
///
///     fn new(_dev_id: CharDeviceID) -> Result<Self, Self::Err> {
///         Ok(Self)
///     }
///
///     fn open(&self, _file: &File) -> Result<Self::OpenPtr, Self::Err> {
///         pr_info!("Opened device!\n");
///         Box::new(MyOpenCharDev, GFP_KERNEL).map_err(Into::into)
///     }
/// }
///
/// #[vtable]
/// impl OpenCharDevice for MyOpenCharDev {
///     type IoctlCmd = ();
///     type Err = Error;
///
///     fn read(
///         &self,
///         _file: &LocalFile,
///         _buf: UserSliceWriter,
///         _offset: &mut i64,
///     ) -> Result<usize, Self::Err> {
///         pr_info!("Read from device!\n");
///         Ok(0)
///     }
/// }
/// ```
#[vtable]
pub trait CharDevice: Sized + Send + Sync + 'static {
    /// The type of a (smart) pointer to the associated open device type.
    ///
    /// This type must implement the [`ForeignOwnable`] trait with the
    /// associated borrowed type being a reference to the open device type.
    ///
    /// Most likely, this will be a smart pointer type like [`Box`] or [`Arc`],
    /// just wrapping the open device type.
    ///
    /// [`Arc`]: crate::sync::Arc
    type OpenPtr: for<'a> ForeignOwnable<Borrowed<'a>: Deref<Target: OpenCharDevice>>;

    /// The error type returned by the device operations.
    ///
    /// This type must be convertible into the kernel [`Error`] type.
    type Err: Into<Error>;

    /// Creates a new instance of the character device.
    ///
    /// This is called when the device is registered with the kernel. The
    /// registered device ID is passed as the `dev_id` parameter.
    ///
    /// # Errors
    ///
    /// This function can return an error if the device creation fails.
    fn new(dev_id: CharDeviceID) -> Result<Self, Self::Err>;

    /// Handles the `open` operation on the device file.
    ///
    /// This is called when the device file is opened. The `file` parameter
    /// contains a reference to the file object representing the opened file.
    ///
    /// The function should return a pointer to an open device type that
    /// implements the [`OpenCharDevice`] trait. This type will handle the
    /// actual I/O operations on the opened device file. The returned pointer
    /// will internally be written to the `private_data` field of the file
    /// object. If the release operation is implemented, you are guaranteed to
    /// receive this exact pointer as an argument when the final close of the
    /// device file happens.
    ///
    /// # Errors
    ///
    /// This function can return an error if the operation fails. In this case,
    /// the error will be propagated back to the user space as an error code.
    fn open(&self, _file: &File) -> Result<Self::OpenPtr, Self::Err>;

    /// Handles the `release` operation on the device file.
    ///
    /// This is called when the device file is closed. The `file` parameter
    /// contains a reference to the file object representing the closed file.
    /// The `open_dev` parameter contains the pointer to the open device type
    /// that was returned by the corresponding `open` operation.
    ///
    /// This function is optional. If not implemented, the kernel will just
    /// drop the open device type pointer and return success to the user space.
    ///
    /// # Errors
    ///
    /// This function can return an error if the operation fails. In this case,
    /// the error will be propagated back to the user space as an error code.
    fn release(&self, _file: &File, _open_dev: Self::OpenPtr) -> Result<(), Self::Err> {
        kernel::build_error(VTABLE_DEFAULT_ERROR)
    }
}

/// Trait implemented by an open character device.
///
/// An open character device handles the actual I/O operations on the device
/// file. It is returned by the `open` operation of the associated character
/// device type that implements the [`CharDevice`] trait.
///
/// # Example
///
/// ```
/// # use kernel::{
/// #     char_dev::{CharDevice, CharDeviceID, OpenCharDevice},
/// #     fs::{File, LocalFile},
/// #     prelude::*,
/// #     uaccess::UserSliceWriter,
/// # };
/// #
/// struct MyCharDev;
/// struct MyOpenCharDev;
///
/// #[vtable]
/// impl CharDevice for MyCharDev {
///     type OpenPtr = Box<MyOpenCharDev>;
///     type Err = Error;
///
///     fn new(_dev_id: CharDeviceID) -> Result<Self, Self::Err> {
///         Ok(Self)
///     }
///
///     fn open(&self, _file: &File) -> Result<Self::OpenPtr, Self::Err> {
///         pr_info!("Opened device!\n");
///         Box::new(MyOpenCharDev, GFP_KERNEL).map_err(Into::into)
///     }
/// }
///
/// #[vtable]
/// impl OpenCharDevice for MyOpenCharDev {
///     type IoctlCmd = ();
///     type Err = Error;
///
///     fn read(
///         &self,
///         _file: &LocalFile,
///         _buf: UserSliceWriter,
///         _offset: &mut i64,
///     ) -> Result<usize, Self::Err> {
///         pr_info!("Read from device!\n");
///         Ok(0)
///     }
/// }
/// ```
#[vtable]
pub trait OpenCharDevice: Send + Sync {
    /// The type of the ioctl command that can be passed to the device.
    ///
    /// This type must implement the [`IoctlCommand`] trait, which is
    /// normally done by deriving the trait using the eponymous macro.
    ///
    /// If the device does not support any ioctl commands, this type can be
    /// set to `()`.
    type IoctlCmd: IoctlCommand;

    /// The error type returned by the device operations.
    ///
    /// This type must be convertible into the kernel [`Error`] type.
    type Err: Into<Error>;

    /// Handles the `read` operation on the device file.
    ///
    /// This is called when the device file is read from. The `file` parameter
    /// contains a reference to the file object representing the opened file.
    /// The `user_writer` parameter contains a writer that can be used to write
    /// the requested data to the user space buffer.
    /// The `offset` parameter contains the current offset in the file, and
    /// should be updated to the new offset after the read operation.
    ///
    /// The function should return the number of bytes read from the device.
    ///
    /// # Errors
    ///
    /// This function can return an error if the operation fails. In this case,
    /// the error will be propagated back to the user space as an error code.
    fn read(
        &self,
        _file: &LocalFile,
        _user_writer: UserSliceWriter,
        _offset: &mut i64,
    ) -> Result<usize, Self::Err> {
        kernel::build_error(VTABLE_DEFAULT_ERROR)
    }

    /// Handles the `write` operation on the device file.
    ///
    /// This is called when the device file is written to. The `file` parameter
    /// contains a reference to the file object representing the opened file.
    /// The `user_reader` parameter contains a reader that can be used to read
    /// the provided data from the user space buffer.
    /// The `offset` parameter contains the current offset in the file, and
    /// should be updated to the new offset after the write operation.
    ///
    /// The function should return the number of bytes written to the device.
    ///
    /// # Errors
    ///
    /// This function can return an error if the operation fails. In this case,
    /// the error will be propagated back to the user space as an error code.
    fn write(
        &self,
        _file: &LocalFile,
        _user_reader: UserSliceReader,
        _offset: &mut i64,
    ) -> Result<usize, Self::Err> {
        kernel::build_error(VTABLE_DEFAULT_ERROR)
    }

    /// Handles the `ioctl` operation on the device file.
    ///
    /// This is called when the user space application performs an ioctl
    /// operation on the device file. The `file` parameter contains a reference
    /// to the file object representing the opened file.
    /// The `cmd` parameter contains the ioctl command to execute.
    /// The `compat` parameter indicates whether the ioctl operation is
    /// performed in compatibility mode, meaning that the user space application
    /// is running in 32-bit mode on a 64-bit kernel.
    ///
    /// The function should return the result of the ioctl operation, which
    /// could include writing data to a user space buffer, depending on the
    /// command in question.
    ///
    /// # Errors
    ///
    /// This function can return an error if the operation fails. In this case,
    /// the error will be propagated back to the user space as an error code.
    fn ioctl(
        &self,
        _file: &File,
        _cmd: Self::IoctlCmd,
        #[cfg(CONFIG_COMPAT)] _compat: bool,
    ) -> Result<u64, Self::Err> {
        kernel::build_error(VTABLE_DEFAULT_ERROR)
    }

    /// Handles the `llseek` operation on the device file.
    ///
    /// This is called when the device file is seeking. The `file` parameter
    /// contains a reference to the file object representing the opened file.
    /// The `pos` parameter contains the current position in the file, and should
    /// be updated to the new position after the seek operation. The `offset`
    /// parameter contains the new offset to seek to, and the `whence` parameter
    /// contains the seek mode.
    ///
    /// The function should return the new offset after the seek operation.
    ///
    /// # Errors
    ///
    /// This function can return an error if the operation fails. In this case,
    /// the error will be propagated back to the user space as an error code.
    fn llseek(
        &self,
        _file: &LocalFile,
        _pos: &mut i64,
        _offset: i64,
        _whence: Whence,
    ) -> Result<u64, Self::Err> {
        kernel::build_error(VTABLE_DEFAULT_ERROR)
    }
}

/// This type alias saves a lot of long types in the fops functions.
#[rustfmt::skip]
type OpenDev<'a, T> = <
    <<T as CharDevice>::OpenPtr as ForeignOwnable>::Borrowed<'a> as Deref
>::Target;

/// This struct wraps a [`CharDevice`] together with a kernel [`cdev`] object.
///
/// This allows us to retrieve the device type from the `inode` object in the
/// `open` function using the `container_of` macro.
#[pin_data]
struct CharDeviceContainer<T: CharDevice> {
    #[pin]
    cdev: Opaque<bindings::cdev>,
    inner: T,
}

impl<T: CharDevice> CharDeviceContainer<T> {
    /// Creates a new `CharDeviceContainer` from a device type and a base device ID.
    ///
    /// This function initializes a new `cdev` object and registers it with the
    /// kernel using the device ID calculated from the base device ID and the
    /// index `i`.
    fn register(
        i: usize,
        base_dev_id: CharDeviceID,
        fops: *const bindings::file_operations,
    ) -> impl PinInit<Self, Error> {
        let dev_id = CharDeviceID(base_dev_id.0 + i as u32);

        try_pin_init!(Self {
            cdev <- Opaque::try_ffi_init(|slot: *mut bindings::cdev| {
                // SAFETY: The `slot` pointer is valid but uninitialized and
                // `cdev_init` only writes to it. Also, the `fops` pointer is
                // never dereferenced without checking for null first.
                unsafe { bindings::cdev_init(slot, fops) };

                // SAFETY: Since `cdev_init` was called, `slot` is valid and
                // initialized, which means that `cdev_add` is safe to call.
                to_result(unsafe {
                    bindings::cdev_add(slot, dev_id.0, 1)
                })
            }),
            inner: T::new(dev_id).map_err(Into::into)?,
        }? Error)
    }
}

/// This struct represents a character device registration.
///
/// It manages the registration of `NUM_MINORS` character devices of type `T`
/// with consecutive device IDs starting from `base_dev_id`.
///
/// The devices are automatically unregistered when the registration object is
/// dropped.
///
/// # Example
///
/// ```
/// # use kernel::{
/// #     c_str,
/// #     char_dev::{CharDevice, CharDeviceID, DeviceRegistration, OpenCharDevice},
/// #     fs::File,
/// #     prelude::*,
/// # };
/// #
/// struct MyCharDev;
/// struct MyOpenCharDev;
///
/// #[vtable]
/// impl CharDevice for MyCharDev {
///     // --snip--
/// #     type OpenPtr = Box<MyOpenCharDev>;
/// #     type Err = Error;
/// #
/// #     fn new(_dev_id: CharDeviceID) -> Result<Self, Self::Err> {
/// #         Ok(Self)
/// #     }
/// #
/// #     fn open(&self, _file: &File) -> Result<Self::OpenPtr, Self::Err> {
/// #         Box::new(MyOpenCharDev, GFP_KERNEL).map_err(Into::into)
/// #     }
/// }
///
/// #[vtable]
/// impl OpenCharDevice for MyOpenCharDev {
///     // --snip--
/// #     type IoctlCmd = ();
/// #     type Err = Error;
/// }
///
/// const DEV_NAME: &CStr = c_str!("my_char_dev");
/// const NUM_MINORS: usize = 5;
///
/// struct MyModule {
///     reg: Pin<Box<DeviceRegistration<MyCharDev, NUM_MINORS>>>,
/// }
///
/// impl kernel::Module for MyModule {
///     fn init(module: &'static ThisModule) -> Result<Self> {
///         let reg = Box::pin_init(DeviceRegistration::register(module, DEV_NAME), GFP_KERNEL)?;
///
///         let dev_id = reg.get_base_dev_id();
///         pr_info!(
///             "Registered device {DEV_NAME} with major {} and minors {} through {} \n",
///             dev_id.major(),
///             dev_id.minor(),
///             dev_id.minor() + NUM_MINORS as u32 - 1
///         );
///
///         Ok(Self { reg })
///     }
/// }
/// ```
#[pin_data(PinnedDrop)]
pub struct DeviceRegistration<T: CharDevice, const NUM_MINORS: usize> {
    base_dev_id: CharDeviceID,
    #[pin]
    fops: Opaque<bindings::file_operations>,
    #[pin]
    devices: [CharDeviceContainer<T>; NUM_MINORS],
}

impl<T: CharDevice, const NUM_MINORS: usize> DeviceRegistration<T, NUM_MINORS> {
    /// Registers `NUM_MINORS` character devices of type `T` with the kernel.
    ///
    /// The devices are registered with the name `name` and the module `module`.
    ///
    /// The function returns a `PinInit` that resolves to a `Registration`
    /// object if the registration was successful.
    pub fn register(module: &'static ThisModule, name: &'static CStr) -> impl PinInit<Self, Error> {
        try_pin_init!(&this in Self {
            base_dev_id: Self::alloc_region(name)?,
            fops <- fops::create_vtable::<T>(module),
            devices <- init::pin_init_array_from_fn(|i| {
                // SAFETY: `this` is a non-null pointer to a partially
                // initialized `Registration` object, where `base_dev_id`
                // and `fops` are already initialized, so it is safe to
                // dereference it and access these fields.
                unsafe {
                    CharDeviceContainer::register(
                        i,
                        (*this.as_ptr()).base_dev_id,
                        (*this.as_ptr()).fops.get(),
                    )
                }
            }),
        })
    }

    /// Returns the base device ID of the registration.
    ///
    /// The base device ID is the device ID of the first device registered by
    /// this registration. The device IDs of the other devices are consecutive
    /// numbers starting from this ID.
    pub fn get_base_dev_id(&self) -> CharDeviceID {
        self.base_dev_id
    }

    fn alloc_region(name: &'static CStr) -> Result<CharDeviceID> {
        let mut dev_id = CharDeviceID::default();

        // SAFETY: The `dev_id` pointer is valid and thus safe to write to,
        // which means that `alloc_chrdev_region` is safe to call.
        to_result(unsafe {
            bindings::alloc_chrdev_region(
                &mut dev_id as *mut CharDeviceID as *mut bindings::dev_t,
                0,
                NUM_MINORS as u32,
                name.as_char_ptr(),
            )
        })?;

        Ok(dev_id)
    }
}

// SAFETY: Registration with and unregistration of character devices can happen
// from any thread or CPU, so `Registration` can be `Send`.
unsafe impl<T: CharDevice, const NUM_MINORS: usize> Send for DeviceRegistration<T, NUM_MINORS> {}

// SAFETY: `Registration` doesn't offer any methods or access to fields when
// shared between threads or CPUs, so it is safe to share it.
unsafe impl<T: CharDevice, const NUM_MINORS: usize> Sync for DeviceRegistration<T, NUM_MINORS> {}

#[pinned_drop]
impl<T: CharDevice, const NUM_MINORS: usize> PinnedDrop for DeviceRegistration<T, NUM_MINORS> {
    /// Unregisters the character devices.
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: We never move out of `this`.
        let this = unsafe { self.get_unchecked_mut() };

        for i in 0..NUM_MINORS {
            // SAFETY: All `cdev`s in the device containers have been
            // initialized and added to the system, so it is safe to call
            // `cdev_del` on them.
            unsafe {
                bindings::cdev_del(this.devices[i].cdev.get());
            }
        }

        // SAFETY: The device region has been allocated and registered, so it is
        // safe to call `unregister_chrdev_region` on it.
        unsafe {
            bindings::unregister_chrdev_region(this.base_dev_id.0, NUM_MINORS as u32);
        }
    }
}

mod fops {
    use super::*;

    /// Creates a file operations table for a character device of type `T`.
    pub(super) fn create_vtable<T: CharDevice>(
        module: &'static ThisModule,
    ) -> impl PinInit<Opaque<bindings::file_operations>> {
        Opaque::ffi_init(|slot: *mut bindings::file_operations| {
            let fops = bindings::file_operations {
                owner: module.as_ptr(),
                open: Some(open::<T>),
                release: Some(close::<T>),
                read: <OpenDev<'_, T>>::HAS_READ.then_some(read::<T>),
                write: <OpenDev<'_, T>>::HAS_WRITE.then_some(write::<T>),
                unlocked_ioctl: <OpenDev<'_, T>>::HAS_IOCTL.then_some(unlocked_ioctl::<T>),
                #[cfg(CONFIG_COMPAT)]
                compat_ioctl: <OpenDev<'_, T>>::HAS_IOCTL.then_some(compat_ioctl::<T>),
                llseek: <OpenDev<'_, T>>::HAS_LLSEEK.then_some(llseek::<T>),
                ..bindings::file_operations::default()
            };

            // SAFETY: `slot` is a valid but uninitialized pointer to a
            // `file_operations` object, so it is safe to write to it.
            unsafe {
                slot.write(fops);
            }
        })
    }

    /// Handles the `open` operation for a character device.
    ///
    /// # Safety
    ///
    /// * The caller must ensure that `inode` points at a valid inode and that
    ///   its `i_cdev` field points at a valid `cdev`, contained by a
    ///   `CharDeviceContainer<T>`.
    /// * The caller must ensure that `file` points at a valid file and that the
    ///   file's refcount is positive for the duration of the function call.
    /// * The caller must ensure that if there are active `fdget_pos` calls on
    ///   this file, then they took the `f_pos_lock` mutex.
    unsafe extern "C" fn open<T: CharDevice>(
        inode: *mut bindings::inode,
        file: *mut bindings::file,
    ) -> i32 {
        // Handle non O_LARGEFILE open on 32-bit systems
        //
        // SAFETY: The caller guarantees that `inode` points at a valid inode
        // and that `file` points at a valid file, so it is safe to call
        // `generic_file_open` on them.
        let ret = unsafe { bindings::generic_file_open(inode, file) };
        if ret != 0 {
            return ret;
        }

        // SAFETY: The caller guarantees that `inode` points at a valid inode
        // and that its `i_cdev` field points at a valid `cdev`, contained by a
        // `CharDeviceContainer<T>`, so it is safe to access this container.
        let container = unsafe {
            &*container_of!(
                (*inode).__bindgen_anon_4.i_cdev,
                CharDeviceContainer<T>,
                cdev
            )
        };

        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that the file's refcount is positive, as well as that any active
        // `fdget_pos` calls on this file took the `f_pos_lock` mutex, so it is
        // safe to call `File::from_raw_file` on it.
        let file = unsafe { File::from_raw_file(file) };

        let open_dev = match container.inner.open(file).map_err(Into::into) {
            Ok(open_dev) => open_dev,
            Err(e) => {
                return e.to_errno();
            }
        };

        // SAFETY: The caller guarantees that `file` points at a valid file, so
        // it is safe to access and modify the `private_data` field on it.
        unsafe {
            (*file.as_ptr()).private_data = open_dev.into_foreign().cast_mut();
        }

        0
    }

    /// Handles the `close` operation for a character device.
    ///
    /// # Safety
    ///
    /// * The caller must ensure that `inode` points at a valid inode and that
    ///   its `i_cdev` field points at a valid `cdev`, contained by a
    ///   `CharDeviceContainer<T>`.
    /// * The caller must ensure that `file` points at a valid file and that the
    ///   file's refcount is positive for the duration of the function call.
    /// * The caller must ensure that if there are active `fdget_pos` calls on
    ///   this file, then they took the `f_pos_lock` mutex.
    /// * The caller must ensure that the `private_data` field of `file` is a
    ///   pointer returned by [`ForeignOwnable::into_foreign`] for which a
    ///   previous matching [`ForeignOwnable::from_foreign`] hasn't been called
    ///   yet. Additionally, all instances (if any) of values returned by
    ///   [`ForeignOwnable::borrow`] for this object must have been dropped.
    unsafe extern "C" fn close<T: CharDevice>(
        inode: *mut bindings::inode,
        file: *mut bindings::file,
    ) -> i32 {
        // SAFETY: The caller guarantees that `inode` points at a valid inode
        // and that its `i_cdev` field points at a valid `cdev`, contained by a
        // `CharDeviceContainer<T>`, so it is safe to access this container.
        let container = unsafe {
            &*container_of!(
                (*inode).__bindgen_anon_4.i_cdev,
                CharDeviceContainer<T>,
                cdev
            )
        };

        // SAFETY: The caller guarantees that `file` points at a valid file,
        // that its `private_data` field is a pointer returned by
        // `ForeignOwnable::into_foreign` for which a previous matching
        // `ForeignOwnable::from_foreign` hasn't been called yet, and that all
        // instances (if any) of values returned by `ForeignOwnable::borrow` for
        // this object have been dropped already, so it is safe to call
        // `ForeignOwnable::from_foreign` on it.
        let open_dev =
            unsafe { <T::OpenPtr as ForeignOwnable>::from_foreign((*file).private_data) };

        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that the file's refcount is positive, as well as that any active
        // `fdget_pos` calls on this file took the `f_pos_lock` mutex, so it is
        // safe to call `File::from_raw_file` on it.
        let file = unsafe { File::from_raw_file(file) };

        if T::HAS_RELEASE {
            if let Err(e) = container.inner.release(file, open_dev).map_err(Into::into) {
                return e.to_errno();
            }
        }

        0
    }

    /// Handles the `read` operation for a character device.
    ///
    /// # Safety
    ///
    /// * The caller must ensure that `file` points at a valid file and that the
    ///   file's refcount is positive for the duration of the function call.
    /// * The caller must ensure that if there is an active `fdget_pos` call on
    ///   this file that didn't take the `f_pos_lock` mutex, then that call is
    ///   on the current thread.
    /// * The caller must ensure that the `private_data` field of `file` is a
    ///   pointer returned by [`ForeignOwnable::into_foreign`] for which a
    ///   previous matching [`ForeignOwnable::from_foreign`] hasn't been called
    ///   yet.
    /// * The caller must ensure that the `offset` pointer is valid and
    ///   initialized, and that we have exclusive access to it for the duration
    ///   of the function call.
    unsafe extern "C" fn read<T: CharDevice>(
        file: *mut bindings::file,
        user_buffer: *mut ffi::c_char,
        count: usize,
        offset: *mut bindings::loff_t,
    ) -> isize {
        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that its `private_data` field is a pointer returned by
        // `ForeignOwnable::into_foreign` for which a previous matching
        // `ForeignOwnable::from_foreign` hasn't been called yet, so it is safe
        // to call `ForeignOwnable::borrow` on it.
        let open_dev = unsafe { <T::OpenPtr as ForeignOwnable>::borrow((*file).private_data) };

        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that the file's refcount is positive, as well as that if there is an
        // active `fdget_pos` call on this file that didn't take the
        // `f_pos_lock` mutex, then that call is on the current thread, so it is
        // safe to call `LocalFile::from_raw_file` on it.
        let file = unsafe { LocalFile::from_raw_file(file) };

        let user_writer = UserSlice::new(user_buffer as UserPtr, count).writer();

        // SAFETY: The caller guarantees that `offset` is a valid and
        // initialized pointer, and that we have exclusive access to it for the
        // duration of the function call, so it is safe to dereference and form
        // a mutable reference to it.
        let offset = unsafe { &mut *offset };

        match open_dev.read(file, user_writer, offset).map_err(Into::into) {
            Ok(n) => n as isize,
            Err(e) => e.to_errno() as isize,
        }
    }

    /// Handles the `write` operation for a character device.
    ///
    /// # Safety
    ///
    /// * The caller must ensure that `file` points at a valid file and that the
    ///   file's refcount is positive for the duration of the function call.
    /// * The caller must ensure that if there is an active `fdget_pos` call on
    ///   this file that didn't take the `f_pos_lock` mutex, then that call is
    ///   on the current thread.
    /// * The caller must ensure that the `private_data` field of `file` is a
    ///   pointer returned by [`ForeignOwnable::into_foreign`] for which a
    ///   previous matching [`ForeignOwnable::from_foreign`] hasn't been called
    ///   yet.
    /// * The caller must ensure that the `offset` pointer is valid and
    ///   initialized, and that we have exclusive access to it for the duration
    ///   of the function call.
    unsafe extern "C" fn write<T: CharDevice>(
        file: *mut bindings::file,
        user_buffer: *const ffi::c_char,
        count: usize,
        offset: *mut bindings::loff_t,
    ) -> isize {
        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that its `private_data` field is a pointer returned by
        // `ForeignOwnable::into_foreign` for which a previous matching
        // `ForeignOwnable::from_foreign` hasn't been called yet, so it is safe
        // to call `ForeignOwnable::borrow` on it.
        let open_dev = unsafe { <T::OpenPtr as ForeignOwnable>::borrow((*file).private_data) };

        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that the file's refcount is positive, as well as that if there is an
        // active `fdget_pos` call on this file that didn't take the
        // `f_pos_lock` mutex, then that call is on the current thread, so it is
        // safe to call `LocalFile::from_raw_file` on it.
        let file = unsafe { LocalFile::from_raw_file(file) };

        let user_reader = UserSlice::new(user_buffer as UserPtr, count).reader();

        // SAFETY: The caller guarantees that `offset` is a valid and
        // initialized pointer, and that we have exclusive access to it for the
        // duration of the function call, so it is safe to dereference and form
        // a mutable reference to it.
        let offset = unsafe { &mut *offset };

        match open_dev
            .write(file, user_reader, offset)
            .map_err(Into::into)
        {
            Ok(n) => n as isize,
            Err(e) => e.to_errno() as isize,
        }
    }

    /// Handles the `unlocked_ioctl` operation for a character device.
    ///
    /// # Safety
    ///
    /// * The caller must ensure that `file` points at a valid file and that the
    ///   file's refcount is positive for the duration of the function call.
    /// * The caller must ensure that if there are active `fdget_pos` calls on
    ///   this file, then they took the `f_pos_lock` mutex.
    /// * The caller must ensure that the `private_data` field of `file` is a
    ///   pointer returned by [`ForeignOwnable::into_foreign`] for which a
    ///   previous matching [`ForeignOwnable::from_foreign`] hasn't been called
    ///   yet.
    unsafe extern "C" fn unlocked_ioctl<T: CharDevice>(
        file: *mut bindings::file,
        cmd: ffi::c_uint,
        arg: ffi::c_ulong,
    ) -> ffi::c_long {
        type Cmd<'a, T> = <OpenDev<'a, T> as OpenCharDevice>::IoctlCmd;

        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that its `private_data` field is a pointer returned by
        // `ForeignOwnable::into_foreign` for which a previous matching
        // `ForeignOwnable::from_foreign` hasn't been called yet, so it is safe
        // to call `ForeignOwnable::borrow` on it.
        let open_dev = unsafe { <T::OpenPtr as ForeignOwnable>::borrow((*file).private_data) };

        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that the file's refcount is positive, as well as that any active
        // `fdget_pos` calls on this file took the `f_pos_lock` mutex, so it is
        // safe to call `File::from_raw_file` on it.
        let file = unsafe { File::from_raw_file(file) };

        let cmd = match <Cmd<'_, T> as IoctlCommand>::parse(cmd, arg).map_err(Into::into) {
            Ok(cmd) => cmd,
            Err(e) => return e.to_errno() as ffi::c_long,
        };

        match open_dev
            .ioctl(
                file,
                cmd,
                #[cfg(CONFIG_COMPAT)]
                false,
            )
            .map_err(Into::into)
        {
            Ok(ret) => ret as ffi::c_long,
            Err(e) => e.to_errno() as ffi::c_long,
        }
    }

    /// Handles the `compat_ioctl` operation for a character device.
    ///
    /// # Safety
    ///
    /// * The caller must ensure that `file` points at a valid file and that the
    ///   file's refcount is positive for the duration of the function call.
    /// * The caller must ensure that if there are active `fdget_pos` calls on
    ///   this file, then they took the `f_pos_lock` mutex.
    /// * The caller must ensure that the `private_data` field of `file` is a
    ///   pointer returned by [`ForeignOwnable::into_foreign`] for which a
    ///   previous matching [`ForeignOwnable::from_foreign`] hasn't been called
    ///   yet.
    #[cfg(CONFIG_COMPAT)]
    unsafe extern "C" fn compat_ioctl<T: CharDevice>(
        file: *mut bindings::file,
        cmd: ffi::c_uint,
        arg: ffi::c_ulong,
    ) -> ffi::c_long {
        type Cmd<'a, T> = <OpenDev<'a, T> as OpenCharDevice>::IoctlCmd;

        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that its `private_data` field is a pointer returned by
        // `ForeignOwnable::into_foreign` for which a previous matching
        // `ForeignOwnable::from_foreign` hasn't been called yet, so it is safe
        // to call `ForeignOwnable::borrow` on it.
        let open_dev = unsafe { <T::OpenPtr as ForeignOwnable>::borrow((*file).private_data) };

        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that the file's refcount is positive, as well as that any active
        // `fdget_pos` calls on this file took the `f_pos_lock` mutex, so it is
        // safe to call `File::from_raw_file` on it.
        let file = unsafe { File::from_raw_file(file) };

        let parse_fn = if <Cmd<'_, T> as IoctlCommand>::HAS_COMPAT_PARSE {
            <Cmd<'_, T> as IoctlCommand>::compat_parse
        } else {
            <Cmd<'_, T> as IoctlCommand>::parse
        };

        let cmd = match parse_fn(cmd, arg).map_err(Into::into) {
            Ok(cmd) => cmd,
            Err(e) => return e.to_errno() as ffi::c_long,
        };

        match open_dev.ioctl(file, cmd, true).map_err(Into::into) {
            Ok(ret) => ret as ffi::c_long,
            Err(e) => e.to_errno() as ffi::c_long,
        }
    }

    /// Handles the `llseek` operation for a character device.
    ///
    /// # Safety
    ///
    /// * The caller must ensure that `file` points at a valid file and that the
    ///   file's refcount is positive for the duration of the function call.
    /// * The caller must ensure that if there is an active `fdget_pos` call on
    ///   this file that didn't take the `f_pos_lock` mutex, then that call is
    ///   on the current thread.
    /// * The caller must ensure that the `private_data` field of `file` is a
    ///   pointer returned by [`ForeignOwnable::into_foreign`] for which a
    ///   previous matching [`ForeignOwnable::from_foreign`] hasn't been called
    ///   yet.
    /// * The caller must ensure that `whence` is less than or equal to `SEEK_MAX`.
    unsafe extern "C" fn llseek<T: CharDevice>(
        file: *mut bindings::file,
        offset: bindings::loff_t,
        whence: ffi::c_int,
    ) -> bindings::loff_t {
        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that its `private_data` field is a pointer returned by
        // `ForeignOwnable::into_foreign` for which a previous matching
        // `ForeignOwnable::from_foreign` hasn't been called yet, so it is safe
        // to call `ForeignOwnable::borrow` on it.
        let open_dev = unsafe { <T::OpenPtr as ForeignOwnable>::borrow((*file).private_data) };

        // SAFETY: The caller guarantees that `file` points at a valid file, so
        // it is safe to access its `f_pos` field.
        let mut pos = unsafe { (*file).f_pos };

        // SAFETY: The caller guarantees that `file` points at a valid file and
        // that the file's refcount is positive, as well as that if there is an
        // active `fdget_pos` call on this file that didn't take the
        // `f_pos_lock` mutex, then that call is on the current thread, so it is
        // safe to call `LocalFile::from_raw_file` on it.
        let file = unsafe { LocalFile::from_raw_file(file) };

        // SAFETY: The caller guarantees that `whence` is less than or equal to
        // `SEEK_MAX`, so it is safe to transmute it to a `Whence` by the
        // constant assertion above.
        let whence = unsafe { mem::transmute::<u32, Whence>(whence as _) };

        let res = match open_dev
            .llseek(file, &mut pos, offset, whence)
            .map_err(Into::into)
        {
            Ok(pos) => pos as bindings::loff_t,
            Err(e) => e.to_errno() as bindings::loff_t,
        };

        // SAFETY: The caller guarantees that `file` points at a valid file, so
        // it is safe to access its `f_pos` field.
        unsafe {
            (*file.as_ptr()).f_pos = pos;
        }

        res
    }
}
