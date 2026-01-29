// SPDX-License-Identifier: GPL-2.0

//! A macro to define register layout and accessors.
//!
//! A single register typically includes several fields, which are accessed through a combination
//! of bit-shift and mask operations that introduce a class of potential mistakes, notably because
//! not all possible field values are necessarily valid.
//!
//! The [`register!`] macro in this module provides an intuitive and readable syntax for defining a
//! dedicated type for each register. Each such type comes with its own field accessors that can
//! return an error if a field's value is invalid.
//!
//! [`register!`]: kernel::register!

use crate::io::{
    IoCapable,
    IoKnownSize, //
};

/// Trait providing a base address to be added to the offset of a relative register to obtain
/// its actual offset.
///
/// The `T` generic argument is used to distinguish which base to use, in case a type provides
/// several bases. It is given to the `register!` macro to restrict the use of the register to
/// implementors of this particular variant.
pub trait RegisterBase<T> {
    /// Base address to which register offsets are added.
    const BASE: usize;
}

/// Trait providing I/O read/write operations for register storage types.
///
/// This trait is implemented for all integer types on which I/O can be performed, allowing the
/// `register!` macro to generate appropriate I/O accessor methods based on the register's storage
/// type.
#[doc(hidden)]
pub trait RegisterIo: Sized {
    /// Read a value from the given offset in the I/O region.
    fn read<I>(io: &I, offset: usize) -> Self
    where
        I: IoKnownSize + IoCapable<Self>;

    /// Write a value to the given offset in the I/O region.
    fn write<I>(self, io: &I, offset: usize)
    where
        I: IoKnownSize + IoCapable<Self>;
}

impl RegisterIo for u8 {
    #[inline(always)]
    fn read<I>(io: &I, offset: usize) -> Self
    where
        I: IoKnownSize + IoCapable<Self>,
    {
        io.read8(offset)
    }

    #[inline(always)]
    fn write<I>(self, io: &I, offset: usize)
    where
        I: IoKnownSize + IoCapable<Self>,
    {
        io.write8(self, offset)
    }
}

impl RegisterIo for u16 {
    #[inline(always)]
    fn read<I>(io: &I, offset: usize) -> Self
    where
        I: IoKnownSize + IoCapable<Self>,
    {
        io.read16(offset)
    }

    #[inline(always)]
    fn write<I>(self, io: &I, offset: usize)
    where
        I: IoKnownSize + IoCapable<Self>,
    {
        io.write16(self, offset)
    }
}

impl RegisterIo for u32 {
    #[inline(always)]
    fn read<I>(io: &I, offset: usize) -> Self
    where
        I: IoKnownSize + IoCapable<Self>,
    {
        io.read32(offset)
    }

    #[inline(always)]
    fn write<I>(self, io: &I, offset: usize)
    where
        I: IoKnownSize + IoCapable<Self>,
    {
        io.write32(self, offset)
    }
}

impl RegisterIo for u64 {
    #[inline(always)]
    fn read<I>(io: &I, offset: usize) -> Self
    where
        I: IoKnownSize + IoCapable<Self>,
    {
        io.read64(offset)
    }

    #[inline(always)]
    fn write<I>(self, io: &I, offset: usize)
    where
        I: IoKnownSize + IoCapable<Self>,
    {
        io.write64(self, offset)
    }
}

