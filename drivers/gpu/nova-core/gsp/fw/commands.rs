// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

use core::ops::Range;

use kernel::{
    alloc::ArrayVec,
    bitfield,
    device,
    pci,
    prelude::*,
    transmute::{
        AsBytes,
        FromBytes, //
    }, //
};

use crate::{
    gpu::Chipset,
    gsp::{
        nvkv::{
            nvkv_decode,
            nvkv_encode,
            Accumulated,
            Array,
            DecoderValue,
            Encodable,
            Encoder,
            Key,
            KeyId,
            Required, //
        },
        GSP_PAGE_SIZE, //
    },
    num::IntoSafeCast, //
};

use super::bindings;

/// Payload of the `GspSetSystemInfo` command.
#[repr(transparent)]
pub(crate) struct GspSetSystemInfo {
    inner: bindings::GspSystemInfo,
}
static_assert!(size_of::<GspSetSystemInfo>() < GSP_PAGE_SIZE);

impl GspSetSystemInfo {
    /// Returns an in-place initializer for the `GspSetSystemInfo` command.
    pub(crate) fn init<'a>(
        dev: &'a pci::Device<device::Bound>,
        chipset: Chipset,
    ) -> impl Init<Self, Error> + 'a {
        type InnerGspSystemInfo = bindings::GspSystemInfo;
        let pci_config_mirror_range = chipset.pci_config_mirror_range();
        let init_inner = try_init!(InnerGspSystemInfo {
            gpuPhysAddr: dev.resource_start(0)?,
            gpuPhysFbAddr: dev.resource_start(1)?,
            gpuPhysInstAddr: dev.resource_start(3)?,
            nvDomainBusDeviceFunc: u64::from(dev.dev_id()),

            // Using TASK_SIZE in r535_gsp_rpc_set_system_info() seems wrong because
            // TASK_SIZE is per-task. That's probably a design issue in GSP-RM though.
            maxUserVa: (1 << 47) - 4096,
            pciConfigMirrorBase: pci_config_mirror_range.start,
            pciConfigMirrorSize: pci_config_mirror_range.end - pci_config_mirror_range.start,

            PCIDeviceID: (u32::from(dev.device_id()) << 16) | u32::from(dev.vendor_id().as_raw()),
            PCISubDeviceID: (u32::from(dev.subsystem_device_id()) << 16)
                | u32::from(dev.subsystem_vendor_id()),
            PCIRevisionID: u32::from(dev.revision_id()),
            bIsPrimary: 0,
            bPreserveVideoMemoryAllocations: 0,
            ..Zeroable::init_zeroed()
        });

        try_init!(GspSetSystemInfo {
            inner <- init_inner,
        })
    }
}

// SAFETY: These structs don't meet the no-padding requirements of AsBytes but
//         that is not a problem because they are not used outside the kernel.
unsafe impl AsBytes for GspSetSystemInfo {}

// SAFETY: These structs don't meet the no-padding requirements of FromBytes but
//         that is not a problem because they are not used outside the kernel.
unsafe impl FromBytes for GspSetSystemInfo {}

#[repr(transparent)]
pub(crate) struct PackedRegistryEntry(bindings::PACKED_REGISTRY_ENTRY);

impl PackedRegistryEntry {
    pub(crate) fn new(offset: u32, value: u32) -> Self {
        Self({
            bindings::PACKED_REGISTRY_ENTRY {
                nameOffset: offset,

                // We only support DWORD types for now. Support for other types
                // will come later if required.
                type_: bindings::REGISTRY_TABLE_ENTRY_TYPE_DWORD as u8,
                __bindgen_padding_0: Default::default(),
                data: value,
                length: 0,
            }
        })
    }
}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for PackedRegistryEntry {}

/// Payload of the `SetRegistry` command.
#[repr(transparent)]
pub(crate) struct PackedRegistryTable {
    inner: bindings::PACKED_REGISTRY_TABLE,
}

