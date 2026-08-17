// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

#![cfg_attr(not(CONFIG_KUNIT), expect(dead_code))]

use kernel::prelude::*;

use crate::gsp::nvkv::{
    Index,
    KeyId,
    Op,
    Opcode, //
};
use crate::num;

/// A decoded NVKV value.
#[derive(Copy, Clone)]
pub(crate) enum DecoderValue<'a> {
    Scalar32(u32),
    Scalar64(u64),
    Array8(&'a [u8]),
    Array32(&'a [u32]),
    Array64(&'a [u64]),
}

/// Implements `TryFrom` from the given `DecoderValue` variant to the given type.
///
/// `TryFrom` is used by the `Schema` implementations in this file to convert from the
/// `DecoderValue`s into the types to store. Provide the implementations for basic types here.
macro_rules! impl_try_from_decoder_value {
    ($ty:ty, $variant:ident) => {
        impl<'a> TryFrom<DecoderValue<'a>> for $ty {
            type Error = Error;

            fn try_from(value: DecoderValue<'a>) -> Result<Self> {
                if let DecoderValue::$variant(v) = value {
                    Ok(v)
                } else {
                    Err(EINVAL)
                }
            }
        }
    };
}

impl_try_from_decoder_value!(u32, Scalar32);
impl_try_from_decoder_value!(u64, Scalar64);
impl_try_from_decoder_value!(&'a [u8], Array8);
impl_try_from_decoder_value!(&'a [u32], Array32);
impl_try_from_decoder_value!(&'a [u64], Array64);

/// A visitor that consumes decoded NVKV and produces a `Target`.
pub(crate) trait Schema {
    type Target;

    /// Visits one decoded pair. Returns `Ok(true)` if the schema consumed it.
    fn visit<'a>(&mut self, key: KeyId, index: Index, value: DecoderValue<'a>) -> Result<bool>;

    /// Returns an initializer that makes the decoded `Target`.
    fn finish(self) -> impl Init<Self::Target, Error>;
}

/// A read position in an NVKV stream.
struct Cursor<'a> {
    data: &'a [u64],
}

impl<'a> Cursor<'a> {
    fn new(data: &'a [u64]) -> Self {
        Self { data }
    }

    fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    fn take_u64(&mut self) -> Result<u64> {
        // PANIC: `take_u64s(1)` returns exactly one element on success.
        Ok(self.take_u64s(1)?[0])
    }

    fn take_u8s(&mut self, count: usize) -> Result<&[u8]> {
        let values = self.take_u64s(count.div_ceil(8))?;
        values.as_bytes().get(..count).ok_or(EINVAL)
    }

    fn take_u32s(&mut self, count: usize) -> Result<&[u32]> {
        let values = self.take_u64s(count.div_ceil(2))?;
        // SAFETY: `values` is 8 byte aligned and only 4 byte alignment is required. All bit
        // patterns are valid for `u32`.
        Ok(unsafe { core::slice::from_raw_parts(values.as_ptr().cast::<u32>(), count) })
    }

    fn take_u64s(&mut self, count: usize) -> Result<&[u64]> {
        let (prefix, suffix) = self.data.split_at_checked(count).ok_or(EINVAL)?;
        self.data = suffix;
        Ok(prefix)
    }
}

/// A decoder for an NVKV stream.
pub(crate) struct Decoder<'a> {
    data: &'a [u64],
    policy: UnknownKeyPolicy,
}

impl<'a> Decoder<'a> {
    /// Creates a decoder for `data` that handles unknown keys per `policy`.
    pub(crate) fn new(data: &'a [u64], policy: UnknownKeyPolicy) -> Self {
        Self { data, policy }
    }

    fn visit<S: Schema>(
        &self,
        schema: &mut S,
        key: KeyId,
        index: Index,
        value: DecoderValue<'_>,
    ) -> Result {
        let consumed = schema.visit(key, index, value)?;
        if !consumed && self.policy == UnknownKeyPolicy::Error {
            Err(EINVAL)
        } else {
            Ok(())
        }
    }

    fn seq_key(base: KeyId, offset: usize) -> Result<KeyId> {
        base.checked_add(KeyId::try_from(offset)?).ok_or(EINVAL)
    }

    /// Decodes every pair into `schema` and returns the result of [`Schema::finish`].
    pub(crate) fn decode<S: Schema>(&self, mut schema: S) -> Result<impl Init<S::Target, Error>> {
        let mut cursor = Cursor::new(self.data);
        while !cursor.is_empty() {
            let op: Op = cursor.take_u64()?.into();

            let key = op.key().into();
            let index = op.index();
            let op_value: u32 = op.value().into();
            match op.opcode()? {
                Opcode::Imm32 => {
                    self.visit(&mut schema, key, index, DecoderValue::Scalar32(op_value))?;
                }
                Opcode::Seq32 => {
                    let values = cursor.take_u32s(num::u32_as_usize(op_value))?;
                    for (i, &value) in values.iter().enumerate() {
                        let key = Self::seq_key(key, i)?;
                        self.visit(&mut schema, key, index, DecoderValue::Scalar32(value))?;
                    }
                }
                Opcode::Seq64 => {
                    let values = cursor.take_u64s(num::u32_as_usize(op_value))?;
                    for (i, &value) in values.iter().enumerate() {
                        let key = Self::seq_key(key, i)?;
                        self.visit(&mut schema, key, index, DecoderValue::Scalar64(value))?;
                    }
                }
                Opcode::Array8 => {
                    let value = cursor.take_u8s(num::u32_as_usize(op_value))?;
                    self.visit(&mut schema, key, index, DecoderValue::Array8(value))?;
                }
                Opcode::Array32 => {
                    let value = cursor.take_u32s(num::u32_as_usize(op_value))?;
                    self.visit(&mut schema, key, index, DecoderValue::Array32(value))?;
                }
                Opcode::Array64 => {
                    let value = cursor.take_u64s(num::u32_as_usize(op_value))?;
                    self.visit(&mut schema, key, index, DecoderValue::Array64(value))?;
                }
            };
        }
        Ok(schema.finish())
    }
}

/// This is defined per call.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum UnknownKeyPolicy {
    Ignore,
    Error,
}

#[kunit_tests(nova_core_nvkv_decode)]
mod tests {
    use super::*;

    use crate::gsp::nvkv::Encoder;

    // Tests that basic decoding into a manually implemented `Schema` works correctly.
    #[test]
    fn decode_raw_schema() -> Result {
        // Decodes an IMM32 pair and a SEQ64 pair (the encoder emits a u64 as a single-element
        // SEQ64) with a hand written `Schema`. Keys and value constants chosen to distinguish e.g.
        // saving the wrong value to the wrong location.
        const SCALAR32_KEY: KeyId = 0x1001;
        const SCALAR64_KEY: KeyId = 0x1002;
        const UNKNOWN_KEY: KeyId = 0x2001;

        const SCALAR32_VALUE: u32 = 0x1111_2222;
        const SCALAR64_VALUE: u64 = 0x3333_4444_5555_6666;

        // The output type of the hand written Schema. In this case, we can have it also implement
        // `Schema` on itself rather than having a separate carrier type, since the `Schema`
        // implementation is completely stateless.
        #[derive(Default)]
        struct RawSchema {
            scalar32: u32,
            scalar64: u64,
        }

        impl Schema for RawSchema {
            type Target = Self;

            fn visit(&mut self, key: KeyId, index: Index, value: DecoderValue<'_>) -> Result<bool> {
                if index != Index::new::<0>() {
                    return Err(EINVAL);
                }
                match key {
                    SCALAR32_KEY => self.scalar32 = value.try_into()?,
                    SCALAR64_KEY => self.scalar64 = value.try_into()?,
                    _ => return Ok(false),
                }
                Ok(true)
            }

            fn finish(self) -> impl Init<Self::Target, Error> {
                Ok(self)
            }
        }

        let mut encoder = Encoder::new();
        encoder.encode_u32(SCALAR32_KEY, Index::new::<0>(), SCALAR32_VALUE)?;
        encoder.encode_u64(SCALAR64_KEY, Index::new::<0>(), SCALAR64_VALUE)?;
        let serialized = encoder.finish();

        let decoder = Decoder::new(&serialized, UnknownKeyPolicy::Error);
        let decoded = KBox::try_init(decoder.decode(RawSchema::default())?, GFP_KERNEL)?;

        assert_eq!(decoded.scalar32, SCALAR32_VALUE);
        assert_eq!(decoded.scalar64, SCALAR64_VALUE);

        // An unknown key should fail with under `UnknownKeyPolicy::Error` and be skipped under
        // `UnknownKeyPolicy::Ignore`.
        let mut encoder = Encoder::new();
        encoder.encode_u32(UNKNOWN_KEY, Index::new::<0>(), 1)?;

        let serialized = encoder.finish();
        let decoder = Decoder::new(&serialized, UnknownKeyPolicy::Error);
        assert!(decoder.decode(RawSchema::default()).is_err());

        let decoder = Decoder::new(&serialized, UnknownKeyPolicy::Ignore);
        let decoded = KBox::try_init(decoder.decode(RawSchema::default())?, GFP_KERNEL)?;
        assert_eq!(decoded.scalar32, 0);

        Ok(())
    }
}
