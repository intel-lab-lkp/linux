// SPDX-License-Identifier: GPL-2.0

//! Rust virtio driver sample.

use core::{
    ptr::NonNull, //
    sync::atomic::{
        AtomicU16,
        Ordering, //
    },
};

use kernel::{
    device::{
        Bound,
        Core, //
    },
    new_mutex,    //
    new_spinlock, //
    prelude::*,
    sync::{
        Completion,
        Mutex,
        SpinLock, //
    },
    virtio::{
        self,
        utils::*,
        virtqueue::*, //
    },
};

use pin_init::{
    stack_pin_init,
    stack_try_pin_init, //
};

#[pin_data]
struct Token {
    resp_actual_size: u32,
    #[pin]
    responded: Completion,
}

#[derive(Copy, Clone, Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_req_head")]
struct ReqHead {
    msg_type: Le16,
    reserved: [u8; 6],
}

#[derive(Copy, Clone, Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_resp_head")]
struct RespHead {
    status: u8,
    reserved: [u8; 7],
}

#[derive(Copy, Clone, Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_resp_cfg")]
struct RespCfg {
    head: RespHead,
    /** # of clocks -> clock ids < num_clocks are valid */
    num_clocks: Le16,
    reserved: [u8; 6],
}

#[derive(Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_req_clock_cap")]
struct ReqClockCap {
    head: ReqHead,
    clock_id: Le16,
    reserved: [u8; 6],
}

#[derive(Copy, Clone, Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_resp_clock_cap")]
struct RespClockCap {
    head: RespHead,
    clock_type: u8,
    leap_second_smearing: u8,
    flags: u8,
    reserved: [u8; 5],
}

#[derive(Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_req_read")]
struct ReqRead {
    head: ReqHead,
    clock_id: Le16,
    reserved: [u8; 6],
}

#[derive(Copy, Clone, Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_resp_read")]
struct RespRead {
    head: RespHead,
    clock_reading: Le64,
}

#[repr(u8)]
enum ClockType {
    #[doc(alias = "VIRTIO_RTC_CLOCK_UTC")]
    Utc = 0,
    #[doc(alias = "VIRTIO_RTC_CLOCK_TAI")]
    Tai = 1,
    #[doc(alias = "VIRTIO_RTC_CLOCK_MONOTONIC")]
    Monotonic = 2,
    #[doc(alias = "VIRTIO_RTC_CLOCK_UTC_SMEARED")]
    UtcSmeared = 3,
    #[doc(alias = "VIRTIO_RTC_CLOCK_UTC_MAYBE_SMEARED")]
    UtcMaybeSmeared = 4,
}

/// Send a message and receive reply
fn send<Request: Zeroable + 'static, Response: Zeroable + Copy + 'static>(
    req_data: Request,
    vq: &SpinLock<VirtioRtcVq>,
    timeout_jiffies: c_ulong,
) -> Result<Response> {
    // FIXME: This lock should also disable irqs.
    let guard = vq.lock();

    let req = VBox::<Request>::new(req_data, GFP_KERNEL)?;
    let mut resp = VBox::<Response>::new_uninit(GFP_KERNEL)?;
    let resp_ptr = NonNull::new(resp.as_mut_ptr()).unwrap();

    stack_pin_init!(let token = pin_init!(Token {
        resp_actual_size: 0,
        responded <- Completion::new(),
    }));
    stack_try_pin_init!(let req_sgs = guard.reqvq().new_readable_sgtable(req, GFP_KERNEL));
    let req_sgs: Pin<&mut _> = req_sgs?;
    stack_try_pin_init!(let resp_sgs = guard.reqvq().new_writable_sgtable(resp, GFP_KERNEL));
    let resp_sgs: Pin<&mut _> = resp_sgs?;

    guard
        .reqvq()
        .add_sgs(&req_sgs, &resp_sgs, token.as_ref(), GFP_ATOMIC)?;

    if guard.reqvq().kick_prepare() {
        guard.reqvq().notify();
    }
    drop(guard);

    if timeout_jiffies > 0 {
        token
            .responded
            .wait_for_completion_interruptible_timeout(timeout_jiffies)?;
    } else {
        token.responded.wait_for_completion_interruptible()?;
    }

    if token.resp_actual_size as usize >= core::mem::size_of::<RespHead>() {
        // SAFETY: all response types contain a `RespHead` header at the start.
        let head: &RespHead = unsafe { resp_ptr.cast().as_ref() };
        match head.status {
            0 => {
                // OK, do nothing.
            }
            1 => return Err(ENOTSUPP),
            2 => return Err(ENODEV),
            3 => return Err(EINVAL),
            4 | 5_u8..=u8::MAX => return Err(EIO),
        }
    } else if token.resp_actual_size as usize != core::mem::size_of::<Response>() {
        return Err(EINVAL);
    }
    // SAFETY: we have checked that the device wrote the correct amount of bytes for this type and
    // has returned a successful status code.
    let resp = unsafe { *resp_ptr.as_ref() };

    Ok(resp)
}

