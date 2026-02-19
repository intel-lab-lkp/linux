// SPDX-License-Identifier: GPL-2.0

use core::{
    array,
    convert::Infallible,
    ffi::FromBytesUntilNulError,
    marker::PhantomData,
    str::Utf8Error, //
};

use kernel::{
    device,
    pci,
    prelude::*,
    time::Delta,
    transmute::{
        AsBytes,
        FromBytes, //
    }, //
};

use crate::{
    driver::Bar0,
    gsp::{
        cmdq::{
            command_size,
            Cmdq,
            CommandToGsp,
            MessageFromGsp, //
        },
        fw::{
            commands::*,
            GspMsgElement,
            MsgFunction,
            GSP_MSG_QUEUE_ELEMENT_SIZE_MAX, //
        },
    },
    sbuffer::SBufferIter,
};

/// The `GspSetSystemInfo` command.
pub(crate) struct SetSystemInfo<'a> {
    pdev: &'a pci::Device<device::Bound>,
}

impl<'a> SetSystemInfo<'a> {
    /// Creates a new `GspSetSystemInfo` command using the parameters of `pdev`.
    pub(crate) fn new(pdev: &'a pci::Device<device::Bound>) -> Self {
        Self { pdev }
    }
}

impl<'a> CommandToGsp for SetSystemInfo<'a> {
    const FUNCTION: MsgFunction = MsgFunction::GspSetSystemInfo;
    type Command = GspSetSystemInfo;
    type InitError = Error;

    fn init(&self) -> impl Init<Self::Command, Self::InitError> {
        GspSetSystemInfo::init(self.pdev)
    }
}

struct RegistryEntry {
    key: &'static str,
    value: u32,
}

/// The `SetRegistry` command.
pub(crate) struct SetRegistry {
    entries: [RegistryEntry; Self::NUM_ENTRIES],
}

impl SetRegistry {
    // For now we hard-code the registry entries. Future work will allow others to
    // be added as module parameters.
    const NUM_ENTRIES: usize = 3;

    /// Creates a new `SetRegistry` command, using a set of hardcoded entries.
    pub(crate) fn new() -> Self {
        Self {
            entries: [
                // RMSecBusResetEnable - enables PCI secondary bus reset
                RegistryEntry {
                    key: "RMSecBusResetEnable",
                    value: 1,
                },
                // RMForcePcieConfigSave - forces GSP-RM to preserve PCI configuration registers on
                // any PCI reset.
                RegistryEntry {
                    key: "RMForcePcieConfigSave",
                    value: 1,
                },
                // RMDevidCheckIgnore - allows GSP-RM to boot even if the PCI dev ID is not found
                // in the internal product name database.
                RegistryEntry {
                    key: "RMDevidCheckIgnore",
                    value: 1,
                },
            ],
        }
    }
}

impl CommandToGsp for SetRegistry {
    const FUNCTION: MsgFunction = MsgFunction::SetRegistry;
    type Command = PackedRegistryTable;
    type InitError = Infallible;

    fn init(&self) -> impl Init<Self::Command, Self::InitError> {
        PackedRegistryTable::init(Self::NUM_ENTRIES as u32, self.variable_payload_len() as u32)
    }

    fn variable_payload_len(&self) -> usize {
        let mut key_size = 0;
        for i in 0..Self::NUM_ENTRIES {
            key_size += self.entries[i].key.len() + 1; // +1 for NULL terminator
        }
        Self::NUM_ENTRIES * size_of::<PackedRegistryEntry>() + key_size
    }

    fn init_variable_payload(
        &self,
        dst: &mut SBufferIter<core::array::IntoIter<&mut [u8], 2>>,
    ) -> Result {
        let string_data_start_offset =
            size_of::<PackedRegistryTable>() + Self::NUM_ENTRIES * size_of::<PackedRegistryEntry>();

        // Array for string data.
        let mut string_data = KVec::new();

        for entry in self.entries.iter().take(Self::NUM_ENTRIES) {
            dst.write_all(
                PackedRegistryEntry::new(
                    (string_data_start_offset + string_data.len()) as u32,
                    entry.value,
                )
                .as_bytes(),
            )?;

            let key_bytes = entry.key.as_bytes();
            string_data.extend_from_slice(key_bytes, GFP_KERNEL)?;
            string_data.push(0, GFP_KERNEL)?;
        }

        dst.write_all(string_data.as_slice())
    }
}

