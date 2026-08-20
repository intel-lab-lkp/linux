// SPDX-License-Identifier: GPL-2.0

//! Greybus software SVC.
//!
//! A Greybus network needs an SVC to bring interfaces up and to connect their CPorts to the AP.
//! Transports that carry Greybus without one - a UART or an I2C bus rather than a UniPro network -
//! have to provide it themselves.
//!
//! This module implements that SVC in software.

use kernel::error::code;
use kernel::greybus::{self, hd, protocols, CPORT_ID_MAX, GB_OPERATION_SIZE_MAX};
use kernel::id_pool::IdPool;
use kernel::sync::{Arc, Mutex, SpinLock};
use kernel::transmute::AsBytes;
use kernel::{c_str, faux, prelude::*, sync::aref::ARef};
use kernel::{new_mutex, new_spinlock};
use pin_init::init_array_from_fn;

const SVC_VERSION_MAJOR: u8 = 0;
const SVC_VERSION_MINOR: u8 = 1;
const ENDO_ID: u16 = u16::from_le(0x4755);
const AP_INF_ID: u8 = 0;

const MAX_INTF_ID: u8 = u8::MAX;
const INTF_ID_START: u8 = 1;
const INTF_MAP_LEN: usize = (MAX_INTF_ID - INTF_ID_START) as usize;

#[repr(C, packed)]
struct Msg<P> {
    hdr: protocols::GbOperationMsgHdr,
    payload: P,
}

// SAFETY: `Msg` is a POD type with no padding and no interior mutability.
unsafe impl<P> kernel::transmute::AsBytes for Msg<P> {}

impl<P> Msg<P> {
    const GB_SIZE: u16 = size_of::<Self>() as u16;

    /// Builds a request of type `ty`.
    ///
    /// An `operation_id` of zero marks a unidirectional request, for which the AP sends no
    /// response.
    const fn request(operation_id: u16, ty: u8, payload: P) -> Self {
        Self {
            hdr: protocols::GbOperationMsgHdr::new(Self::GB_SIZE, operation_id, ty, 0),
            payload,
        }
    }

    /// Builds the response to `request`, carrying `result`.
    const fn response(request: &protocols::GbOperationMsgHdr, result: u8, payload: P) -> Self {
        Self {
            hdr: protocols::GbOperationMsgHdr::new(
                Self::GB_SIZE,
                request.operation_id(),
                request.msg_type() | protocols::MESSAGE_TYPE_RESPONSE,
                result,
            ),
            payload,
        }
    }
}

const _: () = assert!(size_of::<Msg<()>>() == size_of::<protocols::GbOperationMsgHdr>());

module! {
    type: GreybusSoftSvc,
    name: "gb_softsvc",
    authors: ["Ayush Singh <ayush@beagleboard.org>"],
    description: "Greybus software SVC implementation",
    license: "GPL",
}

kernel::sync::global_lock! {
    unsafe(uninit) static GLOBAL_STATE: Mutex<Option<GlobalState>> = None;
}

struct GlobalState {
    svc: Arc<Svc>,
    gb_hd: ARef<hd::Device>,
}

impl GlobalState {
    fn new(svc: Arc<Svc>, gb_hd: ARef<hd::Device>) -> Self {
        Self { svc, gb_hd }
    }
}

#[pin_data]
struct Svc {
    #[pin]
    svc_operation_id: SpinLock<IdPool>,
    // Pos 0 = INTF_ID_START
    #[pin]
    intf_map: Mutex<[Option<Arc<dyn InterfaceOps>>; INTF_MAP_LEN]>,
}

impl Svc {
    fn new() -> impl PinInit<Self, Error> {
        pin_init!(Self {
            intf_map <- new_mutex!(init_array_from_fn(|_| None)),
            svc_operation_id <- new_spinlock!(IdPool::with_capacity(u16::MAX.into(), GFP_KERNEL)?)
        }? Error)
    }

    fn new_operation_id(&self) -> Result<u16> {
        let mut ida = self.svc_operation_id.lock();
        let res = ida.find_unused_id(1).ok_or(code::EOVERFLOW)?.acquire() as u16;

        Ok(res)
    }

    fn release_operation_id(&self, id: u16) {
        let mut ida = self.svc_operation_id.lock();
        ida.release_id(id as usize);
    }

    fn map_insert(&self, intfs: &[Arc<dyn InterfaceOps>]) -> Result<u8> {
        fn inner(
            intf_map: &[Option<Arc<dyn InterfaceOps>>; INTF_MAP_LEN],
            count: usize,
        ) -> Result<u8> {
            for (id, window) in intf_map.windows(count).enumerate() {
                if window.iter().all(|x| x.is_none()) {
                    return Ok(id as u8);
                }
            }

            Err(code::EOVERFLOW)
        }

        let mut guard = self.intf_map.lock();

        let pos = inner(&guard, intfs.len())?;
        for (i, intf) in intfs.iter().cloned().enumerate() {
            guard[usize::from(pos) + i] = Some(intf);
        }

        Ok(pos + INTF_ID_START)
    }

