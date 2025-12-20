// SPDX-License-Identifier: GPL-2.0

//! Rust Serial device bus device driver sample.

use kernel::{
    acpi,
    device::{
        self,
        property::{
            FwNodeReferenceArgs,
            NArgs, //
        },
        Bound,
        Core, //
    },
    of,
    prelude::*,
    serdev,
    str::CString,
    sync::aref::ARef, //
};

struct SampleDriver {
    sdev: ARef<serdev::Device>,
}

struct Info(u32);

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <SampleDriver as serdev::Driver>::IdInfo,
    [(of::DeviceId::new(c"test,rust_driver_serdev"), Info(42))]
);

kernel::acpi_device_table!(
    ACPI_TABLE,
    MODULE_ACPI_TABLE,
    <SampleDriver as serdev::Driver>::IdInfo,
    [(acpi::DeviceId::new(c"LNUXBEEF"), Info(0))]
);

#[vtable]
impl serdev::Driver for SampleDriver {
    type IdInfo = Info;
    type InitialData = ();
    type LateProbeData = ();
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);

    fn probe(sdev: &serdev::Device, info: Option<&Self::IdInfo>) -> impl PinInit<Self, Error> {
        let dev = sdev.as_ref();

        dev_dbg!(dev, "Probe Rust Serial device bus device driver sample.\n");

        if let Some(info) = info {
            dev_info!(dev, "Probed with info: '{}'.\n", info.0);
        }

        if dev.fwnode().is_some_and(|node| node.is_of_node()) {
            Self::properties_parse(dev)?;
        }

        Ok(Self { sdev: sdev.into() })
    }

    fn configure(
        sdev: &serdev::Device<Core>,
        _this: Pin<&Self>,
        _id_info: Option<&Self::IdInfo>,
    ) -> Result {
        dev_dbg!(
            sdev.as_ref(),
            "Configure Rust Serial device bus device driver sample.\n"
        );

        sdev.set_baudrate(115200);
        sdev.set_flow_control(false);
        sdev.set_parity(serdev::Parity::None)?;
        Ok(())
    }

    fn late_probe(
        sdev: &serdev::Device<Bound>,
        _this: Pin<&Self>,
        _initial_data: Self::InitialData,
    ) -> impl PinInit<Self::LateProbeData, Error> {
        dev_dbg!(
            sdev.as_ref(),
            "Late Probe Rust Serial device bus device driver sample.\n"
        );
        Ok(())
    }

    fn receive(
        sdev: &serdev::Device<Bound>,
        _this: Pin<&Self>,
        _late_probe_this: Pin<&Self::LateProbeData>,
        data: &[u8],
    ) -> usize {
        let _ = sdev.write_all(data, serdev::Timeout::MaxScheduleTimeout);
        data.len()
    }
}

impl SampleDriver {
    fn properties_parse(dev: &device::Device) -> Result {
        let fwnode = dev.fwnode().ok_or(ENOENT)?;

        if let Ok(idx) = fwnode.property_match_string(c"compatible", c"test,rust-device") {
            dev_info!(dev, "matched compatible string idx = {}\n", idx);
        }

        let name = c"compatible";
        let prop = fwnode.property_read::<CString>(name).required_by(dev)?;
        dev_info!(dev, "'{name}'='{prop:?}'\n");

        let name = c"test,bool-prop";
        let prop = fwnode.property_read_bool(c"test,bool-prop");
        dev_info!(dev, "'{name}'='{prop}'\n");

        if fwnode.property_present(c"test,u32-prop") {
            dev_info!(dev, "'test,u32-prop' is present\n");
        }

        let name = c"test,u32-optional-prop";
        let prop = fwnode.property_read::<u32>(name).or(0x12);
        dev_info!(dev, "'{name}'='{prop:#x}' (default = 0x12)\n");

        // A missing required property will print an error. Discard the error to
        // prevent properties_parse from failing in that case.
        let name = c"test,u32-required-prop";
        let _ = fwnode.property_read::<u32>(name).required_by(dev);

        let name = c"test,u32-prop";
        let prop: u32 = fwnode.property_read(name).required_by(dev)?;
        dev_info!(dev, "'{name}'='{prop:#x}'\n");

        let name = c"test,i16-array";
        let prop: [i16; 4] = fwnode.property_read(name).required_by(dev)?;
        dev_info!(dev, "'{name}'='{prop:?}'\n");
        let len = fwnode.property_count_elem::<u16>(name)?;
        dev_info!(dev, "'{name}' length is {len}\n");

        let name = c"test,i16-array";
        let prop: KVec<i16> = fwnode.property_read_array_vec(name, 4)?.required_by(dev)?;
        dev_info!(dev, "'{name}'='{prop:?}' (KVec)\n");

        for child in fwnode.children() {
            let name = c"test,ref-arg";
            let nargs = NArgs::N(2);
            let prop: FwNodeReferenceArgs = child.property_get_reference_args(name, nargs, 0)?;
            dev_info!(dev, "'{name}'='{prop:?}'\n");
        }

        Ok(())
    }
}

impl Drop for SampleDriver {
    fn drop(&mut self) {
        dev_dbg!(
            self.sdev.as_ref(),
            "Remove Rust Serial device bus device driver sample.\n"
        );
    }
}

kernel::module_serdev_device_driver! {
    type: SampleDriver,
    name: "rust_driver_serdev",
    authors: ["Markus Probst"],
    description: "Rust Serial device bus device driver",
    license: "GPL v2",
}
