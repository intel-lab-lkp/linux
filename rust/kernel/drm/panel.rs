// SPDX-License-Identifier: GPL-2.0

//! DRM panel abstractions.
//!
//! C header: [`include/drm/drm_panel.h`](srctree/include/drm/drm_panel.h)

use crate::drm::connector::Connector;
use crate::{
    bindings,
    device::Device,
    error, of,
    prelude::*,
    sync::aref::{ARef, AlwaysRefCounted},
    types::Opaque,
};
use core::marker::PhantomData;
use core::mem::{ManuallyDrop, MaybeUninit};
use core::ptr::NonNull;

/// A DRM panel object.
///
/// Wraps `struct drm_panel`. Instances are reference-counted via [`drm_panel_get`] and
/// [`drm_panel_put`]; use [`ARef<Panel>`] to hold an owned reference.
///
/// The DRM panel methods allow drivers to register panel objects with a
/// central registry and provide functions to retrieve those panels in display
/// drivers.
///
/// # Invariants
///
/// The inner pointer is always a valid, non-null pointer to a `struct drm_panel` with a
/// positive reference count.
///
/// [`drm_panel_get`]: srctree/include/drm/drm_panel.h
/// [`drm_panel_put`]: srctree/include/drm/drm_panel.h
#[repr(transparent)]
pub struct Panel(Opaque<bindings::drm_panel>);

