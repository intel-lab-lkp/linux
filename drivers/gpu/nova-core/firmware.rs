// SPDX-License-Identifier: GPL-2.0

//! Contains structures and functions dedicated to the parsing, building and patching of firmwares
//! to be loaded into a given execution unit.

use core::marker::PhantomData;

use kernel::{
    device,
    firmware,
    prelude::*,
    str::CString,
    transmute::FromBytes, //
};

use crate::{
    dma::DmaObject,
    falcon::FalconFirmware,
    gpu,
    num::{
        FromSafeCast,
        IntoSafeCast, //
    },
};

pub(crate) use elf::elf_section;

pub(crate) mod booter;
pub(crate) mod fsp;
pub(crate) mod fwsec;
pub(crate) mod gsp;
pub(crate) mod riscv;

pub(crate) const FIRMWARE_VERSION: &str = "570.144";

/// Requests the GPU firmware `name` suitable for `chipset`, with version `ver`.
fn request_firmware(
    dev: &device::Device,
    chipset: gpu::Chipset,
    name: &str,
    ver: &str,
) -> Result<firmware::Firmware> {
    let chip_name = chipset.name();

    CString::try_from_fmt(fmt!("nvidia/{chip_name}/gsp/{name}-{ver}.bin"))
        .and_then(|path| firmware::Firmware::request(&path, dev))
}

/// Structure used to describe some firmwares, notably FWSEC-FRTS.
#[repr(C)]
#[derive(Debug, Clone)]
pub(crate) struct FalconUCodeDescV3 {
    /// Header defined by `NV_BIT_FALCON_UCODE_DESC_HEADER_VDESC*` in OpenRM.
    hdr: u32,
    /// Stored size of the ucode after the header.
    stored_size: u32,
    /// Offset in `DMEM` at which the signature is expected to be found.
    pub(crate) pkc_data_offset: u32,
    /// Offset after the code segment at which the app headers are located.
    pub(crate) interface_offset: u32,
    /// Base address at which to load the code segment into `IMEM`.
    pub(crate) imem_phys_base: u32,
    /// Size in bytes of the code to copy into `IMEM`.
    pub(crate) imem_load_size: u32,
    /// Virtual `IMEM` address (i.e. `tag`) at which the code should start.
    pub(crate) imem_virt_base: u32,
    /// Base address at which to load the data segment into `DMEM`.
    pub(crate) dmem_phys_base: u32,
    /// Size in bytes of the data to copy into `DMEM`.
    pub(crate) dmem_load_size: u32,
    /// Mask of the falcon engines on which this firmware can run.
    pub(crate) engine_id_mask: u16,
    /// ID of the ucode used to infer a fuse register to validate the signature.
    pub(crate) ucode_id: u8,
    /// Number of signatures in this firmware.
    pub(crate) signature_count: u8,
    /// Versions of the signatures, used to infer a valid signature to use.
    pub(crate) signature_versions: u16,
    _reserved: u16,
}

impl FalconUCodeDescV3 {
    /// Returns the size in bytes of the header.
    pub(crate) fn size(&self) -> usize {
        const HDR_SIZE_SHIFT: u32 = 16;
        const HDR_SIZE_MASK: u32 = 0xffff0000;

        ((self.hdr & HDR_SIZE_MASK) >> HDR_SIZE_SHIFT).into_safe_cast()
    }
}

/// Trait implemented by types defining the signed state of a firmware.
trait SignedState {}

/// Type indicating that the firmware must be signed before it can be used.
struct Unsigned;
impl SignedState for Unsigned {}

/// Type indicating that the firmware is signed and ready to be loaded.
struct Signed;
impl SignedState for Signed {}

/// A [`DmaObject`] containing a specific microcode ready to be loaded into a falcon.
///
/// This is module-local and meant for sub-modules to use internally.
///
/// After construction, a firmware is [`Unsigned`], and must generally be patched with a signature
/// before it can be loaded (with an exception for development hardware). The
/// [`Self::patch_signature`] and [`Self::no_patch_signature`] methods are used to transition the
/// firmware to its [`Signed`] state.
struct FirmwareDmaObject<F: FalconFirmware, S: SignedState>(DmaObject, PhantomData<(F, S)>);