/// Message type for GSP initialization done notification.
struct GspInitDone;

// SAFETY: `GspInitDone` is a zero-sized type with no bytes, therefore it
// trivially has no uninitialized bytes.
unsafe impl FromBytes for GspInitDone {}

impl MessageFromGsp for GspInitDone {
    const FUNCTION: MsgFunction = MsgFunction::GspInitDone;
    type InitError = Infallible;
    type Message = ();

    fn read(
        _msg: &Self::Message,
        _sbuffer: &mut SBufferIter<array::IntoIter<&[u8], 2>>,
    ) -> Result<Self, Self::InitError> {
        Ok(GspInitDone)
    }
}

/// Waits for GSP initialization to complete.
pub(crate) fn wait_gsp_init_done(cmdq: &mut Cmdq) -> Result {
    loop {
        match cmdq.receive_msg::<GspInitDone>(Delta::from_secs(10)) {
            Ok(_) => break Ok(()),
            Err(ERANGE) => continue,
            Err(e) => break Err(e),
        }
    }
}

/// The `GetGspStaticInfo` command.
struct GetGspStaticInfo;

impl CommandToGsp for GetGspStaticInfo {
    const FUNCTION: MsgFunction = MsgFunction::GetGspStaticInfo;
    type Command = GspStaticConfigInfo;
    type InitError = Infallible;

    fn init(&self) -> impl Init<Self::Command, Self::InitError> {
        GspStaticConfigInfo::init_zeroed()
    }
}

/// The reply from the GSP to the [`GetGspInfo`] command.
pub(crate) struct GetGspStaticInfoReply {
    gpu_name: [u8; 64],
}

impl MessageFromGsp for GetGspStaticInfoReply {
    const FUNCTION: MsgFunction = MsgFunction::GetGspStaticInfo;
    type Message = GspStaticConfigInfo;
    type InitError = Infallible;

    fn read(
        msg: &Self::Message,
        _sbuffer: &mut SBufferIter<array::IntoIter<&[u8], 2>>,
    ) -> Result<Self, Self::InitError> {
        Ok(GetGspStaticInfoReply {
            gpu_name: msg.gpu_name_str(),
        })
    }
}

/// Error type for [`GetGspStaticInfoReply::gpu_name`].
#[derive(Debug)]
pub(crate) enum GpuNameError {
    /// The GPU name string does not contain a null terminator.
    NoNullTerminator(FromBytesUntilNulError),

    /// The GPU name string contains invalid UTF-8.
    #[expect(dead_code)]
    InvalidUtf8(Utf8Error),
}

impl GetGspStaticInfoReply {
    /// Returns the name of the GPU as a string.
    ///
    /// Returns an error if the string given by the GSP does not contain a null terminator or
    /// contains invalid UTF-8.
    pub(crate) fn gpu_name(&self) -> core::result::Result<&str, GpuNameError> {
        CStr::from_bytes_until_nul(&self.gpu_name)
            .map_err(GpuNameError::NoNullTerminator)?
            .to_str()
            .map_err(GpuNameError::InvalidUtf8)
    }
}

/// Send the [`GetGspInfo`] command and awaits for its reply.
pub(crate) fn get_gsp_info(cmdq: &mut Cmdq, bar: &Bar0) -> Result<GetGspStaticInfoReply> {
    cmdq.send_command(bar, GetGspStaticInfo)?;

    loop {
        match cmdq.receive_msg::<GetGspStaticInfoReply>(Delta::from_secs(5)) {
            Ok(info) => return Ok(info),
            Err(ERANGE) => continue,
            Err(e) => return Err(e),
        }
    }
}

/// The `ContinuationRecord` command.
pub(crate) struct ContinuationRecord<'a> {
    data: &'a [u8],
}

impl<'a> ContinuationRecord<'a> {
    /// Creates a new `ContinuationRecord` command with the given data.
    pub(crate) fn new(data: &'a [u8]) -> Self {
        Self { data }
    }
}

impl<'a> CommandToGsp for ContinuationRecord<'a> {
    const FUNCTION: MsgFunction = MsgFunction::ContinuationRecord;
    type Command = ();
    type InitError = Infallible;