impl Panel {
    /// Creates a reference from a raw pointer.
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid, non-null `struct drm_panel` pointer that remains
    /// valid for the lifetime `'a`.
    pub unsafe fn from_raw<'a>(ptr: *const bindings::drm_panel) -> &'a Self {
        // SAFETY: Caller guarantees `ptr` is valid and lives for `'a`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the raw pointer to the underlying `struct drm_panel`.
    pub fn as_raw(&self) -> *mut bindings::drm_panel {
        self.0.get()
    }

    /// Power on a panel.
    ///
    /// Calling this function will enable power and deassert any reset signals to
    /// the panel. After this has completed it is possible to communicate with any
    /// integrated circuitry via a command bus. This function cannot fail (as it is
    /// called from the pre_enable call chain). There will always be a call to
    /// [`Panel::disable`] afterwards.
    pub fn prepare(&self) {
        // SAFETY: The type invariant guarantees the pointer is valid.
        unsafe { bindings::drm_panel_prepare(self.as_raw()) }
    }

    /// Power off a panel.
    ///
    /// Calling this function will completely power off a panel (assert the panel's
    /// reset, turn off power supplies, ...). After this function has completed, it
    /// is usually no longer possible to communicate with the panel until another
    /// call to [`Panel::prepare`].
    pub fn unprepare(&self) {
        // SAFETY: The type invariant guarantees the pointer is valid.
        unsafe { bindings::drm_panel_unprepare(self.as_raw()) }
    }

    /// Enable a panel.
    ///
    /// Calling this function will cause the panel display drivers to be turned on
    /// and the backlight to be enabled. Content will be visible on screen after
    /// this call completes. This function cannot fail (as it is called from the
    /// enable call chain). There will always be a call to [`Panel::disable`]
    /// afterwards.
    pub fn enable(&self) {
        // SAFETY: The type invariant guarantees the pointer is valid.
        unsafe { bindings::drm_panel_enable(self.as_raw()) }
    }

    /// Disable a panel.
    ///
    /// This will typically turn off the panel's backlight or disable the display
    /// drivers. For smart panels it should still be possible to communicate with
    /// the integrated circuitry via any command bus after this call.
    pub fn disable(&self) {
        // SAFETY: The type invariant guarantees the pointer is valid.
        unsafe { bindings::drm_panel_disable(self.as_raw()) }
    }

    /// Probe the available display modes of a panel.
    ///
    /// The modes probed from the panel are automatically added to the connector
    /// that the panel is attached to.
    ///
    /// Return: The number of modes available from the panel on success, or 0 on
    /// failure (no modes).
    pub fn get_modes(&self, connector: &Connector) -> i32 {
        // SAFETY: The type invariants guarantee the pointers are valid.
        unsafe { bindings::drm_panel_get_modes(self.as_raw(), connector.as_raw()) }
    }

    /// Use backlight device node for backlight.
    ///
    /// Use this function to enable backlight handling if your panel
    /// uses device tree and has a backlight phandle.
    ///
    /// When the panel is enabled backlight will be enabled after a
    /// successful call to [`Panel::enable`].
    ///
    /// When the panel is disabled backlight will be disabled before the
    /// call to [`Panel::disable`].
    ///
    /// A typical implementation for a panel driver supporting device tree
    /// will call this function at probe time. Backlight will then be handled
    /// transparently without requiring any intervention from the driver.
    #[cfg(CONFIG_BACKLIGHT_CLASS_DEVICE)]
    pub fn of_backlight(&self) -> Result<()> {
        // SAFETY: The type invariant guarantees the pointer is valid.
        error::to_result(unsafe { bindings::drm_panel_of_backlight(self.as_raw()) })?;
        Ok(())
    }

    /// Look up the panel associated with the given device tree node.
    ///
    /// Searches the set of registered panels for one that matches the given device
    /// tree node. If a matching panel is found, return a pointer to it.
    pub fn from_of_node(node: &of::Node) -> Result<ARef<Self>> {
        // SAFETY: `node.as_raw()` is a valid device_node pointer.
        let panel = error::from_err_ptr(unsafe { bindings::of_drm_find_panel(node.as_raw()) })?;

        // SAFETY: `from_err_ptr` guarantees a non-null pointer on success.
        // `of_drm_find_panel` returns a kref-incremented reference.
        Ok(unsafe { ARef::from_raw(NonNull::new_unchecked(panel).cast()) })
    }

    /// Allocates and initialises a device-managed panel.
    ///
    /// `data` is embedded in the same allocation as the `drm_panel` and its
    /// destructor is called automatically when `dev` is unbound.
    ///
    /// Use [`Registration::register`] to add the panel to the global registry
    /// once it is ready to be used by display drivers.
    pub fn new<T: PanelFuncs>(
        dev: &Device,
        data: T,
        connector_type: ConnectorType,
    ) -> Result<ARef<Self>> {
        // SAFETY: `dev` is valid by its type invariants; `PanelFuncsVTable::build()`
        // returns a valid, static `drm_panel_funcs` pointer.
        let container = error::from_err_ptr(unsafe {
            bindings::__devm_drm_panel_alloc(
                dev.as_raw(),
                core::mem::size_of::<PanelContainer<T>>(),
                core::mem::offset_of!(PanelContainer<T>, panel),
                PanelFuncsVTable::<T>::build(),
                connector_type as i32,
            )
        })? as *mut PanelContainer<T>;

        // SAFETY: `container` is a valid pointer to uninitialized memory.
        unsafe {
            core::ptr::write(
                core::ptr::addr_of_mut!((*container).data),
                ManuallyDrop::new(data),
            )
        };

        // SAFETY:
        // - `dev.as_raw()` is a pointer to a valid and bound device.
        // - `container.cast()` is a valid pointer to the initialized `PanelContainer<T>`.
        error::to_result(unsafe {
            // `devm_add_action_or_reset` calls `drop_panel_data` on failure, so `data`
            // is dropped even if this registration fails.
            // Registering after `__devm_drm_panel_alloc` ensures devres LIFO order:
            // `drop_panel_data` runs before `kfree(container)`.
            bindings::devm_add_action_or_reset(
                dev.as_raw(),
                Some(drop_panel_data::<T>),
                container.cast(),
            )
        })?;

        // SAFETY: `__devm_drm_panel_alloc` was successful, hence `container` is
        // valid and the `drm_panel` at this offset is initialised.
        let raw = unsafe {
            (container as *mut u8)
                .add(core::mem::offset_of!(PanelContainer<T>, panel))
                .cast::<bindings::drm_panel>()
        };

        // SAFETY: `__devm_drm_panel_alloc` was successful, hence `raw` is valid
        // and the refcount is non-zero.
        Ok(unsafe { ARef::from_raw(NonNull::new_unchecked(raw).cast()) })
    }
}

// SAFETY: By the type invariants, this type is always refcounted.
unsafe impl AlwaysRefCounted for Panel {
    fn inc_ref(&self) {
        // SAFETY: The type invariant guarantees the pointer is valid.
        unsafe { bindings::drm_panel_get(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::drm_panel_put(obj.cast().as_ptr()) };
    }
}

/// This enum is used to track the (LCD) panel orientation.
///
/// C header: [`include/drm/drm_connector.h`](srctree/include/drm/drm_connector.h)
#[repr(i32)]
pub enum PanelOrientation {
    /// The drm driver has not provided any panel orientation information.
    Unknown = -1,
    /// The top side of the panel matches the top side of the device's casing.
    Normal = 0,
    /// The top side of the panel matches the bottom side of the device's casing.
    BottomUp = 1,
    /// The left side of the panel matches the top side of the device's casing.
    LeftUp = 2,
    /// The right side of the panel matches the top side of the device's casing.
    RightUp = 3,
}

impl TryFrom<i32> for PanelOrientation {
    type Error = Error;
    fn try_from(v: i32) -> Result<Self> {
        match v {
            -1 => Ok(Self::Unknown),
            0 => Ok(Self::Normal),
            1 => Ok(Self::BottomUp),
            2 => Ok(Self::LeftUp),
            3 => Ok(Self::RightUp),
            _ => Err(EINVAL),
        }
    }
}

impl PanelOrientation {
    /// Look up the orientation of the panel through the "rotation" binding
    /// from a device tree node
    ///
    /// Looks up the rotation of a panel in the device tree. The orientation of the
    /// panel is expressed as a property name "rotation" in the device tree. The
    /// rotation in the device tree is counter clockwise.
    pub fn from_of_node(node: &of::Node) -> Result<Self> {
        let mut orientation = 0i32;
        // SAFETY: `node.as_raw()` is a valid device_node pointer.
        error::to_result(unsafe {
            bindings::of_drm_get_panel_orientation(node.as_raw(), &mut orientation)
        })?;
        Ok(PanelOrientation::try_from(orientation)?)
    }
}

/// The type of a DRM connector.
///
/// Mirrors the `DRM_MODE_CONNECTOR_*` defines in
/// [`include/uapi/drm/drm_mode.h`](srctree/include/uapi/drm/drm_mode.h).
#[repr(u32)]
pub enum ConnectorType {
    /// Unknown connector type (`DRM_MODE_CONNECTOR_Unknown`).
    Unknown = 0,
    /// VGA connector (`DRM_MODE_CONNECTOR_VGA`).
    VGA = 1,
    /// DVI-I connector (`DRM_MODE_CONNECTOR_DVII`).
    DVII = 2,
    /// DVI-D connector (`DRM_MODE_CONNECTOR_DVID`).
    DVID = 3,
    /// DVI-A connector (`DRM_MODE_CONNECTOR_DVIA`).
    DVIA = 4,
    /// Composite connector (`DRM_MODE_CONNECTOR_Composite`).
    Composite = 5,
    /// S-Video connector (`DRM_MODE_CONNECTOR_SVIDEO`).
    SVIDEO = 6,
    /// LVDS connector (`DRM_MODE_CONNECTOR_LVDS`).
    LVDS = 7,
    /// Component connector (`DRM_MODE_CONNECTOR_Component`).
    Component = 8,
    /// 9-pin DIN connector (`DRM_MODE_CONNECTOR_9PinDIN`).
    NinePinDin = 9,
    /// DisplayPort connector (`DRM_MODE_CONNECTOR_DisplayPort`).
    DisplayPort = 10,
    /// HDMI type A connector (`DRM_MODE_CONNECTOR_HDMIA`).
    HDMIA = 11,
    /// HDMI type B connector (`DRM_MODE_CONNECTOR_HDMIB`).
    HDMIB = 12,
    /// TV connector (`DRM_MODE_CONNECTOR_TV`).
    TV = 13,
    /// Embedded DisplayPort connector (`DRM_MODE_CONNECTOR_eDP`).
    #[allow(non_camel_case_types)]
    eDP = 14,
    /// Virtual connector (`DRM_MODE_CONNECTOR_VIRTUAL`).
    Virtual = 15,
    /// MIPI DSI connector (`DRM_MODE_CONNECTOR_DSI`).
    DSI = 16,
    /// DPI connector (`DRM_MODE_CONNECTOR_DPI`).
    DPI = 17,
    /// Writeback connector (`DRM_MODE_CONNECTOR_WRITEBACK`).
    Writeback = 18,
    /// SPI connector (`DRM_MODE_CONNECTOR_SPI`).
    SPI = 19,
    /// USB connector (`DRM_MODE_CONNECTOR_USB`).
    USB = 20,
}

impl TryFrom<u32> for ConnectorType {
    type Error = Error;
    fn try_from(v: u32) -> Result<Self> {
        match v {
            0 => Ok(Self::Unknown),
            1 => Ok(Self::VGA),
            2 => Ok(Self::DVII),
            3 => Ok(Self::DVID),
            4 => Ok(Self::DVIA),
            5 => Ok(Self::Composite),
            6 => Ok(Self::SVIDEO),
            7 => Ok(Self::LVDS),
            8 => Ok(Self::Component),
            9 => Ok(Self::NinePinDin),
            10 => Ok(Self::DisplayPort),
            11 => Ok(Self::HDMIA),
            12 => Ok(Self::HDMIB),
            13 => Ok(Self::TV),
            14 => Ok(Self::eDP),
            15 => Ok(Self::Virtual),
            16 => Ok(Self::DSI),
            17 => Ok(Self::DPI),
            18 => Ok(Self::Writeback),
            19 => Ok(Self::SPI),
            20 => Ok(Self::USB),
            _ => Err(EINVAL),
        }
    }
}

/// A registration of a panel to the global panel registry.
pub struct Registration(ARef<Panel>);

impl Registration {
    /// Registers a panel with the global panel registry.
    pub fn register(panel: ARef<Panel>) -> Self {
        // SAFETY: The type invariant guarantees the pointer is valid.
        unsafe { bindings::drm_panel_add(panel.as_raw()) };
        Self(panel)
    }

