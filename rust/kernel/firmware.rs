// SPDX-License-Identifier: GPL-2.0

//! Firmware abstraction
//!
//! C header: [`include/linux/firmware.h`](srctree/include/linux/firmware.h)

use crate::{
    bindings,
    device::Device,
    error::to_result,
    ffi,
    prelude::*,
    str::{CStr, CStrExt as _},
};
use core::ptr::NonNull;

/// # Invariants
///
/// One of the following: `bindings::request_firmware`, `bindings::firmware_request_nowarn`,
/// `bindings::firmware_request_platform`, `bindings::request_firmware_direct`.
struct FwFunc(
    unsafe extern "C" fn(
        *mut *const bindings::firmware,
        *const ffi::c_char,
        *mut bindings::device,
    ) -> i32,
);

impl FwFunc {
    fn request() -> Self {
        Self(bindings::request_firmware)
    }

    fn request_nowarn() -> Self {
        Self(bindings::firmware_request_nowarn)
    }
}

/// Abstraction around a C `struct firmware`.
///
/// This is a simple abstraction around the C firmware API. Just like with the C API, firmware can
/// be requested. Once requested the abstraction provides direct access to the firmware buffer as
/// `&[u8]`. The firmware is released once [`Firmware`] is dropped.
///
/// # Invariants
///
/// The pointer is valid, and has ownership over the instance of `struct firmware`.
///
/// The `Firmware`'s backing buffer is not modified.
///
/// # Examples
///
/// ```no_run
/// # use kernel::{device::Device, firmware::Firmware};
///
/// # fn no_run() -> Result<(), Error> {
/// # // SAFETY: *NOT* safe, just for the example to get an `ARef<Device>` instance
/// # let dev = unsafe { Device::get_device(core::ptr::null_mut()) };
///
/// let fw = Firmware::request(c"path/to/firmware.bin", &dev)?;
/// let blob = fw.data();
///
/// # Ok(())
/// # }
/// ```
pub struct Firmware(NonNull<bindings::firmware>);

impl Firmware {
    fn request_internal(name: &CStr, dev: &Device, func: FwFunc) -> Result<Self> {
        let mut fw: *mut bindings::firmware = core::ptr::null_mut();
        let pfw: *mut *mut bindings::firmware = &mut fw;
        let pfw: *mut *const bindings::firmware = pfw.cast();

        // SAFETY: `pfw` is a valid pointer to a NULL initialized `bindings::firmware` pointer.
        // `name` and `dev` are valid as by their type invariants.
        let ret = unsafe { func.0(pfw, name.as_char_ptr(), dev.as_raw()) };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        }

        // SAFETY: `func` not bailing out with a non-zero error code, guarantees that `fw` is a
        // valid pointer to `bindings::firmware`.
        Ok(Firmware(unsafe { NonNull::new_unchecked(fw) }))
    }

    /// Send a firmware request and wait for it. See also `bindings::request_firmware`.
    pub fn request(name: &CStr, dev: &Device) -> Result<Self> {
        Self::request_internal(name, dev, FwFunc::request())
    }

    /// Send a request for an optional firmware module. See also
    /// `bindings::firmware_request_nowarn`.
    pub fn request_nowarn(name: &CStr, dev: &Device) -> Result<Self> {
        Self::request_internal(name, dev, FwFunc::request_nowarn())
    }

    fn as_raw(&self) -> *mut bindings::firmware {
        self.0.as_ptr()
    }

    /// Returns the size of the requested firmware in bytes.
    pub fn size(&self) -> usize {
        // SAFETY: `self.as_raw()` is valid by the type invariant.
        unsafe { (*self.as_raw()).size }
    }

    /// Returns the requested firmware as `&[u8]`.
    pub fn data(&self) -> &[u8] {
        // SAFETY: `self.as_raw()` is valid by the type invariant. Additionally,
        // `bindings::firmware` guarantees, if successfully requested, that
        // `bindings::firmware::data` has a size of `bindings::firmware::size` bytes.
        unsafe { core::slice::from_raw_parts((*self.as_raw()).data, self.size()) }
    }
}

impl Drop for Firmware {
    fn drop(&mut self) {
        // SAFETY: `self.as_raw()` is valid by the type invariant.
        unsafe { bindings::release_firmware(self.as_raw()) };
    }
}

