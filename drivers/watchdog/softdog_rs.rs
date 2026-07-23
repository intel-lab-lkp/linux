// SPDX-License-Identifier: GPL-2.0

//! Rust software watchdog driver.
//!
//! A software watchdog implemented with an hrtimer: when the timer expires
//! before the next keepalive ping, the system is restarted.
//!
//! C version of this driver:
//! [`drivers/watchdog/softdog.c`](srctree/drivers/watchdog/softdog.c)

use kernel::{
    impl_has_hr_timer, new_mutex,
    prelude::*,
    reboot,
    sync::{Arc, ArcBorrow, Mutex},
    time::{
        hrtimer::{
            ArcHrTimerHandle, HrTimer, HrTimerCallback, HrTimerCallbackContext, HrTimerPointer,
            HrTimerRestart, RelativeMode,
        },
        Delta, Monotonic,
    },
    watchdog::{self, flags},
};

const DEFAULT_MARGIN: u32 = 60;
const MAX_MARGIN: u32 = 65535;

module! {
    type: SoftdogModule,
    name: "softdog_rs",
    authors: ["Artem Lytkin"],
    description: "Rust Software Watchdog Device Driver",
    license: "GPL",
}

/// The countdown state: the hrtimer and the handle of its last arming.
///
/// Watchdog callbacks may run concurrently (for example the reboot notifier
/// `stop` against an in-flight ioctl), so the handle is protected by a
/// mutex.
#[pin_data]
struct Softdog {
    #[pin]
    timer: HrTimer<Self>,
    #[pin]
    handle: Mutex<Option<ArcHrTimerHandle<Softdog>>>,
}

impl Softdog {
    fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            timer <- HrTimer::new(),
            handle <- new_mutex!(None),
        })
    }

    /// (Re)arms the countdown to fire in `timeout` seconds.
    fn arm(this: &Arc<Self>, timeout: u32) {
        let mut guard = this.handle.lock();
        // Drop the previous handle first: dropping a handle cancels the
        // timer, so this must not happen after the new arming.
        *guard = None;
        *guard = Some(this.clone().start(Delta::from_secs(i64::from(timeout))));
    }

    /// Cancels the countdown.
    fn disarm(this: &Arc<Self>) {
        // Dropping the handle cancels the timer and also breaks the
        // reference cycle `Softdog -> handle -> Arc<Softdog>`.
        *this.handle.lock() = None;
    }
}

impl_has_hr_timer! {
    impl HasHrTimer<Self> for Softdog {
        mode: RelativeMode<Monotonic>, field: self.timer
    }
}

impl HrTimerCallback for Softdog {
    type Pointer<'a> = Arc<Self>;

    fn run(_this: ArcBorrow<'_, Self>, _ctx: HrTimerCallbackContext<'_, Self>) -> HrTimerRestart {
        pr_crit!("Initiating system reboot\n");
        reboot::emergency_restart();
        // Only reached if the machine failed to restart.
        HrTimerRestart::NoRestart
    }
}

struct SoftdogOps;

#[vtable]
impl watchdog::WatchdogOps for SoftdogOps {
    type Data = Arc<Softdog>;

    fn start(dev: &watchdog::Device, data: &Arc<Softdog>) -> Result {
        Softdog::arm(data, dev.timeout());
        Ok(())
    }

    fn ping(dev: &watchdog::Device, data: &Arc<Softdog>) -> Result {
        Softdog::arm(data, dev.timeout());
        Ok(())
    }

    fn stop(_dev: &watchdog::Device, data: &Arc<Softdog>) -> Result {
        Softdog::disarm(data);
        Ok(())
    }
}

static SOFTDOG_INFO: watchdog::Info = watchdog::Info::new(
    flags::SETTIMEOUT | flags::KEEPALIVEPING | flags::MAGICCLOSE,
    "Rust Software Watchdog",
);

struct SoftdogModule {
    _reg: watchdog::Registration<SoftdogOps>,
}

impl kernel::Module for SoftdogModule {
    fn init(module: &'static ThisModule) -> Result<Self> {
        let data = Arc::pin_init(Softdog::new(), GFP_KERNEL)?;

        let options = watchdog::Options {
            timeout: DEFAULT_MARGIN,
            min_timeout: 1,
            max_timeout: MAX_MARGIN,
            // Stop the countdown on reboot so that an orderly reboot is not
            // interrupted by the watchdog firing, like the C softdog does.
            stop_on_reboot: true,
            ..Default::default()
        };

        let reg = watchdog::Registration::register(module, None, &SOFTDOG_INFO, &options, data)?;

        pr_info!("initialized (timeout={}s)\n", DEFAULT_MARGIN);

        Ok(SoftdogModule { _reg: reg })
    }
}