    /// Returns a reference to the panel.
    pub fn panel(&self) -> &Panel {
        &self.0
    }
}

impl Drop for Registration {
    fn drop(&mut self) {
        // SAFETY: The type invariant guarantees the pointer is valid.
        unsafe { bindings::drm_panel_remove(self.0.as_raw()) };
    }
}

/// Operations implemented by a DRM panel driver.
///
/// Implement this trait to provide a DRM panel driver and its callbacks. Use
/// [`Panel::new`] to allocate the panel, passing the driver data as `T`.
///
/// C header: [`include/drm/drm_panel.h`](srctree/include/drm/drm_panel.h)
#[vtable]
pub trait PanelFuncs {
    /// Turn on panel and perform set up.
    ///
    /// This function is optional.
    fn prepare(&self, _panel: &Panel) -> Result<()> {
        Ok(())
    }

    /// Turn off panel.
    ///
    /// This function is optional.
    fn unprepare(&self, _panel: &Panel) -> Result<()> {
        Ok(())
    }

    /// Enable panel (turn on back light, etc.).
    ///
    /// This function is optional.
    fn enable(&self, _panel: &Panel) -> Result<()> {
        Ok(())
    }

    /// Disable panel (turn off back light, etc.).
    ///
    /// This function is optional.
    fn disable(&self, _panel: &Panel) -> Result<()> {
        Ok(())
    }

