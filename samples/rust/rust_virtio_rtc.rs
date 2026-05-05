// SPDX-License-Identifier: GPL-2.0

//! Rust virtio driver sample.

use core::{
    cell::Cell,
    marker::PhantomData,
    ptr::NonNull, //
};

use kernel::{
    device::{
        Core,
        CoreInternal, //
    },
    new_mutex, new_spinlock, page,
    prelude::*,
    scatterlist::SGEntry,
    sync::Completion,
    sync::{Mutex, SpinLock},
    virtio::{
        self,
        utils::*,
        virtqueue::*, //
    },
};

use pin_init::stack_try_pin_init;

#[pin_data]
struct Token {
    resp_actual_size: u32,
    #[pin]
    responded: Completion,
}

#[pin_data]
struct Message<Request: Zeroable, Response: Zeroable> {
    msg_type: u16,
    #[pin]
    req: KVec<u8>,
    #[pin]
    resp: KVec<u8>,
    req_ptr: NonNull<Request>,
    resp_ptr: NonNull<Response>,
    #[pin]
    token: Token,
    _ph_req: PhantomData<Request>,
    _ph_resp: PhantomData<Response>,
}

#[derive(Copy, Clone, Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_req_head")]
struct ReqHead {
    msg_type: Le16,
    reserved: [u8; 6],
}

#[derive(Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_resp_head")]
struct RespHead {
    status: u8,
    reserved: [u8; 7],
}

#[derive(Debug, Zeroable)]
#[repr(C)]
#[doc(alias = "virtio_rtc_resp_cfg")]
struct RespCfg {
    head: ReqHead,
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
    head: ReqHead,
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
    head: ReqHead,
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

// SAFETY: `Message` is safe to be send to any task.
unsafe impl<Request: Zeroable, Response: Zeroable> Send for Message<Request, Response> {}

// SAFETY: `Message` is safe to be accessed concurrently.
unsafe impl<Request: Zeroable, Response: Zeroable> Sync for Message<Request, Response> {}

impl<Request: Zeroable, Response: Zeroable> Message<Request, Response> {
    /// Create an initializer for a new [`Message`].
    fn new(req_data: Request, msg_type: u16) -> Result<impl PinInit<Self, Error>> {
        macro_rules! alloc_buf {
            ($t:ty) => {{
                let size = (core::mem::size_of::<$t>() / page::PAGE_SIZE + 1) * page::PAGE_SIZE;
                KVec::<u8>::with_capacity(size, GFP_KERNEL)
            }};
        }
        let mut req = alloc_buf!(Request)?;
        let mut resp = alloc_buf!(Response)?;
        let req_ptr: NonNull<Request> = NonNull::new(req.as_mut_ptr().cast()).unwrap();
        let resp_ptr = NonNull::new(resp.as_mut_ptr().cast()).unwrap();
        // SAFETY: `req_ptr` is a valid Request allocation
        unsafe {
            core::ptr::write(req_ptr.as_ptr(), req_data);
        }
        Ok(pin_init!(Self {
            req,
            resp,
            msg_type,
            req_ptr,
            resp_ptr,
            token <- pin_init!(Token {
                resp_actual_size: 0,
                responded <- Completion::new(),
            }),
            _ph_req: PhantomData,
            _ph_resp: PhantomData,
        }? Error))
    }

    fn get_response(&self) -> Result<&Response, Error> {
        if self.token.resp_actual_size as usize != core::mem::size_of::<Response>() {
            if self.token.resp_actual_size as usize >= core::mem::size_of::<RespHead>() {
                let head: &RespHead = unsafe { self.resp_ptr.cast().as_ref() };
                return match head.status {
                    0 | 3 => Err(EINVAL),
                    1 => Err(ENOTSUPP),
                    2 => Err(ENODEV),
                    4 | 5_u8..=u8::MAX => Err(EIO),
                };
            }
            return Err(EINVAL);
        }
        Ok(unsafe { self.resp_ptr.as_ref() })
    }

