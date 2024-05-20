// SPDX-License-Identifier: GPL-2.0

//! Firmware abstraction
//!
//! C header: [`include/linux/firmware.h`](../../../../include/linux/firmware.h")

use crate::{bindings, device::Device, error::Error, error::Result, str::CStr, types::Opaque};

/// Abstraction around a C firmware struct.
///
/// This is a simple abstraction around the C firmware API. Just like with the C API, firmware can
/// be requested. Once requested the abstraction provides direct access to the firmware buffer as
/// `&[u8]`. Alternatively, the firmware can be copied to a new buffer using `Firmware::copy`. The
/// firmware is released once [`Firmware`] is dropped.
///
/// # Examples
///
/// ```
/// let fw = Firmware::request("path/to/firmware.bin", dev.as_ref())?;
/// driver_load_firmware(fw.data());
/// ```
pub struct Firmware(Opaque<*const bindings::firmware>);

impl Firmware {
    /// Send a firmware request and wait for it. See also `bindings::request_firmware`.
    pub fn request(name: &CStr, dev: &Device) -> Result<Self> {
        let fw = Opaque::uninit();

        let ret = unsafe { bindings::request_firmware(fw.get(), name.as_char_ptr(), dev.as_raw()) };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        }

        Ok(Firmware(fw))
    }

    /// Send a request for an optional fw module. See also `bindings::request_firmware_nowarn`.
    pub fn request_nowarn(name: &CStr, dev: &Device) -> Result<Self> {
        let fw = Opaque::uninit();

        let ret = unsafe {
            bindings::firmware_request_nowarn(fw.get(), name.as_char_ptr(), dev.as_raw())
        };
        if ret != 0 {
            return Err(Error::from_errno(ret));
        }

        Ok(Firmware(fw))
    }

    /// Returns the size of the requested firmware in bytes.
    pub fn size(&self) -> usize {
        unsafe { (*(*self.0.get())).size }
    }

    /// Returns the requested firmware as `&[u8]`.
    pub fn data(&self) -> &[u8] {
        unsafe { core::slice::from_raw_parts((*(*self.0.get())).data, self.size()) }
    }
}

impl Drop for Firmware {
    fn drop(&mut self) {
        unsafe { bindings::release_firmware(*self.0.get()) };
    }
}

// SAFETY: `Firmware` only holds a pointer to a C firmware struct, which is safe to be used from any
// thread.
unsafe impl Send for Firmware {}

// SAFETY: `Firmware` only holds a pointer to a C firmware struct, references to which are safe to
// be used from any thread.
unsafe impl Sync for Firmware {}