    /// Add modes to the connector that the panel is attached to
    /// and returns the number of modes added.
    ///
    /// This function is mandatory.
    fn get_modes(&self, _panel: &Panel, _connector: &Connector) -> i32 {
        build_error!("get_modes is mandatory")
    }

    /// Return the panel orientation set by device tree or EDID.
    ///
    /// This function is optional.
    fn get_orientation(&self, _panel: &Panel) -> PanelOrientation {
        PanelOrientation::Unknown
    }
}

// Outer allocation layout used by `Panel::new`.
//
// `__devm_drm_panel_alloc` allocates a block of `size_of::<PanelContainer<T>>()`
// bytes, places `drm_panel` at `offset_of!(PanelContainer<T>, panel)`, and
// stores the block's base address in `panel->container`.
//
// Lifetime:
//   1. A devres action registered right after allocation calls `drop_in_place`
//      on the `data` field (T's destructor) when the device is unbound.
//   2. `__drm_panel_free` calls `kfree(panel->container)` when the kref hits
//      zero, freeing the entire block.
//
// `data` is `ManuallyDrop<T>` so that Rust does not implicitly drop it; the
// devres action owns the destructor call.
#[repr(C)]
struct PanelContainer<T> {
    data: ManuallyDrop<T>,
    panel: MaybeUninit<bindings::drm_panel>,
}

// Devres action: run T's destructor before `kfree(container)`.
//
// # Safety
//
// `ptr` must be the base of a live `PanelContainer<T>` whose `data` field was
// initialised by `Panel::new` and has not yet been dropped.
unsafe extern "C" fn drop_panel_data<T>(ptr: *mut core::ffi::c_void) {
    // SAFETY: Caller guarantees `ptr` is the base of a live `PanelContainer<T>`
    // with an initialised `data` field. `data` is at offset 0, so `ptr as *mut T`
    // is valid.
    unsafe { core::ptr::drop_in_place(ptr as *mut T) };
}

/// A vtable for the DRM core to interact with a panel driver.
///
/// A `bindings::drm_panel_funcs` vtable is constructed from pointers to the
/// `extern "C"` functions of this struct, exposed through
/// `PanelFuncsVTable::VTABLE`.
///
/// For general documentation of these methods, see the kernel source
/// documentation related to `struct drm_panel_funcs` in
/// [`include/drm/drm_panel.h`].
///
/// [`include/drm/drm_panel.h`]: srctree/include/drm/drm_panel.h
pub(crate) struct PanelFuncsVTable<T: PanelFuncs>(PhantomData<T>);

impl<T: PanelFuncs> PanelFuncsVTable<T> {
    // Recover &T from panel->container.
    //
    // # Safety
    //
    // `panel` must be a valid pointer to a live `drm_panel` allocated by
    // `Panel::new`, whose `container` field points to the base of a live
    // `PanelContainer<T>` with an initialised `data` field.
    unsafe fn data_from_panel<'a>(panel: *mut bindings::drm_panel) -> &'a T {
        // SAFETY: Caller guarantees `panel` is valid and `panel->container` points
        // to the base of a live `PanelContainer<T>` with an initialised `data`
        // field. `data` is at offset 0, so `container as *const T` is valid.
        unsafe { &*((*panel).container as *const T) }
    }

