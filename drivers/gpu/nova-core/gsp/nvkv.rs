// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

//! Codec for NVKV, the binary key-value format of GMCAPI.
//!
//! Essentially, the format encodes a sequence of calls to some function f(key, index, value),
//! where value is a [u8], u32, u64, [u32], or a [u64]. The key is a u16 and the index is a 12 bit
//! integer. The interpretation of these function calls is per GMCAPI. Generally speaking, the
//! function calls will map to some struct - for example, f(GPU_NAME_STRING_KEY, 0, b"some gpu")
//! naturally maps to storing a &str with the GPU name.

#![expect(unused_imports)]

use core::ops::Deref;

use kernel::{
    alloc::{
        allocator::KVmalloc,
        Allocator, //
    },
    bitfield,
    num::Bounded,
    prelude::*, //
};
use zerocopy::Immutable;

use crate::bounded_enum;

mod encode;
pub(crate) use encode::*;

/// The allocator backing [`EncodedStream`].
type StreamAllocator = KVmalloc;

/// An encoded NVKV byte stream.
///
/// # Invariants
///
/// The byte length is always a multiple of `size_of::<u64>()`.
pub(crate) struct EncodedStream(Vec<u8, StreamAllocator>);

impl EncodedStream {
    /// Creates an empty stream.
    fn new() -> Self {
        // INVARIANT: An empty stream's byte length is 0, a multiple of `size_of::<u64>()`.
        Self(Vec::new())
    }

    /// Appends a single `u64` to the stream.
    fn push_u64(&mut self, value: u64) -> Result {
        // INVARIANT: Appending `size_of::<u64>()` bytes keeps the byte length a multiple of
        // `size_of::<u64>()`.
        Ok(self.0.extend_from_slice(&value.to_ne_bytes(), GFP_KERNEL)?)
    }

    /// Appends `data` as bytes to the stream, zero-padded to a `u64` boundary.
    fn extend_with_padding<T: IntoBytes + Immutable + ?Sized>(&mut self, data: &T) -> Result {
        let bytes = data.as_bytes();
        let padded = bytes.len().next_multiple_of(size_of::<u64>());
        // Reserve so that a failed allocation can't leave the invariant violated.
        self.0.reserve(padded, GFP_KERNEL)?;
        self.0.extend_from_slice(bytes, GFP_KERNEL)?;
        // INVARIANT: The padding ensures the total length remains a multiple of
        // `size_of::<u64>()`.
        Ok(self.0.extend_with(padded - bytes.len(), 0u8, GFP_KERNEL)?)
    }
}

// The Deref to `&[u64]` relies on this alignment guarantee.
static_assert!(align_of::<u64>() <= StreamAllocator::MIN_ALIGN);

impl Deref for EncodedStream {
    type Target = [u64];

    fn deref(&self) -> &Self::Target {
        // An empty `Vec`'s pointer isn't necessarily aligned by `StreamAllocator::MIN_ALIGN`.
        if self.0.is_empty() {
            return &[];
        }

        // PANIC: By the type invariants the byte length is a multiple of `size_of::<u64>()`, and
        // the backing buffer of a non-empty vector has at least `u64` alignment per
        // `StreamAllocator`'s minimum alignment.
        <[u64]>::ref_from_bytes(&self.0).expect("EncodedStream invariant violated")
    }
}

/// The identifier of an NVKV key.
pub(crate) type KeyId = u16;

/// The index of an NVKV value.
pub(crate) type Index = Bounded<u64, 12>;

bitfield! {
    /// The op word that starts each NVKV operation.
    struct Op(u64) {
        15:0 key;
        27:16 index => Index;
        31:28 opcode ?=> Opcode;
        63:32 value;
    }
}

bounded_enum! {
    /// Describes the format of the following NVKV operation.
    #[derive(Debug, Copy, Clone, PartialEq, Eq)]
    #[repr(u8)]
    enum Opcode with TryFrom<Bounded<u64, 4>> {
        /// A 32-bit value in the op word.
        Imm32 = 0,
        /// 32-bit values for consecutive keys, starting at the op word's key.
        Seq32 = 1,
        /// 64-bit values for consecutive keys, starting at the op word's key.
        Seq64 = 2,
        /// An array of bytes.
        Array8 = 3,
        /// An array of 32-bit elements.
        Array32 = 4,
        /// An array of 64-bit elements.
        Array64 = 5,
    }
}
