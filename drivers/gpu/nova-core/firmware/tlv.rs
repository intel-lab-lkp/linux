// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

use kernel::{
    device,
    firmware,
    prelude::*,
    str::CString, //
};

use crate::gpu;

/// Requests the GPU firmware TLV `name` suitable for `chipset`.
pub(crate) fn request_tlv(
    dev: &device::Device,
    chipset: gpu::Chipset,
    name: &str,
) -> Result<firmware::Firmware> {
    let chip_name = chipset.name();

    dev_dbg!(
        dev,
        "loading firmware image {}/gsp/{}.tlv\n",
        chip_name,
        name
    );

    CString::try_from_fmt(fmt!("nvidia/{chip_name}/gsp/{name}.tlv"))
        .and_then(|path| firmware::Firmware::request(&path, dev))
}

struct TlvBlock<'a> {
    tag: &'a [u8; 4],
    value: &'a [u8],
}

/// On-wire TLV block header: 4-byte ASCII tag + little-endian payload length (bytes, excluding
/// padding to a 4-byte boundary).
struct TlvBlockHeader<'a> {
    tag: &'a [u8; 4],
    length: usize,
}

impl<'a> TlvBlockHeader<'a> {
    const SIZE: usize = size_of::<[u8; 4]>() + size_of::<u32>();

    /// Parses the first [`Self::SIZE`] bytes of `hdr` (caller may pass a longer slice).
    fn parse(hdr: &'a [u8]) -> Option<Self> {
        let hdr = hdr.get(..Self::SIZE)?;
        let tag = <&[u8; 4]>::try_from(hdr.get(..4)?).ok()?;
        if !tag.is_ascii() {
            return None;
        }
        let len_arr = <[u8; 4]>::try_from(hdr.get(4..Self::SIZE)?).ok()?;
        let length = u32::from_le_bytes(len_arr) as usize;
        Some(Self { tag, length })
    }
}

/// Iterator over the [`TlvBlock`]s of a [`Tlv`].
///
/// # Invariants
///
/// `pos` is a byte offset into `tlv.data` that always lies on a block boundary (in the sense
/// of the [`Tlv`] invariant): it is either the start of a well-formed block, or equal to
/// `tlv.data.len()` (end of iteration).
struct TlvIter<'tlv, 'a> {
    tlv: &'tlv Tlv<'a>,
    pos: usize,
}

impl<'tlv, 'a> Iterator for TlvIter<'tlv, 'a> {
    type Item = TlvBlock<'a>;

    /// Returns the block starting at `self.pos` and advances the cursor past it, or [`None`]
    /// once the cursor reaches the end of the data or encounters an error.
    ///
    /// Note that errors cannot actually occur because the data is validated in the constructor.
    fn next(&mut self) -> Option<Self::Item> {
        if self.pos >= self.tlv.data.len() {
            return None;
        }

        let tail = self.tlv.data.get(self.pos..)?;

        let hdr = tail.get(..TlvBlockHeader::SIZE)?;
        let header = TlvBlockHeader::parse(hdr)?;

        let stored_size = header.length.checked_next_multiple_of(4)?;
        let advance = TlvBlockHeader::SIZE.checked_add(stored_size)?;
        let payload_end = TlvBlockHeader::SIZE.checked_add(header.length)?;

        let value = tail
            .get(..advance)?
            .get(TlvBlockHeader::SIZE..payload_end)?;

        // INVARIANT: by the `Tlv` invariant the block at `self.pos` occupies exactly `advance`
        // bytes, so `self.pos + advance` is the next block boundary (or `data.len()`).
        self.pos += advance;

        Some(TlvBlock {
            tag: header.tag,
            value,
        })
    }
}

