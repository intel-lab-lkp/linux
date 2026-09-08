// SPDX-License-Identifier: GPL-2.0

#![allow(unreachable_pub)]

use core::fmt;
use core::iter::Iterator;
use core::result::Result;
use core::str;

use ::bindings;

#[derive(Debug, Copy, Clone)]
enum ByteOrder {
    LittleEndian,
    BigEndian,
}

trait ToCpu {
    fn to_cpu(self, byteorder: ByteOrder) -> Self;
}

macro_rules! declare_to_cpu {
    ($t:ty) => {
        impl ToCpu for $t {
            fn to_cpu(self, byteorder: ByteOrder) -> Self {
                match byteorder {
                    ByteOrder::LittleEndian => Self::from_le(self),
                    ByteOrder::BigEndian => Self::from_be(self),
                }
            }
        }
    };
}

declare_to_cpu!(u16);
declare_to_cpu!(u32);
declare_to_cpu!(u64);

#[derive(Debug, Copy, Clone)]
enum Class {
    Elf32,
    Elf64,
}

enum ClassAlternative<T32, T64> {
    Elf32(T32),
    Elf64(T64),
}

#[derive(Debug)]
pub enum ParseError {
    InvalidFileMagic([u8; 4]),
    InvalidFileClass(u32),
    InvalidFileByteOrder(u32),
    InvalidSectionSize,
    MissingStringTable,
    StrtabIndexOutOfRange,
    IndexOutOfRange,
    StrtabInvalidData(str::Utf8Error),
}

pub type ParseResult<T> = Result<T, ParseError>;

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ParseError::InvalidFileMagic(m) => write!(f, "Invalid ELF magic {:?}", m),
            ParseError::InvalidFileClass(c) => write!(f, "Invalid ELF class {}", c),
            ParseError::InvalidFileByteOrder(b) => write!(f, "Invalid ELF byteorder {}", b),
            ParseError::InvalidSectionSize => write!(f, "Invalid ELF section size"),
            ParseError::MissingStringTable => write!(f, "Missing string table"),
            ParseError::IndexOutOfRange => write!(f, "Index out of range"),
            ParseError::StrtabIndexOutOfRange => write!(f, "String table index out of range"),
            ParseError::StrtabInvalidData(e) => write!(f, "Invalid data in string table: {}", e),
        }
    }
}

fn read_from_bytes<T: Copy>(data: &[u8]) -> ParseResult<T> {
    if data.len() < core::mem::size_of::<T>() {
        Err(ParseError::IndexOutOfRange)?
    }
    let ptr = data.as_ptr() as *const T;
    // SAFETY: `T` is `Copy` by the type constraints.
    // `ptr` is valid as it is derived from the slice `data` above.
    // `self.data.len()` being large enough is guaranteed by the runtime check above.
    Ok(unsafe { ptr.read_unaligned() })
}

fn get_data_subslice(data: &[u8], offset: u64, size: u64) -> ParseResult<&[u8]> {
    let start: usize = offset.try_into().map_err(|_| ParseError::IndexOutOfRange)?;
    let size: usize = size.try_into().map_err(|_| ParseError::IndexOutOfRange)?;
    let end = start.checked_add(size).ok_or(ParseError::IndexOutOfRange)?;

    Ok(&data[start..end])
}

/// Representation of a complete ELF file.
#[derive(Debug)]
pub struct File<'a> {
    byteorder: ByteOrder,
    class: Class,
    pub type_: u16,
    pub machine: u16,
    pub data: &'a [u8],
    section_headers: SectionInfo<'a>,
    section_names: StrtabSection<'a>,
}

