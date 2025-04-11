// SPDX-License-Identifier: GPL-2.0

//! impl_flags utilities for working with flags.

/// Declares a impl_flags type with its corresponding flag type.
///
/// This macro generates:
/// - Implementations of common bitmap op. ([`::core::ops::BitOr`], [`::core::ops::BitAnd`], etc.).
/// - Utility methods such as `.contains()` to check flags.
///
/// # Examples
///
/// Defining and using impl_flags:
///
/// ```
/// impl_flags!(
///     /// Represents multiple permissions.
///     #[derive(Debug, Clone, Default, Copy, PartialEq, Eq)]
///     pub Permissions,
///     /// Represents a single permission.
///     #[derive(Debug, Clone, Copy, PartialEq, Eq)]
///     pub Permission {
///         const READ = 1 << 0,
///         const WRITE = 1 << 1,
///         const EXECUTE = 1 << 2,
///         },
///     u32
/// );
///
///
/// // Combine multiple permissions using operation OR (`|`).
/// let read_write: Permissions = Permission::READ | Permission::WRITE;
///
/// assert!(read_write.contains(Permission::READ));
/// assert!(read_write.contains(Permission::WRITE));
/// assert!(!read_write.contains(Permission::EXECUTE));
///
/// // Removing a permission with operation AND (`&`).
/// let read_only: Permissions = read_write & Permission::READ;
/// assert!(read_only.contains(Permission::READ));
/// assert!(!read_only.contains(Permission::WRITE));
///
/// // Toggling permissions with XOR (`^`).
/// let toggled: Permissions = read_only ^ Permission::READ;
/// assert!(!toggled.contains(Permission::READ));
///
/// // Inverting permissions with negation (`!`).
/// let negated = !read_only;
/// assert!(negated.contains(Permission::WRITE));
/// ```
#[macro_export]
macro_rules! impl_flags {
    (
        $(#[$outer_flags:meta])*
        $vis_flags:vis $flags:ident,

        $(#[$outer_flag:meta])*
        $vis_flag:vis $flag:ident {
            $(
                $(#[$inner_flag:meta])*
                $kw:ident $name:ident = $value:expr
            ),* $(,)?
        },
        $ty:ty
    ) => {
        $(#[$outer_flags])*
        #[repr(transparent)]
        $vis_flags struct $flags($ty);

        $(#[$outer_flag])*
        $vis_flag struct $flag($ty);

        impl ::core::convert::From<$flag> for $flags {
            #[inline]
            fn from(value: $flag) -> Self {
                Self(value.0)
            }
        }

        impl ::core::convert::From<$flags> for $ty {
            #[inline]
            fn from(value: $flags) -> Self {
                value.0
            }
        }

        impl ::core::ops::BitOr for $flags {
            type Output = Self;
            #[inline]
            fn bitor(self, rhs: Self) -> Self::Output {
                Self(self.0 | rhs.0)
            }
        }

        impl ::core::ops::BitOrAssign for $flags {
            #[inline]
            fn bitor_assign(&mut self, rhs: Self) {
                *self = *self | rhs;
            }
        }

        impl ::core::ops::BitAnd for $flags {
            type Output = Self;
            #[inline]
            fn bitand(self, rhs: Self) -> Self::Output {
                Self(self.0 & rhs.0)
            }
        }

        impl ::core::ops::BitAndAssign for $flags {
            #[inline]
            fn bitand_assign(&mut self, rhs: Self) {
                *self = *self & rhs;
            }
        }

        impl ::core::ops::BitOr<$flag> for $flags {
            type Output = Self;
            #[inline]
            fn bitor(self, rhs: $flag) -> Self::Output {
                self | Self::from(rhs)
            }
        }

        impl ::core::ops::BitOrAssign<$flag> for $flags {
            #[inline]
            fn bitor_assign(&mut self, rhs: $flag) {
                *self = *self | rhs;
            }
        }

        impl ::core::ops::BitAnd<$flag> for $flags {
            type Output = Self;
            #[inline]
            fn bitand(self, rhs: $flag) -> Self::Output {
                self & Self::from(rhs)
            }
        }

        impl ::core::ops::BitAndAssign<$flag> for $flags {
            #[inline]
            fn bitand_assign(&mut self, rhs: $flag) {
                *self = *self & rhs;
            }
        }

        impl ::core::ops::BitXor for $flags {
            type Output = Self;
            #[inline]
            fn bitxor(self, rhs: Self) -> Self::Output {
                Self(self.0 ^ rhs.0)
            }
        }

        impl ::core::ops::BitXorAssign for $flags {
            #[inline]
            fn bitxor_assign(&mut self, rhs: Self) {
                *self = *self ^ rhs;
            }
        }

        impl ::core::ops::Not for $flags {
            type Output = Self;
            #[inline]
            fn not(self) -> Self::Output {
                Self(!self.0)
            }
        }

        impl ::core::ops::BitOr for $flag {
            type Output = $flags;
            #[inline]
            fn bitor(self, rhs: Self) -> Self::Output {
                $flags(self.0 | rhs.0)
            }
        }

        impl ::core::ops::BitAnd for $flag {
            type Output = $flags;
            #[inline]
            fn bitand(self, rhs: Self) -> Self::Output {
                $flags(self.0 & rhs.0)
            }
        }

        impl ::core::ops::BitXor for $flag {
            type Output = $flags;
            #[inline]
            fn bitxor(self, rhs: Self) -> Self::Output {
                $flags(self.0 ^ rhs.0)
            }
        }

        impl ::core::ops::Not for $flag {
            type Output = $flags;
            #[inline]
            fn not(self) -> Self::Output {
                $flags(!self.0)
            }
        }

        impl ::core::ops::BitXor<$flag> for $flags {
            type Output = Self;
            #[inline]
            fn bitxor(self, rhs: $flag) -> Self::Output {
                self ^ Self::from(rhs)
            }
        }

        impl $flag {
            $(
                $(#[$inner_flag])*
                pub $kw $name: Self = Self($value);
            )*
        }

        impl $flags {
            /// Returns an empty instance of `type` where no flags are set.
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