const VIRTIO_RTC_REQ_READ: u16 = 0x0001;
const VIRTIO_RTC_REQ_CFG: u16 = 0x1000;
const VIRTIO_RTC_REQ_CLOCK_CAP: u16 = 0x1001;

struct VirtioRtcVq {
    inner: Virtqueues,
}

// SAFETY: `VirtioRtcVq` is safe to be send to any task.
unsafe impl Send for VirtioRtcVq {}

impl VirtioRtcVq {
    fn new(inner: Virtqueues) -> impl PinInit<SpinLock<Self>> {
        new_spinlock!(Self { inner })
    }

    fn reqvq(&self) -> &Virtqueue {
        unsafe { self.inner[0].as_ref() }
    }
}

#[pin_data(PinnedDrop)]
struct VirtioRtcDriver {
    #[pin]
    virtqueues: SpinLock<VirtioRtcVq>,
    num_clocks: AtomicU16,
    #[pin]
    registered_clocks: Mutex<KVec<()>>,
}

#[pinned_drop]
impl PinnedDrop for VirtioRtcDriver {
    fn drop(self: Pin<&mut Self>) {
        pr_info!("Remove Rust virtio driver sample.\n");
    }
}

extern "C" fn vq_requestq_callback(vq: *mut kernel::bindings::virtqueue) {
    // SAFETY: The kernel called this virtqueue callback and it must have provided a valid `vq`
    // pointer
    let vq = unsafe { Virtqueue::from_raw(vq) };
    let dev: &virtio::Device<Bound> = vq.dev().expect("Could not get device");
    let data = dev
        .as_ref()
        .drvdata::<VirtioRtcDriver>()
        .expect("Could not borrow drvdata");
    data.process_requestq();
}

impl VirtioRtcDriver {
    /// Submit `VIRTIO_RTC_REQ_CFG` and return response (`num_clocks`)
    fn req_cfg(&self) -> Result<u16> {
        let head = ReqHead {
            msg_type: VIRTIO_RTC_REQ_CFG.into(),
            reserved: [0; 6],
        };
        let response: RespCfg = send(head, &self.virtqueues, 0)?;
        pr_info!("Got response! {response:?}\n");

        Ok(response.num_clocks.into())
    }

    fn process_requestq(&self) {
        let mut cb_enabled = true;
        loop {
            // FIXME: This lock should also disable irqs.
            let guard = self.virtqueues.lock();
            if cb_enabled {
                guard.reqvq().disable_cb();
                cb_enabled = false;
            }
            if let Some((token, len)) = guard.reqvq().get_buf() {
                drop(guard);
                pr_info!("process_requestq got buf {len} bytes\n");
                let mut token = token.cast::<Token>();
                // SAFETY: pointer points to a valid Token that we have added to the virtqueue.
                let token_ref = unsafe { token.as_mut() };
                token_ref.resp_actual_size = len;
                token_ref.responded.complete_all();
                pr_info!("process_requestq ok\n");
            } else {
                if guard.reqvq().enable_cb() {
                    return;
                }
                cb_enabled = true;
            }
        }
    }

    fn clock_cap(&self, clock_id: u16) -> Result<RespClockCap> {
        let req = ReqClockCap {
            head: ReqHead {
                msg_type: VIRTIO_RTC_REQ_CLOCK_CAP.into(),
                reserved: [0; 6],
            },
            clock_id: clock_id.into(),
            reserved: [0; 6],
        };
        let response: RespClockCap = send(req, &self.virtqueues, 0)?;
        pr_info!("Got response: {response:?}\n");
        Ok(response)
    }