/// Load firmware directly into the caller-provided `buf`.
///
/// On success the firmware image has been copied into `buf`; the caller accesses the data
/// through `buf` itself.
///
/// This is intentionally a stand-alone function rather than a `Firmware` constructor. For
/// the `into_buf` path, the firmware data lives in the caller's `buf`, not in a
/// kernel-owned buffer, so returning a `Firmware` would expose `Firmware::data()` as a
/// second handle aliasing `buf` (and `release_firmware()` does not free `buf` anyway).
pub fn request_into_buf(name: &CStr, dev: &Device, buf: &mut [u8]) -> Result {
    // `as_mut_ptr()` on an empty slice returns a non-NULL pointer to
    // memory which the loader does not own. Passing that pointer with `size == 0`
    // makes the loader believe that it is buffer it allocated itself, so when
    // `release_firmware()` is called, it will vfree the pointer and trigger a
    // bug. Reject empty slices to avoid this situation.
    if buf.is_empty() {
        return Err(EINVAL);
    }

    let mut fw: *const bindings::firmware = core::ptr::null();

    // SAFETY: `&raw mut fw` is a valid pointer to a NULL initialized `bindings::firmware` pointer.
    // `name` and `dev` are valid as by their type invariants. `buf` is a valid writable
    // buffer of `buf.len()` bytes.
    to_result(unsafe {
        bindings::request_firmware_into_buf(
            &raw mut fw,
            name.as_char_ptr(),
            dev.as_raw(),
            buf.as_mut_ptr().cast(),
            buf.len(),
        )
    })?;

    // The firmware bytes are now in `buf`, which the caller owns, so we don't need
    // the kernel to hang on to it any more.
    // SAFETY: `fw` is a valid pointer returned by `request_firmware_into_buf`.
    unsafe { bindings::release_firmware(fw) };

    Ok(())
}

// SAFETY: `Firmware` only holds a pointer to a C `struct firmware`, which is safe to be used from
// any thread.
unsafe impl Send for Firmware {}

// SAFETY: `Firmware` only holds a pointer to a C `struct firmware`, references to which are safe to
// be used from any thread.
unsafe impl Sync for Firmware {}

/// Create firmware .modinfo entries.
///
/// This macro is the counterpart of the C macro `MODULE_FIRMWARE()`, but instead of taking a
/// simple string literals, which is already covered by the `firmware` field of
/// [`crate::prelude::module!`], it allows the caller to pass a builder type, based on the
/// [`ModInfoBuilder`], which can create the firmware modinfo strings in a more flexible way.
///
/// Drivers should extend the [`ModInfoBuilder`] with their own driver specific builder type.
///
/// The `builder` argument must be a type which implements the following function.
///
/// `const fn create(module_name: &'static CStr) -> ModInfoBuilder`
///
/// `create` should pass the `module_name` to the [`ModInfoBuilder`] and, with the help of
/// it construct the corresponding firmware modinfo.
///
/// Typically, such contracts would be enforced by a trait, however traits do not (yet) support
/// const functions.
///
/// # Examples
///
/// ```
/// # mod module_firmware_test {
/// # use kernel::firmware;
/// # use kernel::prelude::*;
/// #
/// # struct MyModule;
/// #
/// # impl kernel::Module for MyModule {
/// #     fn init(_module: &'static ThisModule) -> Result<Self> {
/// #         Ok(Self)
/// #     }
/// # }
/// #
/// #
/// struct Builder<const N: usize>;
///
/// impl<const N: usize> Builder<N> {
///     const DIR: &'static str = "vendor/chip/";
///     const FILES: [&'static str; 3] = [ "foo", "bar", "baz" ];
///
///     const fn create(module_name: &'static kernel::str::CStr) -> firmware::ModInfoBuilder<N> {
///         let mut builder = firmware::ModInfoBuilder::new(module_name);
///
///         let mut i = 0;
///         while i < Self::FILES.len() {
///             builder = builder.new_entry()
///                 .push(Self::DIR)
///                 .push(Self::FILES[i])
///                 .push(".bin");
///
///                 i += 1;
///         }
///
///         builder
///      }
/// }
///
/// module! {
///    type: MyModule,
///    name: "module_firmware_test",
///    authors: ["Rust for Linux"],
///    description: "module_firmware! test module",
///    license: "GPL",
/// }
///
/// kernel::module_firmware!(Builder);
/// # }
/// ```
#[macro_export]
macro_rules! module_firmware {
    // The argument is the builder type without the const generic, since it's deferred from within
    // this macro. Hence, we can neither use `expr` nor `ty`.
    ($($builder:tt)*) => {
        const _: () = {
            const __MODULE_FIRMWARE_PREFIX: &'static $crate::str::CStr = if cfg!(MODULE) {
                c""
            } else {
                <LocalModule as $crate::ModuleMetadata>::NAME
            };

            #[link_section = ".modinfo"]
            #[used(compiler)]
            static __MODULE_FIRMWARE: [u8; $($builder)*::create(__MODULE_FIRMWARE_PREFIX)
                .build_length()] = $($builder)*::create(__MODULE_FIRMWARE_PREFIX).build();
        };
    };
}

