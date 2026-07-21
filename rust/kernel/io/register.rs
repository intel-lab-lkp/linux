// SPDX-License-Identifier: GPL-2.0

//! Macro to define register layout and accessors.
//!
//! The [`register!`](kernel::io::register!) macro provides an intuitive and readable syntax for
//! defining a dedicated type for each register and accessing it using [`Io`](super::Io). Each such
//! type comes with its own field accessors that can return an error if a field's value is invalid.
//!
//! Note: most of the items in this module are public so they can be referenced by the macro, but
//! most are not to be used directly by users. Outside of the `register!` macro itself, the only
//! items you might want to import from this module are [`WithBase`] and [`Array`].
//!
//! # Simple example
//!
//! ```no_run
//! use kernel::io::{
//!     register,
//!     Region,
//! };
//!
//! register! {
//!     base: Region<0x1000>;
//!
//!     /// Basic information about the chip.
//!     pub BOOT_0(u32) @ 0x00000100 {
//!         /// Vendor ID.
//!         15:8 vendor_id;
//!         /// Major revision of the chip.
//!         7:4 major_revision;
//!         /// Minor revision of the chip.
//!         3:0 minor_revision;
//!     }
//! }
//! ```
//!
//! This defines a 32-bit `BOOT_0` type which can be read from or written to offset `0x100` of an
//! `Io` region, with the described bitfields. For instance, `minor_revision` consists of the 4
//! least significant bits of the type.
//!
//! Fields are instances of [`Bounded`](kernel::num::Bounded) and can be read by calling their
//! getter method, which is named after them. They also have setter methods prefixed with `with_`
//! for runtime values and `with_const_` for constant values. All setters return the updated
//! register value.
//!
//! Fields can also be transparently converted from/to an arbitrary type by using the `=>` and
//! `?=>` syntaxes.
//!
//! If present, doc comments above register or fields definitions are added to the relevant item
//! they document (the register type itself, or the field's setter and getter methods).
//!
//! Note that multiple registers can be defined in a single `register!` invocation. This can be
//! useful to group related registers together.
//!
//! Here is how the register defined above can be used in code:
//!
//!
//! ```no_run
//! use kernel::{
//!     io::{
//!         register,
//!         Io,
//!         IoLoc,
//!         Region,
//!     },
//!     num::Bounded,
//! };
//! # use kernel::io::Mmio;
//! # register! {
//! #     base: Region<0x1000>;
//! #
//! #     pub BOOT_0(u32) @ 0x00000100 {
//! #         15:8 vendor_id;
//! #         7:4 major_revision;
//! #         3:0 minor_revision;
//! #     }
//! # }
//! # fn test(io: Mmio<'_, Region<0x1000>>) {
//! # fn obtain_vendor_id() -> u8 { 0xff }
//!
//! // Read from the register's defined offset (0x100).
//! let boot0 = io.read(BOOT_0);
//! pr_info!("chip revision: {}.{}", boot0.major_revision().get(), boot0.minor_revision().get());
//!
//! // Update some fields and write the new value back.
//! let new_boot0 = boot0
//!     // Constant values.
//!     .with_const_major_revision::<3>()
//!     .with_const_minor_revision::<10>()
//!     // Runtime value.
//!     .with_vendor_id(obtain_vendor_id());
//! io.write_reg(new_boot0);
//!
//! // Or, build a new value from zero and write it:
//! io.write_reg(BOOT_0::zeroed()
//!     .with_const_major_revision::<3>()
//!     .with_const_minor_revision::<10>()
//!     .with_vendor_id(obtain_vendor_id())
//! );
//!
//! // Or, read and update the register in a single step.
//! io.update(BOOT_0, |r| r
//!     .with_const_major_revision::<3>()
//!     .with_const_minor_revision::<10>()
//!     .with_vendor_id(obtain_vendor_id())
//! );
//!
//! // Constant values can also be built using the const setters.
//! const V: BOOT_0 = pin_init::zeroed::<BOOT_0>()
//!     .with_const_major_revision::<3>()
//!     .with_const_minor_revision::<10>();
//! # }
//! ```
//!
//! For more extensive documentation about how to define registers, see the
//! [`register!`](kernel::io::register!) macro.