impl<'a> File<'a> {
    pub fn new_from_bytes(data: &'a [u8]) -> Result<Self, ParseError> {
        const ELF_MAGIC: [u8; 4] = [
            bindings::ELFMAG0 as u8,
            bindings::ELFMAG1 as u8,
            bindings::ELFMAG2 as u8,
            bindings::ELFMAG3 as u8,
        ];
        let ehdr: bindings::elf32_hdr = read_from_bytes(data)?;

        let magic = [
            ehdr.e_ident[bindings::EI_MAG0 as usize],
            ehdr.e_ident[bindings::EI_MAG1 as usize],
            ehdr.e_ident[bindings::EI_MAG2 as usize],
            ehdr.e_ident[bindings::EI_MAG3 as usize],
        ];

        if magic != ELF_MAGIC {
            return Err(ParseError::InvalidFileMagic(magic));
        }

        let class = match ehdr.e_ident[bindings::EI_CLASS as usize] as u32 {
            bindings::ELFCLASS32 => Class::Elf32,
            bindings::ELFCLASS64 => Class::Elf64,
            c => return Err(ParseError::InvalidFileClass(c)),
        };

        let byteorder = match ehdr.e_ident[bindings::EI_DATA as usize] as u32 {
            bindings::ELFDATA2LSB => ByteOrder::LittleEndian,
            bindings::ELFDATA2MSB => ByteOrder::BigEndian,
            b => return Err(ParseError::InvalidFileByteOrder(b)),
        };

        let (type_, machine, shnum, shoff, shentsize, shstrndx) = match class {
            Class::Elf32 => {
                let ehdr: bindings::elf32_hdr = read_from_bytes(data)?;
                (
                    ehdr.e_type.to_cpu(byteorder),
                    ehdr.e_machine.to_cpu(byteorder),
                    ehdr.e_shnum.to_cpu(byteorder),
                    ehdr.e_shoff.to_cpu(byteorder).into(),
                    ehdr.e_shentsize.to_cpu(byteorder),
                    ehdr.e_shstrndx.to_cpu(byteorder),
                )
            }
            Class::Elf64 => {
                let ehdr: bindings::elf64_hdr = read_from_bytes(data)?;
                (
                    ehdr.e_type.to_cpu(byteorder),
                    ehdr.e_machine.to_cpu(byteorder),
                    ehdr.e_shnum.to_cpu(byteorder),
                    ehdr.e_shoff.to_cpu(byteorder),
                    ehdr.e_shentsize.to_cpu(byteorder),
                    ehdr.e_shstrndx.to_cpu(byteorder),
                )
            }
        };

        let section_headers = SectionInfo {
            byteorder,
            class,
            entsize: shentsize.into(),
            data: get_data_subslice(data, shoff, u64::from(shnum) * u64::from(shentsize))?,
            name: "<section headers>",
        };

        let string_table = SectionHeaderIterator::new(&section_headers, data)?
            .nth(shstrndx.into())
            .ok_or(ParseError::MissingStringTable)??;

        let section_names = StrtabSection(SectionInfo {
            name: "<section header names>",
            byteorder,
            class,
            data: string_table.data,
            entsize: string_table.entsize,
        });

        Ok(File {
            byteorder,
            class,
            type_,
            machine,
            section_headers,
            section_names,
            data,
        })
    }

    pub fn sections(&self) -> ParseResult<SectionIterator<'_>> {
        Ok(SectionIterator {
            file: self,
            section_headers: SectionHeaderIterator::new(&self.section_headers, self.data)?,
        })
    }
}

/// High-level representation of an ELF section.
#[derive(Clone, Debug)]
pub struct SectionInfo<'a> {
    byteorder: ByteOrder,
    class: Class,
    pub name: &'a str,
    entsize: u64,
    pub data: &'a [u8],
}

