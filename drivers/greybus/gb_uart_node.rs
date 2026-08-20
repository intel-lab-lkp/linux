// SPDX-License-Identifier: GPL-2.0

//! Greybus UART Node driver

use kernel::crc_ccitt::crc_ccitt;
use kernel::device::AsBusDevice;
use kernel::device::{Bound, Core};
use kernel::error::code;
use kernel::sync::aref::ARef;
use kernel::sync::{Arc, SpinLock};
use kernel::{new_spinlock, of, prelude::*, serdev};

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
        let payload = &self.rx_buf[2..self.rx_buf.len() - size_of::<u8>()];

        match addr {
            ADDRESS_GREYBUS => {
                let frame = GreybusFrame::ref_from_bytes(payload).map_err(|_| code::EINVAL)?;
                self.node.submit_message(frame.cport.into(), &frame.msg)
            }
            _ => Err(code::EINVAL),
        }
    }

    fn rx(&mut self, data: &[u8]) -> usize {
        for (count, i) in data.iter().enumerate() {
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

                    if self.rx_buf.push(c, GFP_KERNEL).is_err() {
                        return count;
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

    fn write_all(mut crc: u16, bound: &serdev::Device<Bound>, data: &[u8]) -> Result<u16> {
        for i in data {
            if *i == HDLC_ESC || *i == HDLC_FRAME {
                let buf = &[HDLC_ESC, i ^ HDLC_XOR];
                bound.write_all(buf, 0)?;
                crc = crc_ccitt(crc, buf);
            } else {
                bound.write_all(&[*i], 0)?;
                crc = crc_ccitt(crc, &[*i]);
            }
        }

        Ok(crc)
    }
}

impl gb_softsvc::InterfaceOps for GbNode {
    fn write(&self, data: &[u8], cport: u16) -> Result<()> {
        // SAFETY: `GbNode` only exists while its serdev driver is bound, so the device is in the
        // `Bound` state for the duration of this call.
        let bound: &serdev::Device<Bound> =
            unsafe { serdev::Device::from_device(self.sdev.as_ref().as_bound()) };

        let mut crc = 0xffff;

        bound.write_all(&[HDLC_FRAME], 0)?;

        crc = Self::write_all(crc, bound, &[ADDRESS_GREYBUS, 0x03])?;
        crc = Self::write_all(crc, bound, &cport.to_le_bytes())?;
        crc = Self::write_all(crc, bound, data)?;

        crc ^= 0xffff;
        Self::write_all(crc, bound, &crc.to_le_bytes())?;

        bound.write_all(&[HDLC_FRAME], 0)?;

        Ok(())
    }
}

#[pin_data(PinnedDrop)]
struct GbUartNode {
    sdev: ARef<serdev::Device>,
    #[pin]
    rx: SpinLock<HdlcRx>,
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
        let rx = Self::init(sdev);

        try_pin_init!(Self {
            sdev: sdev.into(),
            rx <- new_spinlock!(rx?, "gb_uart_node::rx"),
        }? Error)
    }

    fn receive<'bound>(
        _sdev: &'bound serdev::Device<Bound>,
        this: Pin<&Self>,
        data: &[u8],
    ) -> usize {
        let Some(mut guard) = this.rx.try_lock() else {
            return 0;
        };

        guard.rx(data)
    }
}

#[pinned_drop]
impl PinnedDrop for GbUartNode {
    fn drop(self: Pin<&mut Self>) {
        dev_dbg!(self.sdev.as_ref(), "Remove gb_uart_node.\n");
    }
}

kernel::module_serdev_device_driver! {
    type: GbUartNode,
    name: "gb_uart_node",
    authors: ["Ayush Singh <ayush@beagleboard.org>"],
    description: "Greybus node connected over UART",
    license: "GPL v2",
}
