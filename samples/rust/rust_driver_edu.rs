// SPDX-License-Identifier: GPL-2.0

//! Rust EDU driver sample (based on QEMU's `edu`).
//!
//! To make this driver probe, QEMU must be run with `-device edu`.

use kernel::{
    device::Bound,
    devres::Devres,
    dma::{Coherent, Device, DmaMask},
    io::{
        poll::read_poll_timeout,
        register,
        Io, //
    },
    irq::{self, Flags},
    pci::{self, IrqTypes},
    prelude::*,
    sync::{aref::ARef, Arc, Completion},
    time::Delta, //
};

const QEMU_VENDOR_ID: u16 = 0x1234;
const QEMU_EDU_DEVICE_ID: u32 = 0x11e8;
const QEMU_EDU_DEVICE_MAGIC: u8 = 0xed;
const QEMU_DMA_BASE: u64 = 0x40000;

const IRQ_MAGIC_VALUE: u32 = 42;

/// Bit set in `IRQ_STATUS` when a DMA transfer has completed.
const DMA_IRQ: u32 = 0x100;

mod regs {
    use super::*;

    register! {
        pub(super) IDENTIFICATION(u32) @ 0x0 {
            31:24 major;
            23:16 minor;
            7:0 magic;
        }

        pub(super) LIVENESS_CHECK(u32) @ 0x04 {}

        pub(super) FACTORIAL(u32) @ 0x08 {}

        pub(super) STATUS(u32) @ 0x20 {
            0:0 computing;
            7:7 raise_interrupt;
        }

        pub(super) IRQ_STATUS(u32) @ 0x24 {}
        pub(super) IRQ_RAISE(u32) @ 0x60 {}
        pub(super) IRQ_ACK(u32) @ 0x64 {}

        pub(super) DMA_SRC(u64) @ 0x80 {}
        pub(super) DMA_DST(u64) @ 0x88 {}
        pub(super) DMA_COUNT(u64) @ 0x90 {}
        pub(super) DMA_COMMAND(u64) @ 0x98 {
            0:0 start_transfer;
            1:1 direction;
            2:2 raise_irq;
        }
    }

    pub(super) const END: usize = 0xA0;
}

type Bar0 = pci::Bar<'static, { regs::END }>;

#[pin_data(PinnedDrop)]
struct EduDriver {
    pdev: ARef<pci::Device>,
    data: Arc<EduDriverData>,
    #[pin]
    irq_handler: irq::Registration<Arc<EduDriverData>>,
}

#[pin_data]
struct EduDriverData {
    #[pin]
    bar: Devres<Bar0>,
    #[pin]
    irq_test_completion: Completion,
    #[pin]
    irq_dma_completion: Completion,
    dma: Coherent<u64>,
}

impl EduDriver {
    fn init(pdev: &pci::Device<Bound>, bar: &Bar0, data: &EduDriverData) -> Result {
        Self::magic(pdev, bar)?;
        Self::liveness_check(pdev, bar)?;
        Self::factorial(pdev, bar)?;
        Self::test_irq(pdev, bar, data)?;
        Self::test_dma(pdev, bar, data)?;
        Ok(())
    }

    fn magic(pdev: &pci::Device<Bound>, bar: &Bar0) -> Result {
        let identification = bar.read(regs::IDENTIFICATION);

        let magic: u8 = identification.magic().into();

        if magic != QEMU_EDU_DEVICE_MAGIC {
            dev_err!(
                pdev,
                "magic mismatch: expected {:#x} got {:#x}\n",
                QEMU_EDU_DEVICE_MAGIC,
                magic
            );
            return Err(ENODEV);
        }

        dev_info!(
            pdev,
            "major: {:#x} minor: {:#x}\n",
            identification.major(),
            identification.minor()
        );
        Ok(())
    }

    fn liveness_check(pdev: &pci::Device<Bound>, bar: &Bar0) -> Result {
        let test_value = 0xabcd;

        bar.write(regs::LIVENESS_CHECK, test_value.into());

        let inverse_value = bar.read(regs::LIVENESS_CHECK).into_raw();

        if inverse_value != !test_value {
            dev_err!(
                pdev,
                "inverse mismatch: expected {:#x} got {:#x}\n",
                !test_value,
                inverse_value
            );
            return Err(ENODEV);
        }

        dev_info!(pdev, "inverse test successful\n");
        Ok(())
    }

