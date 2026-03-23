// SPDX-License-Identifier: GPL-2.0

//! Rust software watchdog driver.
//!
//! A simplified Rust implementation of the software watchdog, demonstrating
//! the Rust watchdog device abstraction.

use kernel::prelude::*;
use kernel::watchdog;

const DEFAULT_MARGIN: u32 = 60;
const MAX_MARGIN: u32 = 65535;

module! {
    type: SoftdogModule,
    name: "softdog_rs",
    authors: ["Artem Lytkin"],
    description: "Rust Software Watchdog Device Driver",
    license: "GPL",
}

struct SoftdogOps;

#[vtable]
impl watchdog::WatchdogOps for SoftdogOps {
    fn start(dev: &mut watchdog::Device) -> Result {
        pr_info!("watchdog started (timeout={}s)\n", dev.timeout());
        Ok(())
    }

    fn stop(_dev: &mut watchdog::Device) -> Result {
        pr_info!("watchdog stopped\n");
        Ok(())
    }
}

static SOFTDOG_INFO: bindings::watchdog_info = bindings::watchdog_info {
    options: bindings::WDIOF_SETTIMEOUT
        | bindings::WDIOF_KEEPALIVEPING
        | bindings::WDIOF_MAGICCLOSE,
    firmware_version: 0,
    identity: {
        let mut id = [0u8; 32];
        let s = b"Rust Software Watchdog";
        let mut i = 0;
        while i < s.len() {
            id[i] = s[i];
            i += 1;
        }
        id
    },
};

static mut SOFTDOG_OPS: bindings::watchdog_ops =
    watchdog::create_watchdog_ops::<SoftdogOps>();

struct SoftdogModule {
    _reg: watchdog::Registration,
}

impl kernel::Module for SoftdogModule {
    fn init(module: &'static ThisModule) -> Result<Self> {
        // SAFETY: SOFTDOG_OPS is only mutated here, before registration,
        // and this function is called exactly once during module init.
        let ops = unsafe { &mut *core::ptr::addr_of_mut!(SOFTDOG_OPS) };

        let reg = watchdog::Registration::register(
            module,
            None,
            &SOFTDOG_INFO,
            ops,
            DEFAULT_MARGIN,
            1,
            MAX_MARGIN,
            false,
        )?;

        pr_info!("initialized (timeout={}s)\n", DEFAULT_MARGIN);

        Ok(SoftdogModule { _reg: reg })
    }
}

impl Drop for SoftdogModule {
    fn drop(&mut self) {
        pr_info!("exit\n");
    }
}
