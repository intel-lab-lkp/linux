// SPDX-License-Identifier: GPL-2.0

//! Rust QEMU EDU driver sample (based on QEMU's edu device).
//!
//! To make this driver probe, QEMU must be run with `-device edu`.

use kernel::{
    device::{
        Bound,
        Core, //
    },
    dma::{
        Coherent,
        CoherentBox,
        Device as _,
        DmaMask, //
    },
    io::{
        poll::read_poll_timeout,
        register,
        register::Array,
        Io, //
    },
    irq::{
        self,
        Flags,
        Registration, //
    },
    pci,
    prelude::*,
    sync::Completion,
    time::Delta, //
};

mod regs {
    use super::*;

    register! {
        pub(super) IDENTIFICATION(u32) @ 0x0 {
            31:0 version;
        }

        pub(super) LIVENESS(u32) @ 0x4 {
            31:0 value;
        }

        pub(super) FACTORIAL(u32) @ 0x8 {
            31:0 value;
        }

        pub(super) STATUS(u32) @ 0x20 {
            7:7 irq_on_done;     // 0x80: raise an IRQ when the factorial is done
            0:0 computing;       // 0x01: set while a factorial is being computed
        }

        pub(super) INTERRUPT_STATUS(u32) @ 0x24 {
            31:0 status;
        }

        pub(super) INTERRUPT_RAISE(u32) @ 0x60 {
            31:0 raise;
        }

        pub(super) INTERRUPT_ACK(u32) @ 0x64 {
            31:0 ack;
        }

        // The EDU DMA registers are documented as 64-bit, but QEMU's edu device
        // accepts 4-byte accesses to these registers and zero-extends the
        // written value, and this driver only uses addresses/counts that fit in
        // 32 bits (a 28-bit DMA mask), so a single `u32` access per register is
        // correct and sufficient here.
        pub(super) DMA_SRC_ADDR(u32) @ 0x80 {
            31:0 src;
        }

        pub(super) DMA_DST_ADDR(u32) @ 0x88 {
            31:0 dst;
        }

        pub(super) DMA_CNT(u32) @ 0x90 {
            31:0 size;
        }

        pub(super) DMA_CMD(u32) @ 0x98 {
            31:0 cmd;
        }
    }

    pub(super) const END: usize = 0x9C;
}

type Bar0<'bound> = pci::Bar<'bound, { regs::END }>;

struct SampleDriverData<'bound> {
    pdev: &'bound pci::Device,
    // Held for the lifetime of the bound device so the BAR mapping stays alive;
    // all register access happens during probe, so the field is never read again.
    #[expect(dead_code)]
    bar: Bar0<'bound>,
}

struct SampleDriver;

// The QEMU EDU device's PCI IDs. These are specific to this emulated device and are not
// shared with any other driver, so they are kept local here rather than in `pci_ids.h`.
// `0x1234` is QEMU's conventional vendor ID, which has no dedicated `pci::Vendor` constant.
const VENDOR_ID_QEMU: u16 = 0x1234;
const DEVICE_ID_EDU: u32 = 0x11e8;

kernel::pci_device_table!(
    PCI_TABLE,
    <SampleDriver as pci::Driver>::IdInfo,
    [(
        pci::DeviceId::from_id(pci::Vendor::from_raw(VENDOR_ID_QEMU), DEVICE_ID_EDU),
        ()
    )]
);

