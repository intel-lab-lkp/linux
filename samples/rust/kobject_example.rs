// SPDX-License-Identifier: GPL-2.0

//! Rust example using kobjects.

use kernel::{
    c_str,
    kobject::{subsystems, KObject, KObjectTextAttribute},
    prelude::*,
    str::CString,
};

module! {
    type: RustKObject,
    name: "rust_kobject",
    author: "Rust for Linux Contributors",
    license: "GPL",
}

struct RustKObject {
    // This kobject may be referenced later by other parts of code to update
    // the values stored within [`MyKObject`].
    _kobject: Pin<KBox<KObject<MyKObject>>>,
}

/// Sample KObject state which creates directory in the `/sys/kernel/my_kobject`.
#[derive(Default)]
pub struct MyKObject {
    text: Option<CString>,
}

/// Attribute of the [`MyKObject`], creates file in the `/sys/kernel/my_kobject/my-text`.
/// Where `show()` is triggered when read is called and `store()` is called
/// when new data are written.
pub struct MyKObjectAttribute;
#[vtable]
impl KObjectTextAttribute<MyKObject> for MyKObjectAttribute {
    const NAME: &'static CStr = c_str!("my-text");

    /// Called for example by `cat /sys/kernel/my_kobject/my-text`.
    fn show(this: &mut MyKObject) -> Result<CString> {
        if let Some(text) = &this.text {
            CString::try_from_fmt(fmt!("Text that was stored: '{text:?}'\n"))
        } else {
            CString::try_from_fmt(fmt!("No one stored anything yet, value {}.\n", this.value))
        }
    }

    /// Called for example by `echo "Hi Rust!" > /sys/kernel/my_kobject/my-text`.
    fn store(this: &mut MyKObject, input: &CStr) -> Result {
        this.text = Some(input.to_cstring()?);
        Ok(())
    }
}

impl kernel::Module for RustKObject {
    fn init(_module: &'static ThisModule) -> kernel::error::Result<Self> {
        let mut kobject = KObject::new_with_dynamic_kobject_parent(
            CString::try_from_fmt(fmt!("my_kobject"))?,
            &subsystems::KERNEL_KOBJECT,
            MyKObject::default(),
        )?;
        kobject.add_attribute(MyKObjectAttribute)?;

        Ok(RustKObject { _kobject: kobject })
    }
}
