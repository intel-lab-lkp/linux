// SPDX-License-Identifier: GPL-2.0

use kernel::{
    device,
    dma::Coherent,
    io::poll::read_poll_timeout,
    pci,
    prelude::*,
    time::Delta, //
};

use crate::{
    driver::Bar0,
    falcon::{
        gsp::Gsp,
        sec2::Sec2,
        Falcon,
        FalconEngine, //
    },
    fb::FbLayout,
    firmware::{
        booter::{
            BooterFirmware,
            BooterKind, //
        },
        fwsec::{
            bootloader::FwsecFirmwareWithBl,
            FwsecCommand,
            FwsecFirmware, //
        },
        gsp::GspFirmware,
        FIRMWARE_VERSION, //
    },
    gpu::Chipset,
    gsp::{
        commands,
        sequencer::{
            GspSequencer,
            GspSequencerParams, //
        },
        GspFwWprMeta, //
    },
    regs,
    vbios::Vbios,
};

/// GSP lockdown pattern written by firmware to mbox0 while RISC-V branch privilege
/// lockdown is active. The low byte varies, the upper 24 bits are fixed.
const GSP_LOCKDOWN_PATTERN: u32 = 0xbadf4100;
const GSP_LOCKDOWN_MASK: u32 = 0xffffff00;

/// GSP falcon mailbox state, used to track lockdown release status.
struct GspMbox {
    mbox0: u32,
    mbox1: u32,
}

impl GspMbox {
    /// Read both mailboxes from the GSP falcon.
    fn read(gsp_falcon: &Falcon<Gsp>, bar: &Bar0) -> Self {
        Self {
            mbox0: gsp_falcon.read_mailbox0(bar),
            mbox1: gsp_falcon.read_mailbox1(bar),
        }
    }

    /// Returns true if the lockdown pattern is present in mbox0.
    fn is_locked_down(&self) -> bool {
        self.mbox0 != 0 && (self.mbox0 & GSP_LOCKDOWN_MASK) == GSP_LOCKDOWN_PATTERN
    }

    /// Combines mailbox0 and mailbox1 into a 64-bit address.
    fn combined_addr(&self) -> u64 {
        (u64::from(self.mbox1) << 32) | u64::from(self.mbox0)
    }

    /// Returns true if GSP lockdown has been released.
    ///
    /// Checks the lockdown pattern, validates the boot params address,
    /// and verifies the HWCFG2 lockdown bit is clear.
    fn lockdown_released(&self, bar: &Bar0, fmc_boot_params_addr: u64) -> bool {
        if self.is_locked_down() {
            return false;
        }

        if self.mbox0 != 0 && self.combined_addr() != fmc_boot_params_addr {
            return true;
        }

        let hwcfg2 = regs::NV_PFALCON_FALCON_HWCFG2::read(bar, &crate::falcon::gsp::Gsp::ID);
        !hwcfg2.riscv_br_priv_lockdown()
    }
}

impl super::Gsp {
    /// Helper function to load and run the FWSEC-FRTS firmware and confirm that it has properly
    /// created the WPR2 region.
    fn run_fwsec_frts(
        dev: &device::Device<device::Bound>,
        chipset: Chipset,
        falcon: &Falcon<Gsp>,
        bar: &Bar0,
        bios: &Vbios,
        fb_layout: &FbLayout,
    ) -> Result<()> {
        // Check that the WPR2 region does not already exists - if it does, we cannot run
        // FWSEC-FRTS until the GPU is reset.
        if regs::NV_PFB_PRI_MMU_WPR2_ADDR_HI::read(bar).higher_bound() != 0 {
            dev_err!(
                dev,
                "WPR2 region already exists - GPU needs to be reset to proceed\n"
            );
            return Err(EBUSY);
        }

        // FWSEC-FRTS will create the WPR2 region.
        let fwsec_frts = FwsecFirmware::new(
            dev,
            falcon,
            bar,
            bios,
            FwsecCommand::Frts {
                frts_addr: fb_layout.frts.start,
                frts_size: fb_layout.frts.len(),
            },
        )?;

        if chipset.needs_fwsec_bootloader() {
            let fwsec_frts_bl = FwsecFirmwareWithBl::new(fwsec_frts, dev, chipset)?;
            // Load and run the bootloader, which will load FWSEC-FRTS and run it.
            fwsec_frts_bl.run(dev, falcon, bar)?;
        } else {
            // Load and run FWSEC-FRTS directly.
            fwsec_frts.run(dev, falcon, bar)?;
        }

        // SCRATCH_E contains the error code for FWSEC-FRTS.
        let frts_status = regs::NV_PBUS_SW_SCRATCH_0E_FRTS_ERR::read(bar).frts_err_code();
        if frts_status != 0 {
            dev_err!(
                dev,
                "FWSEC-FRTS returned with error code {:#x}\n",
                frts_status
            );

            return Err(EIO);
        }

        // Check that the WPR2 region has been created as we requested.
        let (wpr2_lo, wpr2_hi) = (
            regs::NV_PFB_PRI_MMU_WPR2_ADDR_LO::read(bar).lower_bound(),
            regs::NV_PFB_PRI_MMU_WPR2_ADDR_HI::read(bar).higher_bound(),
        );

        match (wpr2_lo, wpr2_hi) {
            (_, 0) => {
                dev_err!(dev, "WPR2 region not created after running FWSEC-FRTS\n");

                Err(EIO)
            }
            (wpr2_lo, _) if wpr2_lo != fb_layout.frts.start => {
                dev_err!(
                    dev,
                    "WPR2 region created at unexpected address {:#x}; expected {:#x}\n",
                    wpr2_lo,
                    fb_layout.frts.start,
                );

                Err(EIO)
            }
            (wpr2_lo, wpr2_hi) => {
                dev_dbg!(dev, "WPR2: {:#x}-{:#x}\n", wpr2_lo, wpr2_hi);
                dev_dbg!(dev, "GPU instance built\n");

                Ok(())
            }
        }
    }

