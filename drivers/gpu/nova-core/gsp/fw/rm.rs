// SPDX-License-Identifier: GPL-2.0

use kernel::{
    prelude::*,
    transmute::{
        AsBytes,
        FromBytes, //
    }, //
};

use crate::gsp::commands::{
    Client,
    Handle, //
};

use super::{
    bindings,
    NvStatus, //
};

/// Command code for RM control RPCs sent using [`MsgFunction::GspRmControl`].
#[derive(Copy, Clone, Debug, PartialEq)]
#[repr(u32)]
pub(crate) enum RmControlMsgFunction {
    /// Get the CE fault method buffer size.
    CeGetFaultMethodBufferSize = bindings::NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE,
}

// TODO[FPRI]: replace with 'FromPrimitive'.
impl TryFrom<u32> for RmControlMsgFunction {
    type Error = kernel::error::Error;

    fn try_from(value: u32) -> Result<Self> {
        match value {
            bindings::NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE => {
                Ok(Self::CeGetFaultMethodBufferSize)
            }
            _ => Err(EINVAL),
        }
    }
}

impl From<RmControlMsgFunction> for u32 {
    fn from(value: RmControlMsgFunction) -> Self {
        // CAST: `RmControlMsgFunction` is `repr(u32)` and can thus be cast losslessly.
        value as u32
    }
}

/// RM control message element structure.
#[repr(transparent)]
pub(crate) struct GspRmControl {
    inner: bindings::rpc_gsp_rm_control_v03_00,
}

#[expect(dead_code)]
impl GspRmControl {
    /// Creates a new RM control command.
    pub(crate) fn new<T>(
        client: Handle<Client>,
        object: Handle<T>,
        cmd: RmControlMsgFunction,
        params_size: u32,
    ) -> Self {
        Self {
            inner: bindings::rpc_gsp_rm_control_v03_00 {
                hClient: client.as_raw(),
                hObject: object.as_raw(),
                cmd: u32::from(cmd),
                status: 0,
                paramsSize: params_size,
                flags: 0,
                params: Default::default(),
            },
        }
    }

    /// Returns the status from the RM control response.
    pub(crate) fn status(&self) -> NvStatus {
        NvStatus::from(self.inner.status)
    }
}

// SAFETY: This struct only contains integer types for which all bit patterns are valid.
unsafe impl FromBytes for GspRmControl {}

// SAFETY: This struct contains no padding.
unsafe impl AsBytes for GspRmControl {}