    fn factorial(pdev: &pci::Device<Bound>, bar: &Bar0) -> Result {
        Self::wait_until_compute_has_finished(pdev, bar)?;

        bar.write(regs::FACTORIAL, 4.into());

        Self::wait_until_compute_has_finished(pdev, bar)?;

        let result: u32 = bar.read(regs::FACTORIAL).into();

        let expected = 24;

        if result != expected {
            dev_err!(
                pdev,
                "factorial result wrong: expected {} got {}\n",
                expected,
                result
            );
            return Err(ENODEV);
        }

        dev_info!(pdev, "factorial test successful\n");
        Ok(())
    }

    fn test_irq(pdev: &pci::Device<Bound>, bar: &Bar0, data: &EduDriverData) -> Result {
        dev_dbg!(pdev, "raising irq\n");

        bar.write(regs::IRQ_RAISE, IRQ_MAGIC_VALUE.into());

        data.irq_test_completion.wait_for_completion();
        Ok(())
    }

    fn test_dma(pdev: &pci::Device<Bound>, bar: &Bar0, data: &EduDriverData) -> Result {
        dev_dbg!(pdev, "testing dma\n");

        let dma = &data.dma;

        const DMA_VALUE: u64 = 42;

        kernel::dma_write!(dma, , DMA_VALUE);

        bar.write(regs::DMA_SRC, dma.dma_handle().into());
        bar.write(regs::DMA_DST, QEMU_DMA_BASE.into());
        bar.write(regs::DMA_COUNT, (dma.size() as u64).into());
        bar.write(
            regs::DMA_COMMAND,
            regs::DMA_COMMAND::zeroed()
                .with_start_transfer(true)
                .with_direction(false)
                .with_raise_irq(true),
        );

        data.irq_dma_completion.wait_for_completion();

        // Destroy previous value to test roundtrip
        kernel::dma_write!(dma, , 0);

        bar.write(regs::DMA_SRC, QEMU_DMA_BASE.into());
        bar.write(regs::DMA_DST, dma.dma_handle().into());
        bar.write(regs::DMA_COUNT, (dma.size() as u64).into());
        bar.write(
            regs::DMA_COMMAND,
            regs::DMA_COMMAND::zeroed()
                .with_start_transfer(true)
                .with_direction(true)
                .with_raise_irq(true),
        );

        data.irq_dma_completion.wait_for_completion();

        let result = kernel::dma_read!(dma,);

        if result != DMA_VALUE {
            dev_err!(
                pdev,
                "dma result wrong: expected {} got {}\n",
                DMA_VALUE,
                result
            );
            return Err(ENODEV);
        }

        dev_info!(pdev, "dma test successful\n");
        Ok(())
    }

    fn wait_until_compute_has_finished(pdev: &pci::Device<Bound>, bar: &Bar0) -> Result {
        read_poll_timeout(
            || Ok(bar.read(regs::STATUS)),
            |status| status.computing() == 0,
            Delta::from_millis(10),
            Delta::from_millis(100),
        )
        .inspect_err(|_| dev_err!(pdev, "computation bit did not clear before timeout\n"))
        .map(|_| ())
    }
}

impl pci::Driver for EduDriver {
    type IdInfo = ();
    type Data<'bound> = Self;

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe<'bound>(
        pdev: &'bound pci::Device<kernel::device::Core<'_>>,
        _id_info: &'bound Self::IdInfo,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        pin_init::pin_init_scope(move || {
            let vendor = pdev.vendor_id();
            dev_dbg!(
                pdev,
                "Probe Rust EDU driver sample (PCI ID: {}, 0x{:x}).\n",
                vendor,
                pdev.device_id()
            );

            pdev.enable_device()?;
            pdev.set_master();

            let mask = DmaMask::new::<28>();

