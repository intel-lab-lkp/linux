// SPDX-License-Identifier: GPL-2.0

//! Helper types and utilities

macro_rules! endian_type {
    ($old_type:ident, $new_type:ident, $to_new:ident, $from_new:ident) => {
        /// An unsigned integer type of with an explicit endianness.
        #[derive(Copy, Clone, Eq, PartialEq, Debug, Default, pin_init::Zeroable)]
        #[repr(transparent)]
        pub struct $new_type($old_type);

        $crate::static_assert!(
            ::core::mem::align_of::<$new_type>() == ::core::mem::align_of::<$old_type>()
        );
        $crate::static_assert!(
            ::core::mem::size_of::<$new_type>() == ::core::mem::size_of::<$old_type>()
        );

        impl $new_type {
            /// Convert to CPU/native endianness.
            pub const fn to_cpu(self) -> $old_type {
                $old_type::$from_new(self.0)
            }
        }

        impl PartialEq<$old_type> for $new_type {
            fn eq(&self, other: &$old_type) -> bool {
                self.0 == $old_type::$to_new(*other)
            }
        }

        impl PartialEq<$new_type> for $old_type {
            fn eq(&self, other: &$new_type) -> bool {
                $old_type::$to_new(other.0) == *self
            }
        }

        impl From<$new_type> for $old_type {
            fn from(v: $new_type) -> $old_type {
                v.to_cpu()
            }
        }

        impl From<$old_type> for $new_type {
            fn from(v: $old_type) -> $new_type {
                $new_type($old_type::$to_new(v))
            }
        }
    };
}

endian_type!(u16, Le16, to_le, from_le);
endian_type!(u32, Le32, to_le, from_le);
endian_type!(u64, Le64, to_le, from_le);
endian_type!(u16, Be16, to_be, from_be);
endian_type!(u32, Be32, to_be, from_be);
endian_type!(u64, Be64, to_be, from_be);