impl SampleDriverData<'_> {
    /// The value the EDU device reports from its `IDENTIFICATION` register.
    const EDU_ID: u32 = 0x010000ed;

    /// Confirm we are really talking to an EDU device by reading its ID register.
    fn identification(bar: &Bar0<'_>) -> Result {
        if bar.read(regs::IDENTIFICATION).version().get() != Self::EDU_ID {
            return Err(ENODEV);
        }

        Ok(())
    }

    /// Liveness check: the EDU device inverts (bitwise NOT) whatever we write to the
    /// `LIVENESS` register, so writing `x` and reading back `!x` proves MMIO works.
    fn liveness(bar: &Bar0<'_>) -> Result {
        let pattern: u32 = 0xdead_beef;

        bar.write_reg(regs::LIVENESS::zeroed().with_value(pattern));

        let readback = bar.read(regs::LIVENESS).value().get();

        if readback != !pattern {
            return Err(EIO);
        }

        Ok(())
    }

    /// Exercise the interrupt registers at the MMIO level.
    ///
    /// Writing a bitmask to `INTERRUPT_RAISE` asserts those bits in
    /// `INTERRUPT_STATUS` (and, if an IRQ is enabled, makes the device raise an
    /// interrupt); writing the same bitmask to `INTERRUPT_ACK` clears them
    /// again. Here we raise a test bit, confirm the status register reflects it,
    /// then acknowledge it and confirm it clears -- all without registering a
    /// handler. Wiring up MSI and an actual IRQ handler is a later step.
    fn interrupt_status(bar: &Bar0<'_>) -> Result {
        let pattern: u32 = 0x0000_0001;

        bar.write_reg(regs::INTERRUPT_RAISE::zeroed().with_raise(pattern));

        if bar.read(regs::INTERRUPT_STATUS).status().get() != pattern {
            return Err(EIO);
        }

        bar.write_reg(regs::INTERRUPT_ACK::zeroed().with_ack(pattern));

        if bar.read(regs::INTERRUPT_STATUS).status().get() != 0 {
            return Err(EIO);
        }

        Ok(())
    }

    /// Make the edu device compute a factorial and inform the cpu
    /// of completion via an IRQ
    fn factorial_irq(pdev: &pci::Device<Bound>, bar: &Bar0<'_>) -> Result {
        let operand: u32 = 6;

        let msi = pci::IrqTypes::default().with(pci::IrqType::Msi);
        let vectors = pdev.alloc_irq_vectors(1, 1, msi)?;

        let registration = KBox::pin_init(
            unsafe {
                Registration::new(
                    vectors.index(0)?.into(),
                    Flags::SHARED,
                    c"rust_driver_edu",
                    try_pin_init!(EduIrq {
                        done <- Completion::new(),
                    }? Error),
                )
            },
            GFP_KERNEL,
        )?;

        // Arm the "raise an IRQ when done" bit, then start the computation.
        bar.write_reg(regs::STATUS::zeroed().with_irq_on_done(true));
        bar.write_reg(regs::FACTORIAL::zeroed().with_value(operand));

        // Sleep until the handler calls `complete_all()` from IRQ context.
        registration.handler().done.wait_for_completion();

        // Acknowledge whatever the device raised so it lowers the interrupt.
        let pending = bar.read(regs::INTERRUPT_STATUS).status().get();
        bar.write_reg(regs::INTERRUPT_ACK::zeroed().with_ack(pending));

        let result = bar.read(regs::FACTORIAL).value().get();

        if result != factorial_helper(operand) {
            return Err(EIO);
        }

        Ok(())
    }

    /// Tests a full DMA round-trip, writing from RAM -> device buffer,
    /// and then from device buffer -> RAM
    fn dma_test(pdev: &pci::Device<Core<'_>>, bar: &Bar0<'_>) -> Result {
        const DEV_BUF: u32 = 0x40_000;
        const LEN: usize = 512;
        const CMD_START: u32 = 1 << 0;
        const CMD_TO_RAM: u32 = 1 << 1;

        unsafe { pdev.dma_set_mask_and_coherent(DmaMask::new::<28>())? };

        let mut src: CoherentBox<[u8]> = CoherentBox::zeroed_slice(pdev.as_ref(), LEN, GFP_KERNEL)?;
        src.fill(0xa5);

        let src: Coherent<[u8]> = src.into();

        let dst: Coherent<[u8]> =
            CoherentBox::<[u8]>::zeroed_slice(pdev.as_ref(), LEN, GFP_KERNEL)?.into();

        let src_dma = src.dma_address() as u32;
        let dst_dma = dst.dma_address() as u32;

        let wait_dma = || {
            read_poll_timeout(
                || Ok(bar.read(regs::DMA_CMD).cmd().get()),
                |cmd| *cmd & CMD_START == 0,
                Delta::from_millis(10),
                Delta::from_secs(1),
            )
        };

        // 1) RAM -> device internal buffer. CMD is written last: it triggers the transfer.
        bar.write_reg(regs::DMA_SRC_ADDR::zeroed().with_src(src_dma));
        bar.write_reg(regs::DMA_DST_ADDR::zeroed().with_dst(DEV_BUF));
        bar.write_reg(regs::DMA_CNT::zeroed().with_size(LEN as u32));
        bar.write_reg(regs::DMA_CMD::zeroed().with_cmd(CMD_START));
        wait_dma()?;

        // 2) device internal buffer -> RAM, landing in `dst`.
        bar.write_reg(regs::DMA_SRC_ADDR::zeroed().with_src(DEV_BUF));
        bar.write_reg(regs::DMA_DST_ADDR::zeroed().with_dst(dst_dma));
        bar.write_reg(regs::DMA_CNT::zeroed().with_size(LEN as u32));
        bar.write_reg(regs::DMA_CMD::zeroed().with_cmd(CMD_START | CMD_TO_RAM));
        wait_dma()?;

        // Transfer completed, check dst is the expected value
        let out = unsafe { dst.as_ref() };
        if out.iter().any(|&b| b != 0xa5) {
            return Err(EIO);
        }

        Ok(())
    }

    fn config_space(pdev: &pci::Device<Bound>) {
        let config = pdev.config_space();

        // Some PCI configuration space registers.
        register! {
            VENDOR_ID(u16) @ 0x0 {
                15:0 vendor_id;
            }

            REVISION_ID(u8) @ 0x8 {
                7:0 revision_id;
            }

            BAR(u32)[6] @ 0x10 {
                31:0 value;
            }
        }

        dev_info!(
            pdev,
            "edu config space read8 rev ID: {:x}\n",
            config.read(REVISION_ID).revision_id()
        );

        dev_info!(
            pdev,
            "edu config space read16 vendor ID: {:x}\n",
            config.read(VENDOR_ID).vendor_id()
        );

        dev_info!(
            pdev,
            "edu config space read32 BAR 0: {:x}\n",
            config.read(BAR::at(0)).value()
        );
    }
}