impl PackedRegistryTable {
    pub(crate) fn init(num_entries: u32, size: u32) -> impl Init<Self> {
        type InnerPackedRegistryTable = bindings::PACKED_REGISTRY_TABLE;
        let init_inner = init!(InnerPackedRegistryTable {
            numEntries: num_entries,
            size,
            entries: Default::default()
        });

        init!(PackedRegistryTable { inner <- init_inner })
    }
}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for PackedRegistryTable {}

// SAFETY: This struct only contains integer types for which all bit patterns
// are valid.
unsafe impl FromBytes for PackedRegistryTable {}

/// Payload of the `GetGspStaticInfo` command and message.
#[repr(transparent)]
#[derive(Zeroable)]
pub(crate) struct GspStaticConfigInfo(bindings::GspStaticConfigInfo_t);

impl GspStaticConfigInfo {
    /// Returns a bytes array containing the (hopefully) zero-terminated name of this GPU.
    pub(crate) fn gpu_name_str(&self) -> [u8; 64] {
        self.0.gpuNameString
    }

    /// Returns the BAR1 Page Directory Entry base address.
    ///
    /// This is the root page table address for BAR1 virtual memory,
    /// set up by GSP-RM firmware.
    pub(crate) fn bar1_pde_base(&self) -> u64 {
        self.0.bar1PdeBase
    }

    /// Returns an iterator over valid FB regions from GSP firmware data.
    fn fb_regions(
        &self,
    ) -> impl Iterator<Item = &bindings::NV2080_CTRL_CMD_FB_GET_FB_REGION_FB_REGION_INFO> {
        let fb_info = &self.0.fbRegionInfoParams;
        fb_info
            .fbRegion
            .iter()
            .take(fb_info.numFBRegions.into_safe_cast())
            .filter(|reg| reg.limit >= reg.base)
    }

    /// Iterates over usable FB regions from GSP firmware data.
    ///
    /// Each yielded region is a [`Range<u64>`] suitable for driver memory allocation.
    /// Usable regions are those that satisfy all the following properties:
    /// - Are not reserved for firmware internal use.
    /// - Are not protected (hardware-enforced access restrictions).
    /// - Support compression (can use GPU memory compression for bandwidth).
    /// - Support ISO (isochronous memory for display requiring guaranteed bandwidth).
    pub(crate) fn usable_fb_regions(&self) -> impl Iterator<Item = Range<u64>> + '_ {
        self.fb_regions().filter_map(|reg| {
            // Filter: not reserved, not protected, supports compression and ISO.
            if reg.reserved == 0
                && reg.bProtected == 0
                && reg.supportCompressed != 0
                && reg.supportISO != 0
            {
                reg.limit.checked_add(1).map(|end| reg.base..end)
            } else {
                None
            }
        })
    }

    /// Computes the exclusive end of the FB physical address space.
    pub(crate) fn total_fb_end(&self) -> Option<u64> {
        self.fb_regions().map(|reg| reg.limit).max()?.checked_add(1)
    }
}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for GspStaticConfigInfo {}

// SAFETY: This struct only contains integer types for which all bit patterns
// are valid.
unsafe impl FromBytes for GspStaticConfigInfo {}

/// Power level requested to the [`UnloadingGuestDriver`] command.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
#[expect(unused)]
pub(crate) enum PowerStateLevel {
    /// Full unload.
    Level0 = bindings::NV2080_CTRL_GPU_SET_POWER_STATE_GPU_LEVEL_0,
    /// S3 (suspend to RAM).
    Level3 = bindings::NV2080_CTRL_GPU_SET_POWER_STATE_GPU_LEVEL_3,
    /// Hibernate (suspend to disk).
    Level7 = bindings::NV2080_CTRL_GPU_SET_POWER_STATE_GPU_LEVEL_7,
}

impl PowerStateLevel {
    /// Returns `true` if this state represents a power management transition, i.e. some GPU state
    /// must survive it (as opposed to a full unload).
    pub(crate) fn is_power_transition(self) -> bool {
        self != PowerStateLevel::Level0
    }
}