use core::marker::PhantomData;

use crate::{
    build_assert::build_assert,
    io::IoLoc, //
};

/// Trait implemented by all registers.
pub trait Register: Sized {
    /// Base type for this register.
    type Base: ?Sized;

    /// Backing primitive type of the register.
    type Storage: Into<Self> + From<Self>;

    /// Start offset of the register.
    ///
    /// The interpretation of this offset depends on the type of the register.
    const OFFSET: usize;
}

/// Trait implemented by registers with a fixed offset.
pub trait FixedRegister: Register {}

/// Allows `()` to be used as the `location` parameter of [`Io::write`](super::Io::write) when
/// passing a [`FixedRegister`] value.
impl<Base: ?Sized, T> IoLoc<Base, T> for ()
where
    T: FixedRegister<Base = Base>,
{
    type IoType = T::Storage;

    #[inline(always)]
    fn offset(self) -> usize {
        T::OFFSET
    }
}

/// A [`FixedRegister`] carries its location in its type. Thus `FixedRegister` values can be used
/// as an [`IoLoc`].
impl<Base: ?Sized, T> IoLoc<Base, T> for T
where
    T: FixedRegister<Base = Base>,
{
    type IoType = T::Storage;

    #[inline(always)]
    fn offset(self) -> usize {
        T::OFFSET
    }
}

/// Location of a fixed register.
pub struct FixedRegisterLoc<T: FixedRegister>(PhantomData<T>);

impl<T: FixedRegister> FixedRegisterLoc<T> {
    /// Returns the location of `T`.
    #[inline(always)]
    // We do not implement `Default` so we can be const.
    #[expect(clippy::new_without_default)]
    pub const fn new() -> Self {
        Self(PhantomData)
    }
}

impl<Base: ?Sized, T> IoLoc<Base, T> for FixedRegisterLoc<T>
where
    T: FixedRegister<Base = Base>,
{
    type IoType = T::Storage;

    #[inline(always)]
    fn offset(self) -> usize {
        T::OFFSET
    }
}

/// Trait implemented by arrays of registers.
pub trait RegisterArray: Register {
    /// Number of elements in the registers array.
    const SIZE: usize;
    /// Number of bytes between the start of elements in the registers array.
    const STRIDE: usize;
}

/// Location of an array register.
pub struct RegisterArrayLoc<T: RegisterArray>(usize, PhantomData<T>);

impl<T: RegisterArray> RegisterArrayLoc<T> {
    /// Returns the location of register `T` at position `idx`, with build-time validation.
    #[inline(always)]
    pub fn new(idx: usize) -> Self {
        build_assert!(idx < T::SIZE);

        Self(idx, PhantomData)
    }

    /// Attempts to return the location of register `T` at position `idx`, with runtime validation.
    #[inline(always)]
    pub fn try_new(idx: usize) -> Option<Self> {
        if idx < T::SIZE {
            Some(Self(idx, PhantomData))
        } else {
            None
        }
    }
}

impl<Base: ?Sized, T> IoLoc<Base, T> for RegisterArrayLoc<T>
where
    T: RegisterArray<Base = Base>,
{
    type IoType = T::Storage;

    #[inline(always)]
    fn offset(self) -> usize {
        T::OFFSET + self.0 * T::STRIDE
    }
}

/// Trait providing location builders for [`RegisterArray`]s.
pub trait Array {
    /// Returns the location of the register at position `idx`, with build-time validation.
    #[inline(always)]
    fn at(idx: usize) -> RegisterArrayLoc<Self>
    where
        Self: RegisterArray,
    {
        RegisterArrayLoc::new(idx)
    }

