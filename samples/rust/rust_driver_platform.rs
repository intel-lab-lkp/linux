// SPDX-License-Identifier: GPL-2.0

//! Rust Platform driver sample.

use kernel::{acpi, c_str, device::Core, of, platform, prelude::*, types::ARef};

struct SampleDriver {
    pdev: ARef<platform::Device>,
}

struct Info(u32);

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <SampleDriver as platform::Driver>::IdInfo,
    [(of::DeviceId::new(c_str!("test,rust-device")), Info(42))]
);

// ACPI match table test
//
// This demonstrates how to test an ACPI-based Rust platform driver using QEMU
// with a custom SSDT.
//
// Steps:
//
// 1. **Create an SSDT source file** (`ssdt.dsl`) with the following content:
//
//     ```asl
//     DefinitionBlock ("", "SSDT", 2, "TEST", "VIRTACPI", 0x00000001)
//     {
//         Scope (\_SB)
//         {
//             Device (T432)
//             {
//                 Name (_HID, "TEST4321")  // ACPI hardware ID to match
//                 Name (_UID, 1)
//                 Name (_STA, 0x0F)        // Device present, enabled
//                 Name (_CRS, ResourceTemplate ()
//                 {
//                     Memory32Fixed (ReadWrite, 0xFED00000, 0x1000)
//                 })
//             }
//         }
//     }
//     ```
//
// 2. **Compile the table**:
//
//     ```sh
//     iasl -tc ssdt.dsl
//     ```
//
//     This generates `ssdt.aml`
//
// 3. **Run QEMU** with the compiled AML file:
//
//     ```sh
//     qemu-system-x86_64 -m 512M \
//         -enable-kvm \
//         -kernel path/to/bzImage \
//         -append "root=/dev/sda console=ttyS0" \
//         -hda rootfs.img \
//         -serial stdio \
//         -acpitable file=ssdt.aml
//     ```
//
//     Requirements:
//     - The `rust_driver_platform` must be present either:
//         - built directly into the kernel (`bzImage`), or
//         - available as a `.ko` file and loadable from `rootfs.img`
//
// 4. **Verify it worked** by checking `dmesg`:
//
//     ```
//     rust_driver_platform TEST4321:00: Probed with info: '0'.
//     ```
//
// This demonstrates ACPI table matching using a custom ID in QEMU with a minimal SSDT

kernel::acpi_device_table!(
    ACPI_TABLE,
    MODULE_ACPI_TABLE,
    <SampleDriver as platform::Driver>::IdInfo,
    [(acpi::DeviceId::new(c_str!("TEST4321")), Info(0))]
);

impl platform::Driver for SampleDriver {
    type IdInfo = Info;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        info: Option<&Self::IdInfo>,
    ) -> Result<Pin<KBox<Self>>> {
        dev_dbg!(pdev.as_ref(), "Probe Rust Platform driver sample.\n");

        if let Some(info) = info {
            dev_info!(pdev.as_ref(), "Probed with info: '{}'.\n", info.0);
        }

        let drvdata = KBox::new(Self { pdev: pdev.into() }, GFP_KERNEL)?;

        Ok(drvdata.into())
    }
}

impl Drop for SampleDriver {
    fn drop(&mut self) {
        dev_dbg!(self.pdev.as_ref(), "Remove Rust Platform driver sample.\n");
    }
}

kernel::module_platform_driver! {
    type: SampleDriver,
    name: "rust_driver_platform",
    authors: ["Danilo Krummrich"],
    description: "Rust Platform driver",
    license: "GPL v2",
}