/// Payload of the `UnloadingGuestDriver` command and message.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Zeroable)]
pub(crate) struct UnloadingGuestDriver(bindings::rpc_unloading_guest_driver_v1F_07);

impl UnloadingGuestDriver {
    pub(crate) fn new(level: PowerStateLevel) -> Self {
        Self(bindings::rpc_unloading_guest_driver_v1F_07 {
            bInPMTransition: u8::from(level.is_power_transition()),
            bGc6Entering: 0,
            newLevel: level as u32,
            ..Zeroable::zeroed()
        })
    }
}

// SAFETY: Padding is explicit and will not contain uninitialized data.
unsafe impl AsBytes for UnloadingGuestDriver {}

// SAFETY: This struct only contains integer types for which all bit patterns
// are valid.
unsafe impl FromBytes for UnloadingGuestDriver {}

/// The host CPU architecture.
#[derive(Clone, Copy)]
pub(crate) enum HostArch {
    None = 0,
    X86_64 = 1,
    Ppc64le = 2,
    Arm = 3,
    Aarch64 = 4,
    Riscv64 = 5,
}

// TODO[FPRI]: This is a temporary solution to be replaced with the corresponding derive macros once
// they land.
impl TryFrom<u32> for HostArch {
    type Error = Error;

    fn try_from(value: u32) -> Result<Self> {
        match value {
            0 => Ok(Self::None),
            1 => Ok(Self::X86_64),
            2 => Ok(Self::Ppc64le),
            3 => Ok(Self::Arm),
            4 => Ok(Self::Aarch64),
            5 => Ok(Self::Riscv64),
            _ => Err(EINVAL),
        }
    }
}

impl From<HostArch> for u32 {
    fn from(value: HostArch) -> Self {
        value as u32
    }
}

nvkv_encode! {
    /// A GSP registry entry.
    struct RegKey {
        key_name: Key<&'static [u8], { Self::REGKEY_NAME_KEY }>,
        key_value: Key<u32, { Self::REGKEY_VALUE_U32_KEY }>,
    }
}

impl RegKey {
    // Define the Key IDs read/written by GSP.
    const REGKEY_NAME_KEY: KeyId = 0x3070;
    const REGKEY_VALUE_U32_KEY: KeyId = 0x3071;
}

impl Encodable for KVVec<RegKey> {
    fn encode(&self, encoder: &mut Encoder) -> Result {
        for regkey in self {
            regkey.encode(encoder)?;
        }
        Ok(())
    }
}

nvkv_encode! {
    /// SR-IOV virtual function information.
    struct VfInfo {
        total_vfs: Key<u32, { Self::VF_TOTAL_VFS_KEY }>,
        first_vf_offset: Key<u32, { Self::VF_FIRST_VF_OFFSET_KEY }>,
        flags: Key<u64, { Self::VF_FLAGS_KEY }>,
        first_bar0_address: Key<u64, { Self::VF_FIRST_BAR0_ADDRESS_KEY }>,
        first_bar1_address: Key<u64, { Self::VF_FIRST_BAR1_ADDRESS_KEY }>,
        first_bar2_address: Key<u64, { Self::VF_FIRST_BAR2_ADDRESS_KEY }>,
    }
}

impl VfInfo {
    // Define the Key IDs read/written by GSP.
    const VF_TOTAL_VFS_KEY: KeyId = 0x0080;
    const VF_FIRST_VF_OFFSET_KEY: KeyId = 0x0081;
    const VF_FLAGS_KEY: KeyId = 0x1003;
    const VF_FIRST_BAR0_ADDRESS_KEY: KeyId = 0x1050;
    const VF_FIRST_BAR1_ADDRESS_KEY: KeyId = 0x1051;
    const VF_FIRST_BAR2_ADDRESS_KEY: KeyId = 0x1052;
}