    fn read(&self, clock_id: u16) -> Result<u64> {
        let req = ReqRead {
            head: ReqHead {
                msg_type: VIRTIO_RTC_REQ_READ.into(),
                reserved: [0; 6],
            },
            clock_id: clock_id.into(),
            reserved: [0; 6],
        };
        let response: RespRead = send(req, &self.virtqueues, 0)?;
        pr_info!("Got response: {response:?}\n");
        Ok(response.clock_reading.into())
    }
}

impl virtio::Driver for VirtioRtcDriver {
    type IdInfo = ();

    /// The table of device ids supported by the driver.
    const ID_TABLE: virtio::IdTable<Self::IdInfo> = &VIRTIO_RTC_TABLE;

    fn probe(vdev: &virtio::Device<Core>) -> impl PinInit<Self, Error> {
        let vqs_info: [VirtqueueInfo; 1] = [
            VirtqueueInfo::new(c"requestq", false, Some(vq_requestq_callback)),
            //VirtqueueInfo::new(c"alarmq", false, vq_callback),
        ];
        try_pin_init!(Self {
            num_clocks: AtomicU16::new(0),
            virtqueues <- {
                pr_info!("Probe Rust virtio driver sample.\n");
                let vqs = match vdev.find_vqs(&vqs_info) {
                    Ok(vqs) => {
                        pr_info!("Found {} vqs.\n", vqs.len());
                        vqs
                    }
                    Err(err) => {
                        pr_info!("Could not find vqs: {err:?}.\n");

                        return Err(err);
                    }
                };

                VirtioRtcVq::new(vqs)
            },
            registered_clocks <- new_mutex!(KVec::with_capacity(0, GFP_KERNEL)?),
        })
    }

    fn init(&self, vdev: &virtio::Device<Bound>) -> Result {
        let num_clocks = self.req_cfg()?;
        self.num_clocks.store(num_clocks, Ordering::SeqCst);
        for i in 0..num_clocks {
            let mut is_exposed = false;

            let resp = self.clock_cap(i)?;
            let (clock_type, leap_second_smearing, flags) =
                (resp.clock_type, resp.leap_second_smearing, resp.flags);
            if cfg!(CONFIG_VIRTIO_RTC_CLASS)
                && (clock_type == ClockType::Utc as u8
                    || clock_type == ClockType::UtcSmeared as u8
                    || clock_type == ClockType::UtcMaybeSmeared as u8)
            {
                // TODO:

                // 	ret = viortc_init_rtc_class_clock(viortc, vio_clk_id,
                // 					  clock_type, flags);
                // 	if (ret < 0)
                // 		return ret;
                // 	if (ret > 0)
                // 		is_exposed = true;
                dev_warn!(vdev.as_ref(), "CONFIG_VIRTIO_RTC_CLASS TODO ");
            }

            if cfg!(CONFIG_VIRTIO_RTC_PTP) {
                // TODO:

                // 	ret = viortc_init_ptp_clock(viortc, vio_clk_id, clock_type,
                // 				    leap_second_smearing);
                // 	if (ret < 0)
                // 		return ret;
                // 	if (ret > 0)
                // 		is_exposed = true;
                // todo!()
                dev_warn!(vdev.as_ref(), "CONFIG_VIRTIO_RTC_PTP TODO ");
            }

            if !is_exposed {
                dev_warn!(
                    vdev.as_ref(),
                    "cannot expose clock {i} (type {clock_type}, variant {leap_second_smearing}, \
                    flags {flags}) to userspace\n"
                );
            }
            let clock_reading = self.read(i)?;
            pr_info!("#{i} clock reading = {clock_reading}\n");
        }
        Ok(())
    }

    fn remove(_: &virtio::Device<Core>, _this: Pin<&Self>) {
        pr_info!("Removing Rust virtio driver sample.\n");
    }
}

kernel::virtio_device_table!(
    VIRTIO_RTC_TABLE,
    MODULE_VIRTIO_RTC_TABLE,
    <VirtioRtcDriver as virtio::Driver>::IdInfo,
    [(virtio::DeviceId::new(virtio::VirtioID::Clock), ())]
);

kernel::module_virtio_driver! {
    type: VirtioRtcDriver,
    name: "rust_virtio_rtc",
    authors: ["Manos Pitsidianakis"],
    description: "Rust virtio driver",
    license: "GPL v2",
}
