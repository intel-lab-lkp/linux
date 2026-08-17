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

use kernel::{
    bitfield,
    num::Bounded,
    prelude::*, //
};

mod encode;
pub(crate) use encode::*;

mod decode;
pub(crate) use decode::*;

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

/// Describes the format of the following NVKV operation.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u8)]
enum Opcode {
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

// TODO[FPRI]: This is a temporary solution to be replaced with the corresponding derive macros once
// they land.
impl TryFrom<Bounded<u64, 4>> for Opcode {
    type Error = Error;

    fn try_from(value: Bounded<u64, 4>) -> Result<Self> {
        match value.get() {
            0 => Ok(Self::Imm32),
            1 => Ok(Self::Seq32),
            2 => Ok(Self::Seq64),
            3 => Ok(Self::Array8),
            4 => Ok(Self::Array32),
            5 => Ok(Self::Array64),
            _ => Err(EINVAL),
        }
    }
}

impl From<Opcode> for Bounded<u64, 4> {
    fn from(value: Opcode) -> Self {
        Bounded::from_expr(value as u64)
    }
}