nvkv_encode! {
    /// Payload of the `GSP_INIT` command.
    // TODO: expect() doesn't work here due to Self:: reference, fixed in 1.97.0
    // https://github.com/rust-lang/rust/pull/154377
    #[cfg_attr(not(CONFIG_KUNIT), allow(dead_code))]
    struct GspInitRequest {
        pci_device_id: Key<u32, { Self::PCI_DEVICE_ID_KEY }>,
        pci_sub_device_id: Key<u32, { Self::PCI_SUBDEVICE_ID_KEY }>,
        pci_revision_id: Key<u32, { Self::PCI_REVISION_ID_KEY }>,
        pci_config_mirror_base: Key<u32, { Self::PCI_CONFIG_MIRROR_BASE_KEY }>,
        pci_config_mirror_size: Key<u32, { Self::PCI_CONFIG_MIRROR_SIZE_KEY }>,
        host_arch: Key<HostArch, { Self::HOST_ARCH_KEY }, u32>,
        bus_device_func: Key<u64, { Self::NV_DOMAIN_BUS_DEVICE_FUNC_KEY }>,
        regkeys: KVVec<RegKey>,
        vf_info: Option<VfInfo>,
    }
}

impl GspInitRequest {
    // Define the Key IDs read/written by GSP.
    const PCI_DEVICE_ID_KEY: KeyId = 0x0001;
    const PCI_SUBDEVICE_ID_KEY: KeyId = 0x0002;
    const PCI_REVISION_ID_KEY: KeyId = 0x0003;
    const PCI_CONFIG_MIRROR_BASE_KEY: KeyId = 0x0010;
    const PCI_CONFIG_MIRROR_SIZE_KEY: KeyId = 0x0011;
    const HOST_ARCH_KEY: KeyId = 0x0070;
    const NV_DOMAIN_BUS_DEVICE_FUNC_KEY: KeyId = 0x1020;
}

// Decode:

// Should decode with UnknownKeyPolicy::Ignore.
nvkv_decode! {
    /// Schema for the `GSP_INIT` response.
    // TODO: expect() doesn't work here due to Self:: reference, fixed in 1.97.0
    // https://github.com/rust-lang/rust/pull/154377
    #[cfg_attr(not(CONFIG_KUNIT), allow(dead_code))]
    struct GspInitResponseSchema => GspInitResponse {
        gpu_name:
            Array<u8, { GspInitResponse::MAX_GPU_NAME_LEN }, { Self::GPU_NAME_STRING_KEY }>,
        fb_regions: Accumulated<FbRegionSchema>,
        bar1_pde_base: Required<u64, { Self::BAR1_PDE_BASE_KEY }>,
        vmmu_segment_size: Key<u64, { Self::VMMU_SEGMENT_SIZE_KEY }>,
    }
}

impl GspInitResponseSchema {
    // Define the Key IDs read/written by GSP.
    const GPU_NAME_STRING_KEY: KeyId = 0x2000;
    const BAR1_PDE_BASE_KEY: KeyId = 0x1020;
    const VMMU_SEGMENT_SIZE_KEY: KeyId = 0x1050;
}

/// Payload of the `GSP_INIT` response.
struct GspInitResponse {
    gpu_name: ArrayVec<u8, { Self::MAX_GPU_NAME_LEN }>,
    fb_regions: KVVec<FbRegion>,
    bar1_pde_base: u64,
    vmmu_segment_size: u64,
}

impl GspInitResponse {
    const MAX_GPU_NAME_LEN: usize = 64;
}

nvkv_decode! {
    /// Schema for one FB region of the `GSP_INIT` response.
    struct FbRegionSchema => FbRegion {
        base: Required<u64, { Self::BASE_KEY }>,
        limit: Required<u64, { Self::LIMIT_KEY }>,
        flags: Required<FbRegionFlags, { Self::FLAGS_KEY }>,
        tag: Required<u32, { Self::TAG_KEY }>,
    }
}

impl FbRegionSchema {
    // Define the Key IDs read/written by GSP.
    const BASE_KEY: KeyId = 0x1011;
    const LIMIT_KEY: KeyId = 0x1012;
    const FLAGS_KEY: KeyId = 0x0012;
    const TAG_KEY: KeyId = 0x0013;
}