impl pci::Driver for SampleDriver {
    type IdInfo = ();
    type Data<'bound> = SampleDriverData<'bound>;

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe<'bound>(
        pdev: &'bound pci::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        dev_dbg!(
            pdev,
            "Probe Rust QEMU EDU driver sample (PCI ID: {}, 0x{:x}).\n",
            pdev.vendor_id(),
            pdev.device_id()
        );

        pdev.enable_device_mem()?;
        pdev.set_master();

        let bar = pdev.iomap_region_sized::<{ regs::END }>(0, c"rust_driver_edu")?;

        SampleDriverData::identification(&bar)?;
        SampleDriverData::liveness(&bar)?;
        SampleDriverData::interrupt_status(&bar)?;
        SampleDriverData::factorial_irq(pdev, &bar)?;
        SampleDriverData::dma_test(pdev, &bar)?;
        SampleDriverData::config_space(pdev);

        dev_info!(pdev, "EDU device probed: all checks passed.\n");

        Ok(SampleDriverData { pdev, bar })
    }
}

impl Drop for SampleDriverData<'_> {
    fn drop(&mut self) {
        dev_dbg!(self.pdev, "Remove Rust QEMU EDU driver sample.\n");
    }
}

/// Per-interrupt state shared between process context and IRQ context.
///
/// The factorial computation signals `done` from the IRQ handler; the process
/// context that started it waits on the same [`Completion`].
#[pin_data]
struct EduIrq {
    #[pin]
    done: Completion,
}

impl irq::Handler for EduIrq {
    // Runs in IRQ context when the EDU device raises its interrupt.
    fn handle(&self) -> irq::IrqReturn {
        self.done.complete_all();
        irq::IrqReturn::Handled
    }
}

// Helper functions
fn factorial_helper(n: u32) -> u32 {
    if n <= 1 {
        return 1;
    }

    n * factorial_helper(n - 1)
}

kernel::module_pci_driver! {
    type: SampleDriver,
    name: "rust_driver_edu",
    authors: ["Lucas Costa"],
    description: "Rust QEMU EDU driver",
    license: "GPL v2",
}
