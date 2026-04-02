// SPDX-License-Identifier: GPL-2.0

//! RTNL link-type registration support.
//!
//! C headers:
//! - [`include/net/rtnetlink.h`](srctree/include/net/rtnetlink.h)
//! - [`include/uapi/linux/if_link.h`](srctree/include/uapi/linux/if_link.h)

use crate::{
    bindings,
    error::{from_result, to_result},
    net::netdevice,
    prelude::*,
    types::Opaque,
};

use core::{
    marker::PhantomData,
    mem::size_of,
};

/// Typed link-level netlink attribute selector.
#[derive(Clone, Copy)]
pub struct LinkAttr(usize);

impl LinkAttr {
    /// Equivalent to `IFLA_ADDRESS`.
    pub const ADDRESS: Self = Self(bindings::IFLA_ADDRESS as usize);

    const fn as_index(self) -> usize {
        self.0
    }
}

/// Typed info-level netlink attribute selector.
#[derive(Clone, Copy)]
pub struct InfoAttr(usize);

impl InfoAttr {
    /// Equivalent to `IFLA_INFO_KIND`.
    pub const KIND: Self = Self(bindings::IFLA_INFO_KIND as usize);

    /// Equivalent to `IFLA_INFO_DATA`.
    pub const DATA: Self = Self(bindings::IFLA_INFO_DATA as usize);

    const fn as_index(self) -> usize {
        self.0
    }
}

const LINK_ATTR_TABLE_LEN: usize = bindings::__IFLA_MAX as usize;
const INFO_ATTR_TABLE_LEN: usize = bindings::__IFLA_INFO_MAX as usize;

/// Validation context for `rtnl_link_ops::validate`.
pub struct ValidateContext<'a> {
    link_attrs: NlAttrTable,
    data_attrs: NlAttrTable,
    extack: Option<&'a mut ExtAck>,
}

impl<'a> ValidateContext<'a> {
    fn new(
        link_attrs: NlAttrTable,
        data_attrs: NlAttrTable,
        extack: Option<&'a mut ExtAck>,
    ) -> Self {
        Self {
            link_attrs,
            data_attrs,
            extack,
        }
    }

    /// Returns whether a link-level netlink attribute is present.
    pub fn has_link_attr(&self, attr: LinkAttr) -> bool {
        self.link_attrs.has_attr_index(attr.as_index())
    }

    /// Returns whether an info-level netlink attribute is present.
    pub fn has_info_attr(&self, attr: InfoAttr) -> bool {
        self.data_attrs.has_attr_index(attr.as_index())
    }

    /// Returns the optional extack wrapper.
    pub fn extack(&mut self) -> Option<&mut ExtAck> {
        self.extack.as_deref_mut()
    }
}

/// Safe view over the `struct nlattr *tb[]` / `data[]` arrays passed to validate callbacks.
#[derive(Clone, Copy)]
pub struct NlAttrTable {
    raw: *mut *mut bindings::nlattr,
    len: usize,
}

impl NlAttrTable {
    fn new(raw: *mut *mut bindings::nlattr, len: usize) -> Self {
        Self { raw, len }
    }

    /// Returns `true` if the attribute slot contains a non-null pointer.
    fn has_attr_index(&self, attr: usize) -> bool {
        if self.raw.is_null() || attr >= self.len {
            return false;
        }

        // SAFETY: The RTNL core provides these arrays for validate callbacks. Indexing is kept in
        // the abstraction layer so driver code does not perform raw pointer arithmetic.
        unsafe { !(*self.raw.add(attr)).is_null() }
    }

}

/// Wrapper over `struct netlink_ext_ack`.
#[repr(transparent)]
pub struct ExtAck(Opaque<bindings::netlink_ext_ack>);

impl ExtAck {
    /// Creates a mutable wrapper from a raw extack pointer.
    ///
    /// # Safety
    ///
    /// The pointer must be valid for the returned lifetime.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::netlink_ext_ack) -> &'a mut Self {
        let ptr = ptr.cast::<Self>();
        // SAFETY: The caller guarantees validity for the returned lifetime.
        unsafe { &mut *ptr }
    }
}

/// A Rust RTNL link-type driver.
pub trait Driver: netdevice::Operations {
    /// The RTNL link kind, e.g. `"nlmon"`.
    const KIND: &'static CStr;

    /// Performs link-type setup.
    fn setup(dev: &mut netdevice::Device);

    /// Optional netlink validation.
    fn validate(_ctx: &mut ValidateContext<'_>) -> Result {
        Ok(())
    }
}

/// Owns an RTNL link-type registration.
#[pin_data(PinnedDrop)]
pub struct Registration<T: Driver> {
    #[pin]
    ops: Opaque<bindings::rtnl_link_ops>,
    _driver: PhantomData<T>,
}

// SAFETY: Shared references do not expose interior mutation beyond drop semantics.
unsafe impl<T: Driver> Sync for Registration<T> {}

// SAFETY: Registration and unregistration are handled by RTNL core code and can be performed from
// the module init/exit path.
unsafe impl<T: Driver> Send for Registration<T> {}

impl<T: Driver> Registration<T> {
    extern "C" fn setup_callback(dev: *mut bindings::net_device) {
        // SAFETY: The RTNL core only calls setup with a valid `net_device`.
        let dev = unsafe { netdevice::Device::from_raw(dev) };
        dev.set_netdevice_ops::<T>();
        T::setup(dev);
    }

    extern "C" fn validate_callback(
        tb: *mut *mut bindings::nlattr,
        data: *mut *mut bindings::nlattr,
        extack: *mut bindings::netlink_ext_ack,
    ) -> c_int {
        from_result(|| {
            let extack = if extack.is_null() {
                None
            } else {
                // SAFETY: The RTNL core passes a valid extack pointer when non-null.
                Some(unsafe { ExtAck::from_raw(extack) })
            };
            let mut ctx = ValidateContext::new(
                NlAttrTable::new(tb, LINK_ATTR_TABLE_LEN),
                NlAttrTable::new(data, INFO_ATTR_TABLE_LEN),
                extack,
            );
            T::validate(&mut ctx)?;
            Ok(0)
        })
    }

    /// Creates a new RTNL registration object.
    pub fn new() -> impl PinInit<Self, Error> {
        build_assert!(!core::mem::needs_drop::<T::Private>());
        try_pin_init!(Self {
            ops <- Opaque::try_ffi_init(|ptr: *mut bindings::rtnl_link_ops| {
                // SAFETY: All-zero is a valid initial state for the opaque RTNL ops structure.
                unsafe { ptr.write(core::mem::zeroed()) };

                // SAFETY: `ptr` is valid for writes for the duration of this initializer.
                unsafe {
                    (*ptr).kind = T::KIND.as_char_ptr();
                    (*ptr).priv_size = size_of::<T::Private>();
                    (*ptr).setup = Some(Self::setup_callback);
                    (*ptr).validate = Some(Self::validate_callback);
                }

                // SAFETY: `ptr` now points to a fully initialized `rtnl_link_ops`.
                to_result(unsafe { bindings::rtnl_link_register(ptr) })
            }),
            _driver: PhantomData,
        })
    }
}

#[pinned_drop]
impl<T: Driver> PinnedDrop for Registration<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: The existence of `self` guarantees a successful earlier registration.
        unsafe { bindings::rtnl_link_unregister(self.ops.get()) };
    }
}