    /// Returns the location of the register at position `idx`, with runtime validation.
    #[inline(always)]
    fn try_at(idx: usize) -> Option<RegisterArrayLoc<Self>>
    where
        Self: RegisterArray,
    {
        RegisterArrayLoc::try_new(idx)
    }
}

/// Trait implemented by items that contain both a register value and the absolute I/O location at
/// which to write it.
///
/// Implementors can be used with [`Io::write_reg`](super::Io::write_reg).
pub trait LocatedRegister<Base: ?Sized> {
    /// Register value to write.
    type Value: Register;
    /// Full location information at which to write the value.
    type Location: IoLoc<Base, Self::Value>;

    /// Consumes `self` and returns a `(location, value)` tuple describing a valid I/O write
    /// operation.
    fn into_io_op(self) -> (Self::Location, Self::Value);
}

impl<Base: ?Sized, T> LocatedRegister<Base> for T
where
    T: FixedRegister<Base = Base>,
{
    type Location = FixedRegisterLoc<Self::Value>;
    type Value = T;

    #[inline(always)]
    fn into_io_op(self) -> (FixedRegisterLoc<T>, T) {
        (FixedRegisterLoc::new(), self)
    }
}

/// Defines a dedicated type for a register, including getter and setter methods for its fields and
/// methods to read and write it from an [`Io`](kernel::io::Io) region.
///
/// This documentation focuses on how to declare registers. See the [module-level
/// documentation](mod@kernel::io::register) for examples of how to access them.
///
/// Registers can either be fixed offset registers or arrays of registers.
///
/// ## Fixed offset registers
///
/// These are the simplest kind of registers. Their location is simply an offset inside the I/O
/// region. For instance:
///
/// ```ignore
/// register! {
///     pub FIXED_REG(u16) @ 0x80 {
///         ...
///     }
/// }
/// ```
///
/// This creates a 16-bit register named `FIXED_REG` located at offset `0x80` of an I/O region.
///
/// These registers' location can be built simply by referencing their name:
///
/// ```no_run
/// use kernel::{
///     io::{
///         register,
///         Io,
///         Region,
///     },
/// };
/// # use kernel::io::Mmio;
///
/// register! {
///     base: Region<0x1000>;
///
///     FIXED_REG(u32) @ 0x100 {
///         15:8 high_byte;
///         7:0  low_byte;
///     }
/// }
///
/// # fn test(io: Mmio<'_, Region<0x1000>>) {
/// let val = io.read(FIXED_REG);
///
/// // Write from an already-existing value.
/// io.write(FIXED_REG, val.with_low_byte(0xff));
///
/// // Create a register value from scratch.
/// let val2 = FIXED_REG::zeroed().with_high_byte(0x80);
///
/// // The location of fixed offset registers is already contained in their type. Thus, the
/// // `location` argument of `Io::write` is technically redundant and can be replaced by `()`.
/// io.write((), val2);
///
/// // Or, the single-argument `Io::write_reg` can be used.
/// io.write_reg(val2);
/// # }
///
/// ```
///
/// It is possible to create an alias of an existing register with new field definitions by using
/// the `=> ALIAS` syntax. This is useful for cases where a register's interpretation depends on
/// the context:
///
/// ```no_run
/// use kernel::io::{
///     register,
///     Region,
/// };
///
/// register! {
///     base: Region<0x1000>;
///
///     /// Scratch register.
///     pub SCRATCH(u32) @ 0x00000200 {
///         31:0 value;
///     }
///
///     /// Boot status of the firmware.
///     pub SCRATCH_BOOT_STATUS(u32) => SCRATCH {
///         0:0 completed;
///     }
/// }
/// ```
///
/// In this example, `SCRATCH_BOOT_STATUS` uses the same I/O address as `SCRATCH`, while providing
/// its own `completed` field.
///
/// ## Arrays of registers
///
/// Some I/O areas contain consecutive registers that share the same field layout. These areas can
/// be defined as an array of identical registers, allowing them to be accessed by index with
/// compile-time or runtime bound checking:
///
/// ```ignore
/// register! {
///     pub REGISTER_ARRAY(u8)[10, stride = 4] @ 0x100 {
///         ...
///     }
/// }
/// ```
///
/// This defines `REGISTER_ARRAY`, an array of 10 byte registers starting at offset `0x100`. Each
/// register is separated from its neighbor by 4 bytes.
///
/// The `stride` parameter is optional; if unspecified, the registers are placed consecutively from
/// each other.
///
/// A location for a register in a register array is built using the [`Array::at`] trait method.
/// All arrays of registers implement [`Array`].
///
/// ```no_run
/// use kernel::{
///     io::{
///         register,
///         register::Array,
///         Io,
///         Region,
///     },
/// };
/// # use kernel::io::Mmio;
/// # fn get_scratch_idx() -> usize {
/// #   0x15
/// # }
///
/// // Array of 64 consecutive registers with the same layout starting at offset `0x80`.
/// register! {
///     base: Region<0x1000>;
///
///     /// Scratch registers.
///     pub SCRATCH(u32)[64] @ 0x00000080 {
///         31:0 value;
///     }
/// }
///
/// # fn test(io: Mmio<'_, Region<0x1000>>)
/// #     -> Result<(), Error>{
/// // Read scratch register 0, i.e. I/O address `0x80`.
/// let scratch_0 = io.read(SCRATCH::at(0)).value();
///
/// // Write scratch register 15, i.e. I/O address `0x80 + (15 * 4)`.
/// io.write(Array::at(15), SCRATCH::from(0xffeeaabb));
///
/// // This is out of bounds and won't build.
/// // let scratch_128 = io.read(SCRATCH::at(128)).value();
///
/// // Runtime-obtained array index.
/// let idx = get_scratch_idx();
/// // Access on a runtime index returns an error if it is out-of-bounds.
/// let some_scratch = io.read(SCRATCH::try_at(idx).ok_or(EINVAL)?).value();
///
/// // Alias to a specific register in an array.
/// // Here `SCRATCH[8]` is used to convey the firmware exit code.
/// register! {
///     base: Region<0x1000>;
///
///     /// Firmware exit status code.
///     pub FIRMWARE_STATUS(u32) => SCRATCH[8] {
///         7:0 status;
///     }
/// }
///
/// let status = io.read(FIRMWARE_STATUS).status();
///
/// // Non-contiguous register arrays can be defined by adding a stride parameter.
/// // Here, each of the 16 registers of the array is separated by 8 bytes, meaning that the
/// // registers of the two declarations below are interleaved.
/// register! {
///     base: Region<0x1000>;
///
///     /// Scratch registers bank 0.
///     pub SCRATCH_INTERLEAVED_0(u32)[16, stride = 8] @ 0x000000c0 {
///         31:0 value;
///     }
///
///     /// Scratch registers bank 1.
///     pub SCRATCH_INTERLEAVED_1(u32)[16, stride = 8] @ 0x000000c4 {
///         31:0 value;
///     }
/// }
/// # Ok(())
/// # }
/// ```
#[macro_export]
macro_rules! register {
    // Entry point for the macro, allowing multiple registers to be defined in one call.
    // It matches all possible register declaration patterns to dispatch them to corresponding
    // `@reg` rule that defines a single register.
    (
        base: $reg_base:ty;
        $(
            $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
                $([ $size:expr $(, stride = $stride:expr)? ])?
                $(@ $offset:literal)?
                $(=> $alias:ident $(+ $alias_offset:ident)? $([$alias_idx:expr])? )?
            { $($fields:tt)* }
        )*
    ) => {
        $(
        $crate::register!(
            @reg [$reg_base] $(#[$attr])* $vis $name ($storage) $([$size $(, stride = $stride)?])?
                $(@ $offset)?
                $(=> $alias $(+ $alias_offset)? $([$alias_idx])? )?
            { $($fields)* }
        );
        )*
    };

    // All the rules below are private helpers.

    // Creates a register at a fixed offset of the MMIO space.
    (
        @reg [$reg_base:ty] $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) @ $offset:literal
            { $($fields:tt)* }
    ) => {
        $crate::register!(@bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* });
        $crate::register!(@io_base [$reg_base] $name($storage) @ $offset);
        $crate::register!(@io_fixed $(#[$attr])* $vis $name($storage));
    };

    // Creates an alias register of fixed offset register `alias` with its own fields.
    (
        @reg [$reg_base:ty] $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) => $alias:ident
            { $($fields:tt)* }
    ) => {
        $crate::register!(@bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* });
        $crate::register!(
            @io_base [$reg_base] $name($storage) @
            <$alias as $crate::io::register::Register>::OFFSET
        );
        $crate::register!(@io_fixed $(#[$attr])* $vis $name($storage));
    };

    // Creates an array of registers at a fixed offset of the MMIO space.
    (
        @reg [$reg_base:ty] $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
            [ $size:expr, stride = $stride:expr ] @ $offset:literal { $($fields:tt)* }
    ) => {
        $crate::build_assert::static_assert!(::core::mem::size_of::<$storage>() <= $stride);

        $crate::register!(@bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* });
        $crate::register!(@io_base [$reg_base] $name($storage) @ $offset);
        $crate::register!(@io_array $vis $name($storage) [ $size, stride = $stride ]);
    };

    // Shortcut for contiguous array of registers (stride == size of element).
    (
        @reg [$reg_base:ty]
            $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) [ $size:expr ] @ $offset:literal
            { $($fields:tt)* }
    ) => {
        $crate::register!(@reg [$reg_base]
            $(#[$attr])* $vis $name($storage) [ $size, stride = ::core::mem::size_of::<$storage>() ]
                @ $offset { $($fields)* }
        );
    };

    // Creates an alias of register `idx` of array of registers `alias` with its own fields.
    (
        @reg [$reg_base:ty]
            $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) => $alias:ident [ $idx:expr ]
            { $($fields:tt)* }
    ) => {
        $crate::build_assert::static_assert!(
            $idx < <$alias as $crate::io::register::RegisterArray>::SIZE
        );

        $crate::register!(@bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* });
        $crate::register!(
            @io_base [$reg_base] $name($storage) @
            <$alias as $crate::io::register::Register>::OFFSET
                + $idx * <$alias as $crate::io::register::RegisterArray>::STRIDE
        );
        $crate::register!(@io_fixed $(#[$attr])* $vis $name($storage));
    };

    // Generates the bitfield for the register.
    //
    // `#[allow(non_camel_case_types)]` is added since register names typically use
    // `SCREAMING_CASE`.
    (
        @bitfield $(#[$attr:meta])* $vis:vis struct $name:ident($storage:ty) { $($fields:tt)* }
    ) => {
        $crate::bitfield!(
            #[allow(non_camel_case_types)]
            $(#[$attr])* $vis struct $name($storage) { $($fields)* }
        );
    };

    // Implementations shared by all registers types.
    (@io_base [$reg_base:ty] $name:ident($storage:ty) @ $offset:expr) => {
        impl $crate::io::register::Register for $name {
            type Base = $reg_base;
            type Storage = $storage;

            const OFFSET: usize = $offset;
        }
    };

    // Implementations of fixed registers.
    (@io_fixed $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)) => {
        impl $crate::io::register::FixedRegister for $name {}

        $(#[$attr])*
        $vis const $name: $crate::io::register::FixedRegisterLoc<$name> =
            $crate::io::register::FixedRegisterLoc::<$name>::new();
    };

    // Implementations of register arrays.
    (@io_array $vis:vis $name:ident ($storage:ty) [ $size:expr, stride = $stride:expr ]) => {
        impl $crate::io::register::Array for $name {}

        impl $crate::io::register::RegisterArray for $name {
            const SIZE: usize = $size;
            const STRIDE: usize = $stride;
        }
    };
}
