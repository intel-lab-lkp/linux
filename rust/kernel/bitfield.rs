// SPDX-License-Identifier: GPL-2.0

//! Trait and macro to build bitfield wrappers.
//!
//! A trait and [kernel::bitfield_options] macro to build bitfield wrappers with
//! [std::fs::OpenOptions](https://doc.rust-lang.org/std/fs/struct.OpenOptions.html)-like methods to
//! set bits and _try to ensure an always-valid state_.

/// A trait associating a bitfield type to a wrapper type to allow for shared trait implementations
/// and bitfield retrieval.
pub trait BitField {
    /// The bitfield type, usually [u8] or [u32]
    type Bits;

    /// returns the bits stored by the wrapper.
    fn bits(&self) -> Self::Bits;
}

/// concatenate sequences using a comma separator. Uses a a `, and` (oxford comma) separation
/// instead between the last two items.
#[doc(hidden)]
#[macro_export]
macro_rules! concat_with_oxford_comma {
    ($last:expr) => (concat!("and ", $last));
    ($left:expr, $($right:expr),+ ) => (
        concat!($left, ", ", $crate::concat_with_oxford_comma!($($right),+))
    )
}

/// Builds a bitfield wrapper with
/// [std::fs::OpenOptions](https://doc.rust-lang.org/std/fs/struct.OpenOptions.html)-like methods to
/// set bits.
///
/// This wrapper _tries to ensure an always-valid state_, even when created from a bitfield
/// directly.
///
/// The wrapper implements the [BitField] trait.
///
/// # argument types
///   - `name`: the name of the wrapper to build.
///   - `type`: the bit field type, usually `u32` or `u8`. Must be an int primitive.
///   - 'options' : array of `{ name:ident, true:expr, false:expr }` options where name is the name
///     of a method that will set the `true:expr` bit(s) when called with `true` or else the
///     `false:expr` bit(s). In a standard bitfield, `false:expr` is set to `false:0`, but sometimes
///     different bit flags designate incompatible alternatives : in these cases, set `false` to
///     activate the alternative bit(s).
///
/// # Examples
///
/// Call the macro to create the bitflag wrapper.
///
/// Here we create a wrapper that allows to set `RED`, `BLUE`, `GREEN` bits independently,
/// and additionnaly either the `BIG` bit or the `SMALL` bit.
///
/// `BIG` and `SMALL` are incompatible alternatives.
///
/// ```rust
/// use kernel::bitfield_options;
///
/// const BIG: u32 = 16;
/// const SMALL: u32 = 1;
///
/// const RED: u32 = 2;
/// const GREEN: u32 = 4;
/// const BLUE: u32 = 8;
///
/// bitfield_options! {
///     name: SizeAndColors,
///     type: u32,
///     options: [
///         {name:big, true:BIG, false:SMALL},
///         {name:red, true:RED, false:0u32},
///         {name:blue, true:BLUE, false:0u32},
///         {name:green, true:GREEN, false:0u32}
///     ]
/// };
/// ```
///
/// The `SizeAndColor` type is now available in this scope.
///
/// It implements [Default]: all options set to false.
///
///
/// ```rust
/// # use kernel::bitfield_options;
/// # use kernel::bitfield::BitField; // to have access to the bits() method
/// #
/// # const BIG: u32 = 16;
/// # const SMALL: u32 = 1;
/// #
/// # const RED: u32 = 2;
/// # const GREEN: u32 = 4;
/// # const BLUE: u32 = 8;
/// #
/// # bitfield_options! {
/// #     name: SizeAndColors,
/// #     type: u32,
/// #     options: [
/// #         {name:big, true:BIG, false:SMALL},
/// #         {name:red, true:RED, false:0u32},
/// #         {name:blue, true:BLUE, false:0u32},
/// #         {name:green, true:GREEN, false:0u32}
/// #     ]
/// # };
/// let sac = SizeAndColors::default();
///
/// // the default state, with all the options set to false should be the field with only the
/// // SMALL bit(s) set
/// assert_eq!(
///     sac.bits(),
///     SMALL | 0u32 | 0u32 | 0u32)
/// ```
///
/// It implements [TryFrom] which succeeds only if the passed bitfield is a valid state.
///
/// ```rust
/// # use kernel::bitfield_options;
/// #
/// # const BIG: u32 = 16;
/// # const SMALL: u32 = 1;
/// #
/// # const RED: u32 = 2;
/// # const GREEN: u32 = 4;
/// # const BLUE: u32 = 8;
/// #
/// # bitfield_options! {
/// #     name: SizeAndColors,
/// #     type: u32,
/// #     options: [
/// #         {name:big, true:BIG, false:SMALL},
/// #         {name:red, true:RED, false:0u32},
/// #         {name:blue, true:BLUE, false:0u32},
/// #         {name:green, true:GREEN, false:0u32}
/// #     ]
/// # };
/// # let sac = SizeAndColors::default();
///
/// let small_and_no_colors = SizeAndColors::try_from(SMALL)
///    .expect("having only the SMALL bit to 1 should be a valid state");
///
/// // Having only the SMALL bit set to 1 is the default state.
/// assert_eq!(sac, small_and_no_colors);
///
/// let big_and_rgb = SizeAndColors::try_from(BIG | RED | GREEN | BLUE)
///     .expect("having the BIG, RED, GREEN and BLUE bits set to 1 should be a valid state");
///
/// assert_eq!(
///     SizeAndColors::default()
///         .big(true)
///         .red(true)
///         .blue(true)
///         .green(true),
///     big_and_rgb
/// );
///
/// let big_and_small = SizeAndColors::try_from(BIG | SMALL)
///     .expect_err("BIG and SMALL are incompatible, this should fail");
///
/// let intense:u32 = 1024;
/// let big_and_intense = SizeAndColors::try_from(BIG | intense)
///     .expect_err("BIG|intense is not a valid state, this should fail");
/// ```
#[macro_export]
macro_rules! bitfield_options {{
    name:$name:ident,
    type:$t:ty,
    options:[
        $({name:$fn_name:ident, true: $true_bitval:expr, false: $false_bitval:expr}),+
]} => {
    #[doc = concat!("A wrapper around a `",
        stringify!($t),
        "` bitfield that sets bitflags using its ",
        $crate::concat_with_oxford_comma!(
        $(concat!("[",
            stringify!($fn_name),
            "](",
            stringify!($name),
            "::",
            stringify!($fn_name),
            ")")
        ),+),
        " methods.")]
    #[derive(Debug, PartialEq)]
    pub struct $name($t);

    impl $crate::bitfield::BitField for $name {
        type Bits = $t;

        fn bits(&self) -> Self::Bits {
            self.0
        }
    }

    impl $name {
    $(
        #[doc = concat!("if `true`, unsets the `",
            stringify!($false_bitval),
            "` bit(s) and sets the `",
            stringify!($true_bitval),
            "` bit(s).  "
        )]
        #[doc = "Otherwise, does the opposite."]
        pub fn $fn_name(mut self, $fn_name:bool)->Self{
            if $fn_name{
                self.0 = self.0 & !$false_bitval | $true_bitval;
            }else{
                self.0 = self.0 & !$true_bitval | $false_bitval;
            }
            self
        }
    )+
    }

    impl Default for $name{
        fn default() -> Self {
            Self(0)$(.$fn_name(false))+
        }
    }

    impl TryFrom<<$name as $crate::bitfield::BitField>::Bits> for $name {
        type Error = <$name as $crate::bitfield::BitField>::Bits;

        fn try_from(value: <$name as $crate::bitfield::BitField>::Bits) -> Result<Self, Self::Error>
        {
            let mut to_process = value.clone();

            let mut $(
            matched = false;

            for flag in [$true_bitval, $false_bitval] {
                if (flag & to_process) == flag {
                    matched = true;
                    to_process -= flag;
                    if flag > 0 {
                        break;
                    }
                }
            }
            if !matched {
                return Err(value);
            })+


            if to_process == 0 {
                return Ok(Self(value));
            }
            return Err(value)
        }
    }

    };
}
#[doc(inline)]
pub use bitfield_options;