/// Trait for signatures to be patched directly into a given firmware.
///
/// This is module-local and meant for sub-modules to use internally.
trait FirmwareSignature<F: FalconFirmware>: AsRef<[u8]> {}

impl<F: FalconFirmware> FirmwareDmaObject<F, Unsigned> {
    /// Patches the firmware at offset `sig_base_img` with `signature`.
    fn patch_signature<S: FirmwareSignature<F>>(
        mut self,
        signature: &S,
        sig_base_img: usize,
    ) -> Result<FirmwareDmaObject<F, Signed>> {
        let signature_bytes = signature.as_ref();
        if sig_base_img + signature_bytes.len() > self.0.size() {
            return Err(EINVAL);
        }

        // SAFETY: We are the only user of this object, so there cannot be any race.
        let dst = unsafe { self.0.start_ptr_mut().add(sig_base_img) };

        // SAFETY: `signature` and `dst` are valid, properly aligned, and do not overlap.
        unsafe {
            core::ptr::copy_nonoverlapping(signature_bytes.as_ptr(), dst, signature_bytes.len())
        };

        Ok(FirmwareDmaObject(self.0, PhantomData))
    }

    /// Mark the firmware as signed without patching it.
    ///
    /// This method is used to explicitly confirm that we do not need to sign the firmware, while
    /// allowing us to continue as if it was. This is typically only needed for development
    /// hardware.
    fn no_patch_signature(self) -> FirmwareDmaObject<F, Signed> {
        FirmwareDmaObject(self.0, PhantomData)
    }
}

/// Header common to most firmware files.
#[repr(C)]
#[derive(Debug, Clone)]
struct BinHdr {
    /// Magic number, must be `0x10de`.
    bin_magic: u32,
    /// Version of the header.
    bin_ver: u32,
    /// Size in bytes of the binary (to be ignored).
    bin_size: u32,
    /// Offset of the start of the application-specific header.
    header_offset: u32,
    /// Offset of the start of the data payload.
    data_offset: u32,
    /// Size in bytes of the data payload.
    data_size: u32,
}

// SAFETY: all bit patterns are valid for this type, and it doesn't use interior mutability.
unsafe impl FromBytes for BinHdr {}

// A firmware blob starting with a `BinHdr`.
struct BinFirmware<'a> {
    hdr: BinHdr,
    fw: &'a [u8],
}

impl<'a> BinFirmware<'a> {
    /// Interpret `fw` as a firmware image starting with a [`BinHdr`], and returns the
    /// corresponding [`BinFirmware`] that can be used to extract its payload.
    fn new(fw: &'a firmware::Firmware) -> Result<Self> {
        const BIN_MAGIC: u32 = 0x10de;
        let fw = fw.data();

        fw.get(0..size_of::<BinHdr>())
            // Extract header.
            .and_then(BinHdr::from_bytes_copy)
            // Validate header.
            .and_then(|hdr| {
                if hdr.bin_magic == BIN_MAGIC {
                    Some(hdr)
                } else {
                    None
                }
            })
            .map(|hdr| Self { hdr, fw })
            .ok_or(EINVAL)
    }

    /// Returns the data payload of the firmware, or `None` if the data range is out of bounds of
    /// the firmware image.
    fn data(&self) -> Option<&[u8]> {
        let fw_start = usize::from_safe_cast(self.hdr.data_offset);
        let fw_size = usize::from_safe_cast(self.hdr.data_size);

        self.fw.get(fw_start..fw_start + fw_size)
    }
}

pub(crate) struct ModInfoBuilder<const N: usize>(firmware::ModInfoBuilder<N>);

