// SPDX-License-Identifier: GPL-2.0

//! Nova DRM Driver

use kernel::{
    auxiliary,
    driver::Registration,
    panic::{
        self,
        Lockdep,
        Taint, //
    },
    prelude::*,
    InPlaceModule, //
};

mod driver;
mod file;
mod gem;

use crate::driver::NovaDriver;

module! {
    type: NovaModule,
    name: "nova-drm",
    authors: ["Danilo Krummrich"],
    description: "Nova GPU driver",
    license: "GPL v2",
    params: {
        acknowledge_unstable_uapi: bool {
            default: false,
            description: "Acknowledge that Nova and its uAPI are under active upstream development \
                          and the uAPI may change incompatibly; enable the driver (taints kernel)",
        },
    },
}

#[pin_data]
struct NovaModule {
    #[pin]
    _driver: Registration<auxiliary::Adapter<NovaDriver>>,
}

impl NovaModule {
    fn check_unstable_uapi() -> Result {
        pr_warn!("Nova and its uAPI are under active upstream development\n");

        if !module_parameters::acknowledge_unstable_uapi.value() {
            pr_err!("Driver disabled: Nova's uAPI is unstable and may change incompatibly\n");
            pr_info!("Set nova-drm.acknowledge_unstable_uapi=1 to enable (taints kernel)\n");
            return Err(EPERM);
        }

        pr_warn!("Unstable uAPI enabled; userspace compatibility is not guaranteed\n");
        panic::add_taint(Taint::User, Lockdep::StillOk);

        Ok(())
    }
}

impl InPlaceModule for NovaModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            _: { Self::check_unstable_uapi()? },
            _driver <- Registration::new(<Self as kernel::ModuleMetadata>::NAME, module),
        })
    }
}
