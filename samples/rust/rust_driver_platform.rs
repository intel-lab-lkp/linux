// SPDX-License-Identifier: GPL-2.0

//! Rust Platform driver sample.

use kernel::{c_str, device::Core, of, platform, prelude::*, str::CString, types::ARef};

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

impl platform::Driver for SampleDriver {
    type IdInfo = Info;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        info: Option<&Self::IdInfo>,
    ) -> Result<Pin<KBox<Self>>> {
        let dev = pdev.as_ref();

        dev_dbg!(pdev.as_ref(), "Probe Rust Platform driver sample.\n");

        if let Some(info) = info {
            dev_info!(dev, "Probed with info: '{}'.\n", info.0);
        }

        Self::properties_parse(dev)?;

        let drvdata = KBox::new(Self { pdev: pdev.into() }, GFP_KERNEL)?;

        Ok(drvdata.into())
    }
}

impl SampleDriver {
    fn properties_parse(dev: &kernel::device::Device) -> Result<()> {
        let fwnode = dev.fwnode().ok_or(ENOENT)?;

        if let Ok(idx) =
            fwnode.property_match_string(c_str!("compatible"), c_str!("test,rust-device"))
        {
            dev_info!(dev, "matched compatible string idx = {}\n", idx);
        }

        if let Ok(str) = fwnode
            .property_read::<CString>(c_str!("compatible"))
            .required_by(dev)
        {
            dev_info!(dev, "compatible string = {:?}\n", str);
        }

        let prop = fwnode.property_read_bool(c_str!("test,bool-prop"));
        dev_info!(dev, "bool prop is {}\n", prop);

        if fwnode.property_present(c_str!("test,u32-prop")) {
            dev_info!(dev, "'test,u32-prop' is present\n");
        }

        let prop = fwnode
            .property_read::<u32>(c_str!("test,u32-optional-prop"))
            .or(0x12);
        dev_info!(
            dev,
            "'test,u32-optional-prop' is {:#x} (default = {:#x})\n",
            prop,
            0x12
        );

        // Missing property without a default will print an error
        let _ = fwnode
            .property_read::<u32>(c_str!("test,u32-required-prop"))
            .required_by(dev);

        let prop: u32 = fwnode
            .property_read(c_str!("test,u32-prop"))
            .required_by(dev)?;
        dev_info!(dev, "'test,u32-prop' is {:#x}\n", prop);

        let prop: [i16; 4] = fwnode
            .property_read(c_str!("test,i16-array"))
            .required_by(dev)?;
        dev_info!(dev, "'test,i16-array' is {:?}\n", prop);
        dev_info!(
            dev,
            "'test,i16-array' length is {}\n",
            fwnode.property_count_elem::<u16>(c_str!("test,i16-array"))?,
        );

        let prop: KVec<i16> = fwnode
            .property_read_array_vec(c_str!("test,i16-array"), 4)?
            .required_by(dev)?;
        dev_info!(dev, "'test,i16-array' is KVec {:?}\n", prop);

        Ok(())
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
