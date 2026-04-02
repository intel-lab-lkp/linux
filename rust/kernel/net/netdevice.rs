// SPDX-License-Identifier: GPL-2.0

//! Network device support.
//!
//! C headers: [`include/linux/netdevice.h`](srctree/include/linux/netdevice.h)

use crate::{
    bindings,
    error::from_result,
    net::skbuff,
    prelude::*,
    types::Opaque,
};

/// Network device feature flags.
pub type Features = bindings::netdev_features_t;

/// `features` bits used by the MVP link-type setup path.
pub mod features {
    use super::{bindings, Features};

    const fn bit(index: u32) -> Features {
        1u64 << index
    }

    /// Equivalent to `NETIF_F_SG`.
    pub const SG: Features = bit(bindings::NETIF_F_SG_BIT as u32);

    /// Equivalent to `NETIF_F_FRAGLIST`.
    pub const FRAGLIST: Features = bit(bindings::NETIF_F_FRAGLIST_BIT as u32);

    /// Equivalent to `NETIF_F_HIGHDMA`.
    pub const HIGHDMA: Features = bit(bindings::NETIF_F_HIGHDMA_BIT as u32);
}

/// MTU-related constants and helpers used by the MVP link-type setup path.
pub mod mtu {
    use super::bindings;

    const fn align(value: usize, alignment: usize) -> usize {
        (value + alignment - 1) & !(alignment - 1)
    }

    /// Equivalent to `NLMSG_GOODSIZE`.
    pub const fn nlmsg_goodsize() -> u32 {
        let page = if bindings::PAGE_SIZE < 8192 {
            bindings::PAGE_SIZE
        } else {
            8192
        };
        let overhead = align(
            core::mem::size_of::<bindings::skb_shared_info>(),
            bindings::SMP_CACHE_BYTES as usize,
        );

        (page - overhead) as u32
    }

    /// Equivalent to `sizeof(struct nlmsghdr)`.
    pub const NLMSGHDR: u32 = core::mem::size_of::<bindings::nlmsghdr>() as u32;
}

/// `priv_flags` bits used by the MVP link-type setup path.
pub mod priv_flags {
    use super::bindings;

    /// Equivalent to `IFF_NO_QUEUE`.
    pub const NO_QUEUE: u32 = bindings::netdev_priv_flags_IFF_NO_QUEUE;
}

/// `flags` bits used by the MVP link-type setup path.
pub mod flags {
    use super::bindings;

    /// Equivalent to `IFF_NOARP`.
    pub const NO_ARP: u32 = bindings::net_device_flags_IFF_NOARP;
}

/// Device type constants used by the MVP link-type setup path.
pub mod device_type {
    use super::bindings;

    /// Equivalent to `ARPHRD_NETLINK`.
    pub const NETLINK: u16 = bindings::ARPHRD_NETLINK as u16;
}

/// Per-cpu stat type constants used by the MVP link-type setup path.
pub mod pcpu_stat_type {
    use super::bindings;

    /// Equivalent to `NETDEV_PCPU_STAT_LSTATS`.
    pub const LSTATS: bindings::netdev_stat_type =
        bindings::netdev_stat_type_NETDEV_PCPU_STAT_LSTATS;
}

/// Result of an `ndo_start_xmit` callback.
pub enum TxOutcome {
    /// The skb has been consumed and may be dropped by the abstraction.
    Ok,

    /// The device is temporarily busy; ownership of the skb goes back to the networking core.
    Busy(skbuff::SkBuff),
}

