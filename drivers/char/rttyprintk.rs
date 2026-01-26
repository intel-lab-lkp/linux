// SPDX-License-Identifier: GPL-2.0

//! Rust TTY printk driver.
//!
//! Allows user messages to be written to the kernel log via printk.

use kernel::{
    bindings,
    c_str,
    new_spinlock,
    prelude::*,
    sync::{
        Arc,
        SpinLock,
    },
    tty::{
        self,
        port,
        DriverPort,
        Tty,
    },
};

module! {
    type: RttyPrintk,
    name: "rttyprintk",
    authors: ["SeungJong Ha"],
    description: "Rust TTY driver to output user messages via printk",
    license: "GPL",
}

const TPK_STR_SIZE: usize = 508;
const TPK_MAX_ROOM: u32 = 4096;

/// Mutable state protected by spinlock.
struct TpkState {
    curr: usize,
    buffer: [u8; TPK_STR_SIZE + 4],
}

impl TpkState {
    const fn new() -> Self {
        Self {
            curr: 0,
            buffer: [0u8; TPK_STR_SIZE + 4],
        }
    }

    fn flush(&mut self) {
        if self.curr > 0 {
            self.buffer[self.curr] = 0;
            // SAFETY: buffer is null-terminated.
            unsafe {
                bindings::_printk(c_str!("\x016[U] %s\n").as_char_ptr(), self.buffer.as_ptr());
            }
            self.curr = 0;
        }
    }

    fn do_write(&mut self, buf: &[u8]) -> usize {
        for (i, &c) in buf.iter().enumerate() {
            if self.curr >= TPK_STR_SIZE {
                self.buffer[self.curr] = b'\\';
                self.curr += 1;
                self.flush();
            }

            match c {
                b'\r' => {
                    self.flush();
                    if buf.get(i + 1) == Some(&b'\n') {
                        continue;
                    }
                }
                b'\n' => self.flush(),
                _ => {
                    self.buffer[self.curr] = c;
                    self.curr += 1;
                }
            }
        }
        buf.len()
    }
}

struct TpkPortOps;
type TpkPort = DriverPort<TpkPortOps>;

#[vtable]
impl port::Operations for TpkPortOps {
    type PortData = SpinLock<TpkState>;

    fn shutdown(port: &TpkPort) {
        port.data().lock().flush();
    }
}

struct TpkDevice;
type TpkTty = Tty<Arc<TpkPort>, Arc<TpkPort>>;

#[vtable]
impl tty::Operations for TpkDevice {
    type DriverData = Arc<TpkPort>;
    type DriverState = Arc<TpkPort>;
    type PortOps = TpkPortOps;

    fn open(tty: &TpkTty, _file: *mut bindings::file) -> Result<()> {
        // Clone the Arc from driver_state and set it as driver_data.
        // This mirrors the original ttyprintk.c pattern where tty->driver_data is set
        // to the port in open(). In practice, since Arc allows shared access and
        // SpinLock protects the state, we could just use driver_state() directly.
        // However, we follow the original C code structure for consistency.
        let port = tty.driver_state().ok_or(ENXIO)?;
        tty.set_driver_data(port);
        Ok(())
    }

    fn close(tty: &TpkTty, _file: *mut bindings::file) {
        // Take and drop driver_data, mirroring tpk_close() which sets
        // tty->driver_data = NULL. The Arc will be dropped, decrementing refcount.
        tty.take_driver_data();
    }

    fn write(tty: &TpkTty, buf: &[u8]) -> Result<usize> {
        // Access port via driver_data (set in open), following original ttyprintk.c.
        // SpinLock inside TpkState protects concurrent writes.
        let port = tty.driver_data().ok_or(ENXIO)?;
        Ok(port.data().lock().do_write(buf))
    }

    fn write_room(_tty: &TpkTty) -> u32 {
        TPK_MAX_ROOM
    }

    fn hangup(_tty: &TpkTty) {}
}

struct RttyPrintk {
    #[allow(dead_code)]
    driver: Pin<KBox<tty::TtyDriver<TpkDevice>>>,
}

impl kernel::Module for RttyPrintk {
    fn init(module: &'static kernel::ThisModule) -> Result<Self> {
        pr_info!("Rust TTY printk driver initializing\n");

        let port = Arc::pin_init(
            TpkPort::new(new_spinlock!(TpkState::new(), "tpk_lock")),
            GFP_KERNEL,
        )?;

        let opts = tty::Options {
            driver_name: c_str!("rttyprintk"),
            name: c_str!("rttyprintk"),
            major: tty::TTYAUX_MAJOR,
            minor_start: 4,
            driver_type: tty::DriverType::Console,
            flags: tty::flags::RESET_TERMIOS | tty::flags::REAL_RAW | tty::flags::UNNUMBERED_NODE,
        };

        // link_port needs a reference, set_driver_state takes ownership of the Arc.
        let builder = tty::TtyDriverBuilder::<TpkDevice>::new(opts, module)?
            .link_port(&port, 0)
            .set_driver_state(port);

        let driver = KBox::pin_init(builder.build(), GFP_KERNEL)?;

        pr_info!("Rust TTY printk driver registered at /dev/rttyprintk\n");

        Ok(Self { driver })
    }
}

impl Drop for RttyPrintk {
    fn drop(&mut self) {
        // Reclaim the driver state before the driver is unregistered.
        self.driver.take_driver_state();
        pr_info!("Rust TTY printk driver unloading\n");
    }
}