/// Typed high-level iterator over all sections in a `File`.
#[derive(Debug)]
pub enum Section<'a> {
    Null(SectionInfo<'a>),
    Rel(RelSection<'a>),
    Rela(RelaSection<'a>),
    Strtab(StrtabSection<'a>),
    Unknown(SectionInfo<'a>),
}

impl<'a> Section<'a> {
    pub fn info(&'a self) -> &'a SectionInfo<'a> {
        match self {
            Section::Null(info) | Section::Unknown(info) => info,
            Section::Rel(rel) => &rel.0,
            Section::Rela(rela) => &rela.0,
            Section::Strtab(strtab) => &strtab.0,
        }
    }
}

pub struct SectionIterator<'a> {
    file: &'a File<'a>,
    section_headers: SectionHeaderIterator<'a, 'a>,
}

impl<'a> Iterator for SectionIterator<'a> {
    type Item = ParseResult<Section<'a>>;

    fn next(&mut self) -> Option<Self::Item> {
        self.section_headers.next().map(|header| {
            let header = header?;
            let info = SectionInfo {
                byteorder: self.file.byteorder,
                class: self.file.class,
                name: self.file.section_names.entry(header.name)?,
                entsize: header.entsize,
                data: header.data,
            };

            Ok(match header.type_ {
                bindings::SHT_NULL => Section::Null(info),
                bindings::SHT_RELA => Section::Rela(RelaSection(info)),
                bindings::SHT_REL => Section::Rel(RelSection(info)),
                bindings::SHT_STRTAB => Section::Strtab(StrtabSection(info)),
                _ => Section::Unknown(info),
            })
        })
    }
}

/// Iterator over a section of data containing instances of type `T`.
struct SectionEntityIterator<'a, T: Copy> {
    data: &'a [u8],
    byteorder: ByteOrder,
    _phantom: core::marker::PhantomData<T>,
}

impl<'a, T: Copy> SectionEntityIterator<'a, T> {
    const ENTITY_SIZE: usize = core::mem::size_of::<T>();

    fn new(section: &'a SectionInfo<'a>) -> ParseResult<Self> {
        let data = section.data;

        if section.entsize != Self::ENTITY_SIZE as u64 {
            return Err(ParseError::InvalidSectionSize);
        }

        if !data.len().is_multiple_of(Self::ENTITY_SIZE) {
            return Err(ParseError::InvalidSectionSize);
        }

        Ok(Self {
            data,
            byteorder: section.byteorder,
            _phantom: core::marker::PhantomData,
        })
    }
}

impl<'a, T: Copy> Iterator for SectionEntityIterator<'a, T> {
    type Item = T;

    fn next(&mut self) -> Option<Self::Item> {
        if self.data.len() != 0 {
            let ptr = self.data.as_ptr() as *const T;
            // SAFETY: `T` is `Copy` by the type constraints.
            // `ptr` is valid as it is derived from the slice `data` above.
            // `self.data.len()` being a multiple of `Self::ENTITY_SIZE` is
            // guaranteed by `SectionEntityIterator::new`.
            let entity: T = unsafe { ptr.read_unaligned() };
            self.data = &self.data[Self::ENTITY_SIZE..];
            Some(entity)
        } else {
            None
        }
    }
}

/// Class-independent representation of an entry in a section header table.
#[derive(Debug)]
struct SectionHeader<'a> {
    name: u32,
    type_: u32,
    entsize: u64,
    data: &'a [u8],
}

/// Iterator over the section header table.
struct SectionHeaderIterator<'f: 'a, 'a>(
    ClassAlternative<
        SectionEntityIterator<'a, bindings::elf32_shdr>,
        SectionEntityIterator<'a, bindings::elf64_shdr>,
    >,
    &'f [u8],
);

impl<'f: 'a, 'a> SectionHeaderIterator<'f, 'a> {
    fn new(section: &'a SectionInfo<'a>, file_data: &'f [u8]) -> ParseResult<Self> {
        Ok(Self(
            match section.class {
                Class::Elf32 => ClassAlternative::Elf32(SectionEntityIterator::new(section)?),
                Class::Elf64 => ClassAlternative::Elf64(SectionEntityIterator::new(section)?),
            },
            file_data,
        ))
    }
}

impl<'f: 'a, 'a> Iterator for SectionHeaderIterator<'f, 'a> {
    type Item = ParseResult<SectionHeader<'f>>;

    fn next(&mut self) -> Option<Self::Item> {
        let file_data = self.1;

        match &mut self.0 {
            ClassAlternative::Elf32(iter) => iter.next().map(|n| {
                Ok(SectionHeader {
                    name: n.sh_name.to_cpu(iter.byteorder).into(),
                    type_: n.sh_type.to_cpu(iter.byteorder).into(),
                    entsize: n.sh_entsize.to_cpu(iter.byteorder).into(),
                    data: get_data_subslice(
                        file_data,
                        n.sh_offset.to_cpu(iter.byteorder).into(),
                        n.sh_size.to_cpu(iter.byteorder).into(),
                    )?,
                })
            }),
            ClassAlternative::Elf64(iter) => iter.next().map(|n| {
                Ok(SectionHeader {
                    name: n.sh_name.to_cpu(iter.byteorder).into(),
                    type_: n.sh_type.to_cpu(iter.byteorder).into(),
                    entsize: n.sh_entsize.to_cpu(iter.byteorder).into(),
                    data: get_data_subslice(
                        file_data,
                        n.sh_offset.to_cpu(iter.byteorder).into(),
                        n.sh_size.to_cpu(iter.byteorder).into(),
                    )?,
                })
            }),
        }
    }
}

