// SPDX-License-Identifier: GPL-2.0

//! vDSO build-time validation
//!
//! Check that the vDSO library does not contain dynamic relocations.

use std::default::Default;
use std::fmt;
use std::fs;
use std::option::Option;
use std::process;

use ::bindings;

mod elf;

#[derive(Default)]
struct AllowedRelocations<'a> {
    ignored_object_file_sections: &'a [&'a str],
    in_object_file: &'a [u32],
}

impl<'a> AllowedRelocations<'a> {
    fn is_ignored_section(&self, section: &elf::Section<'_>) -> bool {
        let name = section.info().name;

        if name.starts_with(".rel.debug_") || name.starts_with(".rela.debug_") {
            true
        } else {
            self.ignored_object_file_sections.contains(&name)
        }
    }
}

fn allowed_relocations_for_machine(machine: u16) -> Option<AllowedRelocations<'static>> {
    match machine as u32 {
        bindings::EM_386 => AllowedRelocations {
            in_object_file: &[
                bindings::R_386_PC32,
                bindings::R_386_GOTOFF,
                bindings::R_386_GOTPC,
            ],
            ..Default::default()
        }
        .into(),
        bindings::EM_X86_64 => AllowedRelocations {
            in_object_file: &[bindings::R_X86_64_PC32, bindings::R_X86_64_PLT32],
            ..Default::default()
        }
        .into(),
        bindings::EM_ARM => AllowedRelocations {
            in_object_file: &[
                bindings::R_ARM_NONE,
                bindings::R_ARM_REL32,
                bindings::R_ARM_PREL31,
            ],
            ..Default::default()
        }
        .into(),
        bindings::EM_AARCH64 => AllowedRelocations {
            in_object_file: &[
                bindings::R_AARCH64_PREL64,
                bindings::R_AARCH64_PREL32,
                bindings::R_AARCH64_PREL16,
                bindings::R_AARCH64_LD_PREL_LO19,
                bindings::R_AARCH64_ADR_PREL_LO21,
                bindings::R_AARCH64_CALL26,
            ],
            ..Default::default()
        }
        .into(),
        bindings::EM_PPC => AllowedRelocations {
            in_object_file: &[
                bindings::R_PPC_REL24,
                bindings::R_PPC_REL14,
                bindings::R_PPC_REL32,
                bindings::R_PPC_REL16,
                bindings::R_PPC_REL16_LO,
                bindings::R_PPC_REL16_HI,
                bindings::R_PPC_REL16_HA,
            ],
            ..Default::default()
        }
        .into(),
        bindings::EM_PPC64 => AllowedRelocations {
            in_object_file: &[
                bindings::R_PPC64_REL24,
                bindings::R_PPC64_REL14,
                bindings::R_PPC64_REL32,
                bindings::R_PPC64_REL64,
                bindings::R_PPC64_REL16,
                bindings::R_PPC64_REL16_LO,
                bindings::R_PPC64_REL16_HI,
                bindings::R_PPC64_REL16_HA,
            ],
            ..Default::default()
        }
        .into(),
        bindings::EM_RISCV => AllowedRelocations {
            in_object_file: &[
                bindings::R_RISCV_BRANCH,
                bindings::R_RISCV_JAL,
                bindings::R_RISCV_CALL,
                bindings::R_RISCV_CALL_PLT,
                bindings::R_RISCV_PCREL_HI20,
                bindings::R_RISCV_PCREL_LO12_I,
                bindings::R_RISCV_PCREL_LO12_S,
                bindings::R_RISCV_ADD8,
                bindings::R_RISCV_ADD16,
                bindings::R_RISCV_ADD32,
                bindings::R_RISCV_ADD64,
                bindings::R_RISCV_SUB8,
                bindings::R_RISCV_SUB16,
                bindings::R_RISCV_SUB32,
                bindings::R_RISCV_SUB64,
                bindings::R_RISCV_ALIGN,
                bindings::R_RISCV_RVC_BRANCH,
                bindings::R_RISCV_RVC_JUMP,
                bindings::R_RISCV_RELAX,
                bindings::R_RISCV_32_PCREL,
            ],
            ..Default::default()
        }
        .into(),
        bindings::EM_LOONGARCH => AllowedRelocations {
            in_object_file: &[
                bindings::R_LARCH_ADD8,
                bindings::R_LARCH_ADD16,
                bindings::R_LARCH_ADD24,
                bindings::R_LARCH_ADD32,
                bindings::R_LARCH_ADD64,
                bindings::R_LARCH_SUB8,
                bindings::R_LARCH_SUB16,
                bindings::R_LARCH_SUB24,
                bindings::R_LARCH_SUB32,
                bindings::R_LARCH_SUB64,
                bindings::R_LARCH_B16,
                bindings::R_LARCH_B21,
                bindings::R_LARCH_B26,
                bindings::R_LARCH_PCALA_HI20,
                bindings::R_LARCH_PCALA_LO12,
                bindings::R_LARCH_PCALA64_LO20,
                bindings::R_LARCH_PCALA64_HI12,
                bindings::R_LARCH_32_PCREL,
                bindings::R_LARCH_PCADD_HI20,
                bindings::R_LARCH_PCADD_LO12,
            ],
            ..Default::default()
        }
        .into(),
        _ => None,
    }
}

