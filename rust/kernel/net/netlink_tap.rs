// SPDX-License-Identifier: GPL-2.0

//! Netlink tap lifecycle helpers.
//!
//! C header: [`include/linux/netlink.h`](srctree/include/linux/netlink.h)

use crate::{
    bindings,
    error::to_result,
    net::netdevice,
    prelude::*,
    types::Opaque,
    ThisModule,
};

/// Owns a `struct netlink_tap` plus its registration state.
#[pin_data]
#[derive(Zeroable)]
pub struct Tap {
    #[pin]
    inner: Opaque<bindings::netlink_tap>,
    registered: bool,
}

impl Tap {
    /// Creates an unregistered tap.
    pub const fn new() -> Self {
        Self {
            inner: Opaque::zeroed(),
            registered: false,
        }
    }

    /// Returns whether the tap is currently registered.
    pub fn is_registered(&self) -> bool {
        self.registered
    }

    /// Registers the tap for the provided device.
    pub fn add(
        self: Pin<&mut Self>,
        dev: &netdevice::Device,
        module: &'static ThisModule,
    ) -> Result {
        // SAFETY: The caller pinned `self`, so accessing the interior through the stable address is
        // valid for the duration of this method.
        let this = unsafe { self.get_unchecked_mut() };

        if this.registered {
            return Err(EBUSY);
        }

        let tap = this.inner.get();

        // SAFETY: `tap` points to valid storage for `struct netlink_tap`.
        unsafe {
            (*tap).dev = dev.as_ptr();
            (*tap).module = module.as_ptr();
        }

        // SAFETY: `tap` points to a valid `struct netlink_tap`.
        to_result(unsafe { bindings::netlink_add_tap(tap) })?;
        this.registered = true;
        Ok(())
    }

    /// Unregisters the tap if it is currently active.
    pub fn remove(self: Pin<&mut Self>) -> Result {
        // SAFETY: The caller pinned `self`, so accessing the interior through the stable address is
        // valid for the duration of this method.
        let this = unsafe { self.get_unchecked_mut() };

        if !this.registered {
            return Ok(());
        }

        let tap = this.inner.get();

        // SAFETY: `self.inner` contains a valid `struct netlink_tap` previously passed to
        // `netlink_add_tap`.
        to_result(unsafe { bindings::netlink_remove_tap(tap) })?;
        this.registered = false;

        // SAFETY: The tap has been removed and `netlink_remove_tap` waited for in-flight users via
        // `synchronize_net`, so restoring the zeroed unregistered state is valid.
        unsafe { tap.write(core::mem::zeroed()) };
        Ok(())
    }
}