    fn send(&self, vq: &SpinLock<VirtioRtcVq>, timeout_jiffies: c_ulong) -> Result {
        let guard = vq.lock();

        let mut sg_in = core::mem::MaybeUninit::zeroed();
        let mut sg_out = core::mem::MaybeUninit::zeroed();
        let req = unsafe {
            SGEntry::init_one(
                &mut sg_out,
                self.req_ptr.cast(),
                core::mem::size_of::<Request>() as u32,
            )
        };
        let resp = unsafe {
            SGEntry::init_one(
                &mut sg_in,
                self.resp_ptr.cast(),
                core::mem::size_of::<Response>() as u32,
            )
        };
        let sgs = [req, resp];
        guard.as_ref().add_sgs(
            &sgs,
            1,
            1,
            (&raw const self.token).cast_mut().cast(),
            GFP_ATOMIC,
        )?;

        if guard.as_ref().kick_prepare() {
            guard.as_ref().notify();
        }
        drop(guard);

        if timeout_jiffies > 0 {
            self.token
                .responded
                .wait_for_completion_interruptible_timeout(timeout_jiffies)?;
        } else {
            self.token.responded.wait_for_completion_interruptible()?;
        }
        Ok(())
    }
}

// TODO: use a proper enum

const VIRTIO_RTC_REQ_READ: u16 = 0x0001;
const VIRTIO_RTC_REQ_CFG: u16 = 0x1000;
const VIRTIO_RTC_REQ_CLOCK_CAP: u16 = 0x1001;

struct VirtioRtcVq {
    ptr: NonNull<Virtqueue>,
}

// SAFETY: `VirtioRtcVq` is safe to be send to any task.
unsafe impl Send for VirtioRtcVq {}

impl VirtioRtcVq {
    fn new(ptr: *mut Virtqueue) -> impl PinInit<SpinLock<Self>> {
        let ptr = NonNull::new(ptr).unwrap();
        new_spinlock!(Self { ptr })
    }

    fn as_ref(&self) -> &Virtqueue {
        unsafe { self.ptr.as_ref() }
    }
}

struct VirtioRtcDriver {
    reqvq: Pin<KBox<SpinLock<VirtioRtcVq>>>,
    alarmvq: Option<Pin<KBox<SpinLock<VirtioRtcVq>>>>,
    num_clocks: Cell<u16>,
    registered_clocks: Pin<KBox<Mutex<KVec<()>>>>,
}

impl Drop for VirtioRtcDriver {
    fn drop(&mut self) {
        pr_info!("Remove Rust virtio driver sample.\n");
    }
}

unsafe extern "C" fn vq_requestq_callback(vq: *mut kernel::bindings::virtqueue) {
    let vq = unsafe { Virtqueue::from_raw(vq) };
    let dev: &virtio::Device<CoreInternal> = vq.dev();
    let data = unsafe { dev.as_ref().drvdata_borrow::<VirtioRtcDriver>() };
    data.process_requestq();
}

impl VirtioRtcDriver {
    /// Submit `VIRTIO_RTC_REQ_CFG` and return response (`num_clocks`)
    fn req_cfg(&self) -> Result<u16> {
        let head = ReqHead {
            msg_type: VIRTIO_RTC_REQ_CFG.into(),
            reserved: [0; 6],
        };
        stack_try_pin_init!(
            let msg: Message::<ReqHead, RespCfg> =
                Message::new(head, VIRTIO_RTC_REQ_CFG)?);
        let msg: core::pin::Pin<&mut Message<ReqHead, RespCfg>> = msg?;
        msg.send(&self.reqvq, 0)?;
        pr_info!("Got response! {:?}\n", msg.get_response());

        let response: &RespCfg = msg.get_response()?;
        Ok(response.num_clocks.into())
    }

