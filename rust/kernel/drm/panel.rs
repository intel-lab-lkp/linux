// SPDX-License-Identifier: GPL-2.0

//! DRM panel abstractions.
//!
//! C header: [`include/drm/drm_panel.h`](srctree/include/drm/drm_panel.h)

use crate::drm::connector::Connector;
use crate::{
    bindings, error, of,
    prelude::*,
    sync::aref::{ARef, AlwaysRefCounted},
    types::Opaque,
};
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
        unsafe { bindings::drm_panel_get_modes(self.as_raw(), connector.as_raw()) } as i32
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
}

// SAFETY: By the type invariants, this type is always refcounted.
unsafe impl AlwaysRefCounted for Panel {
    fn inc_ref(&self) {
        // SAFETY: The type invariant guarantees the pointer is valid.
        unsafe { bindings::drm_panel_get(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The existence of `obj` guarantees the refcount is positive.
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