/// Operations exposed by a network device for the MVP `nlmon` loop.
///
/// The callback trampolines live in this abstraction layer so that driver code does not have to
/// touch raw kernel pointers or private storage casts directly.
pub trait Operations: Send + Sync + 'static {
    /// The type stored in `net_device` private storage.
    ///
    /// The RTNL/netdevice allocation path zero-initializes private storage before any driver
    /// callback observes it, so implementations must accept the all-zero bit pattern.
    type Private: Zeroable;

    /// Device open callback.
    fn open(dev: &mut Device, private: Pin<&mut Self::Private>) -> Result;

    /// Device stop callback.
    fn stop(dev: &mut Device, private: Pin<&mut Self::Private>) -> Result;

    /// Packet transmit callback.
    ///
    /// This only exposes a shared device view: TX callbacks may run concurrently, so the
    /// abstraction must not synthesize an exclusive borrow of either `net_device` or private
    /// driver state here.
    fn start_xmit(skb: skbuff::SkBuff, dev: &Device) -> TxOutcome;
}

/// A Rust wrapper over `struct net_device`.
///
/// # Invariants
///
/// - The wrapped pointer always points to a valid `struct net_device`.
/// - The caller is responsible for only creating references in contexts where the kernel allows
///   the accessed fields to be mutated.
#[repr(transparent)]
pub struct Device(Opaque<bindings::net_device>);

impl Device {
    /// Creates a mutable wrapper from a raw `net_device` pointer.
    ///
    /// # Safety
    ///
    /// The pointer must point to a valid `struct net_device` for the lifetime of the returned
    /// reference, and the caller must ensure that it is safe to mutate through it.
    pub(crate) unsafe fn from_raw<'a>(ptr: *mut bindings::net_device) -> &'a mut Self {
        let ptr = ptr.cast::<Self>();
        // SAFETY: The caller guarantees the pointer is valid for the returned lifetime.
        unsafe { &mut *ptr }
    }

    /// Creates a shared wrapper from a raw `net_device` pointer.
    ///
    /// # Safety
    ///
    /// The pointer must point to a valid `struct net_device` for the lifetime of the returned
    /// reference.
    pub(crate) unsafe fn from_raw_ref<'a>(ptr: *mut bindings::net_device) -> &'a Self {
        let ptr = ptr.cast::<Self>();
        // SAFETY: The caller guarantees the pointer is valid for the returned lifetime.
        unsafe { &*ptr }
    }

    /// Returns the wrapped raw `net_device` pointer.
    pub(crate) fn as_ptr(&self) -> *mut bindings::net_device {
        self.0.get()
    }

