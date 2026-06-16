// SPDX-License-Identifier: GPL-2.0

use kernel::io::register;

// PBUS

register! {
    pub(super) NV_PBUS_SW_SCRATCH(u32)[64] @ 0x00001400 {}

    /// Scratch register 0xe used as FRTS firmware error code.
    pub(super) NV_PBUS_SW_SCRATCH_0E_FRTS_ERR(u32) => NV_PBUS_SW_SCRATCH[0xe] {
        31:16   frts_err_code;
    }
}

// PGSP

register! {
    pub(super) NV_PGSP_QUEUE_HEAD(u32) @ 0x00110c00 {
        31:0    address;
    }
}
