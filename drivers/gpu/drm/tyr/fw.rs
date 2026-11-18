// SPDX-License-Identifier: GPL-2.0 or MIT

//! Firmware loading and management for Mali CSF GPU.
//!
//! This module handles loading the Mali GPU firmware binary, parsing it into sections,
//! and mapping those sections into the MCU's virtual address space. Each firmware section
//! has specific properties (read/write/execute permissions, cache modes) and must be loaded
//! at specific virtual addresses expected by the MCU.
//!
//! See [`Firmware`] for the main firmware management interface and [`Section`] for
//! individual firmware sections.
//!
//! [`Firmware`]: crate::fw::Firmware
//! [`Section`]: crate::fw::Section

use kernel::{
    bitfield,
    device::{
        Bound,
        Device, //
    },
    drm::{
        gem::BaseObject, //
    },
    io::{
        poll,
        Io, //
    },
    num::Bounded,
    prelude::*,
    str::CString,
    time,
    types::ScopeGuard, //
};

use crate::{
    driver::{
        IoMem,
        TyrDrmDevice, //
    },
    fw::parser::{
        FwParser,
        ParsedSection, //
    },
    gem,
    gem::{
        KernelBo,
        KernelBoVaAlloc, //
    },
    gpu::GpuInfo,

    mmu::Mmu,
    regs::{
        gpu_control::{
            McuControlMode,
            McuStatus,
            GPU_ID,
            MCU_CONTROL,
            MCU_STATUS, //
        }, //
        job_control::{
            JOB_IRQ_CLEAR,
            JOB_IRQ_RAWSTAT, //
        }, //
    },
    vm::Vm, //
};

mod parser;

pub(super) const CSF_MCU_SHARED_REGION_START: u32 = 0x04000000;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(super) enum CacheMode {
    None = 0,
    Cached = 1,
    UncachedCoherent = 2,
    CachedCoherent = 3,
}

impl From<Bounded<u32, 2>> for CacheMode {
    fn from(value: Bounded<u32, 2>) -> Self {
        match value.get() {
            0 => Self::None,
            1 => Self::Cached,
            2 => Self::UncachedCoherent,
            3 => Self::CachedCoherent,
            _ => unreachable!(),
        }
    }
}

impl From<CacheMode> for Bounded<u32, 2> {
    fn from(value: CacheMode) -> Self {
        Bounded::try_new(value as u32).unwrap()
    }
}

bitfield! {
    pub(super) struct SectionFlags(u32) {
        0:0 read => bool;
        1:1 write => bool;
        2:2 exec => bool;
        4:3 cache_mode => CacheMode;
        5:5 prot => bool;
        30:30 shared => bool;
        31:31 zero => bool;
    }
}

impl SectionFlags {
    const VALID_MASK: u32 = Self::READ_MASK
        | Self::WRITE_MASK
        | Self::EXEC_MASK
        | Self::CACHE_MODE_MASK
        | Self::PROT_MASK
        | Self::SHARED_MASK
        | Self::ZERO_MASK;

    fn try_from_fw(value: u32) -> Result<Self> {
        if value & !Self::VALID_MASK != 0 {
            Err(EINVAL)
        } else {
            Ok(Self::from_raw(value))
        }
    }
}

/// A parsed section of the firmware binary.
struct Section<'sec, 'res> {
    // Raw firmware section data for reset purposes
    #[expect(dead_code)]
    data: KVec<u8>,

    // Keep the BO backing this firmware section so that both the
    // GPU mapping and CPU mapping remain valid until the Section is dropped.
    #[expect(dead_code)]
    mem: gem::KernelBo<'sec, 'res>,
}

/// Loaded firmware with sections mapped into MCU VM.
#[pin_data(PinnedDrop)]
pub(crate) struct Firmware<'fw>
where
    exists<'res>: 'fw,
{
    /// Iomem need to access registers.
    iomem: &'fw IoMem<'fw>,

    /// List of firmware sections.
    #[uses('res: invariant)]
    sections: KVec<Section<'vm, 'res>>,

    /// MCU VM.
    #[pin]
    #[uses('res: invariant)]
    vm: Vm<'fw, 'res>,
}

#[pinned_drop]
impl PinnedDrop for Firmware<'_> {
    fn drop(self: Pin<&mut Self>) {
        // Stop the MCU before releasing its firmware mappings and memory.
        let _ = self.stop();

        // AS slots retain a VM ref, we need to kill the circular ref manually.
        self.with_vm(|vm| vm.kill());
    }
}

