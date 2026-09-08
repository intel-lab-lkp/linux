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