/// The payload of a validated TLV (type, length, value) firmware image.
///
/// TLV firmware images start with a 4-byte "NVFW" magic header, followed by a sequence of
/// blocks. Each block has a 4-byte type tag, a 4-byte length field, and a data payload
/// whose stored size is the length rounded up to the nearest multiple of 4.
///
/// [`Self::new`] checks the magic header and walks every block: tags must be ASCII,
/// lengths and padding must fit without overflow, and the byte stream after `NVFW` must
/// be exactly partitionable into blocks (no trailing partial header or slack). After
/// that, [`TlvIter`] only signals end-of-stream via [`None`], not parse failure.
///
/// Although the spec forbids duplicate tags, neither the constructor nor the iterator
/// enforces this restriction.  Instead, duplicate tags are simply ignored.
///
/// # Invariants
///
/// `data` is a validated TLV payload (the bytes *after* the `NVFW` magic): it is the exact
/// concatenation of zero or more well-formed blocks, with no trailing partial header or slack.
/// Consequently, any offset `o` into `data` that is a block boundary and satisfies
/// `o < data.len()` is the start of a complete block whose header parses and whose stored
/// extent (`TlvBlockHeader::SIZE + header.length.next_multiple_of(4)` bytes) lies within
/// `data`. `data.len()` is itself a boundary.
pub(crate) struct Tlv<'a> {
    data: &'a [u8],
}

impl<'a> Tlv<'a> {
    const MAGIC: &'static [u8; 4] = b"NVFW";

    /// Parses `data` as a TLV firmware image, returning [`EINVAL`] if the image is malformed.
    pub(crate) fn new(data: &'a [u8]) -> Result<Self> {
        // Verify that the magic bytes exist and are the correct value
        let magic_len = Self::MAGIC.len();
        if data
            .get(..magic_len)
            .is_none_or(|magic| magic != Self::MAGIC)
        {
            return Err(EINVAL);
        }

        // The payload is the contiguous sequence of TLV blocks after the magic.
        let payload = data.get(magic_len..).ok_or(EINVAL)?;

        if payload.is_empty() {
            // Reject empty TLV files
            return Err(EINVAL);
        }

        let mut rest = payload;
        while !rest.is_empty() {
            // Validate and extract the header (type, length).
            let Some(header) = rest
                .get(..TlvBlockHeader::SIZE)
                .and_then(TlvBlockHeader::parse)
            else {
                return Err(EINVAL);
            };

            // The `length` field of a TLV block contains the actual byte length of the
            // value, but each TLV block is aligned to a 4-byte boundary.
            let Some(stored_size) = header.length.checked_next_multiple_of(4) else {
                return Err(EINVAL);
            };

            let length = TlvBlockHeader::SIZE
                .checked_add(stored_size)
                .ok_or(EINVAL)?;

            if length > rest.len() {
                return Err(EINVAL);
            }

            // Advance to the next block. `length <= rest.len()` was just checked, so this
            // slice is always in bounds and lands on the next block boundary (or empties
            // `rest` after the final block).
            rest = &rest[length..];
        }

        Ok(Self { data: payload })
    }

    fn iter(&self) -> TlvIter<'_, 'a> {
        // INVARIANT: 0 is a block boundary, either the start of the first block,
        // or `data.len()` when `data` is empty.
        TlvIter { tlv: self, pos: 0 }
    }

    fn find(&self, tag: &[u8; 4]) -> Result<TlvBlock<'a>> {
        self.iter().find(|b| b.tag == tag).ok_or(EINVAL)
    }

    pub(crate) fn get_bytes(&self, tag: &[u8; 4]) -> Result<&'a [u8]> {
        let tlv = self.find(tag)?;

        Ok(tlv.value)
    }

    pub(crate) fn get_u32(&self, tag: &[u8; 4]) -> Result<u32> {
        let tlv = self.find(tag)?;

        tlv.value
            .try_into()
            .ok()
            .map(u32::from_le_bytes)
            .ok_or(EINVAL)
    }

    pub(crate) fn get_string(&self, tag: &[u8; 4]) -> Result<&'a str> {
        let tlv = self.find(tag)?;

        let bytes = tlv.value;

        // To make sure the value actually is a string, make sure it's all ASCII.
        if !bytes.is_ascii() {
            return Err(EINVAL);
        }

        core::str::from_utf8(bytes).map_err(|_| EINVAL)
    }
}