    fn map_remove(&self, primary_id: u8, intf_count: u8) {
        let id = primary_id - INTF_ID_START;
        let mut guard = self.intf_map.lock();

        for i in 0..intf_count {
            let _ = guard[usize::from(id + i)].take();
        }
    }

    fn intf_by_id(&self, id: u8) -> Result<Arc<dyn InterfaceOps>> {
        let id = id - INTF_ID_START;
        let guard = self.intf_map.lock();

        guard
            .get(usize::from(id))
            .ok_or(code::ENODEV)?
            .as_ref()
            .ok_or(code::ENODEV)
            .cloned()
    }

    fn send_request<P>(&self, hd: &hd::Device, ty: u8, payload: P) -> Result<()> {
        let msg = Msg::request(self.new_operation_id()?, ty, payload);
        hd.data_rcvd(protocols::GB_SVC_CPORT_ID, msg.as_bytes());
        Ok(())
    }

    fn module_insert(&self, hd: &hd::Device, intfs: &[Arc<dyn InterfaceOps>]) -> Result<u8> {
        let intf_id = self.map_insert(intfs)?;
        let payload = protocols::GbSvcModuleInsertedRequest::new(intf_id, 1, 0);
        self.send_request(hd, protocols::GB_SVC_TYPE_MODULE_INSERTED, payload)?;

        Ok(intf_id)
    }

    fn module_remove(&self, hd: &hd::Device, intf_id: u8, intf_count: u8) -> Result<()> {
        self.map_remove(intf_id, intf_count);
        let payload = protocols::GbSvcModuleRemovedRequest::new(intf_id);
        self.send_request(hd, protocols::GB_SVC_TYPE_MODULE_REMOVED, payload)?;

        Ok(())
    }

    fn send_version(&self, hd: &hd::Device) -> Result<()> {
        dev_info!(hd.as_ref(), "Sending SVC version request");

        let payload = protocols::GbSvcVersionRequest::new(SVC_VERSION_MAJOR, SVC_VERSION_MINOR);
        self.send_request(hd, protocols::GB_SVC_TYPE_PROTOCOL_VERSION, payload)?;

        Ok(())
    }

    fn send_svc_hello(&self, hd: &hd::Device) -> Result<()> {
        dev_info!(hd.as_ref(), "Sending SVC Hello request");

        let payload = protocols::GbSvcHelloRequest::new(ENDO_ID, AP_INF_ID);
        self.send_request(hd, protocols::GB_SVC_TYPE_SVC_HELLO, payload)?;

        Ok(())
    }

    fn send_response<P>(&self, hd: &hd::Device, msg: &protocols::GbOperationMsgHdr, payload: P) {
        let msg = Msg::response(msg, 0, payload);
        hd.data_rcvd(protocols::GB_SVC_CPORT_ID, msg.as_bytes());
    }

    fn intf_set_pwrm(&self, hd: &hd::Device, msg: &greybus::Message) -> Result<()> {
        let req_msg: &protocols::GbSvcIntfSetPwrmRequest = msg.payload().ok_or(code::EINVAL)?;
        let result_code = if req_msg.tx_mode() == protocols::GB_SVC_UNIPRO_HIBERNATE_MODE
            && req_msg.rx_mode() == protocols::GB_SVC_UNIPRO_HIBERNATE_MODE
        {
            protocols::GB_SVC_SETPWRM_PWR_OK
        } else {
            protocols::GB_SVC_SETPWRM_PWR_LOCAL
        };

        let payload = protocols::GbSvcIntfSetPwrmResponse::new(result_code);
        self.send_response(hd, msg.header(), payload);

        Ok(())
    }

    fn dme_peer_get(&self, hd: &hd::Device, hdr: &protocols::GbOperationMsgHdr) {
        let payload = protocols::GbSvcDmePeerGetResponse::new(0, u32::from_le(0x0126));
        self.send_response(hd, hdr, payload);
    }

    fn dme_peer_set(&self, hd: &hd::Device, hdr: &protocols::GbOperationMsgHdr) {
        let payload = protocols::GbSvcDmePeerSetResponse::new(0);
        self.send_response(hd, hdr, payload);
    }

    fn pwrmon_rail_count_get(&self, hd: &hd::Device, hdr: &protocols::GbOperationMsgHdr) {
        let payload = protocols::GbSvcPwrmonRailCountGetResponse::new(0);
        self.send_response(hd, hdr, payload);
    }

