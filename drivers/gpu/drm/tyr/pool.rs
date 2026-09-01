// SPDX-License-Identifier: GPL-2.0 or MIT

//! Per-drm-file VM id pool.

use kernel::{
    id_pool::IdPool,
    new_mutex,
    prelude::*,
    sync::{
        Arc,
        ArcBorrow,
        Mutex, //
    },
    xarray::{
        AllocKind,
        XArray, //
    }, //
};

use crate::vm::Vm;

/// Maximum number of VMs per open file. Matches panthor's
/// `PANTHOR_MAX_VMS_PER_FILE`.
pub(crate) const PANTHOR_MAX_VMS_PER_FILE: u32 = 32;

/// Per-open-file pool of VMs.
#[pin_data]
pub(crate) struct VmPool<'drm> {
    #[pin]
    ids: Mutex<IdPool>,
    #[pin]
    vms: XArray<Arc<Vm<'drm>>>,
}

impl<'drm> VmPool<'drm> {
    /// Creates a new [`VmPool`] with capacity for [`PANTHOR_MAX_VMS_PER_FILE`] VMs.
    pub(crate) fn new() -> Result<impl PinInit<Self>> {
        let ids = IdPool::with_capacity(PANTHOR_MAX_VMS_PER_FILE as usize, GFP_KERNEL)?;
        Ok(pin_init!(Self {
            ids <- new_mutex!(ids),
            vms <- XArray::new(AllocKind::Alloc),
        }))
    }

    /// Inserts a VM into the pool, returning the allocated ID.
    pub(crate) fn add(&self, vm: ArcBorrow<'_, Vm<'drm>>) -> Result<u32> {
        let id = {
            let mut ids = self.ids.lock();
            ids.find_unused_id(1).ok_or(ENOSPC)?.acquire()
        };

        let vm: Arc<Vm<'drm>> = vm.into();
        let mut vms = self.vms.lock();
        match vms.store(id, vm, GFP_KERNEL) {
            Ok(previous) => {
                // Drop the previous entry (expected `None`).
                drop(previous);
                Ok(id as u32)
            }
            Err(err) => {
                // Drop the XArray spinlock before acquiring the `ids` mutex.
                drop(vms);
                // Release the stored entry and the pooled id.
                drop(err.value);
                let mut ids = self.ids.lock();
                ids.release_id(id);
                Err(err.error)
            }
        }
    }

    /// Removes the VM with the given ID.
    pub(crate) fn remove(&self, id: u32) -> Result<Arc<Vm<'drm>>> {
        let mut vms = self.vms.lock();
        match vms.remove(id as usize) {
            Some(vm) => {
                drop(vms);
                let mut ids = self.ids.lock();
                ids.release_id(id as usize);
                Ok(vm)
            }
            None => Err(EINVAL),
        }
    }

    /// Gets the VM with the given ID.
    pub(crate) fn get(&self, id: u32) -> Option<Arc<Vm<'drm>>> {
        let vms = self.vms.lock();
        let borrow = vms.get(id as usize)?;
        Some(Arc::from(borrow))
    }

    /// Removes and returns the first VM in the pool.
    pub(crate) fn pop_first(&self) -> Option<Arc<Vm<'drm>>> {
        for id in 0..PANTHOR_MAX_VMS_PER_FILE {
            if let Ok(vm) = self.remove(id) {
                return Some(vm);
            }
        }
        None
    }
}
