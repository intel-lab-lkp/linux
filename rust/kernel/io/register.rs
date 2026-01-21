// SPDX-License-Identifier: GPL-2.0

//! A macro to define register layout and accessors.
//!
//! A single register typically includes several fields, which are accessed through a combination
//! of bit-shift and mask operations that introduce a class of potential mistakes, notably because
//! not all possible field values are necessarily valid.
//!
//! The [`register!`] macro in this module provides an intuitive and readable syntax for defining a
//! dedicated type for each register. Each such type comes with its own field accessors that can
//! return an error if a field's value is invalid. Please look at the [`bitfield!`] macro for the
//! complete syntax of fields definitions.
//!
//! [`register!`]: kernel::register!
//! [`bitfield!`]: crate::bitfield!

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
pub trait RegisterIo: Sized {
    /// Read a value from the given offset in the I/O region.
    fn read<const SIZE: usize, T>(io: &T, offset: usize) -> Self
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>;

    /// Write a value to the given offset in the I/O region.
    fn write<const SIZE: usize, T>(self, io: &T, offset: usize)
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>;
}

impl RegisterIo for u8 {
    #[inline(always)]
    fn read<const SIZE: usize, T>(io: &T, offset: usize) -> Self
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>,
    {
        io.read8(offset)
    }

    #[inline(always)]
    fn write<const SIZE: usize, T>(self, io: &T, offset: usize)
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>,
    {
        io.write8(self, offset)
    }
}

impl RegisterIo for u16 {
    #[inline(always)]
    fn read<const SIZE: usize, T>(io: &T, offset: usize) -> Self
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>,
    {
        io.read16(offset)
    }

    #[inline(always)]
    fn write<const SIZE: usize, T>(self, io: &T, offset: usize)
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>,
    {
        io.write16(self, offset)
    }
}

impl RegisterIo for u32 {
    #[inline(always)]
    fn read<const SIZE: usize, T>(io: &T, offset: usize) -> Self
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>,
    {
        io.read32(offset)
    }

    #[inline(always)]
    fn write<const SIZE: usize, T>(self, io: &T, offset: usize)
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>,
    {
        io.write32(self, offset)
    }
}

#[cfg(CONFIG_64BIT)]
impl RegisterIo for u64 {
    #[inline(always)]
    fn read<const SIZE: usize, T>(io: &T, offset: usize) -> Self
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>,
    {
        io.read64(offset)
    }

    #[inline(always)]
    fn write<const SIZE: usize, T>(self, io: &T, offset: usize)
    where
        T: core::ops::Deref<Target = crate::io::Io<SIZE>>,
    {
        io.write64(self, offset)
    }
}

