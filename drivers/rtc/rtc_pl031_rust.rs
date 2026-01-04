// SPDX-License-Identifier: GPL-2.0-or-later
//! Real Time Clock interface for ARM AMBA PrimeCell 031 RTC
//!
//! This is a Rust port of the C driver in rtc-pl031.c
//!
//! Author: Ke Sun <sunke@kylinos.cn>
//! Based on: drivers/rtc/rtc-pl031.c

use core::ops::Deref;
use kernel::{
    amba,
    bindings,
    c_str,
    device,
    devres::Devres,
    error::code,
    io::mem::IoMem,
    irq::{
        Handler,
        IrqReturn, //
    },
    prelude::*,
    rtc::{
        self,
        RtcDevice,
        RtcDeviceOptions,
        RtcOperations,
        RtcTime,
        RtcWkAlrm, //
    },
    sync::aref::ARef, //
};

// Register definitions
const RTC_DR: usize = 0x00; // Data read register
const RTC_MR: usize = 0x04; // Match register
const RTC_LR: usize = 0x08; // Data load register
const RTC_CR: usize = 0x0c; // Control register
const RTC_IMSC: usize = 0x10; // Interrupt mask and set register
const RTC_RIS: usize = 0x14; // Raw interrupt status register
const RTC_MIS: usize = 0x18; // Masked interrupt status register
const RTC_ICR: usize = 0x1c; // Interrupt clear register
const RTC_YDR: usize = 0x30; // Year data read register
const RTC_YMR: usize = 0x34; // Year match register
const RTC_YLR: usize = 0x38; // Year data load register

// Control register bits
const RTC_CR_EN: u32 = 1 << 0; // Counter enable bit
const RTC_CR_CWEN: u32 = 1 << 26; // Clockwatch enable bit

// Interrupt status and control register bits
const RTC_BIT_AI: u32 = 1 << 0; // Alarm interrupt bit

// RTC event flags
#[allow(dead_code)]
const RTC_AF: u32 = bindings::RTC_AF;
#[allow(dead_code)]
const RTC_IRQF: u32 = bindings::RTC_IRQF;

// ST v2 time format bit definitions
const RTC_SEC_SHIFT: u32 = 0;
const RTC_SEC_MASK: u32 = 0x3F << RTC_SEC_SHIFT; // Second [0-59]
const RTC_MIN_SHIFT: u32 = 6;
const RTC_MIN_MASK: u32 = 0x3F << RTC_MIN_SHIFT; // Minute [0-59]
const RTC_HOUR_SHIFT: u32 = 12;
const RTC_HOUR_MASK: u32 = 0x1F << RTC_HOUR_SHIFT; // Hour [0-23]
const RTC_WDAY_SHIFT: u32 = 17;
const RTC_WDAY_MASK: u32 = 0x7 << RTC_WDAY_SHIFT; // Day of week [1-7], 1=Sunday
const RTC_MDAY_SHIFT: u32 = 20;
const RTC_MDAY_MASK: u32 = 0x1F << RTC_MDAY_SHIFT; // Day of month [1-31]
const RTC_MON_SHIFT: u32 = 25;
const RTC_MON_MASK: u32 = 0xF << RTC_MON_SHIFT; // Month [1-12], 1=January

/// Vendor-specific data for different PL031 variants
#[derive(Copy, Clone, PartialEq)]
enum VendorVariant {
    /// Original ARM version
    Arm,
    /// First ST derivative
    StV1,
    /// Second ST derivative
    StV2,
}

impl VendorVariant {
    fn clockwatch(&self) -> bool {
        matches!(self, VendorVariant::StV1 | VendorVariant::StV2)
    }

    #[allow(dead_code)]
    fn st_weekday(&self) -> bool {
        matches!(self, VendorVariant::StV1 | VendorVariant::StV2)
    }

    #[allow(dead_code)]
    fn range_min(&self) -> i64 {
        match self {
            VendorVariant::Arm | VendorVariant::StV1 => 0,
            VendorVariant::StV2 => bindings::RTC_TIMESTAMP_BEGIN_0000,
        }
    }