    fn process_requestq(&self) {
        let mut cb_enabled = true;
        loop {
            let guard = self.reqvq.lock();
            if cb_enabled {
                guard.as_ref().disable_cb();
                cb_enabled = false;
            }
            if let Some((token, len)) = guard.as_ref().get_buf() {
                drop(guard);
                pr_info!("process_requestq got buf {len} bytes\n");
                let mut token = token.cast::<Token>();

                unsafe { token.as_mut().resp_actual_size = len };
                unsafe { token.as_mut().responded.complete_all() };
            } else {
                if guard.as_ref().enable_cb() {
                    return;
                }
                cb_enabled = true;
            }
        }
    }

    fn clock_cap(&self, clock_id: u16) -> Result<RespClockCap> {
        type ClockCapMsg = Message<ReqClockCap, RespClockCap>;

        let req = ReqClockCap {
            head: ReqHead {
                msg_type: VIRTIO_RTC_REQ_CLOCK_CAP.into(),
                reserved: [0; 6],
            },
            clock_id: clock_id.into(),
            reserved: [0; 6],
        };
        stack_try_pin_init!(
            let msg: ClockCapMsg = Message::new(req, VIRTIO_RTC_REQ_CLOCK_CAP)?
        );
        let msg: core::pin::Pin<&mut ClockCapMsg> = msg?;
        msg.send(&self.reqvq, 0)?;
        pr_info!("Got response! {:?}\n", msg.get_response());
        let response: &RespClockCap = msg.get_response()?;
        Ok(*response)
    }

    fn read(&self, clock_id: u16) -> Result<u64> {
        type ReadMsg = Message<ReqRead, RespRead>;

        let req = ReqRead {
            head: ReqHead {
                msg_type: VIRTIO_RTC_REQ_READ.into(),
                reserved: [0; 6],
            },
            clock_id: clock_id.into(),
            reserved: [0; 6],
        };
        stack_try_pin_init!(
            let msg: ReadMsg = Message::new(req, VIRTIO_RTC_REQ_CLOCK_CAP)?
        );
        let msg: core::pin::Pin<&mut ReadMsg> = msg?;
        msg.send(&self.reqvq, 0)?;
        pr_info!("Got response! {:?}\n", msg.get_response());
        let response: &RespRead = msg.get_response()?;
        Ok(response.clock_reading.into())
    }
}

impl virtio::Driver for VirtioRtcDriver {
    type IdInfo = ();

    /// The table of device ids supported by the driver.
    const ID_TABLE: virtio::IdTable<Self::IdInfo> = &VIRTIO_RTC_TABLE;

    fn probe(vdev: &virtio::Device<Core>) -> impl PinInit<Self, Error> {
        const VQS_INFO: [VirtqueueInfo; 1] = [
            VirtqueueInfo::new(c"requestq", false, vq_requestq_callback),
            //VirtqueueInfo::new(c"alarmq", false, vq_callback),
        ];
        let init_fn = move |slot: *mut Self| {
            pr_info!("Probe Rust virtio driver sample.\n");
            let vqs = match vdev.find_vqs(&VQS_INFO) {
                Ok(vqs) => {
                    pr_info!("Found {} vqs.\n", vqs.len());
                    vqs
                }
                Err(err) => {
                    pr_info!("Could not find vqs: {err:?}.\n");

                    return Err(err);
                }
            };
            let reqvq = KBox::pin_init(VirtioRtcVq::new(vqs[0]), GFP_ATOMIC)?;
            let registered_clocks =
                KBox::pin_init(new_mutex!(KVec::with_capacity(0, GFP_KERNEL)?), GFP_KERNEL)?;
            unsafe {
                core::ptr::write(
                    slot,
                    Self {
                        num_clocks: Cell::new(0),
                        reqvq,
                        alarmvq: None,
                        registered_clocks,
                    },
                )
            };
            Ok(())
        };
        unsafe { pin_init::pin_init_from_closure(init_fn) }
    }

    fn init(&self, vdev: &virtio::Device<Core>) -> Result {
        vdev.ready();
        self.num_clocks.set(self.req_cfg()?);
        for i in 0..(self.num_clocks.get()) {
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

    fn remove(vdev: &virtio::Device, _this: Pin<&Self>) {
        pr_info!("Removing Rust virtio driver sample.\n");
        vdev.reset();
        vdev.del_vqs();
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
