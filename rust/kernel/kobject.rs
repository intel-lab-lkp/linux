// SPDX-License-Identifier: GPL-2.0
// TODO: Add unsafe doc documentation.
#![allow(clippy::undocumented_unsafe_blocks, clippy::missing_safety_doc)]

//! KObject wrappers.
//!
//! TODO: Write more

use bindings::PAGE_SIZE;
use core::marker::PhantomData;
use core::marker::PhantomPinned;
use core::mem;
use core::ptr;
use kernel::c_str;
use kernel::container_of;
use kernel::error::code::*;
use kernel::prelude::*;
use kernel::str::{CStr, CString};
use kernel::types::Mode;
use kernel::types::Opaque;

/// Reference to the subsystems from the non Rust world.
pub mod subsystems {
    use super::DynamicKObject;

    macro_rules! declare_kobject {
        ($name:tt, $subsystem_ptr:expr, $($doc:expr),+) => {
            $(
            #[doc = $doc]
            )*
            pub static $name: DynamicKObject = DynamicKObject {
                pointer: kernel::types::Opaque::new(
                    core::ptr::addr_of!($subsystem_ptr).cast_mut(),
                ),
            };
        };
    }

    declare_kobject!(
        FIRMWARE_KOBJECT,
        bindings::firmware_kobj,
        "Firmware subsystem dynamic KObject."
    );
    declare_kobject!(
        KERNEL_KOBJECT,
        bindings::kernel_kobj,
        "Kernel subsystem dynamic KObject."
    );
    declare_kobject!(
        MM_KOBJECT,
        bindings::mm_kobj,
        "Memory management subsystem dynamic KObject."
    );
    declare_kobject!(
        POWER_KOBJECT,
        bindings::power_kobj,
        "Power subsystem dynamic KObject."
    );

    // TODO: Add rests of the exported Kobjects.
}

/// DynamicKObject is same as [`KObject`], however it does not have any state.
pub struct DynamicKObject {
    // The bindings export `extern static *mut bindings::kobejct`, however
    // we need to have double pointer because rustc complains with
    // `could not evaluate static initializer`.
    pointer: Opaque<*mut *mut bindings::kobject>,
}

unsafe impl Send for DynamicKObject {}
unsafe impl Sync for DynamicKObject {}

/// KObject represent wrapper structure enables interaction with
/// the sysfs, where the KObject represents directory.
///
/// TODO: Add examples?
pub struct KObject<K> {
    data: K,
    pointer: Opaque<bindings::kobject>,

    // Struct must be `!Unpin`, because kobject pointer is self referential.
    _m: PhantomPinned,
}

impl<K> Drop for KObject<K> {
    fn drop(&mut self) {
        // SAFETY: This KObject holds a valid reference, because it was allocated
        // through kobject API.
        unsafe { bindings::kobject_put(self.pointer.get()) }
    }
}

#[doc(hidden)]
struct KObjTypeVTable;
#[doc(hidden)]
impl KObjTypeVTable {
    unsafe extern "C" fn release_callback(kobj: *mut bindings::kobject) {
        unsafe { bindings::kfree(kobj.cast()) }
    }

    pub(crate) const KOBJ_TYPE: bindings::kobj_type = bindings::kobj_type {
        release: Some(Self::release_callback),
        sysfs_ops: ptr::addr_of!(bindings::kobj_sysfs_ops),
        ..unsafe { mem::zeroed() }
    };

    pub(crate) fn kobj_type(&self) -> &'static bindings::kobj_type {
        &Self::KOBJ_TYPE
    }
}

unsafe impl<K> Send for KObject<K> where K: Send {}
unsafe impl<K> Sync for KObject<K> where K: Sync {}

impl<K> KObject<K> {
    /// Attaches KObject to the sysfs root.
    pub fn new_in_root(name: CString, data: K) -> Result<Pin<KBox<Self>>> {
        Self::new(name, ptr::null_mut(), data)
    }

    /// Attaches new KObject to the sysfs with specified KObject parent.
    pub fn new_with_kobject_parent<L>(
        name: CString,
        parent: &KObject<L>,
        data: K,
    ) -> Result<Pin<KBox<Self>>> {
        Self::new(name, parent.pointer.get(), data)
    }