    #[allow(dead_code)]
    fn range_max(&self) -> u64 {
        match self {
            VendorVariant::Arm | VendorVariant::StV1 => u64::from(u32::MAX),
            VendorVariant::StV2 => bindings::RTC_TIMESTAMP_END_9999,
        }
    }
}

/// PL031 RTC driver private data.
#[pin_data(PinnedDrop)]
struct Pl031DrvData {
    #[pin]
    base: Devres<IoMem<0>>,
    variant: VendorVariant,
    /// RTC device reference for interrupt handler.
    ///
    /// Set in `init_rtcdevice` and remains valid for the driver's lifetime
    /// because the RTC device is managed by devres.
    rtc_device: Option<ARef<RtcDevice>>,
}

// SAFETY: `Pl031DrvData` contains only `Send`/`Sync` types: `Devres` (Send+Sync),
// `VendorVariant` (Copy), and `Option<ARef<RtcDevice>>` (Send+Sync because `RtcDevice` is
// Send+Sync).
unsafe impl Send for Pl031DrvData {}
// SAFETY: `Pl031DrvData` contains only `Send`/`Sync` types: `Devres` (Send+Sync),
// `VendorVariant` (Copy), and `Option<ARef<RtcDevice>>` (Send+Sync because `RtcDevice` is
// Send+Sync).
unsafe impl Sync for Pl031DrvData {}

/// Vendor-specific data for different PL031 variants.
#[derive(Copy, Clone)]
struct Pl031Variant {
    variant: VendorVariant,
}

impl Pl031Variant {
    const ARM: Self = Self {
        variant: VendorVariant::Arm,
    };
    const STV1: Self = Self {
        variant: VendorVariant::StV1,
    };
    const STV2: Self = Self {
        variant: VendorVariant::StV2,
    };
}

impl Pl031Variant {
    const fn to_usize(self) -> usize {
        self.variant as usize
    }
}

// Use AMBA device table for matching
kernel::amba_device_table!(
    ID_TABLE,
    MODULE_ID_TABLE,
    <Pl031DrvData as rtc::DriverGeneric<rtc::AmbaBus>>::IdInfo,
    [
        (
            amba::DeviceId::new_with_data(0x00041031, 0x000fffff, Pl031Variant::ARM.to_usize()),
            Pl031Variant::ARM
        ),
        (
            amba::DeviceId::new_with_data(0x00180031, 0x00ffffff, Pl031Variant::STV1.to_usize()),
            Pl031Variant::STV1
        ),
        (
            amba::DeviceId::new_with_data(0x00280031, 0x00ffffff, Pl031Variant::STV2.to_usize()),
            Pl031Variant::STV2
        ),
    ]
);

impl rtc::DriverGeneric<rtc::AmbaBus> for Pl031DrvData {
    type IdInfo = Pl031Variant;

    fn probe(
        adev: &amba::Device<device::Core>,
        id_info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        pin_init::pin_init_scope(move || {
            let io_request = adev.io_request().ok_or(code::ENODEV)?;

            let variant = id_info
                .map(|info| info.variant)
                .unwrap_or(VendorVariant::Arm);

            Ok(try_pin_init!(Self {
                base <- IoMem::new(io_request),
                variant,
                // Set in init_rtcdevice
                rtc_device: None,
            }))
        })
    }

