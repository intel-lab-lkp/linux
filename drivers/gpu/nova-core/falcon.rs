// SPDX-License-Identifier: GPL-2.0

//! Falcon microprocessor base support

use core::ops::Deref;

use hal::FalconHal;

use kernel::{
    device,
    dma::DmaAddress,
    io::poll::read_poll_timeout,
    prelude::*,
    sync::aref::ARef,
    time::{
        Delta, //
    },
    transmute::AsBytes, //
};

use crate::{
    dma::DmaObject,
    driver::Bar0,
    falcon::hal::LoadMethod,
    firmware::fwsec::{
        BootloaderDmemDescV2,
        GenericBootloader, //
    },
    gpu::Chipset,
    num::{
        FromSafeCast,
        IntoSafeCast, //
    },
    regs,
    regs::macros::RegisterBase, //
};

pub(crate) mod gsp;
mod hal;
pub(crate) mod sec2;

// TODO[FPRI]: Replace with `ToPrimitive`.
macro_rules! impl_from_enum_to_u8 {
    ($enum_type:ty) => {
        impl From<$enum_type> for u8 {
            fn from(value: $enum_type) -> Self {
                value as u8
            }
        }
    };
}

/// Revision number of a falcon core, used in the [`crate::regs::NV_PFALCON_FALCON_HWCFG1`]
/// register.
#[repr(u8)]
#[derive(Debug, Default, Copy, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum FalconCoreRev {
    #[default]
    Rev1 = 1,
    Rev2 = 2,
    Rev3 = 3,
    Rev4 = 4,
    Rev5 = 5,
    Rev6 = 6,
    Rev7 = 7,
}
impl_from_enum_to_u8!(FalconCoreRev);

// TODO[FPRI]: replace with `FromPrimitive`.
impl TryFrom<u8> for FalconCoreRev {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        use FalconCoreRev::*;

        let rev = match value {
            1 => Rev1,
            2 => Rev2,
            3 => Rev3,
            4 => Rev4,
            5 => Rev5,
            6 => Rev6,
            7 => Rev7,
            _ => return Err(EINVAL),
        };

        Ok(rev)
    }
}

/// Revision subversion number of a falcon core, used in the
/// [`crate::regs::NV_PFALCON_FALCON_HWCFG1`] register.
#[repr(u8)]
#[derive(Debug, Default, Copy, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum FalconCoreRevSubversion {
    #[default]
    Subversion0 = 0,
    Subversion1 = 1,
    Subversion2 = 2,
    Subversion3 = 3,
}
impl_from_enum_to_u8!(FalconCoreRevSubversion);

// TODO[FPRI]: replace with `FromPrimitive`.
impl TryFrom<u8> for FalconCoreRevSubversion {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        use FalconCoreRevSubversion::*;

        let sub_version = match value & 0b11 {
            0 => Subversion0,
            1 => Subversion1,
            2 => Subversion2,
            3 => Subversion3,
            _ => return Err(EINVAL),
        };

        Ok(sub_version)
    }
}

/// Security model of a falcon core, used in the [`crate::regs::NV_PFALCON_FALCON_HWCFG1`]
/// register.
#[repr(u8)]
#[derive(Debug, Default, Copy, Clone)]
/// Security mode of the Falcon microprocessor.
///
/// See `falcon.rst` for more details.
pub(crate) enum FalconSecurityModel {
    /// Non-Secure: runs unsigned code without privileges.
    #[default]
    None = 0,
    /// Light-Secured (LS): Runs signed code with some privileges.
    /// Entry into this mode is only possible from 'Heavy-secure' mode, which verifies the code's
    /// signature.
    ///
    /// Also known as Low-Secure, Privilege Level 2 or PL2.
    Light = 2,
    /// Heavy-Secured (HS): Runs signed code with full privileges.
    /// The code's signature is verified by the Falcon Boot ROM (BROM).
    ///
    /// Also known as High-Secure, Privilege Level 3 or PL3.
    Heavy = 3,
}
impl_from_enum_to_u8!(FalconSecurityModel);

