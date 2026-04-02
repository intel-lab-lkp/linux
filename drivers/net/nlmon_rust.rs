// SPDX-License-Identifier: GPL-2.0

#![forbid(unsafe_code)]

//! Rust netlink monitoring device.
//!
//! C version of this driver: [`drivers/net/nlmon.c`](./nlmon.c)

use kernel::{
    net::{netdevice, netlink_tap, rtnl, skbuff, stats},
    prelude::*,
};

module! {
    type: NlmonModule,
    name: "nlmon_rust",
    authors: [
        "Daniel Borkmann <dborkman@redhat.com>",
        "Mathieu Geli <geli@enseirb.fr>",
    ],
    description: "Rust netlink monitoring device",
    license: "GPL v2",
    alias: ["rtnl-link-nlmon"],
}

#[pin_data]
struct NlmonModule {
    #[pin]
    registration: rtnl::Registration<NlmonDriver>,
}

impl kernel::InPlaceModule for NlmonModule {
    fn init(_module: &'static ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            registration <- rtnl::Registration::new(),
        })
    }
}

struct NlmonDriver;

#[pin_data]
#[derive(Zeroable)]
#[repr(C)]
struct NlmonPriv {
    #[pin]
    tap: netlink_tap::Tap,
}

impl netdevice::Operations for NlmonDriver {
    type Private = NlmonPriv;

    fn open(dev: &mut netdevice::Device, private: Pin<&mut Self::Private>) -> Result {
        private.project().tap.add(dev, &THIS_MODULE)
    }

    fn stop(_dev: &mut netdevice::Device, private: Pin<&mut Self::Private>) -> Result {
        private.project().tap.remove()
    }

    fn start_xmit(skb: skbuff::SkBuff, dev: &netdevice::Device) -> netdevice::TxOutcome {
        stats::dev_lstats_add(dev, skb.len());
        netdevice::TxOutcome::Ok
    }
}

impl rtnl::Driver for NlmonDriver {
    const KIND: &'static CStr = c"nlmon";

    fn setup(dev: &mut netdevice::Device) {
        let features = netdevice::features::SG
            | netdevice::features::FRAGLIST
            | netdevice::features::HIGHDMA;

        dev.set_type(netdevice::device_type::NETLINK);
        dev.add_priv_flag(netdevice::priv_flags::NO_QUEUE);
        dev.set_lltx(true);
        dev.set_needs_free_netdev(true);
        dev.set_features(features);
        dev.set_flags(netdevice::flags::NO_ARP);
        dev.set_pcpu_stat_type(netdevice::pcpu_stat_type::LSTATS);
        dev.set_mtu(netdevice::mtu::nlmsg_goodsize());
        dev.set_min_mtu(netdevice::mtu::NLMSGHDR);
    }

    fn validate(ctx: &mut rtnl::ValidateContext<'_>) -> Result {
        if ctx.has_link_attr(rtnl::LinkAttr::ADDRESS) {
            return Err(EINVAL);
        }

        Ok(())
    }
}