    fn init_rtcdevice(
        rtc: &RtcDevice,
        drvdata: &mut Self,
        id_info: Option<&Self::IdInfo>,
    ) -> Result {
        let parent = rtc.bound_parent_device();
        let amba_dev_bound: &amba::Device<device::Bound> = parent.try_into()?;

        amba_dev_bound.as_ref().init_wakeup()?;

        let variant = id_info
            .map(|info| info.variant)
            .unwrap_or(VendorVariant::Arm);

        // Initialize RTC control register: enable clockwatch (ST variants) or counter (ARM).
        {
            let base_guard = drvdata.base.try_access().ok_or(code::ENXIO)?;
            let base = base_guard.deref();
            let mut data = base.try_read32(RTC_CR)?;
            if variant.clockwatch() {
                data |= RTC_CR_CWEN;
            } else {
                data |= RTC_CR_EN;
            }
            base.try_write32(data, RTC_CR)?;
        }

        rtc.set_range_min(variant.range_min());
        rtc.set_range_max(variant.range_max());

        // Fix ST weekday hardware bug.
        if variant.st_weekday() {
            let base_guard = drvdata.base.try_access().ok_or(code::ENXIO)?;
            let base = base_guard.deref();
            let bcd_year = base.try_read32(RTC_YDR)?;
            if bcd_year == 0x2000 {
                let st_time = base.try_read32(RTC_DR)?;
                if (st_time & (RTC_MON_MASK | RTC_MDAY_MASK | RTC_WDAY_MASK)) == 0x02120000 {
                    let fixed_time = st_time | (0x7 << RTC_WDAY_SHIFT);
                    base.try_write32(0x2000, RTC_YLR)?;
                    base.try_write32(fixed_time, RTC_LR)?;
                }
            }
        }

        // Store RTC device reference for interrupt handler.
        drvdata.rtc_device = Some(ARef::from(rtc));

        // Determine IRQ flags: ST v2 shares IRQ with another block.
        let irq_flags = if variant == VendorVariant::StV2 {
            kernel::irq::Flags::SHARED | kernel::irq::Flags::COND_SUSPEND
        } else {
            kernel::irq::Flags::SHARED
        };

        // Request IRQ (optional, may not be available).
        match amba_dev_bound.request_irq_by_index(
            irq_flags,
            0,
            c_str!("rtc-pl031"),
            try_pin_init!(Pl031IrqHandler {
                _pin: core::marker::PhantomPinned,
            }),
        ) {
            Ok(init) => {
                kernel::devres::register(
                    amba_dev_bound.as_ref(),
                    init,
                    kernel::alloc::flags::GFP_KERNEL,
                )?;

                if let Ok(irq) = amba_dev_bound.irq_by_index(0) {
                    parent.set_wake_irq(irq.irq() as i32)?;
                }
            }
            Err(_) => {
                // IRQ not available - clear alarm feature.
                rtc.clear_feature(bindings::RTC_FEATURE_ALARM);
            }
        }

        Ok(())
    }

    fn rtc_options() -> RtcDeviceOptions {
        RtcDeviceOptions {
            name: c_str!("rtc-pl031"),
        }
    }
}

impl rtc::AmbaIdInfos for Pl031DrvData {
    const ID_TABLE: Option<amba::IdTable<Self::IdInfo>> = Some(&ID_TABLE);
}

#[pinned_drop]
impl PinnedDrop for Pl031DrvData {
    fn drop(self: Pin<&mut Self>) {
        // Resources are automatically cleaned up by devres.
    }
}

/// Converts a Gregorian date to ST v2 RTC format.
fn stv2_tm_to_time(dev: &device::Device, tm: &RtcTime) -> Result<(u32, u32)> {
    let year = tm.tm_year() + 1900;
    let mut wday = tm.tm_wday();

    // Hardware wday masking doesn't work, so wday must be valid.
    if !(-1..=6).contains(&wday) {
        dev_err!(dev, "invalid wday value {}\n", tm.tm_wday());
        return Err(code::EINVAL);
    } else if wday == -1 {
        // wday is not provided, calculate it here.
        let time64 = tm.to_time64();
        let mut calc_tm = RtcTime::default();
        calc_tm.set_from_time64(time64);
        wday = calc_tm.tm_wday();
    }

    // Convert year to BCD.
    let bcd_year =
        (u32::from(bin2bcd((year % 100) as u8))) | (u32::from(bin2bcd((year / 100) as u8)) << 8);

    let st_time = ((tm.tm_mon() + 1) as u32) << RTC_MON_SHIFT
        | (tm.tm_mday() as u32) << RTC_MDAY_SHIFT
        | ((wday + 1) as u32) << RTC_WDAY_SHIFT
        | (tm.tm_hour() as u32) << RTC_HOUR_SHIFT
        | (tm.tm_min() as u32) << RTC_MIN_SHIFT
        | (tm.tm_sec() as u32) << RTC_SEC_SHIFT;

    Ok((st_time, bcd_year))
}

