// SPDX-License-Identifier: GPL-2.0
// This file is based on rust/kernel/clk.rs.

//! GPIO consumer abstractions.
//!
//! C header: [`include/linux/gpio/consumer.h`](srctree/include/linux/gpio/consumer.h)
//!
//! Reference: <https://docs.kernel.org/driver-api/gpio/consumer.html>

use crate::{
    device::Device,
    error::{
        from_err_ptr,
        to_result,
        Error,
        Result, //
    },
    gpio::{
        LineDirection,
        LogicalLineLevel,
        PhysicalLineLevel, //
    },
    prelude::*, //
};

use core::{ops::Deref, ptr};

/// The GPIO descriptor flags to configure its direction and output value.
///
/// Rust abstraction for the C [`enum gpiod_flags`].
///
/// They can be combined with the operators `|`, and `&`.
///
/// Values can be used from the associated constants such as
/// [`Flags::GPIOD_ASIS`].
#[derive(Clone, Copy, PartialEq)]
pub struct GpiodFlags(bindings::gpiod_flags);

impl GpiodFlags {
    /// Don't change anything.
    pub const ASIS: Self = Self::new(bindings::gpiod_flags_GPIOD_ASIS);

    /// Set lines to input mode.
    pub const IN: Self = Self::new(bindings::gpiod_flags_GPIOD_IN);

    /// Set lines to output and drive them low.
    pub const OUT_LOW: Self = Self::new(bindings::gpiod_flags_GPIOD_OUT_LOW);

    /// Set lines to output and drive them high.
    pub const OUT_HIGH: Self = Self::new(bindings::gpiod_flags_GPIOD_OUT_HIGH);

    /// Set lines to open-drain output and drive them low.
    pub const OUT_LOW_OPEN_DRAIN: Self = Self::new(bindings::gpiod_flags_GPIOD_OUT_LOW_OPEN_DRAIN);

    /// Set lines to open-drain output and drive them high.
    pub const OUT_HIGH_OPEN_DRAIN: Self =
        Self::new(bindings::gpiod_flags_GPIOD_OUT_HIGH_OPEN_DRAIN);

    fn into_inner(self) -> bindings::gpiod_flags {
        self.0
    }

    // Always inline to optimize out error path of `build_assert`.
    #[inline(always)]
    const fn new(value: bindings::gpiod_flags) -> Self {
        build_assert!(value as u64 <= bindings::gpiod_flags::MAX as u64);
        Self(value)
    }
}

/// A reference-counted gpio descriptor.
///
/// Rust abstraction for the C [`struct gpio_desc`].
///
/// # Invariants
///
/// A [`GpioDesc`] instance holds either a pointer to a valid [`struct gpio_desc`] created by the C
/// portion of the kernel or a `NULL` pointer.
///
/// Instances of this type are reference-counted. Calling [`GpioDesc::get`] ensures that the
/// allocation remains valid for the lifetime of the [`GpioDesc`].
///
/// # Examples
///
/// The following example demonstrates how to obtain a GPIO line for a device.
///
/// ```
/// use crate::{
///     device::Device,
///     error::Result,
///     gpio::{
///         consumer::{
///             GpioDesc,
///             GpiodFlags, //
///         },
///         LogicalLineLevel, //
///     }, //
/// };
///
/// fn examine_gpio(dev: &Device) -> Result {
///     let gpiod = GpioDesc::get(dev, Some(c"reset"), GpiodFlags::ASIS)?;
///
///     gpiod.set_value(LogicalLineLevel::Inactive)?;
///
///     gpiod.set_value(LogicalLineLevel::Active)?;
///
///     Ok(())
/// }
/// ```
///
/// [`struct gpio_desc`]: https://docs.kernel.org/driver-api/gpio/consumer.html
#[repr(transparent)]
pub struct GpioDesc(*mut bindings::gpio_desc);

// SAFETY: It is safe to call `gpiod_put` on another thread than where `gpiod_get` was called.
unsafe impl Send for GpioDesc {}

impl GpioDesc {
    /// Gets [`GpioDesc`] corresponding to a [`Device`] and a connection id.
    ///
    /// Equivalent to the kernel's [`gpiod_get`] API.
    ///
    /// [`gpiod_get`]: https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_get
    pub fn get(dev: &Device, name: Option<&CStr>, flags: GpiodFlags) -> Result<Self> {
        let con_id = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: It is safe to call [`gpiod_get`] for a valid device pointer.
        //
        // INVARIANT: The reference-count is decremented when [`GpioDesc`] goes out of scope.
        Ok(Self(from_err_ptr(unsafe {
            bindings::gpiod_get(dev.as_raw(), con_id, flags.into_inner())
        })?))
    }

    /// Obtain the raw [`struct gpio_desc`] pointer.
    #[inline]
    fn as_raw(&self) -> *mut bindings::gpio_desc {
        self.0
    }