impl<const N: usize> ModInfoBuilder<N> {
    const fn make_entry_file(self, chipset: &str, fw: &str) -> Self {
        ModInfoBuilder(
            self.0
                .new_entry()
                .push("nvidia/")
                .push(chipset)
                .push("/gsp/")
                .push(fw)
                .push("-")
                .push(FIRMWARE_VERSION)
                .push(".bin"),
        )
    }

    const fn make_entry_chipset(self, chipset: &str) -> Self {
        self.make_entry_file(chipset, "booter_load")
            .make_entry_file(chipset, "booter_unload")
            .make_entry_file(chipset, "bootloader")
            .make_entry_file(chipset, "gsp")
    }

    pub(crate) const fn create(
        module_name: &'static kernel::str::CStr,
    ) -> firmware::ModInfoBuilder<N> {
        let mut this = Self(firmware::ModInfoBuilder::new(module_name));
        let mut i = 0;

        while i < gpu::Chipset::ALL.len() {
            this = this.make_entry_chipset(gpu::Chipset::ALL[i].name());
            i += 1;
        }

        this.0
    }
}

/// Ad-hoc and temporary module to extract sections from ELF images.
///
/// Some firmware images are currently packaged as ELF files, where sections names are used as keys
/// to specific and related bits of data. Future firmware versions are scheduled to move away from
/// that scheme before nova-core becomes stable, which means this module will eventually be
/// removed.
mod elf {
    use core::mem::size_of;

    use kernel::bindings;
    use kernel::str::CStr;
    use kernel::transmute::FromBytes;

    /// Trait to abstract over ELF header differences (32-bit vs 64-bit).
    trait ElfHeader: FromBytes {
        fn shnum(&self) -> u16;
        fn shoff(&self) -> u64;
        fn shstrndx(&self) -> u16;
    }

    /// Trait to abstract over ELF section header differences (32-bit vs 64-bit).
    trait ElfSectionHeader: FromBytes {
        fn name(&self) -> u32;
        fn offset(&self) -> u64;
        fn size(&self) -> u64;
    }

    /// Newtype to provide [`FromBytes`] and [`ElfHeader`] implementations.
    #[repr(transparent)]
    struct Elf64Hdr(bindings::elf64_hdr);
    // SAFETY: all bit patterns are valid for this type, and it doesn't use interior mutability.
    unsafe impl FromBytes for Elf64Hdr {}

    impl ElfHeader for Elf64Hdr {
        fn shnum(&self) -> u16 {
            self.0.e_shnum
        }

        fn shoff(&self) -> u64 {
            self.0.e_shoff
        }

        fn shstrndx(&self) -> u16 {
            self.0.e_shstrndx
        }
    }

    /// Newtype to provide [`FromBytes`] and [`ElfSectionHeader`] implementations.
    #[repr(transparent)]
    struct Elf64SHdr(bindings::elf64_shdr);
    // SAFETY: all bit patterns are valid for this type, and it doesn't use interior mutability.
    unsafe impl FromBytes for Elf64SHdr {}

    impl ElfSectionHeader for Elf64SHdr {
        fn name(&self) -> u32 {
            self.0.sh_name
        }

        fn offset(&self) -> u64 {
            self.0.sh_offset
        }

        fn size(&self) -> u64 {
            self.0.sh_size
        }
    }

    /// Newtype to provide [`FromBytes`] and [`ElfHeader`] implementations for ELF32.
    #[repr(transparent)]
    struct Elf32Hdr(bindings::elf32_hdr);
    // SAFETY: all bit patterns are valid for this type, and it doesn't use interior mutability.
    unsafe impl FromBytes for Elf32Hdr {}

    impl ElfHeader for Elf32Hdr {
        fn shnum(&self) -> u16 {
            self.0.e_shnum
        }

        fn shoff(&self) -> u64 {
            u64::from(self.0.e_shoff)
        }

        fn shstrndx(&self) -> u16 {
            self.0.e_shstrndx
        }
    }

