// SPDX-License-Identifier: GPL-2.0

//! Greybus UART Node driver

use kernel::{
    alloc::Flags,
    crc_ccitt::crc_ccitt,
    device::{
        AsBusDevice,
        Bound,
        Core, //
    },
    error::code,
    new_spinlock, of,
    prelude::*,
    serdev,
    sync::{
        aref::ARef,
        Arc,
        SpinLock, //
    },
};

use zerocopy::little_endian;
use zerocopy_derive::{FromBytes, Immutable, KnownLayout};

const HDLC_MAX_FRAME_LEN: usize = 256;

const HDLC_FRAME: u8 = 0x7E;
const HDLC_ESC: u8 = 0x7D;
const HDLC_XOR: u8 = 0x20;
const HDLC_EXPECTED_CRC: u16 = 0xf0b8;

const ADDRESS_GREYBUS: u8 = 0x01;

#[repr(C, packed)]
#[derive(FromBytes, Immutable, KnownLayout)]
struct GreybusFrame {
    cport: little_endian::U16,
    msg: [u8],
}

struct HdlcRx {
    rx_buf: KVec<u8>,
    rx_in_esc: bool,
    sdev: ARef<serdev::Device>,
    node: gb_softsvc::Module,
}

impl HdlcRx {
    fn new(sdev: ARef<serdev::Device>, node: gb_softsvc::Module) -> Result<Self> {
        Ok(Self {
            node,
            sdev,
            rx_buf: KVec::with_capacity(HDLC_MAX_FRAME_LEN, GFP_KERNEL)?,
            rx_in_esc: false,
        })
    }

    fn frame_finish(&self) -> Result<()> {
        if self.rx_buf.len() < 4 {
            return Err(code::EFAULT);
        }

        let crc = crc_ccitt(0xffff, &self.rx_buf);
        if crc != HDLC_EXPECTED_CRC {
            dev_warn!(self.sdev.as_ref(), "CRC failed {}", crc);
            return Ok(());
        }

        let addr = self.rx_buf[0];
        let _ctrl = self.rx_buf[1];
        let payload = &self.rx_buf[2..self.rx_buf.len() - size_of::<u16>()];

        match addr {
            ADDRESS_GREYBUS => {
                let frame = GreybusFrame::ref_from_bytes(payload).map_err(|_| code::EINVAL)?;
                self.node.submit_message(0, frame.cport.into(), &frame.msg)
            }
            _ => Err(code::EINVAL),
        }
    }

    fn rx(&mut self, data: &[u8]) -> usize {
        for i in data.iter() {
            match *i {
                HDLC_FRAME => {
                    if !self.rx_buf.is_empty() {
                        if let Err(e) = self.frame_finish() {
                            dev_warn!(self.sdev.as_ref(), "bad frame: {e:?}\n");
                        }
                    }

                    self.rx_buf.clear();
                    self.rx_in_esc = false;
                }
                HDLC_ESC => self.rx_in_esc = true,
                _ => {
                    let c = if self.rx_in_esc { *i ^ HDLC_XOR } else { *i };
                    self.rx_in_esc = false;

                    if self.rx_buf.push_within_capacity(c).is_err() {
                        dev_warn!(self.sdev.as_ref(), "buffer overflow. Dropping frame");

                        self.rx_buf.clear();
                        self.rx_in_esc = false;
                    }
                }
            }
        }

        data.len()
    }
}

struct GbNode {
    sdev: ARef<serdev::Device>,
}

impl GbNode {
    const fn new(sdev: ARef<serdev::Device>) -> Self {
        Self { sdev }
    }

    fn fill_buf(mut crc: u16, data: &[u8], buf: &mut KVec<u8>) -> Result<u16> {
        for i in data {
            crc = crc_ccitt(crc, &[*i]);
            if *i == HDLC_ESC || *i == HDLC_FRAME {
                buf.push_within_capacity(HDLC_ESC)?;
                buf.push_within_capacity(i ^ HDLC_XOR)?;
            } else {
                buf.push_within_capacity(*i)?;
            }
        }

        Ok(crc)
    }
}

impl gb_softsvc::InterfaceOps for GbNode {
    fn write(&self, data: &[u8], cport: u16, gfp_mask: Flags) -> Result<()> {
        // SAFETY: `GbNode` only exists while its serdev driver is bound, so the device is in the
        // `Bound` state for the duration of this call.
        let bound: &serdev::Device<Bound> =
            unsafe { serdev::Device::from_device(self.sdev.as_ref().as_bound()) };

        let mut buf = KVec::with_capacity(HDLC_MAX_FRAME_LEN, gfp_mask)?;

        let mut crc = 0xffff;

        buf.push_within_capacity(HDLC_FRAME)?;

        crc = Self::fill_buf(crc, &[ADDRESS_GREYBUS, 0x03], &mut buf)?;
        crc = Self::fill_buf(crc, &cport.to_le_bytes(), &mut buf)?;
        crc = Self::fill_buf(crc, data, &mut buf)?;

        crc ^= 0xffff;
        Self::fill_buf(crc, &crc.to_le_bytes(), &mut buf)?;

        buf.push_within_capacity(HDLC_FRAME)?;

        bound.write_all(&buf, 0)?;

        Ok(())
    }
}

#[pin_data]
struct GbUartNode {
    #[pin]
    rx: SpinLock<Option<HdlcRx>>,
}

impl GbUartNode {
    fn init(sdev: &serdev::Device<Core<'_>>) -> Result<HdlcRx> {
        if sdev
            .set_baudrate(
                sdev.as_ref()
                    .fwnode()
                    .and_then(|fwnode| fwnode.property_read(c"baudrate").optional())
                    .unwrap_or(115200),
            )
            .is_err()
        {
            return Err(EINVAL);
        }
        sdev.set_flow_control(false);
        sdev.set_parity(serdev::Parity::None)?;

        let node = gb_softsvc::Module::new(&[Arc::new(GbNode::new(sdev.into()), GFP_KERNEL)?])?;

        HdlcRx::new(sdev.into(), node)
    }
}

kernel::of_device_table!(
    OF_TABLE,
    <GbUartNode as serdev::Driver>::IdInfo,
    [(of::DeviceId::new(c"beagle,beagleconnect-freedom"), ())]
);

#[vtable]
impl serdev::Driver for GbUartNode {
    type IdInfo = ();
    type Data<'bound> = Self;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe<'bound>(
        sdev: &'bound serdev::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self, Error> + 'bound {
        dev_dbg!(sdev.as_ref(), "Probe gb_uart_node.\n");

        try_pin_init!(Self {
            rx <- new_spinlock!(Some(Self::init(sdev)?), "gb_uart_node::rx"),
        }? Error)
    }

    fn receive<'bound>(
        _sdev: &'bound serdev::Device<Bound>,
        this: Pin<&Self>,
        data: &[u8],
    ) -> usize {
        if let Some(mut guard) = this.rx.try_lock() {
            if let Some(ref mut hdlc_rx) = *guard {
                return hdlc_rx.rx(data);
            }
        }

        0
    }

    fn unbind<'bound>(_: &'bound serdev::Device<Core<'_>>, this: Pin<&Self::Data<'bound>>) {
        // Getting a bound device is not possible after this point. So drop HdlcRx.
        let _ = this.rx.lock().take();
    }
}

kernel::module_serdev_device_driver! {
    type: GbUartNode,
    name: "gb_uart_node",
    authors: ["Ayush Singh <ayush@beagleboard.org>"],
    description: "Greybus node connected over UART",
    license: "GPL v2",
}