    /// Sets `dev->type`.
    pub fn set_type(&mut self, device_type: u16) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`.
        unsafe { (*dev).type_ = device_type };
    }

    /// ORs a bit into `dev->priv_flags`.
    pub fn add_priv_flag(&mut self, flag: u32) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`, and the
        // bindgen-generated accessors preserve the layout of the `priv_flags/lltx` union.
        unsafe {
            let flags = &mut (*dev).__bindgen_anon_1.__bindgen_anon_1;
            let current = flags.priv_flags();
            let flag = flag as usize;
            flags.set_priv_flags(current | flag);
        }
    }

    /// Sets `dev->lltx`.
    pub fn set_lltx(&mut self, enabled: bool) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`, and the
        // bindgen-generated accessors preserve the layout of the `priv_flags/lltx` union.
        unsafe {
            let flags = &mut (*dev).__bindgen_anon_1.__bindgen_anon_1;
            flags.set_lltx(if enabled { 1 } else { 0 });
        }
    }

    /// Stores a typed `net_device_ops` vtable pointer into `dev->netdev_ops`.
    pub fn set_netdevice_ops<T: Operations>(&mut self) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`.
        unsafe { (*dev).netdev_ops = OperationsVTable::<T>::build() };
    }

    /// Sets `dev->needs_free_netdev`.
    pub fn set_needs_free_netdev(&mut self, enabled: bool) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`.
        unsafe { (*dev).needs_free_netdev = enabled };
    }

    /// Sets `dev->features`.
    pub fn set_features(&mut self, features: Features) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`.
        unsafe { (*dev).features = features };
    }

    /// Sets `dev->flags`.
    pub fn set_flags(&mut self, flags: u32) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`.
        unsafe { (*dev).flags = flags };
    }

    /// Sets `dev->mtu`.
    pub fn set_mtu(&mut self, mtu: u32) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`.
        unsafe { (*dev).mtu = mtu };
    }

    /// Sets `dev->min_mtu`.
    pub fn set_min_mtu(&mut self, mtu: u32) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`.
        unsafe { (*dev).min_mtu = mtu };
    }

    /// Sets `dev->pcpu_stat_type`.
    pub fn set_pcpu_stat_type(&mut self, stat_type: bindings::netdev_stat_type) {
        let dev = self.as_ptr();
        // SAFETY: The type invariant guarantees that `dev` points to a valid `net_device`.
        unsafe { (*dev).set_pcpu_stat_type(stat_type) };
    }

    /// Returns a typed raw pointer to `net_device` private storage.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the current `net_device` was allocated with private storage for
    /// `T`. Dereferencing the returned pointer additionally requires exclusive access to that
    /// storage.
    pub(crate) unsafe fn private_ptr<T>(&self) -> *mut T {
        // SAFETY: The caller guarantees that the private storage really contains a `T`, and the
        // helper/exported wrapper preserves the C `netdev_priv` semantics.
        unsafe { bindings::netdev_priv(self.as_ptr() as *const bindings::net_device).cast::<T>() }
    }
}

struct OperationsVTable<T: Operations>(core::marker::PhantomData<T>);

impl<T: Operations> OperationsVTable<T> {
    extern "C" fn open_callback(dev: *mut bindings::net_device) -> c_int {
        from_result(|| {
            // SAFETY: The networking core only calls this callback with a valid `net_device`.
            let dev = unsafe { Device::from_raw(dev) };
            // SAFETY: The `rtnl::Registration<T>` abstraction ties `priv_size` to `T::Private`.
            let private = unsafe { Pin::new_unchecked(&mut *dev.private_ptr::<T::Private>()) };
            T::open(dev, private)?;
            Ok(0)
        })
    }

    extern "C" fn stop_callback(dev: *mut bindings::net_device) -> c_int {
        from_result(|| {
            // SAFETY: The networking core only calls this callback with a valid `net_device`.
            let dev = unsafe { Device::from_raw(dev) };
            // SAFETY: The `rtnl::Registration<T>` abstraction ties `priv_size` to `T::Private`.
            let private = unsafe { Pin::new_unchecked(&mut *dev.private_ptr::<T::Private>()) };
            T::stop(dev, private)?;
            Ok(0)
        })
    }

    extern "C" fn start_xmit_callback(
        skb: *mut bindings::sk_buff,
        dev: *mut bindings::net_device,
    ) -> bindings::netdev_tx {
        // SAFETY: The networking core only calls this callback with a valid `net_device`.
        let dev = unsafe { Device::from_raw_ref(dev) };
        // SAFETY: The networking core transfers ownership of the callback skb to the driver.
        let skb = unsafe { skbuff::SkBuff::from_raw_owned(skb) };

        match T::start_xmit(skb, dev) {
            TxOutcome::Ok => bindings::netdev_tx_NETDEV_TX_OK,
            TxOutcome::Busy(skb) => {
                let _ = skb.into_raw();
                bindings::netdev_tx_NETDEV_TX_BUSY
            }
        }
    }

    const VTABLE: bindings::net_device_ops = bindings::net_device_ops {
        ndo_open: Some(Self::open_callback),
        ndo_stop: Some(Self::stop_callback),
        ndo_start_xmit: Some(Self::start_xmit_callback),
        // SAFETY: A zeroed `net_device_ops` is valid because omitted callbacks are represented by
        // null pointers and all remaining fields are plain data/pointers.
        ..unsafe { core::mem::zeroed() }
    };

    pub(crate) const fn build() -> &'static bindings::net_device_ops {
        &Self::VTABLE
    }
}