impl<'fw> Firmware<'fw> {
    fn init_section_mem(dev: &Device, mem: &mut KernelBo<'_, '_>, data: &KVec<u8>) -> Result {
        if data.is_empty() {
            return Ok(());
        }

        let vmap = mem.bo().vmap::<0>()?;
        let size = mem.bo().size();

        if data.len() > size {
            dev_err!(dev, "fw section {} bigger than BO {}", data.len(), size);
            return Err(EINVAL);
        }

        for (i, &byte) in data.iter().enumerate() {
            vmap.try_write8(byte, i)?;
        }

        Ok(())
    }

    fn request(ddev: &TyrDrmDevice, gpu_info: &GpuInfo) -> Result<kernel::firmware::Firmware> {
        let gpu_id = GPU_ID::from_raw(gpu_info.gpu_id);

        let path = CString::try_from_fmt(fmt!(
            "arm/mali/arch{}.{}/mali_csffw.bin",
            gpu_id.arch_major().get(),
            gpu_id.arch_minor().get()
        ))?;

        kernel::firmware::Firmware::request(&path, ddev.as_ref().as_ref())
    }

    fn load(
        dev: &Device,
        ddev: &TyrDrmDevice,
        gpu_info: &GpuInfo,
    ) -> Result<(kernel::firmware::Firmware, KVec<ParsedSection>)> {
        let fw = Self::request(ddev, gpu_info)?;
        let mut parser = FwParser::new(dev, fw.data());

        let parsed_sections = parser.parse()?;

        Ok((fw, parsed_sections))
    }

    /// Load firmware and map sections into MCU VM.
    pub(crate) fn new<'a, 'res>(
        dev: &'res Device<Bound>,
        iomem: &'fw IoMem<'fw>,
        ddev: &'a TyrDrmDevice,
        mmu: &'fw Mmu<'res>,
        gpu_info: &'a GpuInfo,
    ) -> impl PinInit<Self, Error> + use<'a, 'fw, 'res> {
        try_pin_init!(Self {
            iomem,
            vm <- Vm::new(dev, ddev, mmu, gpu_info),
            sections: {
                let vm = vm.get_ref();
                vm.activate()?;
                let guard = ScopeGuard::new(|| vm.kill());

                let (fw, parsed_sections) = Self::load(dev, ddev, gpu_info)?;
                let mut sections = KVec::new();
                for parsed in parsed_sections {
                    let size =
                        u64::from(parsed.va.end.checked_sub(parsed.va.start).ok_or(EINVAL)?);

                    let va = u64::from(parsed.va.start);

                    let mut mem = KernelBo::new(
                        ddev,
                        vm,
                        size,
                        KernelBoVaAlloc::Explicit(va),
                        parsed.vm_map_flags,
                    )?;

                    let section_start = parsed.data_range.start as usize;
                    let section_end = parsed.data_range.end as usize;
                    let mut data = KVec::new();

                    // Ensure that the firmware slice is not out of bounds.
                    let fw_data = fw.data();
                    let bytes = fw_data.get(section_start..section_end).ok_or(EINVAL)?;
                    data.extend_from_slice(bytes, GFP_KERNEL)?;

                    Self::init_section_mem(dev, &mut mem, &data)?;

                    sections.push(Section { data, mem }, GFP_KERNEL)?;
                }

                guard.dismiss();
                sections
            },
        })
    }

    pub(crate) fn boot(&self) -> Result {
        let io = self.iomem;

        // Discard any stale global interrupt.
        io.write_reg(JOB_IRQ_CLEAR::zeroed().with_glb(true));

        io.write_reg(MCU_CONTROL::zeroed().with_req(McuControlMode::Auto));

        if let Err(e) = poll::read_poll_timeout(
            || Ok((io.read(MCU_STATUS), io.read(JOB_IRQ_RAWSTAT))),
            |(mcu_status, irq_rawstat)| {
                mcu_status.value() == McuStatus::Enabled && irq_rawstat.glb()
            },
            time::Delta::from_millis(1),
            time::Delta::from_millis(100),
        ) {
            let status = io.read(MCU_STATUS);
            dev_err!(
                self.with_vm(|vm| vm.dev()),
                "MCU failed to boot, status: {:?}",
                status.value()
            );
            return Err(e);
        }

        io.write_reg(JOB_IRQ_CLEAR::zeroed().with_glb(true));

        Ok(())
    }

    fn stop(&self) -> Result {
        let io = self.iomem;
        io.write_reg(MCU_CONTROL::zeroed().with_req(McuControlMode::Disable));

        if let Err(e) = poll::read_poll_timeout(
            || Ok(io.read(MCU_STATUS)),
            |status| status.value() == McuStatus::Disabled,
            time::Delta::from_micros(10),
            time::Delta::from_millis(100),
        ) {
            let status = io.read(MCU_STATUS);
            dev_err!(
                self.with_vm(|vm| vm.dev()),
                "MCU failed to stop, status: {:?}",
                status.value()
            );
            return Err(e);
        }

        Ok(())
    }
}