    fn intf_vsys_enable_disable(&self, hd: &hd::Device, hdr: &protocols::GbOperationMsgHdr) {
        let payload = protocols::GbSvcIntfVsysResponse::new(protocols::GB_SVC_INTF_VSYS_OK);
        self.send_response(hd, hdr, payload);
    }

    fn intf_refclk_enable_disable(&self, hd: &hd::Device, hdr: &protocols::GbOperationMsgHdr) {
        let payload = protocols::GbSvcIntfRefclkResponse::new(protocols::GB_SVC_INTF_VSYS_OK);
        self.send_response(hd, hdr, payload);
    }

    fn intf_unipro_enable_disable(&self, hd: &hd::Device, hdr: &protocols::GbOperationMsgHdr) {
        let payload = protocols::GbSvcIntfUniproResponse::new(protocols::GB_SVC_INTF_UNIPRO_OK);
        self.send_response(hd, hdr, payload);
    }

    fn intf_activate(&self, hd: &hd::Device, msg: &greybus::Message) {
        let payload = protocols::GbSvcIntfActivateResponse::new(
            protocols::GB_SVC_OP_SUCCESS,
            protocols::GB_SVC_INTF_TYPE_GREYBUS,
        );

        // TODO: Maybe call a callback?

        self.send_response(hd, msg.header(), payload);
    }

    fn intf_resume(&self, hd: &hd::Device, msg: &greybus::Message) {
        let payload = protocols::GbSvcIntfResumeResponse::new(protocols::GB_SVC_OP_SUCCESS);

        // TODO: Maybe call a callback?

        self.send_response(hd, msg.header(), payload);
    }

    fn conn_create(&self, hd: &hd::Device, msg: &greybus::Message) -> Result<()> {
        // TODO: Evaluate if we need to add callback to NodeOps
        self.send_response(hd, msg.header(), ());

        Ok(())
    }

    fn conn_destroy(&self, hd: &hd::Device, msg: &greybus::Message) -> Result<()> {
        // TODO: Evaluate if we need to add callback to NodeOps
        self.send_response(hd, msg.header(), ());

        Ok(())
    }

    fn handler(&self, msg: &greybus::Message) -> Result<()> {
        let hdr = msg.header();
        let hd = msg.operation().connection().host_device();

        if hdr.is_response() {
            self.release_operation_id(hdr.operation_id());
        }

        match (hdr.is_response(), hdr.request_type()) {
            (
                false,
                protocols::GB_SVC_TYPE_INTF_DEVICE_ID
                | protocols::GB_SVC_TYPE_ROUTE_CREATE
                | protocols::GB_SVC_TYPE_ROUTE_DESTROY
                | protocols::GB_SVC_TYPE_PING,
            ) => self.send_response(hd, hdr, ()),
            (false, protocols::GB_SVC_TYPE_CONN_CREATE) => self.conn_create(hd, msg)?,
            (false, protocols::GB_SVC_TYPE_CONN_DESTROY) => self.conn_destroy(hd, msg)?,
            (false, protocols::GB_SVC_TYPE_DME_PEER_GET) => self.dme_peer_get(hd, hdr),
            (false, protocols::GB_SVC_TYPE_DME_PEER_SET) => self.dme_peer_set(hd, hdr),
            (false, protocols::GB_SVC_TYPE_INTF_SET_PWRM) => self.intf_set_pwrm(hd, msg)?,
            (false, protocols::GB_SVC_TYPE_PWRMON_RAIL_COUNT_GET) => {
                self.pwrmon_rail_count_get(hd, hdr)
            }
            (
                false,
                protocols::GB_SVC_TYPE_INTF_VSYS_ENABLE | protocols::GB_SVC_TYPE_INTF_VSYS_DISABLE,
            ) => self.intf_vsys_enable_disable(hd, hdr),
            (
                false,
                protocols::GB_SVC_TYPE_INTF_REFCLK_ENABLE
                | protocols::GB_SVC_TYPE_INTF_REFCLK_DISABLE,
            ) => self.intf_refclk_enable_disable(hd, hdr),
            (
                false,
                protocols::GB_SVC_TYPE_INTF_UNIPRO_ENABLE
                | protocols::GB_SVC_TYPE_INTF_UNIPRO_DISABLE,
            ) => self.intf_unipro_enable_disable(hd, hdr),
            (false, protocols::GB_SVC_TYPE_INTF_ACTIVATE) => self.intf_activate(hd, msg),
            (false, protocols::GB_SVC_TYPE_INTF_RESUME) => self.intf_resume(hd, msg),
            (true, protocols::GB_SVC_TYPE_PROTOCOL_VERSION) => self.send_svc_hello(hd)?,
            (
                true,
                protocols::GB_SVC_TYPE_MODULE_INSERTED
                | protocols::GB_SVC_TYPE_SVC_HELLO
                | protocols::GB_SVC_TYPE_MODULE_REMOVED,
            ) => {}
            _ => return Err(code::ENOTSUPP),
        };

        Ok(())
    }
}

