// SPDX-License-Identifier: GPL-2.0

//! Bitmask utilities for working with flags in Rust.

/// Declares a bitmask type with its corresponding flag type.
///
/// This macro generates:
/// - Implementations of common bitwise operations (`BitOr`, `BitAnd`, etc.).
/// - Utility methods such as `.contains()` to check flags.
///
/// # Examples
///
/// Defining and using a bitmask:
/// ```
/// bitmask!(Permissions, Permission, u32);
///
/// // Define some individual permissions
/// const READ: Permission = Permission(1 << 0);
/// const WRITE: Permission = Permission(1 << 1);
/// const EXECUTE: Permission = Permission(1 << 2);
///
/// // Combine multiple permissions using bitwise OR (`|`)
/// let read_write = Permissions::from(READ) | WRITE;
///
/// assert!(read_write.contains(READ));   // READ is set
/// assert!(read_write.contains(WRITE));  // WRITE is set
/// assert!(!read_write.contains(EXECUTE)); // EXECUTE is not set
///
/// // Removing a permission with bitwise AND (`&`)
/// let read_only = read_write & READ;
/// assert!(read_only.contains(READ)); // Still has READ
/// assert!(!read_only.contains(WRITE)); // WRITE was removed
///
/// // Toggling permissions with XOR (`^`)
/// let toggled = read_only ^ Permissions::from(READ);
/// assert!(!toggled.contains(READ)); // READ was removed
///
/// // Inverting permissions with negation (`-`)
/// let negated = -read_only;
/// assert!(negated.contains(WRITE)); // Previously unset bits are now set
/// ```
#[macro_export]
macro_rules! bitmask {
    ($flags:ident, $flag:ident, $ty:ty) => {
        #[allow(missing_docs)]
        #[repr(transparent)]
        #[derive(Copy, Clone, Default, PartialEq, Eq)]
        pub struct $flags($ty);

        #[allow(missing_docs)]
        #[derive(Copy, Clone, PartialEq, Eq)]
        pub struct $flag($ty);

        impl From<$flag> for $flags {
            #[inline]
            fn from(value: $flag) -> Self {
                Self(value.0)
            }
        }

        impl From<$flags> for $ty {
            #[inline]
            fn from(value: $flags) -> Self {
                value.0
            }
        }

        impl core::ops::BitOr for $flags {
            type Output = Self;

            #[inline]
            fn bitor(self, rhs: Self) -> Self::Output {
                Self(self.0 | rhs.0)
            }
        }

        impl core::ops::BitOrAssign for $flags {
            #[inline]
            fn bitor_assign(&mut self, rhs: Self) {
                *self = *self | rhs;
            }
        }

        impl core::ops::BitAnd for $flags {
            type Output = Self;

            #[inline]
            fn bitand(self, rhs: Self) -> Self::Output {
                Self(self.0 & rhs.0)
            }
        }

        impl core::ops::BitAndAssign for $flags {
            #[inline]
            fn bitand_assign(&mut self, rhs: Self) {
                *self = *self & rhs;
            }
        }

        impl core::ops::BitOr<$flag> for $flags {
            type Output = Self;

            #[inline]
            fn bitor(self, rhs: $flag) -> Self::Output {
                self | Self::from(rhs)
            }
        }

        impl core::ops::BitOrAssign<$flag> for $flags {
            #[inline]
            fn bitor_assign(&mut self, rhs: $flag) {
                *self = *self | rhs;
            }
        }

        impl core::ops::BitAnd<$flag> for $flags {
            type Output = Self;

            #[inline]
            fn bitand(self, rhs: $flag) -> Self::Output {
                self & Self::from(rhs)
            }
        }

        impl core::ops::BitAndAssign<$flag> for $flags {
            #[inline]
            fn bitand_assign(&mut self, rhs: $flag) {
                *self = *self & rhs;
            }
        }

        impl core::ops::BitXor for $flags {
            type Output = Self;

            #[inline]
            fn bitxor(self, rhs: Self) -> Self::Output {
                Self(self.0 ^ rhs.0)
            }
        }

        impl core::ops::BitXorAssign for $flags {
            #[inline]
            fn bitxor_assign(&mut self, rhs: Self) {
                *self = *self ^ rhs;
            }
        }

        impl core::ops::Neg for $flags {
            type Output = Self;

            #[inline]
            fn neg(self) -> Self::Output {
                Self(!self.0)
            }
        }

        impl $flags {
            /// Returns an empty instance of <type> where no flags are set.
            #[inline]
            pub const fn empty() -> Self {
                Self(0)
            }

            /// Checks if a specific flag is set.
            #[inline]
            pub fn contains(self, flag: $flag) -> bool {
                (self.0 & flag.0) == flag.0
            }
        }
    };
}