    /// Get the direction.
    ///
    /// Equivalent to the kernel's [`gpiod_get_direction`] API.
    ///
    /// [`gpiod_get_direction`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_get_direction
    #[inline]
    pub fn get_direction(&self) -> Result<LineDirection> {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_get_direction`].
        let ret = unsafe { bindings::gpiod_get_direction(self.as_raw()) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            LineDirection::try_from(ret)
        }
    }

    /// Set the GPIO direction to input.
    ///
    /// Equivalent to the kernel's [`gpiod_direction_input`] API.
    ///
    /// [`gpiod_direction_input`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_direction_input
    #[inline]
    pub fn direction_input(&self) -> Result {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_direction_input`].
        to_result(unsafe { bindings::gpiod_direction_input(self.as_raw()) })
    }

    /// Set the GPIO direction to output and assign the logical value.
    ///
    /// Equivalent to the kernel's [`gpiod_direction_output`] API.
    ///
    /// [`gpiod_direction_output`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_direction_output
    #[inline]
    pub fn direction_output(&self, value: LogicalLineLevel) -> Result {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_direction_output`].
        to_result(unsafe { bindings::gpiod_direction_output(self.as_raw(), value.as_c_int()) })
    }

    /// Set the GPIO direction to output and assign the physical value.
    ///
    /// Equivalent to the kernel's [`gpiod_direction_output_raw`] API.
    ///
    /// [`gpiod_direction_output_raw`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_direction_output_raw
    #[inline]
    pub fn direction_output_raw(&self, value: PhysicalLineLevel) -> Result {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_direction_output_raw`].
        to_result(unsafe { bindings::gpiod_direction_output_raw(self.as_raw(), value.as_c_int()) })
    }

    /// Get the logical GPIO value.
    ///
    /// Equivalent to the kernel's [`gpiod_get_value`] API.
    ///
    /// [`gpiod_get_value`]: https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_get_value
    #[inline]
    pub fn get_value(&self) -> Result<LogicalLineLevel> {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_get_value`].
        let ret = unsafe { bindings::gpiod_get_value(self.as_raw()) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            LogicalLineLevel::try_from(ret)
        }
    }

    /// Assign the logical value.
    ///
    /// Equivalent to the kernel's [`gpiod_set_value`] API.
    ///
    /// [`gpiod_set_value`]: https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_set_value
    #[inline]
    pub fn set_value(&self, value: LogicalLineLevel) -> Result {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_set_value`].
        to_result(unsafe { bindings::gpiod_set_value(self.as_raw(), value.as_c_int()) })
    }

    /// Get the physical GPIO value.
    ///
    /// Equivalent to the kernel's [`gpiod_get_raw_value`] API.
    ///
    /// [`gpiod_get_raw_value`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_get_raw_value
    #[inline]
    pub fn get_raw_value(&self) -> Result<PhysicalLineLevel> {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_get_raw_value`].
        let ret = unsafe { bindings::gpiod_get_raw_value(self.as_raw()) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            PhysicalLineLevel::try_from(ret)
        }
    }

    /// Assign the physical value.
    ///
    /// Equivalent to the kernel's [`gpiod_set_raw_value`] API.
    ///
    /// [`gpiod_set_raw_value`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_set_raw_value
    #[inline]
    pub fn set_raw_value(&self, value: PhysicalLineLevel) -> Result {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_set_raw_value`].
        to_result(unsafe { bindings::gpiod_set_raw_value(self.as_raw(), value.as_c_int()) })
    }

    /// Get the logical GPIO value.
    ///
    /// Equivalent to the kernel's [`gpiod_get_value_cansleep`] API.
    ///
    /// [`gpiod_get_value_cansleep`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_get_value_cansleep
    #[inline]
    pub fn get_value_cansleep(&self) -> Result<LogicalLineLevel> {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_get_value_cansleep`].
        let ret = unsafe { bindings::gpiod_get_value_cansleep(self.as_raw()) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            LogicalLineLevel::try_from(ret)
        }
    }

    /// Assign the logical value.
    ///
    /// Equivalent to the kernel's [`gpiod_set_value_cansleep`] API.
    ///
    /// [`gpiod_set_value_cansleep`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_set_value_cansleep
    #[inline]
    pub fn set_value_cansleep(&self, value: LogicalLineLevel) -> Result {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_set_value_cansleep`].
        to_result(unsafe { bindings::gpiod_set_value_cansleep(self.as_raw(), value.as_c_int()) })
    }

    /// Get the physical GPIO value.
    ///
    /// Equivalent to the kernel's [`gpiod_get_raw_value_cansleep`] API.
    ///
    /// [`gpiod_get_raw_value_cansleep`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_get_raw_value_cansleep
    #[inline]
    pub fn get_raw_value_cansleep(&self) -> Result<PhysicalLineLevel> {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_get_raw_value_cansleep`].
        let ret = unsafe { bindings::gpiod_get_raw_value_cansleep(self.as_raw()) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            PhysicalLineLevel::try_from(ret)
        }
    }

    /// Assign the physical value.
    ///
    /// Equivalent to the kernel's [`gpiod_set_raw_value_cansleep`] API.
    ///
    /// [`gpiod_set_raw_value_cansleep`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_set_raw_value_cansleep
    #[inline]
    pub fn set_raw_value_cansleep(&self, value: PhysicalLineLevel) -> Result {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_set_raw_value_cansleep`].
        to_result(unsafe {
            bindings::gpiod_set_raw_value_cansleep(self.as_raw(), value.as_c_int())
        })
    }

    /// Test whether the GPIO is active-low or not.
    ///
    /// Equivalent to the kernel's [`gpiod_is_active_low`] API.
    ///
    /// [`gpiod_is_active_low`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_is_active_low
    #[inline]
    pub fn is_active_low(&self) -> Result<bool> {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_is_active_low`].
        match unsafe { bindings::gpiod_is_active_low(self.as_raw()) } {
            0 => Ok(false),
            1 => Ok(true),
            err => Err(Error::from_errno(err)),
        }
    }

    /// Report whether gpio value access may sleep or not.
    ///
    /// Equivalent to the kernel's [`gpiod_cansleep`] API.
    ///
    /// [`gpiod_cansleep`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_cansleep
    #[inline]
    pub fn cansleep(&self) -> Result<bool> {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for
        // [`gpiod_cansleep`].
        match unsafe { bindings::gpiod_cansleep(self.as_raw()) } {
            0 => Ok(false),
            1 => Ok(true),
            err => Err(Error::from_errno(err)),
        }
    }
}

impl Drop for GpioDesc {
    fn drop(&mut self) {
        // SAFETY: By the type invariants, self.as_raw() is a valid argument for [`gpiod_put`].
        unsafe { bindings::gpiod_put(self.as_raw()) };
    }
}

/// A reference-counted optional gpio descriptor.
///
/// A lightweight wrapper around an optional [`GpioDesc`]. An [`OptionalGpioDesc`] represents
/// a [`GpioDesc`] that a driver can function without but may improve performance or enable
/// additional features when available.
///
/// # Invariants
///
/// An [`OptionalGpioDesc`] instance encapsulates a [`GpioDesc`] with either a valid
/// [`struct gpio_desc`] or `NULL` pointer.
///
/// Instances of this type are reference-counted. Calling [`OptionalGpioDesc::get`] ensures that
/// the allocation remains valid for the lifetime of the [`OptionalGpioDesc`].
///
/// # Examples
///
/// The following example demonstrates how to obtain and configure an optional GPIO for a
/// device. The code functions correctly whether or not the GPIO is available.
///
/// ```
/// use crate::{
///     device::Device,
///     error::Result,
///     gpio::{
///         consumer::{
///             OptionalGpioDesc,
///             GpiodFlags, //
///         },
///         LogicalLineLevel, //
///     }, //
/// };
///
/// fn examine_gpio(dev: &Device) -> Result {
///     let gpiod = OptionalGpioDesc::get(dev, Some(c"reset"), GpiodFlags::ASIS)?;
///
///     gpiod.set_value(LogicalLineLevel::Inactive)?;
///
///     gpiod.set_value(LogicalLineLevel::Active)?;
///
///     Ok(())
/// }
/// ```
///
/// [`struct gpio_desc`]: https://docs.kernel.org/driver-api/gpio/consumer.html
pub struct OptionalGpioDesc(GpioDesc);

impl OptionalGpioDesc {
    /// Gets [`OptionalGpioDesc`] corresponding to a [`Device`] and a connection id.
    ///
    /// Equivalent to the kernel's [`gpiod_get_optional`] API.
    ///
    /// [`gpiod_get_optional`]:
    /// https://docs.kernel.org/driver-api/gpio/index.html#c.gpiod_get_optional
    pub fn get(dev: &Device, name: Option<&CStr>, flags: GpiodFlags) -> Result<Self> {
        let con_id = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: It is safe to call [`gpiod_get_optional`] for a valid device pointer.
        //
        // INVARIANT: The reference-count is decremented when [`OptionalGpioDesc`] goes out of
        // scope.
        Ok(Self(GpioDesc(from_err_ptr(unsafe {
            bindings::gpiod_get_optional(dev.as_raw(), con_id, flags.into_inner())
        })?)))
    }
}

// Make [`OptionalGpioDesc`] behave like [`GpioDesc`].
impl Deref for OptionalGpioDesc {
    type Target = GpioDesc;

    fn deref(&self) -> &GpioDesc {
        &self.0
    }
}