bitfield! {
    /// FB region attribute flags.
    struct FbRegionFlags(u32) {
        0:0 support_compressed => bool;
        1:1 support_iso => bool;
        2:2 protected => bool;
    }
}

impl TryFrom<DecoderValue<'_>> for FbRegionFlags {
    type Error = Error;

    fn try_from(value: DecoderValue<'_>) -> Result<Self> {
        if let DecoderValue::Scalar32(v) = value {
            Ok(v.into())
        } else {
            Err(EINVAL)
        }
    }
}

/// One FB memory region.
struct FbRegion {
    base: u64,
    limit: u64,
    flags: FbRegionFlags,
    tag: u32,
}

#[kunit_tests(nova_core_fw_commands)]
mod tests {
    use crate::gsp::nvkv::{
        Decoder,
        DecoderValue,
        Index,
        Schema,
        UnknownKeyPolicy,
        Visit, //
    };

    use super::*;

    /// Decodes the registry keys of a `GspInitRequest` into (name, value) pairs.
    #[derive(Default)]
    struct RegKeysSchema<'d> {
        pairs: KVVec<(&'d [u8], u32)>,
        // A name whose value has not been decoded yet.
        name: Option<&'d [u8]>,
    }

    impl<'d> Schema for RegKeysSchema<'d> {
        type Target = KVVec<(&'d [u8], u32)>;

        fn init() -> impl Init<Self> {
            Self::default()
        }

        fn finish(&mut self) -> impl Init<Self::Target, Error> + '_ {
            // A name must be followed by its value.
            if self.name.take().is_some() {
                return Err(EINVAL);
            }
            Ok(core::mem::take(&mut self.pairs))
        }
    }