    unsafe extern "C" fn prepare_callback(panel: *mut bindings::drm_panel) -> i32 {
        // SAFETY: The C DRM core only invokes callbacks on a live, initialised
        // panel allocated by `Panel::new` (see `data_from_panel`).
        let data = unsafe { Self::data_from_panel(panel) };
        // SAFETY: `panel` is a valid `drm_panel` pointer per the callback contract.
        let panel_ref = unsafe { Panel::from_raw(panel) };
        match T::prepare(data, panel_ref) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    unsafe extern "C" fn unprepare_callback(panel: *mut bindings::drm_panel) -> i32 {
        // SAFETY: The C DRM core only invokes callbacks on a live, initialised
        // panel allocated by `Panel::new` (see `data_from_panel`).
        let data = unsafe { Self::data_from_panel(panel) };
        // SAFETY: `panel` is a valid `drm_panel` pointer per the callback contract.
        let panel_ref = unsafe { Panel::from_raw(panel) };
        match T::unprepare(data, panel_ref) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    unsafe extern "C" fn enable_callback(panel: *mut bindings::drm_panel) -> i32 {
        // SAFETY: The C DRM core only invokes callbacks on a live, initialised
        // panel allocated by `Panel::new` (see `data_from_panel`).
        let data = unsafe { Self::data_from_panel(panel) };
        // SAFETY: `panel` is a valid `drm_panel` pointer per the callback contract.
        let panel_ref = unsafe { Panel::from_raw(panel) };
        match T::enable(data, panel_ref) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    unsafe extern "C" fn disable_callback(panel: *mut bindings::drm_panel) -> i32 {
        // SAFETY: The C DRM core only invokes callbacks on a live, initialised
        // panel allocated by `Panel::new` (see `data_from_panel`).
        let data = unsafe { Self::data_from_panel(panel) };
        // SAFETY: `panel` is a valid `drm_panel` pointer per the callback contract.
        let panel_ref = unsafe { Panel::from_raw(panel) };
        match T::disable(data, panel_ref) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    unsafe extern "C" fn get_modes_callback(
        panel: *mut bindings::drm_panel,
        connector: *mut bindings::drm_connector,
    ) -> i32 {
        // SAFETY: The C DRM core only invokes callbacks on a live, initialised
        // panel allocated by `Panel::new` (see `data_from_panel`).
        let data = unsafe { Self::data_from_panel(panel) };
        // SAFETY: `panel` is a valid `drm_panel` pointer per the callback contract.
        let panel_ref = unsafe { Panel::from_raw(panel) };
        // SAFETY: `connector` is a valid, non-null `drm_connector` pointer
        // supplied by the DRM core for the duration of the callback.
        let connector_ref = unsafe { Connector::from_raw(connector) };
        T::get_modes(data, panel_ref, connector_ref)
    }

    unsafe extern "C" fn get_orientation_callback(panel: *mut bindings::drm_panel) -> i32 {
        // SAFETY: The C DRM core only invokes callbacks on a live, initialised
        // panel allocated by `Panel::new` (see `data_from_panel`).
        let data = unsafe { Self::data_from_panel(panel) };
        // SAFETY: `panel` is a valid `drm_panel` pointer per the callback contract.
        let panel_ref = unsafe { Panel::from_raw(panel) };
        T::get_orientation(data, panel_ref) as i32
    }

    const VTABLE: bindings::drm_panel_funcs = bindings::drm_panel_funcs {
        // Initialize optional callbacks based on the traits of `T`.
        prepare: if T::HAS_PREPARE {
            Some(Self::prepare_callback)
        } else {
            None
        },
        unprepare: if T::HAS_UNPREPARE {
            Some(Self::unprepare_callback)
        } else {
            None
        },
        enable: if T::HAS_ENABLE {
            Some(Self::enable_callback)
        } else {
            None
        },
        disable: if T::HAS_DISABLE {
            Some(Self::disable_callback)
        } else {
            None
        },
        get_orientation: if T::HAS_GET_ORIENTATION {
            Some(Self::get_orientation_callback)
        } else {
            None
        },

        // Initialize mandatory callbacks.
        get_modes: Some(Self::get_modes_callback),

        get_timings: None,
        debugfs_init: None,
    };

    pub(crate) const fn build() -> &'static bindings::drm_panel_funcs {
        &Self::VTABLE
    }
}

#[cfg(CONFIG_RUST_DRM_PANEL_KUNIT_TEST)]
#[macros::kunit_tests(rust_kernel_drm_panel)]
mod tests {
    use super::*;
    use core::mem::{align_of, offset_of, size_of, ManuallyDrop, MaybeUninit};

    struct TestData {
        _ptr: *const u8,
        _byte: u8,
    }

    #[test]
    fn panel_orientation() {
        // Test valid values.
        assert!(matches!(
            PanelOrientation::try_from(-1),
            Ok(PanelOrientation::Unknown)
        ));
        assert!(matches!(
            PanelOrientation::try_from(0),
            Ok(PanelOrientation::Normal)
        ));
        assert!(matches!(
            PanelOrientation::try_from(1),
            Ok(PanelOrientation::BottomUp)
        ));
        assert!(matches!(
            PanelOrientation::try_from(2),
            Ok(PanelOrientation::LeftUp)
        ));
        assert!(matches!(
            PanelOrientation::try_from(3),
            Ok(PanelOrientation::RightUp)
        ));

        // Test invalid values.
        assert!(PanelOrientation::try_from(4).is_err());
        assert!(PanelOrientation::try_from(-2).is_err());
    }

    #[test]
    fn connector_type() {
        // Test valid values.
        assert!(matches!(
            ConnectorType::try_from(0),
            Ok(ConnectorType::Unknown)
        ));
        assert!(matches!(
            ConnectorType::try_from(10),
            Ok(ConnectorType::DisplayPort)
        ));
        assert!(matches!(
            ConnectorType::try_from(14),
            Ok(ConnectorType::eDP)
        ));
        assert!(matches!(
            ConnectorType::try_from(16),
            Ok(ConnectorType::DSI)
        ));
        assert!(matches!(
            ConnectorType::try_from(20),
            Ok(ConnectorType::USB)
        ));

        // Test invalid values.
        assert!(ConnectorType::try_from(21).is_err());
    }

    #[test]
    fn panel_container_layout() {
        assert_eq!(offset_of!(PanelContainer<TestData>, data), 0);

        assert_eq!(size_of::<ManuallyDrop<TestData>>(), size_of::<TestData>());

        let panel_offset = offset_of!(PanelContainer<TestData>, panel);
        assert_eq!(
            panel_offset % align_of::<MaybeUninit<bindings::drm_panel>>(),
            0
        );

        assert!(
            size_of::<PanelContainer<TestData>>()
                >= panel_offset + size_of::<bindings::drm_panel>()
        );
    }
}