/// Builder for firmware module info.
///
/// [`ModInfoBuilder`] is a helper component to flexibly compose firmware paths strings for the
/// .modinfo section in const context.
///
/// Therefore the [`ModInfoBuilder`] provides the methods [`ModInfoBuilder::new_entry`] and
/// [`ModInfoBuilder::push`], where the latter is used to push path components and the former to
/// mark the beginning of a new path string.
///
/// [`ModInfoBuilder`] is meant to be used in combination with [`kernel::module_firmware!`].
///
/// The const generic `N` as well as the `module_name` parameter of [`ModInfoBuilder::new`] is an
/// internal implementation detail and supplied through the above macro.
pub struct ModInfoBuilder<const N: usize> {
    buf: [u8; N],
    n: usize,
    module_name: &'static CStr,
}

impl<const N: usize> ModInfoBuilder<N> {
    /// Create an empty builder instance.
    pub const fn new(module_name: &'static CStr) -> Self {
        Self {
            buf: [0; N],
            n: 0,
            module_name,
        }
    }

    const fn push_internal(mut self, bytes: &[u8]) -> Self {
        let mut j = 0;

        if N == 0 {
            self.n += bytes.len();
            return self;
        }

        while j < bytes.len() {
            if self.n < N {
                self.buf[self.n] = bytes[j];
            }
            self.n += 1;
            j += 1;
        }
        self
    }

    /// Push an additional path component.
    ///
    /// Append path components to the [`ModInfoBuilder`] instance. Paths need to be separated
    /// with [`ModInfoBuilder::new_entry`].
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::firmware::ModInfoBuilder;
    ///
    /// # const DIR: &str = "vendor/chip/";
    /// # const fn no_run<const N: usize>(builder: ModInfoBuilder<N>) {
    /// let builder = builder.new_entry()
    ///     .push(DIR)
    ///     .push("foo.bin")
    ///     .new_entry()
    ///     .push(DIR)
    ///     .push("bar.bin");
    /// # }
    /// ```
    pub const fn push(self, s: &str) -> Self {
        // Check whether there has been an initial call to `next_entry()`.
        if N != 0 && self.n == 0 {
            crate::build_error!("Must call next_entry() before push().");
        }

        self.push_internal(s.as_bytes())
    }

    const fn push_module_name(self) -> Self {
        let mut this = self;
        let module_name = this.module_name;

        if !this.module_name.is_empty() {
            this = this.push_internal(module_name.to_bytes_with_nul());

            if N != 0 {
                // Re-use the space taken by the NULL terminator and swap it with the '.' separator.
                this.buf[this.n - 1] = b'.';
            }
        }

        this
    }

    /// Prepare the [`ModInfoBuilder`] for the next entry.
    ///
    /// This method acts as a separator between module firmware path entries.
    ///
    /// Must be called before constructing a new entry with subsequent calls to
    /// [`ModInfoBuilder::push`].
    ///
    /// See [`ModInfoBuilder::push`] for an example.
    pub const fn new_entry(self) -> Self {
        self.push_internal(b"\0")
            .push_module_name()
            .push_internal(b"firmware=")
    }

    /// Build the byte array.
    pub const fn build(self) -> [u8; N] {
        // Add the final NULL terminator.
        let this = self.push_internal(b"\0");

        if this.n == N {
            this.buf
        } else {
            crate::build_error!("Length mismatch.");
        }
    }
}

impl ModInfoBuilder<0> {
    /// Return the length of the byte array to build.
    pub const fn build_length(self) -> usize {
        // Compensate for the NULL terminator added by `build`.
        self.n + 1
    }
}

/// Firmware upload: let userspace hand a driver an image to write to its device.
///
/// Registering creates `/sys/class/firmware/<name>/` with the `loading`/`data` handshake plus
/// `status`, `error`, `remaining_size` and `cancel`, which is the counterpart to [`Firmware`]:
/// the same image works either way, but this one is pushed by userspace rather than pulled from
/// `/lib/firmware`. Drivers use it when an image has to be written on demand -- a re-flash, or a
/// deliberate downgrade -- rather than only when a newer version appears.
pub mod upload {
    use super::*;
    use crate::types::ForeignOwnable;

