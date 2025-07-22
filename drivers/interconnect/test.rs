// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

//! Test interconnect consumer driver
use kernel::{
    c_str, device::Core, icc::*, module_platform_driver, of, of::DeviceId, platform, prelude::*,
};

#[pin_data]
struct IccTestConsumerDriver {
    #[pin]
    path: IccPath,
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <IccTestConsumerDriver as platform::Driver>::IdInfo,
    [(DeviceId::new(c_str!("linux,icc-consumer-test")), ())]
);

impl platform::Driver for IccTestConsumerDriver {
    type IdInfo = ();
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        _id_info: Option<&Self::IdInfo>,
    ) -> Result<Pin<KBox<Self>>> {
        let path = IccPath::of_get(pdev.as_ref(), None)?;

        path.set_bw(
            IccBwUnit::from_megabits_per_sec(400),
            IccBwUnit::from_megabits_per_sec(800),
        )?;

        Ok(KBox::pin_init(Self { path }, GFP_KERNEL)?.into())
    }
}

module_platform_driver! {
    type: IccTestConsumerDriver,
    name: "icc-test-consumer",
    authors: ["Konrad Dybcio <konrad.dybcio@oss.qualcomm.com>"],
    description: "Test interconnect consumer driver",
    license: "GPL",
}