// TODO[FPRI]: replace with `FromPrimitive`.
impl TryFrom<u8> for FalconSecurityModel {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        use FalconSecurityModel::*;

        let sec_model = match value {
            0 => None,
            2 => Light,
            3 => Heavy,
            _ => return Err(EINVAL),
        };

        Ok(sec_model)
    }
}

/// Signing algorithm for a given firmware, used in the [`crate::regs::NV_PFALCON2_FALCON_MOD_SEL`]
/// register. It is passed to the Falcon Boot ROM (BROM) as a parameter.
#[repr(u8)]
#[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
pub(crate) enum FalconModSelAlgo {
    /// AES.
    #[expect(dead_code)]
    Aes = 0,
    /// RSA3K.
    #[default]
    Rsa3k = 1,
}
impl_from_enum_to_u8!(FalconModSelAlgo);

// TODO[FPRI]: replace with `FromPrimitive`.
impl TryFrom<u8> for FalconModSelAlgo {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            1 => Ok(FalconModSelAlgo::Rsa3k),
            _ => Err(EINVAL),
        }
    }
}

/// Valid values for the `size` field of the [`crate::regs::NV_PFALCON_FALCON_DMATRFCMD`] register.
#[repr(u8)]
#[derive(Debug, Default, Copy, Clone, PartialEq, Eq)]
pub(crate) enum DmaTrfCmdSize {
    /// 256 bytes transfer.
    #[default]
    Size256B = 0x6,
}
impl_from_enum_to_u8!(DmaTrfCmdSize);

// TODO[FPRI]: replace with `FromPrimitive`.
impl TryFrom<u8> for DmaTrfCmdSize {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0x6 => Ok(Self::Size256B),
            _ => Err(EINVAL),
        }
    }
}

/// Currently active core on a dual falcon/riscv (Peregrine) controller.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub(crate) enum PeregrineCoreSelect {
    /// Falcon core is active.
    #[default]
    Falcon = 0,
    /// RISC-V core is active.
    Riscv = 1,
}

impl From<bool> for PeregrineCoreSelect {
    fn from(value: bool) -> Self {
        match value {
            false => PeregrineCoreSelect::Falcon,
            true => PeregrineCoreSelect::Riscv,
        }
    }
}

impl From<PeregrineCoreSelect> for bool {
    fn from(value: PeregrineCoreSelect) -> Self {
        match value {
            PeregrineCoreSelect::Falcon => false,
            PeregrineCoreSelect::Riscv => true,
        }
    }
}

/// Different types of memory present in a falcon core.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum FalconMem {
    /// Secure Instruction Memory.
    ImemSecure,
    /// Non-Secure Instruction Memory.
    ImemNonSecure,
    /// Data Memory.
    Dmem,
}

/// Defines the Framebuffer Interface (FBIF) aperture type.
/// This determines the memory type for external memory access during a DMA transfer, which is
/// performed by the Falcon's Framebuffer DMA (FBDMA) engine. See falcon.rst for more details.
#[derive(Debug, Clone, Default)]
pub(crate) enum FalconFbifTarget {
    /// VRAM.
    #[default]
    /// Local Framebuffer (GPU's VRAM memory).
    LocalFb = 0,
    /// Coherent system memory (System DRAM).
    CoherentSysmem = 1,
    /// Non-coherent system memory (System DRAM).
    NoncoherentSysmem = 2,
}
impl_from_enum_to_u8!(FalconFbifTarget);

// TODO[FPRI]: replace with `FromPrimitive`.
impl TryFrom<u8> for FalconFbifTarget {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        let res = match value {
            0 => Self::LocalFb,
            1 => Self::CoherentSysmem,
            2 => Self::NoncoherentSysmem,
            _ => return Err(EINVAL),
        };

        Ok(res)
    }
}

/// Type of memory addresses to use.
#[derive(Debug, Clone, Default)]
pub(crate) enum FalconFbifMemType {
    /// Virtual memory addresses.
    #[default]
    Virtual = 0,
    /// Physical memory addresses.
    Physical = 1,
}