    /// Why an upload step failed. `None` means success.
    ///
    /// The values are the `enum fw_upload_err` the core reports back through `sysfs`, so a driver
    /// says what went wrong in the vocabulary userspace already reads out of `error`.
    #[derive(Clone, Copy, PartialEq, Eq)]
    #[repr(u32)]
    pub enum Error {
        /// The device reported a failure; see the kernel log.
        Hardware = bindings::fw_upload_err_FW_UPLOAD_ERR_HW_ERROR,
        /// A handshake with the device timed out.
        Timeout = bindings::fw_upload_err_FW_UPLOAD_ERR_TIMEOUT,
        /// Userspace wrote `cancel`.
        Canceled = bindings::fw_upload_err_FW_UPLOAD_ERR_CANCELED,
        /// Another upload is already running.
        Busy = bindings::fw_upload_err_FW_UPLOAD_ERR_BUSY,
        /// The image is not a size this device can take.
        InvalidSize = bindings::fw_upload_err_FW_UPLOAD_ERR_INVALID_SIZE,
        /// A read or write to the device failed; see the kernel log.
        ReadWrite = bindings::fw_upload_err_FW_UPLOAD_ERR_RW_ERROR,
        /// The flash is wearing out; wait and retry.
        WearOut = bindings::fw_upload_err_FW_UPLOAD_ERR_WEAROUT,
        /// The image is not one this device accepts.
        InvalidFirmware = bindings::fw_upload_err_FW_UPLOAD_ERR_FW_INVALID,
    }

    /// A driver's side of an upload.
    ///
    /// `prepare` runs once with the whole image, which is where a driver rejects one that is not
    /// for this device; `write` is then called repeatedly until every byte is written.
    pub trait Upload: Sized {
        /// Shared driver state, handed back to every callback.
        type Data: ForeignOwnable + Send + Sync;

        /// Validate the image and get the device ready. Runs before any write.
        fn prepare(
            data: <Self::Data as ForeignOwnable>::Borrowed<'_>,
            image: &[u8],
        ) -> Result<(), Error>;

        /// Write `chunk`, which starts at `offset` in the image, returning how much was written.
        ///
        /// Called repeatedly until the image is consumed, so a driver may write less than it was
        /// offered and be called again with the remainder.
        fn write(
            data: <Self::Data as ForeignOwnable>::Borrowed<'_>,
            image: &[u8],
            offset: u32,
            chunk: &[u8],
        ) -> Result<u32, Error>;