#[derive(Debug)]
enum ValidationError<'a> {
    ParseError(elf::ParseError),
    UnsupportedArchitecture(u16),
    UnrecognizedElfFileType(u32),
    UnexpectedSection(elf::Section<'a>),
    InvalidRelocation(elf::Section<'a>, u32),
}

impl<'a> From<elf::ParseError> for ValidationError<'a> {
    fn from(parse_error: elf::ParseError) -> Self {
        Self::ParseError(parse_error)
    }
}

impl fmt::Display for ValidationError<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ValidationError::ParseError(e) => write!(f, "Parsing error: {}", e),
            ValidationError::UnsupportedArchitecture(n) => {
                write!(f, "Unsupported ELF architecture {}", n)
            }
            ValidationError::UnrecognizedElfFileType(t) => {
                write!(f, "Unrecognized ELF file type {}", t)
            }
            ValidationError::UnexpectedSection(ref s) => {
                write!(f, "Unexpected section '{}'", s.info().name)
            }
            ValidationError::InvalidRelocation(ref s, t) => {
                write!(f, "Invalid relocation {} in section '{}'", t, s.info().name)
            }
        }
    }
}

type ValidationResult<'a> = Result<(), ValidationError<'a>>;

fn validate_linked_dso<'a>(file: &'a elf::File<'a>) -> ValidationResult<'a> {
    for section in file.sections()? {
        let section = section?;

        /* No relocations are allowed */
        match section {
            elf::Section::Rel(_) | elf::Section::Rela(_) => {
                return Err(ValidationError::UnexpectedSection(section))
            }
            _ => {}
        }
    }

    Ok(())
}

fn validate_object_file<'a>(file: &'a elf::File<'a>) -> ValidationResult<'a> {
    let allowed_relocs = allowed_relocations_for_machine(file.machine)
        .ok_or(ValidationError::UnsupportedArchitecture(file.machine))?;

    for section in file.sections()? {
        let section = section?;

        if allowed_relocs.is_ignored_section(&section) {
            continue;
        }

        match section {
            elf::Section::Rel(ref rel) => {
                for entry in rel.entries()? {
                    if !allowed_relocs.in_object_file.contains(&entry.type_) {
                        return Err(ValidationError::InvalidRelocation(section, entry.type_));
                    }
                }
            }
            elf::Section::Rela(ref rela) => {
                for entry in rela.entries()? {
                    if !allowed_relocs.in_object_file.contains(&entry.type_) {
                        return Err(ValidationError::InvalidRelocation(section, entry.type_));
                    }
                }
            }
            _ => {}
        };
    }

    Ok(())
}

fn main() {
    let mut args = std::env::args_os();

    let program_name = args.next().unwrap_or("vdsocheck".into());

    for path in args {
        let data = fs::read(&path).unwrap_or_else(|err| {
            println!("{}: {}: {}", program_name.display(), path.display(), err);
            process::exit(1);
        });

        let file = elf::File::new_from_bytes(&data).unwrap_or_else(|err| {
            println!("{}: {}: {}", program_name.display(), path.display(), err);
            process::exit(2);
        });

        let result = match file.type_ as u32 {
            bindings::ET_DYN => validate_linked_dso(&file),
            bindings::ET_REL => validate_object_file(&file),
            t => Err(ValidationError::UnrecognizedElfFileType(t)),
        };

        result.unwrap_or_else(|err| {
            println!("{}: {}: {}", program_name.display(), path.display(), err);
            process::exit(3);
        });
    }
}