/// Conversion from a single-bit register field.
impl From<bool> for FalconFbifMemType {
    fn from(value: bool) -> Self {
        match value {
            false => Self::Virtual,
            true => Self::Physical,
        }
    }
}

impl From<FalconFbifMemType> for bool {
    fn from(value: FalconFbifMemType) -> Self {
        match value {
            FalconFbifMemType::Virtual => false,
            FalconFbifMemType::Physical => true,
        }
    }
}

/// Type used to represent the `PFALCON` registers address base for a given falcon engine.
pub(crate) struct PFalconBase(());

/// Type used to represent the `PFALCON2` registers address base for a given falcon engine.
pub(crate) struct PFalcon2Base(());

/// Trait defining the parameters of a given Falcon engine.
///
/// Each engine provides one base for `PFALCON` and `PFALCON2` registers. The `ID` constant is used
/// to identify a given Falcon instance with register I/O methods.
pub(crate) trait FalconEngine:
    Send + Sync + RegisterBase<PFalconBase> + RegisterBase<PFalcon2Base> + Sized
{
    /// Singleton of the engine, used to identify it with register I/O methods.
    const ID: Self;
}

/// Represents a portion of the firmware to be loaded into a particular memory (e.g. IMEM or DMEM).
#[derive(Debug, Clone)]
pub(crate) struct FalconLoadTarget {
    /// Offset from the start of the source object to copy from.
    pub(crate) src_start: u32,
    /// Offset from the start of the destination memory to copy into.
    pub(crate) dst_start: u32,
    /// Number of bytes to copy.
    pub(crate) len: u32,
}

/// Parameters for the falcon boot ROM.
#[derive(Debug, Clone)]
pub(crate) struct FalconBromParams {
    /// Offset in `DMEM`` of the firmware's signature.
    pub(crate) pkc_data_offset: u32,
    /// Mask of engines valid for this firmware.
    pub(crate) engine_id_mask: u16,
    /// ID of the ucode used to infer a fuse register to validate the signature.
    pub(crate) ucode_id: u8,
}

/// Trait for providing load parameters of falcon firmwares.
pub(crate) trait FalconLoadParams {
    /// Returns the load parameters for Secure `IMEM`.
    fn imem_sec_load_params(&self) -> FalconLoadTarget;

    /// Returns the load parameters for Non-Secure `IMEM`,
    /// used only on Turing and GA100.
    fn imem_ns_load_params(&self) -> Option<FalconLoadTarget>;

    /// Returns the load parameters for `DMEM`.
    fn dmem_load_params(&self) -> FalconLoadTarget;

    /// Returns the parameters to write into the BROM registers.
    fn brom_params(&self) -> FalconBromParams;

    /// Returns the start address of the firmware.
    fn boot_addr(&self) -> u32;
}

/// Trait for a falcon firmware.
///
/// A falcon firmware can be loaded on a given engine, and is presented in the form of a DMA
/// object.
pub(crate) trait FalconFirmware: FalconLoadParams + Deref<Target = DmaObject> {
    /// Engine on which this firmware is to be loaded.
    type Target: FalconEngine;
}

/// Contains the base parameters common to all Falcon instances.
pub(crate) struct Falcon<E: FalconEngine> {
    hal: KBox<dyn FalconHal<E>>,
    dev: ARef<device::Device>,
}