/// Defines a dedicated type for a register with an absolute offset, including getter and setter
/// methods for its fields and methods to read and write it from an `Io` region.
///
/// A register is essentially a [`bitfield!`] with I/O capabilities. The syntax of the `register!`
/// macro reflects that fact, being essentially identical to that of [`bitfield!`] with the
/// addition of addressing information after the `@` token.
///
/// Example:
///
/// ```
/// use kernel::register;
///
/// register!(pub BOOT_0(u32) @ 0x00000100, "Basic revision information about the chip" {
///     7:4 major_revision, "Major revision of the chip";
///     3:0 minor_revision, "Minor revision of the chip";
/// });
/// ```
///
/// This defines a `BOOT_0` type which can be read or written from offset `0x100` of an `Io`
/// region. For instance, `minor_revision` is made of the 4 least significant bits of the
/// register. Each field can be accessed and modified using accessor
/// methods:
///
/// ```no_run
/// use kernel::register;
/// use kernel::num::Bounded;
///
/// # register!(pub BOOT_0(u32) @ 0x00000100, "Basic revision information about the chip" {
/// #     7:4 major_revision, "Major revision of the chip";
/// #     3:0 minor_revision, "Minor revision of the chip";
/// # });
/// # fn test(bar: &kernel::io::Io) {
/// // Read from the register's defined offset (0x100).
/// let boot0 = BOOT_0::read(&bar);
/// pr_info!("chip revision: {}.{}", boot0.major_revision().get(), boot0.minor_revision().get());
///
/// // Update some fields and write the value back.
/// boot0
///     .set_major_revision(Bounded::<u32, _>::new::<3>())
///     .set_minor_revision(Bounded::<u32, _>::new::<10>())
///     .write(&bar);
///
/// // Or, just read and update the register in a single step:
/// BOOT_0::update(&bar, |r| r
///     .set_major_revision(Bounded::<u32, _>::new::<3>())
///     .set_minor_revision(Bounded::<u32, _>::new::<10>())
/// );
/// # }
/// ```
///
/// The documentation strings are optional. If present, they will be added to the type's
/// definition, or the field getter and setter methods they are attached to.
///
/// Attributes can be applied to the generated struct. The `#[allow(non_camel_case_types)]`
/// attribute is automatically added since register names typically use SCREAMING_CASE:
///
/// ```
/// use kernel::register;
///
/// register! {
///     pub STATUS(u32) @ 0x00000000, "Status register" {
///         0:0 ready, "Device ready flag";
///     }
/// }
/// ```
///
/// It is also possible to create an alias register by using the `=> ALIAS` syntax. This is useful
/// for cases where a register's interpretation depends on the context:
///
/// ```
/// use kernel::register;
///
/// register!(pub SCRATCH(u32) @ 0x00000200, "Scratch register" {
///     31:0 value, "Raw value";
/// });
///
/// register!(pub SCRATCH_BOOT_STATUS(u32) => SCRATCH, "Boot status of the firmware" {
///     0:0 completed, "Whether the firmware has completed booting";
/// });
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
/// This can be done using the `Base[Offset]` syntax when specifying the register's address.
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
/// # fn test(bar: &kernel::io::Io) {
/// // This makes `CPU_CTL` accessible from all implementors of `RegisterBase<CpuCtlBase>`.
/// register!(pub CPU_CTL(u32) @ CpuCtlBase[0x10], "CPU core control" {
///     0:0 start, "Start the CPU core";
/// });
///
/// // The `read`, `write` and `update` methods of relative registers take an extra `base` argument
/// // that is used to resolve its final address by adding its `BASE` to the offset of the
/// // register.
///
/// // Start `CPU0`.
/// CPU_CTL::update(&bar, &CPU0, |r| r.set_start(true));
///
/// // Start `CPU1`.
/// CPU_CTL::update(&bar, &CPU1, |r| r.set_start(true));
///
/// // Aliases can also be defined for relative register.
/// register!(pub CPU_CTL_ALIAS(u32) => CpuCtlBase[CPU_CTL], "Alias to CPU core control" {
///     1:1 alias_start, "Start the aliased CPU core";
/// });
///
/// // Start the aliased `CPU0`.
/// CPU_CTL_ALIAS::update(&bar, &CPU0, |r| r.set_alias_start(true));
/// # }
/// ```
///
/// ## Arrays of registers
///
/// Some I/O areas contain consecutive values that can be interpreted in the same way. These areas
/// can be defined as an array of identical registers, allowing them to be accessed by index with
/// compile-time or runtime bound checking. Simply define their address as `Address[Size]`, and add
/// an `idx` parameter to their `read`, `write` and `update` methods:
///
/// ```no_run
/// use kernel::register;
///
/// # fn no_run(bar: &kernel::io::Io) -> Result<(), Error> {
/// # fn get_scratch_idx() -> usize {
/// #   0x15
/// # }
/// // Array of 64 consecutive registers with the same layout starting at offset `0x80`.
/// register!(pub SCRATCH(u32) @ 0x00000080[64], "Scratch registers" {
///     31:0 value;
/// });
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
/// register!(pub FIRMWARE_STATUS(u32) => SCRATCH[8], "Firmware exit status code" {
///     7:0 status;
/// });
///
/// let status = FIRMWARE_STATUS::read(&bar).status();
///
/// // Non-contiguous register arrays can be defined by adding a stride parameter.
/// // Here, each of the 16 registers of the array are separated by 8 bytes, meaning that the
/// // registers of the two declarations below are interleaved.
/// register!(pub SCRATCH_INTERLEAVED_0(u32) @ 0x000000c0[16 ; 8], "Scratch registers bank 0" {
///     31:0 value;
/// });
/// register!(pub SCRATCH_INTERLEAVED_1(u32) @ 0x000000c4[16 ; 8], "Scratch registers bank 1" {
///     31:0 value;
/// });
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
/// # fn no_run(bar: &kernel::io::Io) -> Result<(), Error> {
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
/// register!(pub CPU_SCRATCH(u32) @ CpuCtlBase[0x00000080[64]], "Per-CPU scratch registers" {
///     31:0 value;
/// });
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
/// register!(pub CPU_FIRMWARE_STATUS(u32) => CpuCtlBase[CPU_SCRATCH[8]],
///     "Per-CPU firmware exit status code" {
///     7:0 status;
/// });
///
/// let cpu0_status = CPU_FIRMWARE_STATUS::read(&bar, &Cpu0).status();
///
/// // Non-contiguous register arrays can be defined by adding a stride parameter.
/// // Here, each of the 16 registers of the array are separated by 8 bytes, meaning that the
/// // registers of the two declarations below are interleaved.
/// register!(pub CPU_SCRATCH_INTERLEAVED_0(u32) @ CpuCtlBase[0x00000d00[16 ; 8]],
///     "Scratch registers bank 0" {
///     31:0 value;
/// });
/// register!(pub CPU_SCRATCH_INTERLEAVED_1(u32) @ CpuCtlBase[0x00000d04[16 ; 8]],
///     "Scratch registers bank 1" {
///     31:0 value;
/// });
/// # Ok(())
/// # }
/// ```
/// [`bitfield!`]: crate::bitfield!
#[macro_export]
macro_rules! register {
    // Creates a register at a fixed offset of the MMIO space.
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) @ $offset:literal
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        ::kernel::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) $(, $comment)? { $($fields)* }
        );
        ::kernel::register!(@io_fixed $name($storage) @ $offset);
    };

    // Creates an alias register of fixed offset register `alias` with its own fields.
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) => $alias:ident
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        ::kernel::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) $(, $comment)? { $($fields)* }
        );
        ::kernel::register!(@io_fixed $name($storage) @ $alias::OFFSET);
    };

    // Creates a register at a relative offset from a base address provider.
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) @ $base:ty [ $offset:literal ]
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        ::kernel::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) $(, $comment)? { $($fields)* }
        );
        ::kernel::register!(@io_relative $name($storage) @ $base [ $offset ]);
    };

    // Creates an alias register of relative offset register `alias` with its own fields.
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) => $base:ty [ $alias:ident ]
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        ::kernel::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) $(, $comment)? { $($fields)* }
        );
        ::kernel::register!(@io_relative $name($storage) @ $base [ $alias::OFFSET ]);
    };

    // Creates an array of registers at a fixed offset of the MMIO space.
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
            @ $offset:literal [ $size:expr ; $stride:expr ]
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        static_assert!(::core::mem::size_of::<$storage>() <= $stride);

        ::kernel::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) $(, $comment)? { $($fields)* }
        );
        ::kernel::register!(@io_array $name($storage) @ $offset [ $size ; $stride ]);
    };

    // Shortcut for contiguous array of registers (stride == size of element).
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) @ $offset:literal [ $size:expr ]
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        ::kernel::register!(
            $(#[$attr])* $vis $name($storage)
                @ $offset [ $size ; ::core::mem::size_of::<$storage>() ]
                $(, $comment)? { $($fields)* }
        );
    };

    // Creates an array of registers at a relative offset from a base address provider.
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
            @ $base:ty [ $offset:literal [ $size:expr ; $stride:expr ] ]
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        static_assert!(::core::mem::size_of::<$storage>() <= $stride);

        ::kernel::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) $(, $comment)? { $($fields)* }
        );
        ::kernel::register!(
            @io_relative_array $name($storage) @ $base [ $offset [ $size ; $stride ] ]
        );
    };

    // Shortcut for contiguous array of relative registers (stride == size of element).
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
            @ $base:ty [ $offset:literal [ $size:expr ] ]
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        ::kernel::register!(
            $(#[$attr])* $vis $name($storage)
                @ $base [ $offset [ $size ; ::core::mem::size_of::<$storage>() ] ]
                $(, $comment)? { $($fields)* }
        );
    };

    // Creates an alias of register `idx` of relative array of registers `alias` with its own
    // fields.
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty)
            => $base:ty [ $alias:ident [ $idx:expr ] ]
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        static_assert!($idx < $alias::SIZE);

        ::kernel::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) $(, $comment)? { $($fields)* }
        );
        ::kernel::register!(
            @io_relative $name($storage) @ $base [ $alias::OFFSET + $idx * $alias::STRIDE ]
        );
    };

    // Creates an alias of register `idx` of array of registers `alias` with its own fields.
    // This rule belongs to the (non-relative) register arrays set, but needs to be put last
    // to avoid it being interpreted in place of the relative register array alias rule.
    (
        $(#[$attr:meta])* $vis:vis $name:ident ($storage:ty) => $alias:ident [ $idx:expr ]
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        static_assert!($idx < $alias::SIZE);

        ::kernel::register!(
            @bitfield $(#[$attr])* $vis struct $name($storage) $(, $comment)? { $($fields)* }
        );
        ::kernel::register!(@io_fixed $name($storage) @ $alias::OFFSET + $idx * $alias::STRIDE);
    };

    // All rules below are helpers.

    // Generates the bitfield for the register.
    //
    // `#[allow(non_camel_case_types)]` is added since register names typically use SCREAMING_CASE.
    (
        @bitfield $(#[$attr:meta])* $vis:vis struct $name:ident($storage:ty)
            $(, $comment:literal)? { $($fields:tt)* }
    ) => {
        ::kernel::register!(@bitfield_core
            #[allow(non_camel_case_types)]
            $(#[$attr])* $vis $name $storage $(, $comment)?
        );
        ::kernel::register!(@bitfield_fields $vis $name $storage { $($fields)* });
    };

    // Generates the IO accessors for a fixed offset register.
    (@io_fixed $name:ident ($storage:ty) @ $offset:expr) => {
        #[allow(dead_code)]
        impl $name {
            pub const OFFSET: usize = $offset;

            /// Read the register from its address in `io`.
            #[inline(always)]
            pub fn read<const SIZE: usize, T>(io: &T) -> Self where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
            {
                Self(<$storage as $crate::io::register::RegisterIo>::read(io, $offset))
            }

            /// Write the value contained in `self` to the register address in `io`.
            #[inline(always)]
            pub fn write<const SIZE: usize, T>(self, io: &T) where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
            {
                <$storage as $crate::io::register::RegisterIo>::write(self.0, io, $offset)
            }

            /// Read the register from its address in `io` and run `f` on its value to obtain a new
            /// value to write back.
            ///
            /// Note that this operation is not atomic. In concurrent contexts, external
            /// synchronization may be required to prevent race conditions.
            #[inline(always)]
            pub fn update<const SIZE: usize, T, F>(
                io: &T,
                f: F,
            ) where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
                F: ::core::ops::FnOnce(Self) -> Self,
            {
                let reg = f(Self::read(io));
                reg.write(io);
            }
        }
    };

    // Generates the IO accessors for a relative offset register.
    (@io_relative $name:ident ($storage:ty) @ $base:ty [ $offset:expr ]) => {
        #[allow(dead_code)]
        impl $name {
            pub const OFFSET: usize = $offset;

            /// Read the register from `io`, using the base address provided by `base` and adding
            /// the register's offset to it.
            #[inline(always)]
            pub fn read<const SIZE: usize, T, B>(
                io: &T,
                #[allow(unused_variables)]
                base: &B,
            ) -> Self where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                let offset = <B as $crate::io::register::RegisterBase<$base>>::BASE + $name::OFFSET;

                Self(<$storage as $crate::io::register::RegisterIo>::read(io, offset))
            }

            /// Write the value contained in `self` to `io`, using the base address provided by
            /// `base` and adding the register's offset to it.
            #[inline(always)]
            pub fn write<const SIZE: usize, T, B>(
                self,
                io: &T,
                #[allow(unused_variables)]
                base: &B,
            ) where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                let offset = <B as $crate::io::register::RegisterBase<$base>>::BASE + $name::OFFSET;

                <$storage as $crate::io::register::RegisterIo>::write(self.0, io, offset)
            }

            /// Read the register from `io`, using the base address provided by `base` and adding
            /// the register's offset to it, then run `f` on its value to obtain a new value to
            /// write back.
            ///
            /// Note that this operation is not atomic. In concurrent contexts, external
            /// synchronization may be required to prevent race conditions.
            #[inline(always)]
            pub fn update<const SIZE: usize, T, B, F>(
                io: &T,
                base: &B,
                f: F,
            ) where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
                B: $crate::io::register::RegisterBase<$base>,
                F: ::core::ops::FnOnce(Self) -> Self,
            {
                let reg = f(Self::read(io, base));
                reg.write(io, base);
            }
        }
    };

    // Generates the IO accessors for an array of registers.
    (@io_array $name:ident ($storage:ty) @ $offset:literal [ $size:expr ; $stride:expr ]) => {
        #[allow(dead_code)]
        impl $name {
            pub const OFFSET: usize = $offset;
            pub const SIZE: usize = $size;
            pub const STRIDE: usize = $stride;

            /// Read the array register at index `idx` from its address in `io`.
            #[inline(always)]
            pub fn read<const SIZE: usize, T>(
                io: &T,
                idx: usize,
            ) -> Self where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
            {
                build_assert!(idx < Self::SIZE);

                let offset = Self::OFFSET + (idx * Self::STRIDE);

                Self(<$storage as $crate::io::register::RegisterIo>::read(io, offset))
            }

            /// Write the value contained in `self` to the array register with index `idx` in `io`.
            #[inline(always)]
            pub fn write<const SIZE: usize, T>(
                self,
                io: &T,
                idx: usize
            ) where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
            {
                build_assert!(idx < Self::SIZE);

                let offset = Self::OFFSET + (idx * Self::STRIDE);

                <$storage as $crate::io::register::RegisterIo>::write(self.0, io, offset)
            }

            /// Read the array register at index `idx` in `io` and run `f` on its value to obtain a
            /// new value to write back.
            ///
            /// Note that this operation is not atomic. In concurrent contexts, external
            /// synchronization may be required to prevent race conditions.
            #[inline(always)]
            pub fn update<const SIZE: usize, T, F>(
                io: &T,
                idx: usize,
                f: F,
            ) where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
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
            pub fn try_read<const SIZE: usize, T>(
                io: &T,
                idx: usize,
            ) -> ::kernel::error::Result<Self> where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
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
            pub fn try_write<const SIZE: usize, T>(
                self,
                io: &T,
                idx: usize,
            ) -> ::kernel::error::Result where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
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
            pub fn try_update<const SIZE: usize, T, F>(
                io: &T,
                idx: usize,
                f: F,
            ) -> ::kernel::error::Result where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
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
        @io_relative_array $name:ident ($storage:ty) @ $base:ty
            [ $offset:literal [ $size:expr ; $stride:expr ] ]
    ) => {
        #[allow(dead_code)]
        impl $name {
            pub const OFFSET: usize = $offset;
            pub const SIZE: usize = $size;
            pub const STRIDE: usize = $stride;

            /// Read the array register at index `idx` from `io`, using the base address provided
            /// by `base` and adding the register's offset to it.
            #[inline(always)]
            pub fn read<const SIZE: usize, T, B>(
                io: &T,
                #[allow(unused_variables)]
                base: &B,
                idx: usize,
            ) -> Self where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                build_assert!(idx < Self::SIZE);

                let offset = <B as $crate::io::register::RegisterBase<$base>>::BASE +
                    Self::OFFSET + (idx * Self::STRIDE);

                Self(<$storage as $crate::io::register::RegisterIo>::read(io, offset))
            }

            /// Write the value contained in `self` to `io`, using the base address provided by
            /// `base` and adding the offset of array register `idx` to it.
            #[inline(always)]
            pub fn write<const SIZE: usize, T, B>(
                self,
                io: &T,
                #[allow(unused_variables)]
                base: &B,
                idx: usize
            ) where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
                B: $crate::io::register::RegisterBase<$base>,
            {
                build_assert!(idx < Self::SIZE);

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
            pub fn update<const SIZE: usize, T, B, F>(
                io: &T,
                base: &B,
                idx: usize,
                f: F,
            ) where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
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
            pub fn try_read<const SIZE: usize, T, B>(
                io: &T,
                base: &B,
                idx: usize,
            ) -> ::kernel::error::Result<Self> where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
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
            pub fn try_write<const SIZE: usize, T, B>(
                self,
                io: &T,
                base: &B,
                idx: usize,
            ) -> ::kernel::error::Result where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
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
            pub fn try_update<const SIZE: usize, T, B, F>(
                io: &T,
                base: &B,
                idx: usize,
                f: F,
            ) -> ::kernel::error::Result where
                T: ::core::ops::Deref<Target = ::kernel::io::Io<SIZE>>,
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
    (@bitfield_core $(#[$attr:meta])* $vis:vis $name:ident $storage:ty $(, $comment:literal)?) => {
        $(
        #[doc=$comment]
        )?
        $(#[$attr])*
        #[repr(transparent)]
        #[derive(Clone, Copy, PartialEq, Eq)]
        $vis struct $name($storage);

        #[allow(dead_code)]
        impl $name {
            /// Returns the raw value of this bitfield.
            ///
            /// This is similar to the [`From`] implementation, but is shorter to invoke in
            /// most cases.
            $vis fn as_raw(self) -> $storage {
                self.0
            }
        }

        impl ::core::convert::From<$name> for $storage {
            fn from(val: $name) -> $storage {
                val.0
            }
        }

        impl ::core::convert::From<$storage> for $name {
            fn from(val: $storage) -> $name {
                Self(val)
            }
        }
    };

    // Definitions requiring knowledge of individual fields: private and public field accessors,
    // and `Debug` and `Default` implementations.
    (@bitfield_fields $vis:vis $name:ident $storage:ty {
        $($hi:tt:$lo:tt $field:ident
            $(?=> $try_into_type:ty)?
            $(=> $into_type:ty)?
            $(, $comment:literal)?
        ;
        )*
    }
    ) => {
        #[allow(dead_code)]
        impl $name {
        $(
        ::kernel::register!(@private_field_accessors $vis $name $storage : $hi:$lo $field);
        ::kernel::register!(@public_field_accessors $vis $name $storage : $hi:$lo $field
            $(?=> $try_into_type)?
            $(=> $into_type)?
            $(, $comment)?
        );
        )*
        }

        ::kernel::register!(@debug $name { $($field;)* });
        ::kernel::register!(@default $name { $($field;)* });
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

        fn [<__set_ $field>](
            mut self,
            value: ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>,
        ) -> Self
        {
            const MASK: $storage = $name::[<$field:upper _MASK>];
            const SHIFT: u32 = $name::[<$field:upper _SHIFT>];

            let value = value.get() << SHIFT;
            self.0 = (self.0 & !MASK) | value;

            self
        }
        );
    };

    // Public accessors for fields infallibly (`=>`) converted to a type.
    (
        @public_field_accessors $vis:vis $name:ident $storage:ty : $hi:tt:$lo:tt $field:ident
            => $into_type:ty $(, $comment:literal)?
    ) => {
        ::kernel::macros::paste!(

        $(
        #[doc="Returns the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn $field(self) -> $into_type
        {
            self.[<__ $field>]().into()
        }

        $(
        #[doc="Sets the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<set_ $field>](self, value: $into_type) -> Self
        {
            self.[<__set_ $field>](value.into())
        }

        /// Private method, for use in the [`Default`] implementation.
        fn [<$field _default>]() -> $into_type {
            Default::default()
        }

        );
    };

    // Public accessors for fields fallibly (`?=>`) converted to a type.
    (
        @public_field_accessors $vis:vis $name:ident $storage:ty : $hi:tt:$lo:tt $field:ident
            ?=> $try_into_type:ty $(, $comment:literal)?
    ) => {
        ::kernel::macros::paste!(

        $(
        #[doc="Returns the value of this field:"]
        #[doc=$comment]
        )?
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

        $(
        #[doc="Sets the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<set_ $field>](self, value: $try_into_type) -> Self
        {
            self.[<__set_ $field>](value.into())
        }

        /// Private method, for use in the [`Default`] implementation.
        fn [<$field _default>]() -> $try_into_type {
            Default::default()
        }

        );
    };

    // Public accessors for fields not converted to a type.
    (
        @public_field_accessors $vis:vis $name:ident $storage:ty : $hi:tt:$lo:tt $field:ident
            $(, $comment:literal)?
    ) => {
        ::kernel::macros::paste!(

        $(
        #[doc="Returns the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn $field(self) ->
            ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>
        {
            self.[<__ $field>]()
        }

        $(
        #[doc="Sets the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<set_ $field>]<T>(
            self,
            value: T,
        ) -> Self
            where T: Into<::kernel::num::Bounded<$storage, { $hi + 1 - $lo }>>,
        {
            self.[<__set_ $field>](value.into())
        }

        $(
        #[doc="Attempts to set the value of this field:"]
        #[doc=$comment]
        )?
        #[inline(always)]
        $vis fn [<try_set_ $field>]<T>(
            self,
            value: T,
        ) -> ::kernel::error::Result<Self>
            where T: ::kernel::num::TryIntoBounded<$storage, { $hi + 1 - $lo }>,
        {
            Ok(
                self.[<__set_ $field>](
                    value.try_into_bounded().ok_or(::kernel::error::code::EOVERFLOW)?
                )
            )
        }

        /// Private method, for use in the [`Default`] implementation.
        fn [<$field _default>]() -> ::kernel::num::Bounded<$storage, { $hi + 1 - $lo }> {
            Default::default()
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

    // `Default` implementation.
    (@default $name:ident { $($field:ident;)* }) => {
        /// Returns a value for the bitfield where all fields are set to their default value.
        impl ::core::default::Default for $name {
            fn default() -> Self {
                #[allow(unused_mut)]
                let mut value = Self(Default::default());

                ::kernel::macros::paste!(
                $(
                value = value.[<set_ $field>](Self::[<$field _default>]());
                )*
                );

                value
            }
        }
    };
}