/// Defines a dedicated type for a register, including getter and setter methods for its fields and
/// methods to read and write it from an [`Io`] region.
///
/// Example:
///
/// ```
/// use kernel::register;
///
/// register! {
///     /// Basic information about the chip.
///     pub BOOT_0(u32) @ 0x00000100 {
///         /// Vendor ID.
///         15:8 vendor_id;
///         /// Major revision of the chip.
///         7:4 major_revision;
///         /// Minor revision of the chip.
///         3:0 minor_revision;
///     }
/// }
/// ```
///
/// This defines a `BOOT_0` type which can be read from or written to offset `0x100` of an `Io`
/// region. For instance, `minor_revision` consists of the 4 least significant bits of the
/// register.
///
/// Fields are instances of [`Bounded`](kernel::num::Bounded) and can be read by calling their
/// getter method, which is named after them. They also have setter methods prefixed with `with_`
/// for runtime values and `with_const_` for constant values. All setters return the updated
/// register value.
///
/// ```no_run
/// use kernel::register;
/// use kernel::num::Bounded;
///
/// # register! {
/// #     pub BOOT_0(u32) @ 0x00000100 {
/// #         15:8 vendor_id;
/// #         7:4 major_revision;
/// #         3:0 minor_revision;
/// #     }
/// # }
/// # fn test<T: kernel::io::IoKnownSize + kernel::io::IoCapable<u32>>(bar: &T) {
/// # fn obtain_vendor_id() -> u8 { 0xff }
/// // Read from the register's defined offset (0x100).
/// let boot0 = BOOT_0::read(&bar);
/// pr_info!("chip revision: {}.{}", boot0.major_revision().get(), boot0.minor_revision().get());
///
/// // Update some fields and write the new value back.
/// boot0
///     // Constant values.
///     .with_const_major_revision::<3>()
///     .with_const_minor_revision::<10>()
///     // Run-time value.
///     .with_vendor_id(obtain_vendor_id())
///     .write(&bar);
///
/// // Or, just read and update the register in a single step.
/// BOOT_0::update(&bar, |r| r
///     .with_const_major_revision::<3>()
///     .with_const_minor_revision::<10>()
///     .with_vendor_id(obtain_vendor_id())
/// );
///
/// // Constant values can also be built using the const setters.
/// const V: BOOT_0 = BOOT_0::zeroed()
///     .with_const_major_revision::<3>()
///     .with_const_minor_revision::<10>();
/// # }
/// ```
///
/// Fields can also be transparently converted from/to an arbitrary type by using the bitfield `=>`
/// and `?=>` syntaxes.
///
/// If present, doccomments above register or fields definitions are added to the relevant item
/// they document (the register type itself, or the field's setter and getter methods).
///
/// Note that multiple registers can be defined in a single `register!` invocation. This can be
/// useful to group related registers together.
///
/// ```
/// use kernel::register;
///
/// register! {
///     pub BOOT_0(u8) @ 0x00000100 {
///         7:4 major_revision;
///         3:0 minor_revision;
///     }
///
///     pub BOOT_1(u8) @ 0x00000101 {
///         7:5 num_threads;
///         4:0 num_cores;
///     }
/// };
/// ```
///
/// It is possible to create an alias of an existing register with new field definitions by using
/// the `=> ALIAS` syntax. This is useful for cases where a register's interpretation depends on
/// the context:
///
/// ```
/// use kernel::register;
///
/// register! {
///     /// Scratch register.
///     pub SCRATCH(u32) @ 0x00000200 {
///         /// Raw value.
///         31:0 value;
///     }
///
///     /// Boot status of the firmware.
///     pub SCRATCH_BOOT_STATUS(u32) => SCRATCH {
///         /// Whether the firmware has completed booting.
///         0:0 completed;
///     }
/// }
/// ```
///
/// In this example, `SCRATCH_BOOT_STATUS` uses the same I/O address as `SCRATCH`, while also
/// providing its own `completed` field.
///
/// ## Relative registers
///
/// A register can be defined as being accessible from a fixed offset of a provided base. For
/// instance, imagine the following I/O space:
///
/// ```text
///           +-----------------------------+
///           |             ...             |
///           |                             |
///  0x100--->+------------CPU0-------------+
///           |                             |
///  0x110--->+-----------------------------+
///           |           CPU_CTL           |
///           +-----------------------------+
///           |             ...             |
///           |                             |
///           |                             |
///  0x200--->+------------CPU1-------------+
///           |                             |
///  0x210--->+-----------------------------+
///           |           CPU_CTL           |
///           +-----------------------------+
///           |             ...             |
///           +-----------------------------+
/// ```
///
/// `CPU0` and `CPU1` both have a `CPU_CTL` register that starts at offset `0x10` of their I/O
/// space segment. Since both instances of `CPU_CTL` share the same layout, we don't want to define
/// them twice and would prefer a way to select which one to use from a single definition
///
/// This can be done using the `Base + Offset` syntax when specifying the register's address.
///
/// `Base` is an arbitrary type (typically a ZST) to be used as a generic parameter of the
/// [`RegisterBase`] trait to provide the base as a constant, i.e. each type providing a base for
/// this register needs to implement `RegisterBase<Base>`. Here is the above example translated
/// into code:
///
/// ```no_run
/// use kernel::register;
/// use kernel::io::register::RegisterBase;
///
/// // Type used to identify the base.
/// pub struct CpuCtlBase;
///
/// // ZST describing `CPU0`.
/// struct Cpu0;
/// impl RegisterBase<CpuCtlBase> for Cpu0 {
///     const BASE: usize = 0x100;
/// }
/// // Singleton of `CPU0` used to identify it.
/// const CPU0: Cpu0 = Cpu0;
///
/// // ZST describing `CPU1`.
/// struct Cpu1;
/// impl RegisterBase<CpuCtlBase> for Cpu1 {
///     const BASE: usize = 0x200;
/// }
/// // Singleton of `CPU1` used to identify it.
/// const CPU1: Cpu1 = Cpu1;
///
/// # fn test<T: kernel::io::IoKnownSize + kernel::io::IoCapable<u32>>(bar: &T) {
/// // This makes `CPU_CTL` accessible from all implementors of `RegisterBase<CpuCtlBase>`.
/// register! {
///     /// CPU core control.
///     pub CPU_CTL(u32) @ CpuCtlBase + 0x10 {
///         /// Start the CPU core.
///         0:0 start;
///     }
/// }
///
/// // The `read`, `write` and `update` methods of relative registers take an extra `base` argument
/// // that is used to resolve its final address by adding its `BASE` to the offset of the
/// // register.
///
/// // Start `CPU0`.
/// CPU_CTL::update(&bar, &CPU0, |r| r.with_start(true));
///
/// // Start `CPU1`.
/// CPU_CTL::update(&bar, &CPU1, |r| r.with_start(true));
///
/// // Aliases can also be defined for relative register.
/// register! {
///     /// Alias to CPU core control.
///     pub CPU_CTL_ALIAS(u32) => CpuCtlBase + CPU_CTL {
///         /// Start the aliased CPU core.
///         1:1 alias_start;
///     }
/// }
///
/// // Start the aliased `CPU0`.
/// CPU_CTL_ALIAS::update(&bar, &CPU0, |r| r.with_alias_start(true));
/// # }
/// ```
///
/// ## Arrays of registers
///
/// Some I/O areas contain consecutive registers that can be interpreted in the same way. These
/// areas can be defined as an array of identical registers, allowing them to be accessed by index
/// with compile-time or runtime bound checking. Simply specify their size inside `[` and `]`
/// brackets, and add an `idx` parameter to their `read`, `write` and `update` methods:
///
/// ```no_run
/// use kernel::register;
///
/// # fn test<T: kernel::io::IoKnownSize + kernel::io::IoCapable<u32>>(bar: &T)
/// #     -> Result<(), Error>{
/// # fn get_scratch_idx() -> usize {
/// #   0x15
/// # }
/// // Array of 64 consecutive registers with the same layout starting at offset `0x80`.
/// register! {
///     /// Scratch registers.
///     pub SCRATCH(u32)[64] @ 0x00000080 {
///         31:0 value;
///     }
/// }
///
/// // Read scratch register 0, i.e. I/O address `0x80`.
/// let scratch_0 = SCRATCH::read(&bar, 0).value();
/// // Read scratch register 15, i.e. I/O address `0x80 + (15 * 4)`.
/// let scratch_15 = SCRATCH::read(&bar, 15).value();
///
/// // This is out of bounds and won't build.
/// // let scratch_128 = SCRATCH::read(&bar, 128).value();
///
/// // Runtime-obtained array index.
/// let scratch_idx = get_scratch_idx();
/// // Access on a runtime index returns an error if it is out-of-bounds.
/// let some_scratch = SCRATCH::try_read(&bar, scratch_idx)?.value();
///
/// // Alias to a particular register in an array.
/// // Here `SCRATCH[8]` is used to convey the firmware exit code.
/// register! {
///     /// Firmware exit status code.
///     pub FIRMWARE_STATUS(u32) => SCRATCH[8] {
///         7:0 status;
///     }
/// }
/// let status = FIRMWARE_STATUS::read(&bar).status();
///
/// // Non-contiguous register arrays can be defined by adding a stride parameter.
/// // Here, each of the 16 registers of the array are separated by 8 bytes, meaning that the
/// // registers of the two declarations below are interleaved.
/// register! {
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
///
/// ## Relative arrays of registers
///
/// Combining the two features described in the sections above, arrays of registers accessible from
/// a base can also be defined:
///
/// ```no_run
/// use kernel::register;
/// use kernel::io::register::RegisterBase;
///
/// # fn test<T: kernel::io::IoKnownSize + kernel::io::IoCapable<u32>>(bar: &T)
/// #     -> Result<(), Error>{
/// # fn get_scratch_idx() -> usize {
/// #   0x15
/// # }
/// // Type used as parameter of `RegisterBase` to specify the base.
/// pub struct CpuCtlBase;
///
/// // ZST describing `CPU0`.
/// struct Cpu0;
/// impl RegisterBase<CpuCtlBase> for Cpu0 {
///     const BASE: usize = 0x100;
/// }
/// // Singleton of `CPU0` used to identify it.
/// const CPU0: Cpu0 = Cpu0;
///
/// // ZST describing `CPU1`.
/// struct Cpu1;
/// impl RegisterBase<CpuCtlBase> for Cpu1 {
///     const BASE: usize = 0x200;
/// }
/// // Singleton of `CPU1` used to identify it.
/// const CPU1: Cpu1 = Cpu1;
///
/// // 64 per-cpu scratch registers, arranged as a contiguous array.
/// register! {
///     /// Per-CPU scratch registers.
///     pub CPU_SCRATCH(u32)[64] @ CpuCtlBase + 0x00000080 {
///         31:0 value;
///     }
/// }
///
/// let cpu0_scratch_0 = CPU_SCRATCH::read(&bar, &Cpu0, 0).value();
/// let cpu1_scratch_15 = CPU_SCRATCH::read(&bar, &Cpu1, 15).value();
///
/// // This won't build.
/// // let cpu0_scratch_128 = CPU_SCRATCH::read(&bar, &Cpu0, 128).value();
///
/// // Runtime-obtained array index.
/// let scratch_idx = get_scratch_idx();
/// // Access on a runtime value returns an error if it is out-of-bounds.
/// let cpu0_some_scratch = CPU_SCRATCH::try_read(&bar, &Cpu0, scratch_idx)?.value();
///
/// // `SCRATCH[8]` is used to convey the firmware exit code.
/// register! {
///     /// Per-CPU firmware exit status code.
///     pub CPU_FIRMWARE_STATUS(u32) => CpuCtlBase + CPU_SCRATCH[8] {
///         7:0 status;
///     }
/// }
/// let cpu0_status = CPU_FIRMWARE_STATUS::read(&bar, &Cpu0).status();
///
/// // Non-contiguous register arrays can be defined by adding a stride parameter.
/// // Here, each of the 16 registers of the array are separated by 8 bytes, meaning that the
/// // registers of the two declarations below are interleaved.
/// register! {
///     /// Scratch registers bank 0.
///     pub CPU_SCRATCH_INTERLEAVED_0(u32)[16, stride = 8] @ CpuCtlBase + 0x00000d00 {
///         31:0 value;
///     }
///
///     /// Scratch registers bank 1.
///     pub CPU_SCRATCH_INTERLEAVED_1(u32)[16, stride = 8] @ CpuCtlBase + 0x00000d04 {
///         31:0 value;
///     }
/// }
/// # Ok(())
/// # }
/// ```
/// [`Io`]: kernel::io::Io
#[macro_export]
macro_rules! register {
    // Entry point for the macro, allowing multiple registers to be defined in one call.
    // It matches all possible register declaration patterns to dispatch them to corresponding
    // `@reg` rule that defines a single register.
    (
        $(
            $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
                $([ $size:expr $(, stride = $stride:expr)? ])?
                $(@ $($base:ident +)? $offset:literal)?
                $(=> $alias:ident $(+ $alias_offset:ident)? $([$alias_idx:expr])? )?
            { $($fields:tt)* }
        )*
    ) => {
        $(
        $crate::register!(
            @reg $(#[$attr])* $vis $name ($storage) $([$size $(, stride = $stride)?])?
                $(@ $($base +)? $offset)?
                $(=> $alias $(+ $alias_offset)? $([$alias_idx])? )?
            { $($fields)* }
        );
        )*
    };

    // All the rules below are private helpers.

    // Creates a register at a fixed offset of the MMIO space.
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) @ $offset:literal
            { $($fields:tt)* }
    ) => {
        $crate::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* }
        );
        $crate::register!(@io_fixed $name($storage) @ $offset);
    };

    // Creates an alias register of fixed offset register `alias` with its own fields.
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) => $alias:ident
            { $($fields:tt)* }
    ) => {
        $crate::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* }
        );
        $crate::register!(@io_fixed $name($storage) @ $alias::OFFSET);
    };

    // Creates a register at a relative offset from a base address provider.
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) @ $base:ident + $offset:literal
            { $($fields:tt)* }
    ) => {
        $crate::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* }
        );
        $crate::register!(@io_relative $name($storage) @ $base + $offset );
    };

    // Creates an alias register of relative offset register `alias` with its own fields.
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) => $base:ident + $alias:ident
            { $($fields:tt)* }
    ) => {
        $crate::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* }
        );
        $crate::register!(@io_relative $name($storage) @ $base + $alias::OFFSET );
    };

    // Creates an array of registers at a fixed offset of the MMIO space.
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
            [ $size:expr, stride = $stride:expr ] @ $offset:literal { $($fields:tt)* }
    ) => {
        static_assert!(::core::mem::size_of::<$storage>() <= $stride);

        $crate::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* }
        );
        $crate::register!(@io_array $name($storage) [ $size, stride = $stride ] @ $offset);
    };

    // Shortcut for contiguous array of registers (stride == size of element).
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) [ $size:expr ] @ $offset:literal
            { $($fields:tt)* }
    ) => {
        $crate::register!(
            $(#[$attr])* $vis $name($storage) [ $size, stride = ::core::mem::size_of::<$storage>() ]
                @ $offset { $($fields)* }
        );
    };

    // Creates an alias of register `idx` of array of registers `alias` with its own fields.
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) => $alias:ident [ $idx:expr ]
            { $($fields:tt)* }
    ) => {
        static_assert!($idx < $alias::SIZE);

        $crate::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* }
        );
        $crate::register!(@io_fixed $name($storage) @ $alias::OFFSET + $idx * $alias::STRIDE);
    };

    // Creates an array of registers at a relative offset from a base address provider.
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
            [ $size:expr, stride = $stride:expr ]
            @ $base:ident + $offset:literal { $($fields:tt)* }
    ) => {
        static_assert!(::core::mem::size_of::<$storage>() <= $stride);

        $crate::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* }
        );
        $crate::register!(
            @io_relative_array $name($storage) [ $size, stride = $stride ] @ $base + $offset
        );
    };

    // Shortcut for contiguous array of relative registers (stride == size of element).
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) [ $size:expr ]
            @ $base:ident + $offset:literal { $($fields:tt)* }
    ) => {
        $crate::register!(
            $(#[$attr])* $vis $name($storage) [ $size, stride = ::core::mem::size_of::<$storage>() ]
                @ $base + $offset { $($fields)* }
        );
    };

    // Creates an alias of register `idx` of relative array of registers `alias` with its own
    // fields.
    (
        @reg $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
            => $base:ident + $alias:ident [ $idx:expr ] { $($fields:tt)* }
    ) => {
        static_assert!($idx < $alias::SIZE);

        $crate::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) { $($fields)* }
        );
        $crate::register!(
            @io_relative $name($storage) @ $base + $alias::OFFSET + $idx * $alias::STRIDE
        );
    };

    // Generates the bitfield for the register.
    //
    // `#[allow(non_camel_case_types)]` is added since register names typically use SCREAMING_CASE.
    (
        @bitfield $(#[$attr:meta])* $vis:vis struct $name:ident($storage:ty) { $($fields:tt)* }
    ) => {
        $crate::register!(@bitfield_core
            #[allow(non_camel_case_types)]
            $(#[$attr])* $vis $name $storage
        );
        $crate::register!(@bitfield_fields $vis $name $storage { $($fields)* });
    };

    // Generates the IO accessors for a fixed offset register.
    (@io_fixed $name:ident ($storage:ty) @ $offset:expr) => {
        #[allow(dead_code)]
        impl $name {
            /// Absolute offset of the register.
            pub const OFFSET: usize = $offset;

            /// Read the register from its address in `io`.
            #[inline(always)]
            pub fn read<T, I>(io: &T) -> Self where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
            {
                let io = io.deref();

                Self(<$storage as $crate::io::register::RegisterIo>::read(io, Self::OFFSET))
            }

            /// Write the value contained in `self` to the register address in `io`.
            #[inline(always)]
            pub fn write<T, I>(self, io: &T) where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
            {
                let io = io.deref();

                <$storage as $crate::io::register::RegisterIo>::write(self.0, io, Self::OFFSET)
            }

            /// Read the register from its address in `io` and run `f` on its value to obtain a new
            /// value to write back.
            ///
            /// Note that this operation is not atomic. In concurrent contexts, external
            /// synchronization may be required to prevent race conditions.
            #[inline(always)]
            pub fn update<T, I, F>(
                io: &T,
                f: F,
            ) where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                F: ::core::ops::FnOnce(Self) -> Self,
            {
                let reg = f(Self::read(io));
                reg.write(io);
            }
        }
    };

    // Generates the IO accessors for a relative offset register.
    (@io_relative $name:ident ($storage:ty) @ $base:ident + $offset:expr ) => {
        #[allow(dead_code)]
        impl $name {
            /// Relative offset of the register.
            pub const OFFSET: usize = $offset;

            /// Read the register from `io`, using the base address provided by `base` and adding
            /// the register's offset to it.
            #[inline(always)]
            pub fn read<T, I, B>(
                io: &T,
                #[allow(unused_variables)]
                base: &B,
            ) -> Self where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                let io = io.deref();
                let offset = <B as $crate::io::register::RegisterBase<$base>>::BASE + Self::OFFSET;

                Self(<$storage as $crate::io::register::RegisterIo>::read(io, offset))
            }

            /// Write the value contained in `self` to `io`, using the base address provided by
            /// `base` and adding the register's offset to it.
            #[inline(always)]
            pub fn write<T, I, B>(
                self,
                io: &T,
                #[allow(unused_variables)]
                base: &B,
            ) where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                let io = io.deref();
                let offset = <B as $crate::io::register::RegisterBase<$base>>::BASE + Self::OFFSET;

                <$storage as $crate::io::register::RegisterIo>::write(self.0, io, offset)
            }

            /// Read the register from `io`, using the base address provided by `base` and adding
            /// the register's offset to it, then run `f` on its value to obtain a new value to
            /// write back.
            ///
            /// Note that this operation is not atomic. In concurrent contexts, external
            /// synchronization may be required to prevent race conditions.
            #[inline(always)]
            pub fn update<T, I, B, F>(
                io: &T,
                base: &B,
                f: F,
            ) where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                B: $crate::io::register::RegisterBase<$base>,
                F: ::core::ops::FnOnce(Self) -> Self,
            {
                let reg = f(Self::read(io, base));
                reg.write(io, base);
            }
        }
    };

    // Generates the IO accessors for an array of registers.
    (@io_array $name:ident ($storage:ty) [ $size:expr, stride = $stride:expr ]
        @ $offset:literal) => {
        #[allow(dead_code)]
        impl $name {
            /// Absolute offset of the register array.
            pub const OFFSET: usize = $offset;
            /// Number of elements in the array of registers.
            pub const SIZE: usize = $size;
            /// Number of bytes separating each element of the array of registers.
            pub const STRIDE: usize = $stride;

            /// Read the array register at index `idx` from its address in `io`.
            #[inline(always)]
            pub fn read<T, I>(
                io: &T,
                idx: usize,
            ) -> Self where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
            {
                build_assert!(idx < Self::SIZE);

                let io = io.deref();
                let offset = Self::OFFSET + (idx * Self::STRIDE);

                Self(<$storage as $crate::io::register::RegisterIo>::read(io, offset))
            }

            /// Write the value contained in `self` to the array register with index `idx` in `io`.
            #[inline(always)]
            pub fn write<T, I>(
                self,
                io: &T,
                idx: usize
            ) where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
            {
                build_assert!(idx < Self::SIZE);

                let io = io.deref();
                let offset = Self::OFFSET + (idx * Self::STRIDE);

                <$storage as $crate::io::register::RegisterIo>::write(self.0, io, offset)
            }

            /// Read the array register at index `idx` in `io` and run `f` on its value to obtain a
            /// new value to write back.
            ///
            /// Note that this operation is not atomic. In concurrent contexts, external
            /// synchronization may be required to prevent race conditions.
            #[inline(always)]
            pub fn update<T, I, F>(
                io: &T,
                idx: usize,
                f: F,
            ) where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                F: ::core::ops::FnOnce(Self) -> Self,
            {
                let reg = f(Self::read(io, idx));
                reg.write(io, idx);
            }

            /// Read the array register at index `idx` from its address in `io`.
            ///
            /// The validity of `idx` is checked at run-time, and `EINVAL` is returned if the
            /// access was out-of-bounds.
            #[inline(always)]
            pub fn try_read<T, I>(
                io: &T,
                idx: usize,
            ) -> ::kernel::error::Result<Self> where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
            {
                if idx < Self::SIZE {
                    Ok(Self::read(io, idx))
                } else {
                    Err(::kernel::error::code::EINVAL)
                }
            }

            /// Write the value contained in `self` to the array register with index `idx` in `io`.
            ///
            /// The validity of `idx` is checked at run-time, and `EINVAL` is returned if the
            /// access was out-of-bounds.
            #[inline(always)]
            pub fn try_write<T, I>(
                self,
                io: &T,
                idx: usize,
            ) -> ::kernel::error::Result where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
            {
                if idx < Self::SIZE {
                    Ok(self.write(io, idx))
                } else {
                    Err(::kernel::error::code::EINVAL)
                }
            }

            /// Read the array register at index `idx` in `io` and run `f` on its value to obtain a
            /// new value to write back.
            ///
            /// The validity of `idx` is checked at run-time, and `EINVAL` is returned if the
            /// access was out-of-bounds.
            ///
            /// Note that this operation is not atomic. In concurrent contexts, external
            /// synchronization may be required to prevent race conditions.
            #[inline(always)]
            pub fn try_update<T, I, F>(
                io: &T,
                idx: usize,
                f: F,
            ) -> ::kernel::error::Result where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                F: ::core::ops::FnOnce(Self) -> Self,
            {
                if idx < Self::SIZE {
                    Ok(Self::update(io, idx, f))
                } else {
                    Err(::kernel::error::code::EINVAL)
                }
            }
        }
    };

    // Generates the IO accessors for an array of relative registers.
    (
        @io_relative_array $name:ident ($storage:ty) [ $size:expr, stride = $stride:expr ]
            @ $base:ident + $offset:literal
    ) => {
        #[allow(dead_code)]
        impl $name {
            /// Relative offset of the register array.
            pub const OFFSET: usize = $offset;
            /// Number of elements in the array of registers.
            pub const SIZE: usize = $size;
            /// Number of bytes separating each element of the array of registers.
            pub const STRIDE: usize = $stride;

            /// Read the array register at index `idx` from `io`, using the base address provided
            /// by `base` and adding the register's offset to it.
            #[inline(always)]
            pub fn read<T, I, B>(
                io: &T,
                #[allow(unused_variables)]
                base: &B,
                idx: usize,
            ) -> Self where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                build_assert!(idx < Self::SIZE);

                let io = io.deref();
                let offset = <B as $crate::io::register::RegisterBase<$base>>::BASE +
                    Self::OFFSET + (idx * Self::STRIDE);

                Self(<$storage as $crate::io::register::RegisterIo>::read(io, offset))
            }

            /// Write the value contained in `self` to `io`, using the base address provided by
            /// `base` and adding the offset of array register `idx` to it.
            #[inline(always)]
            pub fn write<T, I, B>(
                self,
                io: &T,
                #[allow(unused_variables)]
                base: &B,
                idx: usize
            ) where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                build_assert!(idx < Self::SIZE);

                let io = io.deref();
                let offset = <B as $crate::io::register::RegisterBase<$base>>::BASE +
                    Self::OFFSET + (idx * Self::STRIDE);

                <$storage as $crate::io::register::RegisterIo>::write(self.0, io, offset)
            }

            /// Read the array register at index `idx` from `io`, using the base address provided
            /// by `base` and adding the register's offset to it, then run `f` on its value to
            /// obtain a new value to write back.
            ///
            /// Note that this operation is not atomic. In concurrent contexts, external
            /// synchronization may be required to prevent race conditions.
            #[inline(always)]
            pub fn update<T, I, B, F>(
                io: &T,
                base: &B,
                idx: usize,
                f: F,
            ) where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                B: $crate::io::register::RegisterBase<$base>,
                F: ::core::ops::FnOnce(Self) -> Self,
            {
                let reg = f(Self::read(io, base, idx));
                reg.write(io, base, idx);
            }

            /// Read the array register at index `idx` from `io`, using the base address provided
            /// by `base` and adding the register's offset to it.
            ///
            /// The validity of `idx` is checked at run-time, and `EINVAL` is returned if the
            /// access was out-of-bounds.
            #[inline(always)]
            pub fn try_read<T, I, B>(
                io: &T,
                base: &B,
                idx: usize,
            ) -> ::kernel::error::Result<Self> where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                if idx < Self::SIZE {
                    Ok(Self::read(io, base, idx))
                } else {
                    Err(::kernel::error::code::EINVAL)
                }
            }

            /// Write the value contained in `self` to `io`, using the base address provided by
            /// `base` and adding the offset of array register `idx` to it.
            ///
            /// The validity of `idx` is checked at run-time, and `EINVAL` is returned if the
            /// access was out-of-bounds.
            #[inline(always)]
            pub fn try_write<T, I, B>(
                self,
                io: &T,
                base: &B,
                idx: usize,
            ) -> ::kernel::error::Result where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                if idx < Self::SIZE {
                    Ok(self.write(io, base, idx))
                } else {
                    Err(::kernel::error::code::EINVAL)
                }
            }

            /// Read the array register at index `idx` from `io`, using the base address provided
            /// by `base` and adding the register's offset to it, then run `f` on its value to
            /// obtain a new value to write back.
            ///
            /// The validity of `idx` is checked at run-time, and `EINVAL` is returned if the
            /// access was out-of-bounds.
            ///
            /// Note that this operation is not atomic. In concurrent contexts, external
            /// synchronization may be required to prevent race conditions.
            #[inline(always)]
            pub fn try_update<T, I, B, F>(
                io: &T,
                base: &B,
                idx: usize,
                f: F,
            ) -> ::kernel::error::Result where
                T: ::core::ops::Deref<Target = I>,
                I: ::kernel::io::IoKnownSize + ::kernel::io::IoCapable<$storage>,
                B: $crate::io::register::RegisterBase<$base>,
                F: ::core::ops::FnOnce(Self) -> Self,
            {
                if idx < Self::SIZE {
                    Ok(Self::update(io, base, idx, f))
                } else {
                    Err(::kernel::error::code::EINVAL)
                }
            }
        }
    };

    // Defines the wrapper `$name` type and its conversions from/to the storage type.
    (@bitfield_core $(#[$attr:meta])* $vis:vis $name:ident $storage:ty) => {
        $(#[$attr])*
        #[repr(transparent)]
        #[derive(Clone, Copy, PartialEq, Eq)]
        $vis struct $name($storage);

        #[allow(dead_code)]
        impl $name {
            /// Creates a bitfield from a raw value.
            $vis const fn from_raw(value: $storage) -> Self {
                Self(value)
            }

            /// Creates a zeroed bitfield value.
            ///
            /// This is a const alternative to the `Zeroable::zeroed()` trait method.
            $vis const fn zeroed() -> Self {
                Self(0)
            }

            /// Returns the raw value of this bitfield.
            ///
            /// This is similar to the [`From`] implementation, but is shorter to invoke in
            /// most cases.
            $vis const fn as_raw(self) -> $storage {
                self.0
            }
        }

        // SAFETY: `$storage` is `Zeroable` and `$name` is transparent.
        unsafe impl ::pin_init::Zeroable for $name {}

        impl ::core::convert::From<$name> for $storage {
            fn from(val: $name) -> $storage {
                val.as_raw()
            }
        }

        impl ::core::convert::From<$storage> for $name {
            fn from(val: $storage) -> $name {
                Self::from_raw(val)
            }
        }
    };

    // Definitions requiring knowledge of individual fields: private and public field accessors,
    // and `Debug` implementation.
    (@bitfield_fields $vis:vis $name:ident $storage:ty {
        $($(#[doc = $doc:expr])* $hi:literal:$lo:literal $field:ident
            $(?=> $try_into_type:ty)?
            $(=> $into_type:ty)?
        ;
        )*
    }
    ) => {
        #[allow(dead_code)]
        impl $name {
        $(
        $crate::register!(@private_field_accessors $vis $name $storage : $hi:$lo $field);
        $crate::register!(
            @public_field_accessors $(#[doc = $doc])* $vis $name $storage : $hi:$lo $field
            $(?=> $try_into_type)?
            $(=> $into_type)?
        );
        )*
        }

        $crate::register!(@debug $name { $($field;)* });
    };

    // Private field accessors working with the correct `Bounded` type for the field.
    (
        @private_field_accessors $vis:vis $name:ident $storage:ty : $hi:tt:$lo:tt $field:ident
    ) => {
        ::kernel::macros::paste!(
        $vis const [<$field:upper _RANGE>]: ::core::ops::RangeInclusive<u8> = $lo..=$hi;
        $vis const [<$field:upper _MASK>]: $storage =
            ((((1 << $hi) - 1) << 1) + 1) - ((1 << $lo) - 1);
        $vis const [<$field:upper _SHIFT>]: u32 = $lo;
        );

        ::kernel::macros::paste!(
        fn [<__ $field>](self) ->
            ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }> {
            // Left shift to align the field's MSB with the storage MSB.
            const ALIGN_TOP: u32 = $storage::BITS - ($hi + 1);
            // Right shift to move the top-aligned field to bit 0 of the storage.
            const ALIGN_BOTTOM: u32 = ALIGN_TOP + $lo;

            // Extract the field using two shifts. `Bounded::shr` produces the correctly-sized
            // output type.
            let val = ::kernel::num::Bounded::<$storage, { $storage::BITS }>::from(
                self.0 << ALIGN_TOP
            );
            val.shr::<ALIGN_BOTTOM, { $hi + 1 - $lo } >()
        }

        const fn [<__with_ $field>](
            mut self,
            value: ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>,
        ) -> Self
        {
            const MASK: $storage = $name::[<$field:upper _MASK>];
            const SHIFT: u32 = $name::[<$field:upper _SHIFT>];

            let value = value.into_inner() << SHIFT;
            self.0 = (self.0 & !MASK) | value;

            self
        }
        );
    };

    // Public accessors for fields infallibly (`=>`) converted to a type.
    (
        @public_field_accessors $(#[doc = $doc:expr])* $vis:vis $name:ident $storage:ty :
            $hi:literal:$lo:literal $field:ident => $into_type:ty
    ) => {
        ::kernel::macros::paste!(

        $(#[doc = $doc])*
        #[doc = "Returns the value of this field."]
        #[inline(always)]
        $vis fn $field(self) -> $into_type
        {
            self.[<__ $field>]().into()
        }

        $(#[doc = $doc])*
        #[doc = "Sets this field to the given `value`."]
        #[inline(always)]
        $vis fn [<with_ $field>](self, value: $into_type) -> Self
        {
            self.[<__with_ $field>](value.into())
        }

        );
    };

    // Public accessors for fields fallibly (`?=>`) converted to a type.
    (
        @public_field_accessors $(#[doc = $doc:expr])* $vis:vis $name:ident $storage:ty :
            $hi:tt:$lo:tt $field:ident ?=> $try_into_type:ty
    ) => {
        ::kernel::macros::paste!(

        $(#[doc = $doc])*
        #[doc = "Returns the value of this field."]
        #[inline(always)]
        $vis fn $field(self) ->
            Result<
                $try_into_type,
                <$try_into_type as ::core::convert::TryFrom<
                    ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>
                >>::Error
            >
        {
            self.[<__ $field>]().try_into()
        }

        $(#[doc = $doc])*
        #[doc = "Sets this field to the given `value`."]
        #[inline(always)]
        $vis fn [<with_ $field>](self, value: $try_into_type) -> Self
        {
            self.[<__with_ $field>](value.into())
        }

        );
    };

    // Public accessors for fields not converted to a type.
    (
        @public_field_accessors $(#[doc = $doc:expr])* $vis:vis $name:ident $storage:ty :
            $hi:tt:$lo:tt $field:ident
    ) => {
        ::kernel::macros::paste!(

        $(#[doc = $doc])*
        #[doc = "Returns the value of this field."]
        #[inline(always)]
        $vis fn $field(self) ->
            ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>
        {
            self.[<__ $field>]()
        }

        $(#[doc = $doc])*
        #[doc = "Sets this field to the compile-time constant `VALUE`."]
        #[inline(always)]
        $vis const fn [<with_const_ $field>]<const VALUE: $storage>(self) -> Self {
            self.[<__with_ $field>](
                ::kernel::num::Bounded::<$storage, { $hi + 1 - $lo }>::new::<VALUE>()
            )
        }

        $(#[doc = $doc])*
        #[doc = "Sets this field to the given `value`."]
        #[inline(always)]
        $vis fn [<with_ $field>]<T>(
            self,
            value: T,
        ) -> Self
            where T: Into<::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>>,
        {
            self.[<__with_ $field>](value.into())
        }

        $(#[doc = $doc])*
        #[doc = "Tries to set this field to `value`, returning an error if it is out of range."]
        #[inline(always)]
        $vis fn [<try_with_ $field>]<T>(
            self,
            value: T,
        ) -> ::kernel::error::Result<Self>
            where T: ::kernel::num::TryIntoBounded<$storage, { $hi + 1 - $lo }>,
        {
            Ok(
                self.[<__with_ $field>](
                    value.try_into_bounded().ok_or(::kernel::error::code::EOVERFLOW)?
                )
            )
        }

        );
    };

    // `Debug` implementation.
    (@debug $name:ident { $($field:ident;)* }) => {
        impl ::kernel::fmt::Debug for $name {
            fn fmt(&self, f: &mut ::kernel::fmt::Formatter<'_>) -> ::kernel::fmt::Result {
                f.debug_struct(stringify!($name))
                    .field("<raw>", &::kernel::prelude::fmt!("{:#x}", self.0))
                $(
                    .field(stringify!($field), &self.$field())
                )*
                    .finish()
            }
        }
    };
}