impl<E: FalconEngine + 'static> Falcon<E> {
    /// Create a new falcon instance.
    pub(crate) fn new(dev: &device::Device, chipset: Chipset) -> Result<Self> {
        Ok(Self {
            hal: hal::falcon_hal(chipset)?,
            dev: dev.into(),
        })
    }

    /// Resets DMA-related registers.
    pub(crate) fn dma_reset(&self, bar: &Bar0) {
        regs::NV_PFALCON_FBIF_CTL::update(bar, &E::ID, |v| v.set_allow_phys_no_ctx(true));
        regs::NV_PFALCON_FALCON_DMACTL::default().write(bar, &E::ID);
    }

    /// Reset the controller, select the falcon core, and wait for memory scrubbing to complete.
    pub(crate) fn reset(&self, bar: &Bar0) -> Result {
        self.hal.reset_eng(bar)?;
        self.hal.select_core(self, bar)?;
        self.hal.reset_wait_mem_scrubbing(bar)?;

        regs::NV_PFALCON_FALCON_RM::default()
            .set_value(regs::NV_PMC_BOOT_0::read(bar).into())
            .write(bar, &E::ID);

        Ok(())
    }

    /// Write a byte array to Falcon memory using programmed I/O (PIO).
    ///
    /// Writes `img` to the specified `target_mem` (IMEM or DMEM) starting at `mem_base`.
    /// For IMEM writes, tags are set for each 256-byte block starting from `tag`.
    ///
    /// Returns `EINVAL` if `img.len()` is not a multiple of 4.
    fn pio_wr_bytes(
        &self,
        bar: &Bar0,
        img: &[u8],
        mem_base: u16,
        target_mem: FalconMem,
        port: u8,
        tag: u16,
    ) -> Result {
        // Rejecting misaligned images here allows us to avoid checking
        // inside the loops.
        if img.len() % 4 != 0 {
            return Err(EINVAL);
        }

        let port = usize::from(port);

        match target_mem {
            FalconMem::ImemSecure | FalconMem::ImemNonSecure => {
                regs::NV_PFALCON_FALCON_IMEMC::default()
                    .set_secure(target_mem == FalconMem::ImemSecure)
                    .set_aincw(true)
                    .set_offs(mem_base)
                    .write(bar, &E::ID, port);

                let mut tag = tag;
                for block in img.chunks(256) {
                    regs::NV_PFALCON_FALCON_IMEMT::default()
                        .set_tag(tag)
                        .write(bar, &E::ID, port);
                    for word in block.chunks_exact(4) {
                        let w = [word[0], word[1], word[2], word[3]];
                        regs::NV_PFALCON_FALCON_IMEMD::default()
                            .set_data(u32::from_le_bytes(w))
                            .write(bar, &E::ID, port);
                    }
                    tag += 1;
                }
            }
            FalconMem::Dmem => {
                regs::NV_PFALCON_FALCON_DMEMC::default()
                    .set_aincw(true)
                    .set_offs(mem_base)
                    .write(bar, &E::ID, port);

                for block in img.chunks(256) {
                    for word in block.chunks_exact(4) {
                        let w = [word[0], word[1], word[2], word[3]];
                        regs::NV_PFALCON_FALCON_DMEMD::default()
                            .set_data(u32::from_le_bytes(w))
                            .write(bar, &E::ID, port);
                    }
                }
            }
        }

        Ok(())
    }

    fn pio_wr<F: FalconFirmware<Target = E>>(
        &self,
        bar: &Bar0,
        fw: &F,
        target_mem: FalconMem,
        load_offsets: &FalconLoadTarget,
        port: u8,
        tag: u16,
    ) -> Result {
        let start = usize::from_safe_cast(load_offsets.src_start);
        let len = usize::from_safe_cast(load_offsets.len);
        let mem_base = u16::try_from(load_offsets.dst_start)?;

        // SAFETY: we are the only user of the firmware image at this stage
        let data = unsafe { fw.as_slice(start, len).map_err(|_| EINVAL)? };

        self.pio_wr_bytes(bar, data, mem_base, target_mem, port, tag)
    }

    /// Perform a PIO copy into `IMEM` and `DMEM` of `fw`, and prepare the falcon to run it.
    pub(crate) fn pio_load<F: FalconFirmware<Target = E>>(
        &self,
        bar: &Bar0,
        fw: &F,
        gbl: Option<&GenericBootloader>,
    ) -> Result {
        let imem_sec = fw.imem_sec_load_params();
        let imem_ns = fw.imem_ns_load_params().ok_or(EINVAL)?;
        let dmem = fw.dmem_load_params();

        regs::NV_PFALCON_FBIF_CTL::read(bar, &E::ID)
            .set_allow_phys_no_ctx(true)
            .write(bar, &E::ID);

        regs::NV_PFALCON_FALCON_DMACTL::default().write(bar, &E::ID);

        // If the Generic Bootloader was passed, then use it to boot FRTS
        if let Some(gbl) = gbl {
            let dst_start = u16::try_from(0x10000 - gbl.desc.code_size)?;
            let data = &gbl.ucode[..usize::from_safe_cast(gbl.desc.code_size)];
            let tag = u16::try_from(gbl.desc.start_tag)?;

            self.pio_wr_bytes(bar, data, dst_start, FalconMem::ImemNonSecure, 0, tag)?;

            // This structure tells the generic bootloader where to find the FWSEC
            // image.
            let dmem_desc = BootloaderDmemDescV2 {
                reserved: [0; 4],
                signature: [0; 4],
                ctx_dma: 4, // FALCON_DMAIDX_PHYS_SYS_NCOH
                code_dma_base: fw.dma_handle(),
                non_sec_code_off: imem_ns.dst_start,
                non_sec_code_size: imem_ns.len,
                sec_code_off: imem_sec.dst_start,
                sec_code_size: imem_sec.len,
                code_entry_point: 0,
                data_dma_base: fw.dma_handle() + u64::from(dmem.src_start),
                data_size: dmem.len,
                argc: 0,
                argv: 0,
            };

            regs::NV_PFALCON_FBIF_TRANSCFG::update(bar, &E::ID, 4, |v| {
                v.set_target(FalconFbifTarget::CoherentSysmem)
                    .set_mem_type(FalconFbifMemType::Physical)
            });

            self.pio_wr_bytes(bar, dmem_desc.as_bytes(), 0, FalconMem::Dmem, 0, 0)?;
        } else {
            self.pio_wr(
                bar,
                fw,
                FalconMem::ImemNonSecure,
                &imem_ns,
                0,
                u16::try_from(imem_ns.dst_start >> 8)?,
            )?;
            self.pio_wr(
                bar,
                fw,
                FalconMem::ImemSecure,
                &imem_sec,
                0,
                u16::try_from(imem_sec.dst_start >> 8)?,
            )?;
            self.pio_wr(bar, fw, FalconMem::Dmem, &dmem, 0, 0)?;
        }

        self.hal.program_brom(self, bar, &fw.brom_params())?;

        // Set `BootVec` to start of non-secure code.
        regs::NV_PFALCON_FALCON_BOOTVEC::default()
            .set_value(fw.boot_addr())
            .write(bar, &E::ID);

        Ok(())
    }

    /// Perform a DMA write according to `load_offsets` from `dma_handle` into the falcon's
    /// `target_mem`.
    ///
    /// `sec` is set if the loaded firmware is expected to run in secure mode.
    fn dma_wr<F: FalconFirmware<Target = E>>(
        &self,
        bar: &Bar0,
        fw: &F,
        target_mem: FalconMem,
        load_offsets: FalconLoadTarget,
    ) -> Result {
        const DMA_LEN: u32 = 256;

        // For IMEM, we want to use the start offset as a virtual address tag for each page, since
        // code addresses in the firmware (and the boot vector) are virtual.
        //
        // For DMEM we can fold the start offset into the DMA handle.
        let (src_start, dma_start) = match target_mem {
            FalconMem::ImemSecure | FalconMem::ImemNonSecure => {
                (load_offsets.src_start, fw.dma_handle())
            }
            FalconMem::Dmem => (
                0,
                fw.dma_handle_with_offset(load_offsets.src_start.into_safe_cast())?,
            ),
        };
        if dma_start % DmaAddress::from(DMA_LEN) > 0 {
            dev_err!(
                self.dev,
                "DMA transfer start addresses must be a multiple of {}",
                DMA_LEN
            );
            return Err(EINVAL);
        }

        // DMA transfers can only be done in units of 256 bytes. Compute how many such transfers we
        // need to perform.
        let num_transfers = load_offsets.len.div_ceil(DMA_LEN);

        // Check that the area we are about to transfer is within the bounds of the DMA object.
        // Upper limit of transfer is `(num_transfers * DMA_LEN) + load_offsets.src_start`.
        match num_transfers
            .checked_mul(DMA_LEN)
            .and_then(|size| size.checked_add(load_offsets.src_start))
        {
            None => {
                dev_err!(self.dev, "DMA transfer length overflow");
                return Err(EOVERFLOW);
            }
            Some(upper_bound) if usize::from_safe_cast(upper_bound) > fw.size() => {
                dev_err!(self.dev, "DMA transfer goes beyond range of DMA object");
                return Err(EINVAL);
            }
            Some(_) => (),
        };

        // Set up the base source DMA address.

        regs::NV_PFALCON_FALCON_DMATRFBASE::default()
            // CAST: `as u32` is used on purpose since we do want to strip the upper bits, which
            // will be written to `NV_PFALCON_FALCON_DMATRFBASE1`.
            .set_base((dma_start >> 8) as u32)
            .write(bar, &E::ID);
        regs::NV_PFALCON_FALCON_DMATRFBASE1::default()
            // CAST: `as u16` is used on purpose since the remaining bits are guaranteed to fit
            // within a `u16`.
            .set_base((dma_start >> 40) as u16)
            .write(bar, &E::ID);

        let cmd = regs::NV_PFALCON_FALCON_DMATRFCMD::default()
            .set_size(DmaTrfCmdSize::Size256B)
            .with_falcon_mem(target_mem);

        for pos in (0..num_transfers).map(|i| i * DMA_LEN) {
            // Perform a transfer of size `DMA_LEN`.
            regs::NV_PFALCON_FALCON_DMATRFMOFFS::default()
                .set_offs(load_offsets.dst_start + pos)
                .write(bar, &E::ID);
            regs::NV_PFALCON_FALCON_DMATRFFBOFFS::default()
                .set_offs(src_start + pos)
                .write(bar, &E::ID);
            cmd.write(bar, &E::ID);

            // Wait for the transfer to complete.
            // TIMEOUT: arbitrarily large value, no DMA transfer to the falcon's small memories
            // should ever take that long.
            read_poll_timeout(
                || Ok(regs::NV_PFALCON_FALCON_DMATRFCMD::read(bar, &E::ID)),
                |r| r.idle(),
                Delta::ZERO,
                Delta::from_secs(2),
            )?;
        }

        Ok(())
    }

    /// Perform a DMA load into `IMEM` and `DMEM` of `fw`, and prepare the falcon to run it.
    pub(crate) fn dma_load<F: FalconFirmware<Target = E>>(&self, bar: &Bar0, fw: &F) -> Result {

        // The Non-Secure section only exists on firmware used by Turing and GA100, and
        // those platforms do not use DMA.
        if fw.imem_ns_load_params().is_some() {
            return Err(EINVAL);
        }

        self.dma_reset(bar);
        regs::NV_PFALCON_FBIF_TRANSCFG::update(bar, &E::ID, 0, |v| {
            v.set_target(FalconFbifTarget::CoherentSysmem)
                .set_mem_type(FalconFbifMemType::Physical)
        });

        self.dma_wr(bar, fw, FalconMem::ImemSecure, fw.imem_sec_load_params())?;
        self.dma_wr(bar, fw, FalconMem::Dmem, fw.dmem_load_params())?;

        self.hal.program_brom(self, bar, &fw.brom_params())?;

        // Set `BootVec` to start of non-secure code.
        regs::NV_PFALCON_FALCON_BOOTVEC::default()
            .set_value(fw.boot_addr())
            .write(bar, &E::ID);

        Ok(())
    }

    /// Wait until the falcon CPU is halted.
    pub(crate) fn wait_till_halted(&self, bar: &Bar0) -> Result<()> {
        // TIMEOUT: arbitrarily large value, firmwares should complete in less than 2 seconds.
        read_poll_timeout(
            || Ok(regs::NV_PFALCON_FALCON_CPUCTL::read(bar, &E::ID)),
            |r| r.halted(),
            Delta::ZERO,
            Delta::from_secs(2),
        )?;

        Ok(())
    }

    /// Start the falcon CPU.
    pub(crate) fn start(&self, bar: &Bar0) -> Result<()> {
        match regs::NV_PFALCON_FALCON_CPUCTL::read(bar, &E::ID).alias_en() {
            true => regs::NV_PFALCON_FALCON_CPUCTL_ALIAS::default()
                .set_startcpu(true)
                .write(bar, &E::ID),
            false => regs::NV_PFALCON_FALCON_CPUCTL::default()
                .set_startcpu(true)
                .write(bar, &E::ID),
        }

        Ok(())
    }

    /// Writes values to the mailbox registers if provided.
    pub(crate) fn write_mailboxes(&self, bar: &Bar0, mbox0: Option<u32>, mbox1: Option<u32>) {
        if let Some(mbox0) = mbox0 {
            regs::NV_PFALCON_FALCON_MAILBOX0::default()
                .set_value(mbox0)
                .write(bar, &E::ID);
        }

        if let Some(mbox1) = mbox1 {
            regs::NV_PFALCON_FALCON_MAILBOX1::default()
                .set_value(mbox1)
                .write(bar, &E::ID);
        }
    }

    /// Reads the value from `mbox0` register.
    pub(crate) fn read_mailbox0(&self, bar: &Bar0) -> u32 {
        regs::NV_PFALCON_FALCON_MAILBOX0::read(bar, &E::ID).value()
    }

    /// Reads the value from `mbox1` register.
    pub(crate) fn read_mailbox1(&self, bar: &Bar0) -> u32 {
        regs::NV_PFALCON_FALCON_MAILBOX1::read(bar, &E::ID).value()
    }

    /// Reads values from both mailbox registers.
    pub(crate) fn read_mailboxes(&self, bar: &Bar0) -> (u32, u32) {
        let mbox0 = self.read_mailbox0(bar);
        let mbox1 = self.read_mailbox1(bar);

        (mbox0, mbox1)
    }

    /// Start running the loaded firmware.
    ///
    /// `mbox0` and `mbox1` are optional parameters to write into the `MBOX0` and `MBOX1` registers
    /// prior to running.
    ///
    /// Wait up to two seconds for the firmware to complete, and return its exit status read from
    /// the `MBOX0` and `MBOX1` registers.
    pub(crate) fn boot(
        &self,
        bar: &Bar0,
        mbox0: Option<u32>,
        mbox1: Option<u32>,
    ) -> Result<(u32, u32)> {
        self.write_mailboxes(bar, mbox0, mbox1);
        self.start(bar)?;
        self.wait_till_halted(bar)?;
        Ok(self.read_mailboxes(bar))
    }

    /// Returns the fused version of the signature to use in order to run a HS firmware on this
    /// falcon instance. `engine_id_mask` and `ucode_id` are obtained from the firmware header.
    pub(crate) fn signature_reg_fuse_version(
        &self,
        bar: &Bar0,
        engine_id_mask: u16,
        ucode_id: u8,
    ) -> Result<u32> {
        self.hal
            .signature_reg_fuse_version(self, bar, engine_id_mask, ucode_id)
    }

    /// Check if the RISC-V core is active.
    ///
    /// Returns `true` if the RISC-V core is active, `false` otherwise.
    pub(crate) fn is_riscv_active(&self, bar: &Bar0) -> bool {
        self.hal.is_riscv_active(bar)
    }

    // Load a firmware image into Falcon memory
    pub(crate) fn load<F: FalconFirmware<Target = E>>(
        &self,
        bar: &Bar0,
        fw: &F,
        gbl: Option<&GenericBootloader>,
    ) -> Result {
        match self.hal.load_method() {
            LoadMethod::Pio => self.pio_load(bar, fw, gbl),
            LoadMethod::Dma => self.dma_load(bar, fw),
        }
    }

    /// Write the application version to the OS register.
    pub(crate) fn write_os_version(&self, bar: &Bar0, app_version: u32) {
        regs::NV_PFALCON_FALCON_OS::default()
            .set_value(app_version)
            .write(bar, &E::ID);
    }
}