/// Converts ST v2 RTC format to a Gregorian date.
fn stv2_time_to_tm(st_time: u32, bcd_year: u32, tm: &mut RtcTime) {
    let year_low = bcd2bin((bcd_year & 0xFF) as u8);
    let year_high = bcd2bin(((bcd_year >> 8) & 0xFF) as u8);
    tm.set_tm_year(i32::from(year_low) + i32::from(year_high) * 100);
    tm.set_tm_mon((((st_time & RTC_MON_MASK) >> RTC_MON_SHIFT) - 1) as i32);
    tm.set_tm_mday(((st_time & RTC_MDAY_MASK) >> RTC_MDAY_SHIFT) as i32);
    tm.set_tm_wday((((st_time & RTC_WDAY_MASK) >> RTC_WDAY_SHIFT) - 1) as i32);
    tm.set_tm_hour(((st_time & RTC_HOUR_MASK) >> RTC_HOUR_SHIFT) as i32);
    tm.set_tm_min(((st_time & RTC_MIN_MASK) >> RTC_MIN_SHIFT) as i32);
    tm.set_tm_sec(((st_time & RTC_SEC_MASK) >> RTC_SEC_SHIFT) as i32);

    // Values are from valid RTC time structures and are non-negative.
    tm.set_tm_yday(tm.year_days());
    tm.set_tm_year(tm.tm_year() - 1900);
}

/// Converts a binary value to BCD.
fn bin2bcd(val: u8) -> u8 {
    ((val / 10) << 4) | (val % 10)
}

/// Converts a BCD value to binary.
fn bcd2bin(val: u8) -> u8 {
    ((val >> 4) * 10) + (val & 0x0F)
}

/// IRQ handler for PL031 RTC.
#[pin_data]
struct Pl031IrqHandler {
    #[pin]
    _pin: core::marker::PhantomPinned,
}

impl Handler for Pl031IrqHandler {
    fn handle(&self, dev: &device::Device<device::Bound>) -> IrqReturn {
        // Get driver data using drvdata.
        let driver = match dev.drvdata::<Pl031DrvData>() {
            Ok(driver) => driver,
            Err(_) => return IrqReturn::None,
        };

        // Access the MMIO base.
        let base_guard = match driver.base.try_access() {
            Some(guard) => guard,
            None => return IrqReturn::None,
        };
        let base = base_guard.deref();

        // Read masked interrupt status.
        let rtcmis = match base.try_read32(RTC_MIS) {
            Ok(val) => val,
            Err(_) => return IrqReturn::None,
        };

        if (rtcmis & RTC_BIT_AI) != 0 {
            // Clear the interrupt.
            if base.try_write32(RTC_BIT_AI, RTC_ICR).is_err() {
                return IrqReturn::None;
            }

            // Get RTC device from driver and call rtc_update_irq.
            if let Some(rtc) = &driver.rtc_device {
                rtc.update_irq(1, (RTC_AF | RTC_IRQF) as usize);
            }

            return IrqReturn::Handled;
        }

        IrqReturn::None
    }
}

#[vtable]
impl RtcOperations for Pl031DrvData {
    type Ptr = Pin<KBox<Self>>;

    fn read_time(drvdata: Pin<&Self>, tm: &mut RtcTime) -> Result {
        let base_guard = drvdata.base.try_access().ok_or(code::ENXIO)?;
        let base = base_guard.deref();

        match drvdata.variant {
            VendorVariant::Arm | VendorVariant::StV1 => {
                let time32: u32 = base.try_read32(RTC_DR)?;
                let time64 = i64::from(time32);
                tm.set_from_time64(time64);
            }
            VendorVariant::StV2 => {
                let st_time = base.try_read32(RTC_DR)?;
                let bcd_year = base.try_read32(RTC_YDR)?;
                stv2_time_to_tm(st_time, bcd_year, tm);
            }
        }

        Ok(())
    }

