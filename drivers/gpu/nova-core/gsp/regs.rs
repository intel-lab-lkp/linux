// SPDX-License-Identifier: GPL-2.0

use kernel::io::register;

use crate::driver::NovaRegisters;

// PGSP

register! {
    base: NovaRegisters;

    pub(super) NV_PGSP_QUEUE_HEAD(u32) @ 0x00110c00 {
        31:0    address;
    }
}