/// High-level interface to a SHT_STRTAB string table.
#[derive(Debug)]
pub struct StrtabSection<'a>(SectionInfo<'a>);

impl<'a> StrtabSection<'a> {
    pub fn entry(&'a self, index: u32) -> ParseResult<&'a str> {
        let data = self.0.data;
        let index = index as usize;

        if index >= data.len() {
            Err(ParseError::StrtabIndexOutOfRange)
        } else {
            let len = data[index..]
                .iter()
                .position(|b| *b == 0x00)
                .ok_or(ParseError::StrtabIndexOutOfRange)?;
            let s = str::from_utf8(&data[index..index + len])
                .map_err(|e| ParseError::StrtabInvalidData(e))?;
            Ok(s)
        }
    }
}

/// High-level interface to a SHT_REL relocation table.
#[derive(Debug)]
pub struct RelSection<'a>(SectionInfo<'a>);

impl<'a> RelSection<'a> {
    pub fn entries(&'a self) -> ParseResult<RelSectionIterator<'a>> {
        RelSectionIterator::new(&self.0)
    }
}

#[derive(Debug)]
pub struct Rel {
    pub type_: u32,
}

pub struct RelSectionIterator<'a>(
    ClassAlternative<
        SectionEntityIterator<'a, bindings::elf32_rel>,
        SectionEntityIterator<'a, bindings::elf64_rel>,
    >,
);

impl<'a> RelSectionIterator<'a> {
    fn new(section: &'a SectionInfo<'a>) -> ParseResult<Self> {
        Ok(Self(match section.class {
            Class::Elf32 => ClassAlternative::Elf32(SectionEntityIterator::new(section)?),
            Class::Elf64 => ClassAlternative::Elf64(SectionEntityIterator::new(section)?),
        }))
    }
}

impl<'a> Iterator for RelSectionIterator<'a> {
    type Item = Rel;

    fn next(&mut self) -> Option<Self::Item> {
        match &mut self.0 {
            ClassAlternative::Elf32(iter) => iter.next().map(|n| {
                let type_ = n.r_info.to_cpu(iter.byteorder) & 0xff;
                Self::Item { type_ }
            }),
            ClassAlternative::Elf64(iter) => iter.next().map(|n| {
                let type_ = n.r_info.to_cpu(iter.byteorder) as u32;
                Self::Item { type_ }
            }),
        }
    }
}

/// High-level interface to a SHT_RELA relocation table.
#[derive(Debug)]
pub struct RelaSection<'a>(SectionInfo<'a>);

impl<'a> RelaSection<'a> {
    pub fn entries(&'a self) -> ParseResult<RelaSectionIterator<'a>> {
        RelaSectionIterator::new(&self.0)
    }
}

#[derive(Debug)]
pub struct Rela {
    pub type_: u32,
}

pub struct RelaSectionIterator<'a>(
    ClassAlternative<
        SectionEntityIterator<'a, bindings::elf32_rela>,
        SectionEntityIterator<'a, bindings::elf64_rela>,
    >,
);

impl<'a> RelaSectionIterator<'a> {
    fn new(section: &'a SectionInfo<'a>) -> ParseResult<Self> {
        Ok(Self(match section.class {
            Class::Elf32 => ClassAlternative::Elf32(SectionEntityIterator::new(section)?),
            Class::Elf64 => ClassAlternative::Elf64(SectionEntityIterator::new(section)?),
        }))
    }
}

impl<'a> Iterator for RelaSectionIterator<'a> {
    type Item = Rela;

    fn next(&mut self) -> Option<Self::Item> {
        match &mut self.0 {
            ClassAlternative::Elf32(iter) => iter.next().map(|n| {
                let type_ = n.r_info.to_cpu(iter.byteorder) & 0xff;
                Self::Item { type_ }
            }),
            ClassAlternative::Elf64(iter) => iter.next().map(|n| {
                let type_ = n.r_info.to_cpu(iter.byteorder) as u32;
                Self::Item { type_ }
            }),
        }
    }
}