        /// Report whether the device has finished programming what it was sent.
        fn poll_complete(data: <Self::Data as ForeignOwnable>::Borrowed<'_>) -> Result<(), Error>;

        /// Asked to stop, from another thread: set a flag the other callbacks observe.
        fn cancel(data: <Self::Data as ForeignOwnable>::Borrowed<'_>);

        /// Undo whatever `prepare` set up. Runs on success and on failure alike.
        fn cleanup(_data: <Self::Data as ForeignOwnable>::Borrowed<'_>) {}
    }

    /// The C vtable for `U`, built once at compile time.
    struct Vtable<U: Upload>(core::marker::PhantomData<U>);

    impl<U: Upload> Vtable<U> {
        /// Turn a driver result into the `enum fw_upload_err` the core expects.
        fn err(r: Result<(), Error>) -> bindings::fw_upload_err {
            match r {
                Ok(()) => bindings::fw_upload_err_FW_UPLOAD_ERR_NONE,
                Err(e) => e as bindings::fw_upload_err,
            }
        }

        /// # Safety
        ///
        /// Called by the firmware core with a valid `fw_upload` whose `dd_handle` is the pointer
        /// [`Registration::new`] passed it, and `data` valid for `size` bytes.
        unsafe extern "C" fn prepare(
            fw: *mut bindings::fw_upload,
            data: *const u8,
            size: u32,
        ) -> bindings::fw_upload_err {
            // SAFETY: the core owns `fw` for the duration of the call.
            let handle = unsafe { (*fw).dd_handle };
            // SAFETY: `handle` came from `into_foreign()` in `Registration::new` and outlives the
            // registration; `data`/`size` describe the image the core is holding.
            let (d, image) = unsafe {
                (
                    <U::Data as ForeignOwnable>::borrow(handle.cast()),
                    core::slice::from_raw_parts(data, size as usize),
                )
            };
            Self::err(U::prepare(d, image))
        }

        /// # Safety
        ///
        /// As [`Self::prepare`]; `written` is a valid out-parameter.
        unsafe extern "C" fn write(
            fw: *mut bindings::fw_upload,
            data: *const u8,
            offset: u32,
            size: u32,
            written: *mut u32,
        ) -> bindings::fw_upload_err {
            // SAFETY: as above.
            let handle = unsafe { (*fw).dd_handle };
            // SAFETY: as above; `data + offset` is within the image the core holds.
            let (d, image, chunk) = unsafe {
                (
                    <U::Data as ForeignOwnable>::borrow(handle.cast()),
                    core::slice::from_raw_parts(data, (offset + size) as usize),
                    core::slice::from_raw_parts(data.add(offset as usize), size as usize),
                )
            };
            match U::write(d, image, offset, chunk) {
                Ok(n) => {
                    // SAFETY: the core passes a valid pointer for the result.
                    unsafe { *written = n };
                    bindings::fw_upload_err_FW_UPLOAD_ERR_NONE
                }
                Err(e) => e as bindings::fw_upload_err,
            }
        }

        /// # Safety
        ///
        /// As [`Self::prepare`].
        unsafe extern "C" fn poll_complete(
            fw: *mut bindings::fw_upload,
        ) -> bindings::fw_upload_err {
            // SAFETY: as above.
            let handle = unsafe { (*fw).dd_handle };
            // SAFETY: as above.
            let d = unsafe { <U::Data as ForeignOwnable>::borrow(handle.cast()) };
            Self::err(U::poll_complete(d))
        }

        /// # Safety
        ///
        /// As [`Self::prepare`]. Runs on a different thread from the rest.
        unsafe extern "C" fn cancel(fw: *mut bindings::fw_upload) {
            // SAFETY: as above.
            let handle = unsafe { (*fw).dd_handle };
            // SAFETY: as above.
            let d = unsafe { <U::Data as ForeignOwnable>::borrow(handle.cast()) };
            U::cancel(d)
        }

        /// # Safety
        ///
        /// As [`Self::prepare`].
        unsafe extern "C" fn cleanup(fw: *mut bindings::fw_upload) {
            // SAFETY: as above.
            let handle = unsafe { (*fw).dd_handle };
            // SAFETY: as above.
            let d = unsafe { <U::Data as ForeignOwnable>::borrow(handle.cast()) };
            U::cleanup(d)
        }

        const VTABLE: bindings::fw_upload_ops = bindings::fw_upload_ops {
            prepare: Some(Self::prepare),
            write: Some(Self::write),
            poll_complete: Some(Self::poll_complete),
            cancel: Some(Self::cancel),
            cleanup: Some(Self::cleanup),
        };
    }

    /// A live `/sys/class/firmware/<name>/` upload interface, unregistered when dropped.
    pub struct Registration<U: Upload> {
        fw: *mut bindings::fw_upload,
        data: *mut core::ffi::c_void,
        _p: core::marker::PhantomData<U>,
    }

    // SAFETY: the C side is internally locked, and `U::Data` is `Send + Sync`.
    unsafe impl<U: Upload> Send for Registration<U> {}
    // SAFETY: as above.
    unsafe impl<U: Upload> Sync for Registration<U> {}

    impl<U: Upload> Registration<U> {
        /// Publish an upload interface named `name` under `parent`.
        pub fn new(
            module: &'static crate::ThisModule,
            parent: &Device,
            name: &CStr,
            data: U::Data,
        ) -> Result<Self> {
            let handle = data.into_foreign();
            // SAFETY: `parent` and `name` are valid for the call; the vtable is 'static; `handle`
            // is kept alive by this registration and released in `drop`.
            let fw = unsafe {
                bindings::firmware_upload_register(
                    module.as_ptr(),
                    parent.as_raw(),
                    name.as_char_ptr(),
                    &Vtable::<U>::VTABLE,
                    handle.cast(),
                )
            };
            let fw = match crate::error::from_err_ptr(fw) {
                Ok(fw) => fw,
                Err(e) => {
                    // SAFETY: registration failed, so nothing else observes `handle`.
                    drop(unsafe { <U::Data as ForeignOwnable>::from_foreign(handle) });
                    return Err(e);
                }
            };
            Ok(Self {
                fw,
                data: handle.cast(),
                _p: core::marker::PhantomData,
            })
        }
    }

    impl<U: Upload> Drop for Registration<U> {
        fn drop(&mut self) {
            // SAFETY: `self.fw` came from `firmware_upload_register` and is unregistered once.
            unsafe { bindings::firmware_upload_unregister(self.fw) };
            // SAFETY: the core no longer holds `dd_handle` after unregistering.
            drop(unsafe { <U::Data as ForeignOwnable>::from_foreign(self.data.cast()) });
        }
    }
}