    fn run_booter(
        dev: &device::Device<device::Bound>,
        bar: &Bar0,
        chipset: Chipset,
        sec2_falcon: &Falcon<Sec2>,
        wpr_meta: &Coherent<GspFwWprMeta>,
    ) -> Result {
        let booter = BooterFirmware::new(
            dev,
            BooterKind::Loader,
            chipset,
            FIRMWARE_VERSION,
            sec2_falcon,
            bar,
        )?;

        booter.run(dev, bar, sec2_falcon, wpr_meta)
    }

    /// Wait for GSP lockdown to be released after FSP Chain of Trust.
    #[expect(dead_code)]
    fn wait_for_gsp_lockdown_release(
        dev: &device::Device<device::Bound>,
        bar: &Bar0,
        gsp_falcon: &Falcon<Gsp>,
        fmc_boot_params_addr: u64,
    ) -> Result {
        dev_dbg!(dev, "Waiting for GSP lockdown release\n");

        let mbox = read_poll_timeout(
            || Ok(GspMbox::read(gsp_falcon, bar)),
            |mbox| mbox.lockdown_released(bar, fmc_boot_params_addr),
            Delta::from_millis(10),
            Delta::from_secs(30),
        )
        .inspect_err(|_| {
            dev_err!(dev, "GSP lockdown release timeout\n");
        })?;

        if mbox.mbox0 != 0 {
            dev_err!(dev, "GSP-FMC boot failed (mbox: {:#x})\n", mbox.mbox0);
            return Err(EIO);
        }

        dev_dbg!(dev, "GSP lockdown released\n");
        Ok(())
    }

    /// Attempt to boot the GSP.
    ///
    /// This is a GPU-dependent and complex procedure that involves loading firmware files from
    /// user-space, patching them with signatures, and building firmware-specific intricate data
    /// structures that the GSP will use at runtime.
    ///
    /// Upon return, the GSP is up and running, and its runtime object given as return value.
    pub(crate) fn boot(
        self: Pin<&mut Self>,
        pdev: &pci::Device<device::Bound>,
        bar: &Bar0,
        chipset: Chipset,
        gsp_falcon: &Falcon<Gsp>,
        sec2_falcon: &Falcon<Sec2>,
    ) -> Result {
        let dev = pdev.as_ref();

        let bios = Vbios::new(dev, bar)?;

        let gsp_fw = KBox::pin_init(GspFirmware::new(dev, chipset, FIRMWARE_VERSION), GFP_KERNEL)?;

        let fb_layout = FbLayout::new(chipset, bar, &gsp_fw)?;
        dev_dbg!(dev, "{:#x?}\n", fb_layout);

        Self::run_fwsec_frts(dev, chipset, gsp_falcon, bar, &bios, &fb_layout)?;

        let wpr_meta = Coherent::init(dev, GFP_KERNEL, GspFwWprMeta::new(&gsp_fw, &fb_layout))?;

        self.cmdq
            .send_command_no_wait(bar, commands::SetSystemInfo::new(pdev, chipset))?;
        self.cmdq
            .send_command_no_wait(bar, commands::SetRegistry::new())?;

        gsp_falcon.reset(bar)?;
        let libos_handle = self.libos.dma_handle();
        let (mbox0, mbox1) = gsp_falcon.boot(
            bar,
            Some(libos_handle as u32),
            Some((libos_handle >> 32) as u32),
        )?;
        dev_dbg!(pdev, "GSP MBOX0: {:#x}, MBOX1: {:#x}\n", mbox0, mbox1);

        dev_dbg!(
            pdev,
            "Using SEC2 to load and run the booter_load firmware...\n"
        );

        Self::run_booter(dev, bar, chipset, sec2_falcon, &wpr_meta)?;

        gsp_falcon.write_os_version(bar, gsp_fw.bootloader.app_version);

        // Poll for RISC-V to become active before running sequencer
        read_poll_timeout(
            || Ok(gsp_falcon.is_riscv_active(bar)),
            |val: &bool| *val,
            Delta::from_millis(10),
            Delta::from_secs(5),
        )?;

        dev_dbg!(pdev, "RISC-V active? {}\n", gsp_falcon.is_riscv_active(bar),);

        // Create and run the GSP sequencer.
        let seq_params = GspSequencerParams {
            bootloader_app_version: gsp_fw.bootloader.app_version,
            libos_dma_handle: libos_handle,
            gsp_falcon,
            sec2_falcon,
            dev: pdev.as_ref().into(),
            bar,
        };
        GspSequencer::run(&self.cmdq, seq_params)?;

        // Wait until GSP is fully initialized.
        commands::wait_gsp_init_done(&self.cmdq)?;

        // Obtain and display basic GPU information.
        let info = commands::get_gsp_info(&self.cmdq, bar)?;
        match info.gpu_name() {
            Ok(name) => dev_info!(pdev, "GPU name: {}\n", name),
            Err(e) => dev_warn!(pdev, "GPU name unavailable: {:?}\n", e),
        }

        Ok(())
    }
}