    fn init(&self) -> impl Init<Self::Command, Self::InitError> {
        <()>::init_zeroed()
    }

    fn variable_payload_len(&self) -> usize {
        self.data.len()
    }

    fn init_variable_payload(
        &self,
        dst: &mut SBufferIter<core::array::IntoIter<&mut [u8], 2>>,
    ) -> Result {
        dst.write_all(self.data)
    }
}

/// Wrapper that splits a command across continuation records if needed.
pub(crate) struct SplitState<C: CommandToGsp> {
    state: Option<(KVVec<u8>, usize)>,
    _phantom: PhantomData<C>,
}

impl<C: CommandToGsp> SplitState<C> {
    /// Maximum command size that fits in a single queue element.
    const MAX_CMD_SIZE: usize = GSP_MSG_QUEUE_ELEMENT_SIZE_MAX - size_of::<GspMsgElement>();

    /// Maximum size of the variable payload that can be sent in the main command.
    const MAX_FIRST_PAYLOAD_SIZE: usize = Self::MAX_CMD_SIZE - size_of::<C::Command>();

    /// Creates a new `SplitState` for the given command.
    ///
    /// If the command is too large, it will be split into a main command and some number of
    /// continuation records.
    pub(crate) fn new(inner: &C) -> Result<Self> {
        if command_size(inner) > Self::MAX_CMD_SIZE {
            let mut staging =
                KVVec::<u8>::from_elem(0u8, inner.variable_payload_len(), GFP_KERNEL)?;
            let mut sbuffer = SBufferIter::new_writer([staging.as_mut_slice(), &mut []]);
            inner.init_variable_payload(&mut sbuffer)?;
            if !sbuffer.is_empty() {
                return Err(EIO);
            }
            drop(sbuffer);

            Ok(Self {
                state: Some((staging, Self::MAX_FIRST_PAYLOAD_SIZE)),
                _phantom: PhantomData,
            })
        } else {
            Ok(Self {
                state: None,
                _phantom: PhantomData,
            })
        }
    }

    /// Returns the main command.
    pub(crate) fn command(&self, inner: C) -> SplitCommand<'_, C> {
        if let Some((staging, _)) = &self.state {
            SplitCommand::Split(inner, staging)
        } else {
            SplitCommand::Single(inner)
        }
    }

    /// Returns the next continuation record, or `None` if there are no more.
    pub(crate) fn next_continuation_record(&mut self) -> Option<ContinuationRecord<'_>> {
        let (staging, offset) = self.state.as_mut()?;

        let remaining = staging.len() - *offset;
        if remaining > 0 {
            let chunk_size = remaining.min(Self::MAX_CMD_SIZE);
            let record = ContinuationRecord::new(&staging[*offset..(*offset + chunk_size)]);
            *offset += chunk_size;
            Some(record)
        } else {
            None
        }
    }
}

/// Wrapper enum that represents either a single command or a split command with its staging buffer.
pub(crate) enum SplitCommand<'a, C: CommandToGsp> {
    Single(C),
    Split(C, &'a [u8]),
}

impl<'a, C: CommandToGsp> CommandToGsp for SplitCommand<'a, C> {
    const FUNCTION: MsgFunction = C::FUNCTION;
    type Command = C::Command;
    type InitError = C::InitError;

    fn init(&self) -> impl Init<Self::Command, Self::InitError> {
        match self {
            SplitCommand::Single(cmd) => cmd.init(),
            SplitCommand::Split(cmd, _) => cmd.init(),
        }
    }

    fn variable_payload_len(&self) -> usize {
        match self {
            SplitCommand::Single(cmd) => cmd.variable_payload_len(),
            SplitCommand::Split(_, _) => SplitState::<C>::MAX_FIRST_PAYLOAD_SIZE,
        }
    }

    fn init_variable_payload(
        &self,
        dst: &mut SBufferIter<core::array::IntoIter<&mut [u8], 2>>,
    ) -> Result {
        match self {
            SplitCommand::Single(cmd) => cmd.init_variable_payload(dst),
            SplitCommand::Split(_, staging) => {
                dst.write_all(&staging[..self.variable_payload_len()])
            }
        }
    }
}