    impl<'d> Visit<'d> for RegKeysSchema<'d> {
        fn visit(&mut self, key: KeyId, index: Index, value: DecoderValue<'d>) -> Result<bool> {
            if key != RegKey::REGKEY_NAME_KEY && key != RegKey::REGKEY_VALUE_U32_KEY {
                return Ok(false);
            }
            if index != Index::new::<0>() {
                return Err(EINVAL);
            }
            if key == RegKey::REGKEY_NAME_KEY {
                // A name must be followed by its value before the next name.
                if self.name.replace(value.try_into()?).is_some() {
                    return Err(EINVAL);
                }
            } else {
                let name = self.name.take().ok_or(EINVAL)?;
                self.pairs.push((name, value.try_into()?), GFP_KERNEL)?;
            }
            Ok(true)
        }
    }

    // Tests that `GspInitRequest` encodes correctly.
    #[test]
    fn gsp_init_request() -> Result {
        let mut encoder = Encoder::new();

        let mut regkeys = KVVec::new();
        regkeys.push(
            RegKey {
                key_name: b"testkey1\0".into(),
                key_value: 0xdead_beef.into(),
            },
            GFP_KERNEL,
        )?;
        regkeys.push(
            RegKey {
                key_name: b"testkey2\0".into(),
                key_value: 0x1234_5678.into(),
            },
            GFP_KERNEL,
        )?;

        let gsp_init = GspInitRequest {
            pci_device_id: 45.into(),
            pci_sub_device_id: 67.into(),
            pci_revision_id: 3.into(),
            pci_config_mirror_base: 0x1234_5678.into(),
            pci_config_mirror_size: 0x1000.into(),
            host_arch: HostArch::Aarch64.into(),
            bus_device_func: 0x0001_0203_0405_0607.into(),
            regkeys,
            vf_info: Some(VfInfo {
                total_vfs: 8.into(),
                first_vf_offset: 1.into(),
                flags: 0x7.into(),
                first_bar0_address: 0x1000_0000.into(),
                first_bar1_address: 0x2000_0000.into(),
                first_bar2_address: 0x3000_0000.into(),
            }),
        };

        gsp_init.encode(&mut encoder)?;
        let encoded = encoder.finish();
        assert_eq!(encoded.len(), 26);

        type Req = GspInitRequest;
        let index0 = Index::new::<0>();
        let mut expected = Encoder::new();
        expected.encode_u32(Req::PCI_DEVICE_ID_KEY, index0, 45)?;
        expected.encode_u32(Req::PCI_SUBDEVICE_ID_KEY, index0, 67)?;
        expected.encode_u32(Req::PCI_REVISION_ID_KEY, index0, 3)?;
        expected.encode_u32(Req::PCI_CONFIG_MIRROR_BASE_KEY, index0, 0x1234_5678)?;
        expected.encode_u32(Req::PCI_CONFIG_MIRROR_SIZE_KEY, index0, 0x1000)?;
        expected.encode_u32(Req::HOST_ARCH_KEY, index0, HostArch::Aarch64.into())?;
        expected.encode_u64(
            Req::NV_DOMAIN_BUS_DEVICE_FUNC_KEY,
            index0,
            0x0001_0203_0405_0607,
        )?;
        // Each registry key is its name followed by its value, both at index 0.
        expected.encode_array8(RegKey::REGKEY_NAME_KEY, index0, b"testkey1\0")?;
        expected.encode_u32(RegKey::REGKEY_VALUE_U32_KEY, index0, 0xdead_beef)?;
        expected.encode_array8(RegKey::REGKEY_NAME_KEY, index0, b"testkey2\0")?;
        expected.encode_u32(RegKey::REGKEY_VALUE_U32_KEY, index0, 0x1234_5678)?;
        expected.encode_u32(VfInfo::VF_TOTAL_VFS_KEY, index0, 8)?;
        expected.encode_u32(VfInfo::VF_FIRST_VF_OFFSET_KEY, index0, 1)?;
        expected.encode_u64(VfInfo::VF_FLAGS_KEY, index0, 0x7)?;
        expected.encode_u64(VfInfo::VF_FIRST_BAR0_ADDRESS_KEY, index0, 0x1000_0000)?;
        expected.encode_u64(VfInfo::VF_FIRST_BAR1_ADDRESS_KEY, index0, 0x2000_0000)?;
        expected.encode_u64(VfInfo::VF_FIRST_BAR2_ADDRESS_KEY, index0, 0x3000_0000)?;
        assert_eq!(*encoded, *expected.finish());

        // Decode the registry keys back out of the request, ignoring the other keys.
        let decoder = Decoder::new(&encoded, UnknownKeyPolicy::Ignore);
        let mut schema = KBox::init(RegKeysSchema::init(), GFP_KERNEL)?;
        let decoded = KBox::try_init(decoder.decode(&mut *schema)?, GFP_KERNEL)?;
        assert_eq!(
            decoded.as_slice(),
            &[
                (&b"testkey1\0"[..], 0xdead_beef),
                (&b"testkey2\0"[..], 0x1234_5678),
            ]
        );

        Ok(())
    }

    // Tests that FB region decoding fails when required keys are missing.
    #[test]
    fn decode_fb_region_missing_required_fails() -> Result {
        let mut encoder = Encoder::new();
        encoder.encode_u64(FbRegionSchema::BASE_KEY, Index::new::<0>(), 0x1000_0000)?;
        let data = encoder.finish();

        let decoder = Decoder::new(&data, UnknownKeyPolicy::Ignore);
        let mut schema = KBox::init(FbRegionSchema::init(), GFP_KERNEL)?;
        let init = decoder.decode(&mut *schema)?;
        assert!(KBox::try_init(init, GFP_KERNEL).is_err());

        Ok(())
    }

    // Tests that a minimal and a full `GSP_INIT` response decode correctly.
    #[test]
    fn gsp_init_response() -> Result {
        let name = b"test name\0";
        const BAR1_PDE_BASE: u64 = 0xdead_0000;
        const FB_REGION0_BASE: u64 = 0x1000_0000;
        const FB_REGION0_LIMIT: u64 = 0x1fff_ffff;
        const FB_REGION0_FLAGS: u32 = 0x7;
        const FB_REGION0_TAG: u32 = 0;
        const FB_REGION1_BASE: u64 = 0x2000_0000;
        const FB_REGION1_LIMIT: u64 = 0x2fff_ffff;
        const FB_REGION1_FLAGS: u32 = 0x3;
        const FB_REGION1_TAG: u32 = 1;
        const VMMU_SEGMENT_SIZE: u64 = 0x0200_0000;

        type Resp = GspInitResponseSchema;

        let index0 = Index::new::<0>();
        let index1 = Index::new::<1>();

        // A minimal response: only the BAR1 PDE base, so the FB region list stays empty.
        let mut encoder = Encoder::new();
        encoder.encode_u64(Resp::BAR1_PDE_BASE_KEY, index0, BAR1_PDE_BASE)?;
        let data = encoder.finish();

        let decoder = Decoder::new(&data, UnknownKeyPolicy::Ignore);
        let mut schema = KBox::init(Resp::init(), GFP_KERNEL)?;
        let response = KBox::try_init(decoder.decode(&mut *schema)?, GFP_KERNEL)?;
        assert_eq!(response.bar1_pde_base, BAR1_PDE_BASE);
        assert!(response.fb_regions.is_empty());

        // A full response.
        let mut encoder = Encoder::new();
        encoder.encode_array8(Resp::GPU_NAME_STRING_KEY, index0, name)?;
        encoder.encode_u64(Resp::BAR1_PDE_BASE_KEY, index0, BAR1_PDE_BASE)?;
        encoder.encode_u64(FbRegionSchema::BASE_KEY, index0, FB_REGION0_BASE)?;
        encoder.encode_u64(FbRegionSchema::LIMIT_KEY, index0, FB_REGION0_LIMIT)?;
        encoder.encode_u32(FbRegionSchema::FLAGS_KEY, index0, FB_REGION0_FLAGS)?;
        encoder.encode_u32(FbRegionSchema::TAG_KEY, index0, FB_REGION0_TAG)?;

        // Test that this unrelated key can safely interleave.
        encoder.encode_u64(Resp::VMMU_SEGMENT_SIZE_KEY, index0, VMMU_SEGMENT_SIZE)?;

        encoder.encode_u64(FbRegionSchema::BASE_KEY, index1, FB_REGION1_BASE)?;
        encoder.encode_u64(FbRegionSchema::LIMIT_KEY, index1, FB_REGION1_LIMIT)?;
        encoder.encode_u32(FbRegionSchema::FLAGS_KEY, index1, FB_REGION1_FLAGS)?;
        encoder.encode_u32(FbRegionSchema::TAG_KEY, index1, FB_REGION1_TAG)?;
        let data = encoder.finish();

        let decoder = Decoder::new(&data, UnknownKeyPolicy::Error);
        let mut schema = KBox::init(Resp::init(), GFP_KERNEL)?;
        let response = KBox::try_init(decoder.decode(&mut *schema)?, GFP_KERNEL)?;

        assert_eq!(&*response.gpu_name, &name[..]);
        assert_eq!(response.bar1_pde_base, BAR1_PDE_BASE);
        assert_eq!(response.fb_regions.len(), 2);

        let fb_region0 = &response.fb_regions[0];
        assert_eq!(fb_region0.base, FB_REGION0_BASE);
        assert_eq!(fb_region0.limit, FB_REGION0_LIMIT);
        assert_eq!(fb_region0.flags.into_raw(), FB_REGION0_FLAGS);
        assert!(fb_region0.flags.support_compressed());
        assert!(fb_region0.flags.support_iso());
        assert!(fb_region0.flags.protected());
        assert_eq!(fb_region0.tag, FB_REGION0_TAG);

        let fb_region1 = &response.fb_regions[1];
        assert_eq!(fb_region1.base, FB_REGION1_BASE);
        assert_eq!(fb_region1.limit, FB_REGION1_LIMIT);
        assert_eq!(fb_region1.flags.into_raw(), FB_REGION1_FLAGS);
        assert_eq!(fb_region1.tag, FB_REGION1_TAG);

        assert_eq!(response.vmmu_segment_size, VMMU_SEGMENT_SIZE);

        Ok(())
    }
}