            // SAFETY: There are no concurrent calls to DMA allocation and mapping primitives.
            unsafe { pdev.dma_set_mask_and_coherent(mask)? };

            let ca: Coherent<u64> = Coherent::zeroed(pdev.as_ref(), GFP_KERNEL)?;

            let irq = pdev
                .alloc_irq_vectors(1, 1, IrqTypes::default().with(pci::IrqType::Msi))
                .inspect_err(|e| dev_err!(pdev, "alloc_irq_vectors failed: {:?}\n", e))?;

            // State shared with the IRQ handler (the BAR and the completion the
            // handler signals) lives in an `Arc<EduDriverData>`. `EduDriverData`
            // itself implements `irq::Handler`, and the registration takes an
            // `Arc<T>` via the `impl Handler for Arc<T>` blanket impl. This keeps
            // the handler's state out of `EduDriver` and avoids a self-reference.
            let data = Arc::pin_init(
                try_pin_init!(EduDriverData {
                    bar <- pdev
                        .iomap_region_sized(0, c"rust_driver_edu")
                        .and_then(|bar| bar.into_devres()),
                    irq_test_completion <- Completion::new(),
                    irq_dma_completion <- Completion::new(),
                    dma: ca,
                }),
                GFP_KERNEL,
            )?;

            let req = irq::Registration::new(
                (*irq.start()).try_into()?,
                Flags::TRIGGER_NONE,
                c"rust_edu_irq",
                Ok(data.clone()),
            );

            // Ordering matters: the handler is registered (`irq_handler <- req`)
            // *before* the `_:` block runs the self-tests, one of which raises an
            // interrupt and waits for the handler. Raising before the handler is
            // registered would hang (the completion is never signalled).
            Ok(try_pin_init!(Self {
                irq_handler <- req,
                // Side-effect block: run the staged self-tests against the mapped
                // BAR now that the handler is live. A failure here aborts probe.
                _: {
                    let bar = data.bar.access(pdev.as_ref())?;
                    EduDriver::init(pdev, bar, &data)?;
                    dev_info!(
                        pdev,
                        "rust_driver_edu successfully initialized\n",
                    );
                },
                data,
                pdev: pdev.into()
            }))
        })
    }
}

impl irq::Handler for EduDriverData {
    fn handle(&self, pdev: &kernel::device::Device<Bound>) -> irq::IrqReturn {
        dev_dbg!(pdev, "irq handler called\n");
        // `access()` only fails on device mismatch, so this branch is
        // structurally unreachable here, but it must be handled.
        let Ok(bar) = self.bar.access(pdev.as_ref()) else {
            dev_err!(pdev, "cannot access bar register inside irq handler\n");
            return irq::IrqReturn::None;
        };
        let status: u32 = bar.read(regs::IRQ_STATUS).into();

        // DMA_IRQ
        if status & DMA_IRQ != 0 {
            dev_dbg!(pdev, "handling dma completion in irq\n");
            bar.write(regs::IRQ_ACK, DMA_IRQ.into());
            self.irq_dma_completion.complete();
        }

        // TEST_IRQ
        let magic = status & !DMA_IRQ;
        if magic == IRQ_MAGIC_VALUE {
            dev_dbg!(pdev, "handling test completion in irq\n");
            bar.write(regs::IRQ_ACK, magic.into());
            self.irq_test_completion.complete();
        }

        irq::IrqReturn::Handled
    }
}

#[pinned_drop]
impl PinnedDrop for EduDriver {
    fn drop(self: Pin<&mut Self>) {
        dev_dbg!(self.pdev, "Remove Rust EDU driver sample.\n");
    }
}

kernel::pci_device_table!(
    PCI_TABLE,
    MODULE_PCI_TABLE,
    <EduDriver as pci::Driver>::IdInfo,
    [(
        pci::DeviceId::from_id(pci::Vendor::from_raw(QEMU_VENDOR_ID), QEMU_EDU_DEVICE_ID),
        ()
    )]
);

kernel::module_pci_driver! {
    type: EduDriver,
    name: "rust_driver_edu",
    authors: ["Maurice Hieronymus"],
    description: "Rust EDU driver",
    license: "GPL v2",
}