    /// Attaches new KObject to the sysfs with specified **dynamic** KObject parent.
    pub fn new_with_dynamic_kobject_parent(
        name: CString,
        parent: &DynamicKObject,
        data: K,
    ) -> Result<Pin<KBox<Self>>> {
        Self::new(name, unsafe { **parent.pointer.get() }, data)
    }

    fn new(name: CString, parent: *mut bindings::kobject, data: K) -> Result<Pin<KBox<Self>>> {
        let this = KBox::pin(
            Self {
                pointer: Opaque::new(unsafe { mem::zeroed() }),
                data,
                _m: PhantomPinned,
            },
            GFP_KERNEL,
        )?;

        let code = unsafe {
            bindings::kobject_init_and_add(
                this.pointer.get(),
                KObjTypeVTable.kobj_type(),
                parent,
                c_str!("%s").as_char_ptr(),
                name.as_char_ptr(),
            )
        };
        if code < 0 {
            return Err(Error::from_errno(code));
        }

        // TODO: Not sure whether we must check for return code of `kobject_uevent`.
        unsafe {
            // We are  responsible for sending uevent that kobject was added to the system.
            bindings::kobject_uevent(this.pointer.get(), bindings::kobject_action_KOBJ_ADD)
        };

        Ok(this)
    }

    /// Creates a file for a given KObject.
    pub fn add_attribute<A: KObjectTextAttribute<K>>(
        self: &mut Pin<KBox<KObject<K>>>,
        attribute: A,
    ) -> Result<()> {
        let vtable = TextAttributeVTable::<K, A>::new();
        // TODO: Can we use it later?
        let _ = attribute;

        let code = unsafe {
            bindings::sysfs_create_file_ns(
                self.pointer.get(),
                ptr::addr_of!(*vtable.attr()) as *const bindings::attribute,
                ptr::null(),
            )
        };

        if code == 0 {
            Ok(())
        } else {
            Err(kernel::error::Error::from_errno(code))
        }
    }
}

/// Attribute represents a file in a directory.
#[macros::vtable]
pub trait KObjectTextAttribute<K>
where
    Self: Sized,
{
    /// File permissions.
    const MODE: Mode = Mode::from_u16(0o777);

    /// Name of the file in the sysfs.
    const NAME: &'static CStr;

    /// Gets called when the read is called on the file.
    fn show(this: &mut K) -> Result<CString> {
        let _ = this;
        Err(EIO)
    }

    /// Gets called when the write is called on the file.
    fn store(this: &mut K, input: &CStr) -> Result {
        let _ = this;
        let _ = input;
        Err(EIO)
    }
}

#[doc(hidden)]
struct TextAttributeVTable<K, A: KObjectTextAttribute<K>>(PhantomData<(A, K)>);
#[doc(hidden)]
impl<K, A: KObjectTextAttribute<K>> TextAttributeVTable<K, A> {
    #[doc(hidden)]
    const fn new() -> Self {
        Self(PhantomData)
    }
    unsafe extern "C" fn show_callback(
        kobj: *mut bindings::kobject,
        _attr: *mut bindings::kobj_attribute,
        buf: *mut core::ffi::c_char,
    ) -> isize {
        match A::show(
            &mut unsafe { &mut *container_of!(kobj, KObject<K>, pointer).cast_mut() }.data,
        ) {
            Ok(cstring) => unsafe {
                bindings::sized_strscpy(buf.cast(), cstring.as_char_ptr(), PAGE_SIZE)
            },
            Err(error) => error.to_errno() as isize,
        }
    }
    unsafe extern "C" fn store_callback(
        kobj: *mut bindings::kobject,
        _attr: *mut bindings::kobj_attribute,
        buf: *const core::ffi::c_char,
        count: usize,
    ) -> isize {
        match A::store(
            &mut unsafe { &mut *container_of!(kobj, KObject<K>, pointer).cast_mut() }.data,
            unsafe { kernel::str::CStr::from_char_ptr(buf) },
        ) {
            Ok(()) => count as isize,
            Err(error) => error.to_errno() as isize,
        }
    }
    pub(crate) const ATTR: bindings::kobj_attribute = bindings::kobj_attribute {
        attr: bindings::attribute {
            name: A::NAME.as_char_ptr(),
            mode: A::MODE.as_u16(),
        },
        show: Some(Self::show_callback),
        store: Some(Self::store_callback),
    };

    const fn attr(&self) -> &'static bindings::kobj_attribute {
        &TextAttributeVTable::<K, A>::ATTR
    }
}
