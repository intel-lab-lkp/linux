// SPDX-License-Identifier: GPL-2.0

use kernel::build_assert;
use kernel::device;
use kernel::pci;
use kernel::prelude::*;
use kernel::time::Delta;
use kernel::transmute::{
    AsBytes,
    FromBytes, //
};

use super::fw::commands::*;
use super::fw::GspStaticConfigInfo_t;
use super::fw::MsgFunction;
use crate::driver::Bar0;
use crate::gsp::cmdq::Cmdq;
use crate::gsp::cmdq::{
    CommandToGsp,
    CommandToGspBase,
    CommandToGspWithPayload,
    MessageFromGsp, //
};
use crate::gsp::GSP_PAGE_SIZE;
use crate::sbuffer::SBufferIter;

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for GspStaticConfigInfo_t {}

// SAFETY: This struct only contains integer types for which all bit patterns
// are valid.
unsafe impl FromBytes for GspStaticConfigInfo_t {}

pub(crate) struct GspStaticConfigInfo {
    pub gpu_name: [u8; 40],
}

/// Message type for GSP initialization done notification.
struct GspInitDone {}

// SAFETY: `GspInitDone` is a zero-sized type with no bytes, therefore it
// trivially has no uninitialized bytes.
unsafe impl AsBytes for GspInitDone {}

// SAFETY: `GspInitDone` is a zero-sized type with no bytes, therefore it
// trivially has no uninitialized bytes.
unsafe impl FromBytes for GspInitDone {}

impl MessageFromGsp for GspInitDone {
    const FUNCTION: MsgFunction = MsgFunction::GspInitDone;
}

/// Waits for GSP initialization to complete.
pub(crate) fn gsp_init_done(cmdq: &mut Cmdq, timeout: Delta) -> Result {
    loop {
        match cmdq.receive_msg_from_gsp::<GspInitDone, ()>(timeout, |_, _| Ok(())) {
            Ok(()) => break Ok(()),
            Err(ERANGE) => continue,
            Err(e) => break Err(e),
        }
    }
}

impl MessageFromGsp for GspStaticConfigInfo_t {
    const FUNCTION: MsgFunction = MsgFunction::GetGspStaticInfo;
}

impl CommandToGspBase for GspStaticConfigInfo_t {
    const FUNCTION: MsgFunction = MsgFunction::GetGspStaticInfo;
}

impl CommandToGsp for GspStaticConfigInfo_t {}

// SAFETY: This struct only contains integer types and fixed-size arrays for which
// all bit patterns are valid.
unsafe impl Zeroable for GspStaticConfigInfo_t {}

impl GspStaticConfigInfo_t {
    fn init() -> impl Init<Self> {
        init!(GspStaticConfigInfo_t {
            ..Zeroable::init_zeroed()
        })
    }
}

pub(crate) fn get_gsp_info(cmdq: &mut Cmdq, bar: &Bar0) -> Result<GspStaticConfigInfo> {
    cmdq.send_gsp_command(bar, GspStaticConfigInfo_t::init())?;
    cmdq.receive_msg_from_gsp::<GspStaticConfigInfo_t, GspStaticConfigInfo>(
        Delta::from_secs(5),
        |info, _| {
            let gpu_name_str = info
                .gpuNameString
                .get(
                    0..=info
                        .gpuNameString
                        .iter()
                        .position(|&b| b == 0)
                        .unwrap_or(info.gpuNameString.len() - 1),
                )
                .and_then(|bytes| CStr::from_bytes_with_nul(bytes).ok())
                .and_then(|cstr| cstr.to_str().ok())
                .unwrap_or("invalid utf8");

            let mut gpu_name = [0u8; 40];
            let bytes = gpu_name_str.as_bytes();
            let copy_len = core::cmp::min(bytes.len(), gpu_name.len());
            gpu_name[..copy_len].copy_from_slice(&bytes[..copy_len]);
            gpu_name[copy_len] = b'\0';

            Ok(GspStaticConfigInfo { gpu_name })
        },
    )
}

// For now we hard-code the registry entries. Future work will allow others to
// be added as module parameters.
const GSP_REGISTRY_NUM_ENTRIES: usize = 3;
pub(crate) struct RegistryEntry {
    key: &'static str,
    value: u32,
}

pub(crate) struct RegistryTable {
    entries: [RegistryEntry; GSP_REGISTRY_NUM_ENTRIES],
}

impl CommandToGspBase for PackedRegistryTable {
    const FUNCTION: MsgFunction = MsgFunction::SetRegistry;
}
impl CommandToGspWithPayload for PackedRegistryTable {}

impl RegistryTable {
    fn write_payload<'a, I: Iterator<Item = &'a mut [u8]>>(
        &self,
        mut sbuffer: SBufferIter<I>,
    ) -> Result {
        let string_data_start_offset = size_of::<PackedRegistryTable>()
            + GSP_REGISTRY_NUM_ENTRIES * size_of::<PackedRegistryEntry>();

        // Array for string data.
        let mut string_data = KVec::new();

        for entry in self.entries.iter().take(GSP_REGISTRY_NUM_ENTRIES) {
            sbuffer.write_all(
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

        sbuffer.write_all(string_data.as_slice())
    }

    fn size(&self) -> usize {
        let mut key_size = 0;
        for i in 0..GSP_REGISTRY_NUM_ENTRIES {
            key_size += self.entries[i].key.len() + 1; // +1 for NULL terminator
        }
        GSP_REGISTRY_NUM_ENTRIES * size_of::<PackedRegistryEntry>() + key_size
    }
}

pub(crate) fn build_registry(cmdq: &mut Cmdq, bar: &Bar0) -> Result {
    let registry = RegistryTable {
        entries: [
            // RMSecBusResetEnable - enables PCI secondary bus reset
            RegistryEntry {
                key: "RMSecBusResetEnable",
                value: 1,
            },
            // RMForcePcieConfigSave - forces GSP-RM to preserve PCI
            //   configuration registers on any PCI reset.
            RegistryEntry {
                key: "RMForcePcieConfigSave",
                value: 1,
            },
            // RMDevidCheckIgnore - allows GSP-RM to boot even if the PCI dev ID
            //   is not found in the internal product name database.
            RegistryEntry {
                key: "RMDevidCheckIgnore",
                value: 1,
            },
        ],
    };

    cmdq.send_gsp_command_with_payload(
        bar,
        registry.size(),
        PackedRegistryTable::init(GSP_REGISTRY_NUM_ENTRIES as u32, registry.size() as u32),
        |sbuffer| registry.write_payload(sbuffer),
    )
}

impl CommandToGspBase for GspSystemInfo {
    const FUNCTION: MsgFunction = MsgFunction::GspSetSystemInfo;
}

impl CommandToGsp for GspSystemInfo {}

pub(crate) fn set_system_info(
    cmdq: &mut Cmdq,
    dev: &pci::Device<device::Bound>,
    bar: &Bar0,
) -> Result {
    build_assert!(size_of::<GspSystemInfo>() < GSP_PAGE_SIZE);
    cmdq.send_gsp_command(bar, GspSystemInfo::init(dev))?;

    Ok(())
}