struct GbHdDriver(Arc<Svc>);

impl GbHdDriver {
    fn message_send_inner(&self, dest_cport_id: u16, msg: &greybus::Message) -> Result {
        if dest_cport_id == protocols::GB_SVC_CPORT_ID {
            self.0.handler(msg)
        } else {
            let conn = msg.operation().connection();
            let intf = self.0.intf_by_id(conn.interface().unwrap().id())?;

            let mut buf = KVec::with_capacity(msg.header().size().into(), GFP_KERNEL)?;

            buf.extend_from_slice(msg.header().as_bytes(), GFP_KERNEL)?;
            buf.extend_from_slice(msg.payload_bytes(), GFP_KERNEL)?;

            intf.write(&buf, conn.intf_cport_id())
        }
    }
}

#[vtable]
impl hd::HdDriver for GbHdDriver {
    fn message_send(data: &Self, dest_cport_id: u16, msg: greybus::Message) -> Result {
        let res = data.message_send_inner(dest_cport_id, &msg);
        msg.sent(0);

        res
    }

    fn message_cancel(_msg: greybus::Message) {}
}

struct GreybusSoftSvc {
    _hd: hd::Registration<GbHdDriver>,
    _faux: faux::Registration,
}

impl kernel::Module for GreybusSoftSvc {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("gb_softsvc (init)\n");

        // SAFETY: This runs once at module init, before anything else can reach `GLOBAL_STATE`.
        unsafe { GLOBAL_STATE.init() };

        let faux = faux::Registration::new(c_str!("gb-softsvc"), None)?;
        let svc = Arc::pin_init(Svc::new(), GFP_KERNEL)?;
        let data = GbHdDriver(svc.clone());

        let dev = faux.as_ref().as_ref();
        let hd = hd::Registration::new(dev, GB_OPERATION_SIZE_MAX, CPORT_ID_MAX + 1, Ok(data))?;

        let global_state = GlobalState::new(svc.clone(), hd.as_ref().into());
        let _ = GLOBAL_STATE.lock().replace(global_state);

        svc.send_version(hd.as_ref())?;

        Ok(GreybusSoftSvc {
            _hd: hd,
            _faux: faux,
        })
    }
}

impl Drop for GreybusSoftSvc {
    fn drop(&mut self) {
        let _ = GLOBAL_STATE.lock().take();
        pr_info!("gb_softsvc (exit)\n");
    }
}

/// The operations a Greybus interface provides.
///
/// A node is whatever sits behind an interface — a real transport, an in-kernel emulation, or
/// anything else that can accept Greybus traffic. This module does not care which.
pub trait InterfaceOps: Send + Sync {
    /// Delivers `data` to the node's `cport`.
    fn write(&self, data: &[u8], cport: u16) -> Result<()>;
}

/// A module attached to the Greybus network.
pub struct Module {
    id: u8,
    intf_count: u8,
    gb_hd: ARef<hd::Device>,
    svc: Arc<Svc>,
}

impl Module {
    /// Attaches `node` as a new module and returns the interface id assigned to it.
    pub fn new(intfs: &[Arc<dyn InterfaceOps>]) -> Result<Self> {
        let Ok(intf_count) = u8::try_from(intfs.len()) else {
            return Err(code::E2BIG);
        };

        let guard = GLOBAL_STATE.lock();
        let state = guard.as_ref().ok_or(code::EAGAIN)?;
        let id = state.svc.module_insert(&state.gb_hd, intfs)?;

        Ok(Self {
            id,
            intf_count,
            gb_hd: state.gb_hd.clone(),
            svc: state.svc.clone(),
        })
    }

    /// Delivers `msg`, received from interface `id` on its `cport`, to the Greybus core.
    ///
    /// `cport` is the interface-side CPort id.
    ///
    /// Fails with `EINVAL` if no connection is bound to that pair, or `EAGAIN` if the
    /// host device has not been brought up yet.
    pub fn submit_message(&self, cport: u16, msg: &[u8]) -> Result<()> {
        let intf = self
            .gb_hd
            .find_connection_by_intf(self.id, cport)
            .ok_or(code::EINVAL)?;
        self.gb_hd.data_rcvd(intf.hd_cport_id(), msg);

        Ok(())
    }
}

impl Drop for Module {
    fn drop(&mut self) {
        let _ = self
            .svc
            .module_remove(&self.gb_hd, self.id, self.intf_count);
    }
}