    fn set_time(drvdata: Pin<&Self>, tm: &mut RtcTime) -> Result {
        let base_guard = drvdata.base.try_access().ok_or(code::ENXIO)?;
        let base = base_guard.deref();
        let rtc_dev = drvdata.base.device();

        match drvdata.variant {
            VendorVariant::Arm | VendorVariant::StV1 => {
                let time64 = tm.to_time64();
                base.try_write32(time64 as u32, RTC_LR)?;
            }
            VendorVariant::StV2 => {
                let (st_time, bcd_year) = stv2_tm_to_time(rtc_dev, tm)?;
                base.try_write32(bcd_year, RTC_YLR)?;
                base.try_write32(st_time, RTC_LR)?;
            }
        }

        Ok(())
    }

    fn read_alarm(drvdata: Pin<&Self>, alarm: &mut RtcWkAlrm) -> Result {
        let base_guard = drvdata.base.try_access().ok_or(code::ENXIO)?;
        let base = base_guard.deref();

        match drvdata.variant {
            VendorVariant::Arm | VendorVariant::StV1 => {
                let time32: u32 = base.try_read32(RTC_MR)?;
                let time64 = i64::from(time32);
                crate::rtc::RtcTime::time64_to_tm(time64, alarm.get_time_mut());
            }
            VendorVariant::StV2 => {
                let st_time = base.try_read32(RTC_MR)?;
                let bcd_year = base.try_read32(RTC_YMR)?;
                stv2_time_to_tm(st_time, bcd_year, alarm.get_time_mut());
            }
        }

        alarm.set_pending(if (base.try_read32(RTC_RIS)? & RTC_BIT_AI) != 0 {
            1
        } else {
            0
        });
        alarm.set_enabled(if (base.try_read32(RTC_IMSC)? & RTC_BIT_AI) != 0 {
            1
        } else {
            0
        });

        Ok(())
    }

    fn set_alarm(drvdata: Pin<&Self>, alarm: &mut RtcWkAlrm) -> Result {
        let base_guard = drvdata.base.try_access().ok_or(code::ENXIO)?;
        let base = base_guard.deref();
        let rtc_dev = drvdata.base.device();

        match drvdata.variant {
            VendorVariant::Arm | VendorVariant::StV1 => {
                let time64 = alarm.get_time().to_time64();
                base.try_write32(time64 as u32, RTC_MR)?;
            }
            VendorVariant::StV2 => {
                let (st_time, bcd_year) = stv2_tm_to_time(rtc_dev, alarm.get_time())?;
                base.try_write32(bcd_year, RTC_YMR)?;
                base.try_write32(st_time, RTC_MR)?;
            }
        }

        Self::alarm_irq_enable(drvdata, u32::from(alarm.enabled()))
    }

    fn alarm_irq_enable(drvdata: Pin<&Self>, enabled: u32) -> Result {
        let base_guard = drvdata.base.try_access().ok_or(code::ENXIO)?;
        let base = base_guard.deref();

        // Clear any pending alarm interrupts.
        base.try_write32(RTC_BIT_AI, RTC_ICR)?;

        let mut imsc = base.try_read32(RTC_IMSC)?;
        if enabled == 1 {
            imsc |= RTC_BIT_AI;
        } else {
            imsc &= !RTC_BIT_AI;
        }
        base.try_write32(imsc, RTC_IMSC)?;

        Ok(())
    }
}

kernel::module_rtc_driver! {
    bus: AmbaBus,
    type: Pl031DrvData,
    name: "rtc-pl031-rust",
    authors: ["Ke Sun <sunke@kylinos.cn>"],
    description: "Rust PL031 RTC driver",
    license: "GPL v2",
}