    /// Newtype to provide [`FromBytes`] and [`ElfSectionHeader`] implementations for ELF32.
    #[repr(transparent)]
    struct Elf32SHdr(bindings::elf32_shdr);
    // SAFETY: all bit patterns are valid for this type, and it doesn't use interior mutability.
    unsafe impl FromBytes for Elf32SHdr {}

    impl ElfSectionHeader for Elf32SHdr {
        fn name(&self) -> u32 {
            self.0.sh_name
        }

        fn offset(&self) -> u64 {
            u64::from(self.0.sh_offset)
        }

        fn size(&self) -> u64 {
            u64::from(self.0.sh_size)
        }
    }

    /// Check if the section name at `strtab_offset + name_offset` equals `target`.
    fn section_name_eq(elf: &[u8], strtab_offset: u64, name_offset: u32, target: &str) -> bool {
        strtab_offset
            // Compute the index into the ELF image.
            .checked_add(u64::from(name_offset))
            .and_then(|idx| usize::try_from(idx).ok())
            // Get the start of the name.
            .and_then(|name_idx| elf.get(name_idx..))
            // Stop at the first `0`.
            .and_then(|s| s.get(0..=s.iter().position(|b| *b == 0)?))
            // Convert into CStr.
            .and_then(|s| CStr::from_bytes_with_nul(s).ok())
            // Convert into str.
            .and_then(|s| s.to_str().ok())
            // Check that the name matches.
            .is_some_and(|s| s == target)
    }

    fn elf_section_generic<'a, H, S>(elf: &'a [u8], name: &str) -> Option<&'a [u8]>
    where
        H: ElfHeader,
        S: ElfSectionHeader,
    {
        let hdr = H::from_bytes(elf.get(0..size_of::<H>())?)?;

        let shdr_num = usize::from(hdr.shnum());
        let shdr_start = usize::try_from(hdr.shoff()).ok()?;
        let shdr_end = shdr_num
            .checked_mul(size_of::<S>())
            .and_then(|v| v.checked_add(shdr_start))?;

        // Get all the section headers as an iterator over byte chunks.
        let shdr_bytes = elf.get(shdr_start..shdr_end)?;
        let mut shdr_iter = shdr_bytes.chunks_exact(size_of::<S>());

        // Get the strings table.
        let strhdr = shdr_iter
            .clone()
            .nth(usize::from(hdr.shstrndx()))
            .and_then(S::from_bytes)?;

        // Find the section which name matches `name` and return it.
        shdr_iter.find_map(|sh_bytes| {
            let sh = S::from_bytes(sh_bytes)?;

            if section_name_eq(elf, strhdr.offset(), sh.name(), name) {
                let start = usize::try_from(sh.offset()).ok()?;
                let end = usize::try_from(sh.size())
                    .ok()
                    .and_then(|sz| start.checked_add(sz))?;
                elf.get(start..end)
            } else {
                None
            }
        })
    }

    /// Extract the section with name `name` from the ELF64 image `elf`.
    fn elf64_section<'a>(elf: &'a [u8], name: &str) -> Option<&'a [u8]> {
        elf_section_generic::<Elf64Hdr, Elf64SHdr>(elf, name)
    }

    /// Extract section with name `name` from the ELF32 image `elf`.
    fn elf32_section<'a>(elf: &'a [u8], name: &str) -> Option<&'a [u8]> {
        elf_section_generic::<Elf32Hdr, Elf32SHdr>(elf, name)
    }

    /// Automatically detects ELF32 vs ELF64 based on the ELF header.
    pub(crate) fn elf_section<'a>(elf: &'a [u8], name: &str) -> Option<&'a [u8]> {
        // Check ELF magic.
        if elf.len() < 5 || elf.get(0..4)? != b"\x7fELF" {
            return None;
        }

        // Check ELF class: 1 = 32-bit, 2 = 64-bit.
        match elf.get(4)? {
            1 => elf32_section(elf, name),
            2 => elf64_section(elf, name),
            _ => None,
        }
    }
}
