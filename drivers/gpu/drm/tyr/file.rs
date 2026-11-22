// SPDX-License-Identifier: GPL-2.0 or MIT

use kernel::{
    drm::{
        self,
        gem::BaseObject,
        Registered, //
    },
    num::casts::FromSafeCast,
    prelude::*,
    sizes::SizeConstants,
    transmute::FromBytes,
    uaccess::UserSlice,
    uapi, //
};

use crate::{
    driver::{
        TyrDrmDevice,
        TyrDrmDriver,
        TyrDrmRegistrationData, //
    },
    vm::{
        UserVaRequest,
        Vm,
        VmBindOpType,
        VmMapFlags,
        VmPool, //
    }, //
};

#[pin_data]
pub(crate) struct TyrDrmFileData<'a> {
    reg: &'a TyrDrmRegistrationData<'a>,

    #[pin]
    vm_pool: VmPool<'a>,
}

/// Convenience type alias for our DRM `File` type.
pub(crate) type TyrDrmFile = drm::file::File<TyrDrmDriver>;

impl<'a> drm::file::DriverFile<'a> for TyrDrmFileData<'a> {
    type Driver = TyrDrmDriver;

    fn open(
        _device: &TyrDrmDevice<Registered>,
        reg_data: &'a TyrDrmRegistrationData<'a>,
    ) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            reg: reg_data,
            vm_pool <- VmPool::new(reg_data.pdev.as_ref())?,
        })
    }
}

impl TyrDrmFileData<'_> {
    pub(crate) fn dev_query(
        _ddev: &TyrDrmDevice<Registered>,
        reg_data: &TyrDrmRegistrationData<'_>,
        devquery: &mut uapi::drm_panthor_dev_query,
        _file: &TyrDrmFile,
    ) -> Result<u32> {
        if devquery.pointer == 0 {
            match devquery.type_ {
                uapi::drm_panthor_dev_query_type_DRM_PANTHOR_DEV_QUERY_GPU_INFO => {
                    devquery.size = core::mem::size_of_val(&reg_data.gpu_info) as u32;
                    Ok(0)
                }
                _ => Err(EINVAL),
            }
        } else {
            match devquery.type_ {
                uapi::drm_panthor_dev_query_type_DRM_PANTHOR_DEV_QUERY_GPU_INFO => {
                    let mut writer = UserSlice::new(
                        UserPtr::from_addr(devquery.pointer as usize),
                        devquery.size as usize,
                    )
                    .writer();

                    writer.write(&reg_data.gpu_info)?;

                    Ok(0)
                }
                _ => Err(EINVAL),
            }
        }
    }

    pub(crate) fn vm_create(
        ddev: &TyrDrmDevice<Registered>,
        _reg_data: &TyrDrmRegistrationData<'_>,
        vmcreate: &mut uapi::drm_panthor_vm_create,
        file: &TyrDrmFile,
    ) -> Result<u32> {
        if vmcreate.flags != 0 {
            dev_dbg!(
                ddev.as_ref(),
                "Invalid VM create flags: {:#x}\n",
                vmcreate.flags
            );
            return Err(EINVAL);
        }

        let ret: Result<u32, Error> = file.inner_with(|pfile| {
            let vm = Vm::new_for_user(
                pfile.reg.pdev.as_ref(),
                ddev,
                pfile.reg.mmu.as_arc_borrow(),
                &pfile.reg.gpu_info,
                UserVaRequest::from_uapi(vmcreate.user_va_range),
                pfile.reg.coherent,
            )?;
            let user_va_range = vm.layout.user.end;
            let id = pfile.vm_pool.add(vm).map_err(|e| {
                e.vm.kill();
                e.error
            })?;
            vmcreate.user_va_range = user_va_range;
            vmcreate.id = id;

            Ok(0)
        });
        ret
    }

    pub(crate) fn vm_destroy(
        ddev: &TyrDrmDevice<Registered>,
        _reg_data: &TyrDrmRegistrationData<'_>,
        vmdestroy: &mut uapi::drm_panthor_vm_destroy,
        file: &TyrDrmFile,
    ) -> Result<u32> {
        if vmdestroy.pad != 0 {
            dev_dbg!(
                ddev.as_ref(),
                "Invalid VM destroy pad: {:#x}\n",
                vmdestroy.pad
            );
            return Err(EINVAL);
        }

        let ret: Result<u32, Error> = file.inner_with(|pfile| {
            pfile.vm_pool.remove(vmdestroy.id)?;
            Ok(0)
        });
        ret
    }

    pub(crate) fn vm_bind(
        ddev: &TyrDrmDevice<Registered>,
        _reg_data: &TyrDrmRegistrationData<'_>,
        vmbind: &mut uapi::drm_panthor_vm_bind,
        file: &TyrDrmFile,
    ) -> Result<u32> {
        let async_flag = uapi::drm_panthor_vm_bind_flags_DRM_PANTHOR_VM_BIND_ASYNC;

        if vmbind.flags & !async_flag != 0 {
            dev_dbg!(
                ddev.as_ref(),
                "Invalid VM_BIND flags: {:#x}\n",
                vmbind.flags
            );
            return Err(EINVAL);
        }

        if vmbind.flags & async_flag != 0 {
            dev_dbg!(ddev.as_ref(), "Async VM_BIND not supported\n");
            return Err(EOPNOTSUPP);
        }

        let count = usize::from_safe_cast(vmbind.ops.count);
        if count == 0 {
            return file.inner_with(|pfile| {
                pfile.vm_pool.get(vmbind.vm_id).ok_or(EINVAL)?;
                Ok(0)
            });
        }

        // Panthor allows larger strides; UserSliceReader::read needs an exact one.
        if vmbind.ops.stride as usize != size_of::<VmBindOp>() {
            dev_dbg!(
                ddev.as_ref(),
                "Invalid VM_BIND op stride {} (expected {})\n",
                vmbind.ops.stride,
                size_of::<VmBindOp>()
            );
            return Err(EINVAL);
        }
        // TODO: support variable strides with copy_struct_from_user()
        // semantics: zero-pad shorter structs, reject non-zero trailing bytes.

        let total_len = size_of::<VmBindOp>().checked_mul(count).ok_or_else(|| {
            dev_dbg!(ddev.as_ref(), "VM_BIND ops length overflow\n");
            EINVAL
        })?;
        let mut reader =
            UserSlice::new(UserPtr::from_addr(vmbind.ops.array as usize), total_len).reader();
        let mut ops = KVec::new();
        for _ in 0..count {
            ops.push(reader.read::<VmBindOp>()?, GFP_KERNEL)?;
        }

        let ret: Result<u32, Error> = file.inner_with(|pfile| {
            let vm = pfile.vm_pool.get(vmbind.vm_id).ok_or_else(|| {
                dev_dbg!(ddev.as_ref(), "Invalid VM_BIND vm_id: {}\n", vmbind.vm_id);
                EINVAL
            })?;

            for (i, op) in ops.iter().enumerate() {
                if let Err(e) = vm_bind_exec_op(&vm, file, op) {
                    dev_dbg!(ddev.as_ref(), "VM_BIND op {} failed: {:?}\n", i, e);
                    vmbind.ops.count = i as u32;
                    return Err(e);
                }
            }

            Ok(0)
        });
        ret
    }

    pub(crate) fn vm_get_state(
        ddev: &TyrDrmDevice<Registered>,
        _reg_data: &TyrDrmRegistrationData<'_>,
        vmgetstate: &mut uapi::drm_panthor_vm_get_state,
        file: &TyrDrmFile,
    ) -> Result<u32> {
        file.inner_with(|pfile| {
            let vm = pfile.vm_pool.get(vmgetstate.vm_id).ok_or_else(|| {
                dev_dbg!(
                    ddev.as_ref(),
                    "Invalid VM_GET_STATE vm_id: {}\n",
                    vmgetstate.vm_id
                );
                EINVAL
            })?;
            vmgetstate.state = if vm.is_unusable() {
                uapi::drm_panthor_vm_state_DRM_PANTHOR_VM_STATE_UNUSABLE
            } else {
                uapi::drm_panthor_vm_state_DRM_PANTHOR_VM_STATE_USABLE
            };
            Ok(0)
        })
    }

    pub(crate) fn bo_create(
        ddev: &TyrDrmDevice<Registered>,
        reg_data: &TyrDrmRegistrationData<'_>,
        bocreate: &mut uapi::drm_panthor_bo_create,
        file: &TyrDrmFile,
    ) -> Result<u32> {
        if bocreate.size == 0 || bocreate.pad != 0 {
            dev_dbg!(
                ddev.as_ref(),
                "Invalid BO_CREATE params: size={}, pad={}\n",
                bocreate.size,
                bocreate.pad
            );
            return Err(EINVAL);
        }

        let no_mmap = uapi::drm_panthor_bo_flags_DRM_PANTHOR_BO_NO_MMAP;
        let wb_mmap = uapi::drm_panthor_bo_flags_DRM_PANTHOR_BO_WB_MMAP;
        let valid_flags = no_mmap | wb_mmap;

        if bocreate.flags & !valid_flags != 0 {
            dev_dbg!(
                ddev.as_ref(),
                "Invalid BO_CREATE flags: {:#x}\n",
                bocreate.flags
            );
            return Err(EINVAL);
        }
        if bocreate.flags & no_mmap != 0 && bocreate.flags & wb_mmap != 0 {
            dev_dbg!(ddev.as_ref(), "BO_CREATE: NO_MMAP conflicts with WB_MMAP\n");
            return Err(EINVAL);
        }

        if bocreate.exclusive_vm_id != 0 {
            dev_dbg!(ddev.as_ref(), "BO_CREATE: exclusive_vm_id not supported\n");
            return Err(EOPNOTSUPP);
        }

        let size = usize::try_from(bocreate.size).map_err(|_| {
            dev_dbg!(
                ddev.as_ref(),
                "BO_CREATE size {:#x} too large\n",
                bocreate.size
            );
            EINVAL
        })?;
        let bo = file.inner_with(|pfile| {
            crate::gem::new_object(
                ddev,
                pfile.reg.pdev.as_ref(),
                size,
                bocreate.flags,
                reg_data.coherent,
            )
        })?;
        bocreate.handle = bo.create_handle(file)?;
        bocreate.size = u64::from_safe_cast(bo.size());

        Ok(0)
    }

    pub(crate) fn bo_mmap_offset(
        ddev: &TyrDrmDevice<Registered>,
        _reg_data: &TyrDrmRegistrationData<'_>,
        bommap: &mut uapi::drm_panthor_bo_mmap_offset,
        file: &TyrDrmFile,
    ) -> Result<u32> {
        if bommap.pad != 0 {
            dev_dbg!(
                ddev.as_ref(),
                "BO mmap offset pad not zero: {}\n",
                bommap.pad
            );
            return Err(EINVAL);
        }

        let bo = crate::gem::lookup_handle(file, bommap.handle).inspect_err(|_| {
            dev_dbg!(ddev.as_ref(), "Invalid BO mmap handle: {}\n", bommap.handle);
        })?;
        if bo.flags() & uapi::drm_panthor_bo_flags_DRM_PANTHOR_BO_NO_MMAP != 0 {
            dev_dbg!(ddev.as_ref(), "BO mmap offset on NO_MMAP object\n");
            return Err(EPERM);
        }
        bommap.offset = bo.create_mmap_offset().inspect_err(|_| {
            dev_err!(
                ddev.as_ref(),
                "Failed to create mmap offset for handle {}\n",
                bommap.handle
            );
        })?;

        Ok(0)
    }
}

fn vm_bind_exec_op(vm: &Vm<'_>, file: &TyrDrmFile, op: &VmBindOp) -> Result {
    if op.syncs.count != 0 {
        dev_dbg!(vm.dev(), "VM_BIND op syncs not supported\n");
        return Err(EINVAL);
    }

    if op.size == 0 {
        return Ok(());
    }

    let end = match op.va.checked_add(op.size) {
        Some(end) => end,
        None => {
            dev_dbg!(vm.dev(), "VM_BIND op VA range overflow\n");
            return Err(EINVAL);
        }
    };
    if op.va < vm.layout.user.start || end > vm.layout.user.end {
        dev_dbg!(
            vm.dev(),
            "VM_BIND op VA range {:#x}..{:#x} outside user range\n",
            op.va,
            end
        );
        return Err(EINVAL);
    }

    if (op.va | op.size | op.bo_offset) & (u64::SZ_4K - 1) != 0 {
        dev_dbg!(vm.dev(), "VM_BIND op not GPU-page-aligned\n");
        return Err(EINVAL);
    }

    match VmBindOpType::try_from(op.flags) {
        Ok(VmBindOpType::Map) => {
            // Once the VM is unusable only MAP ops are rejected; UNMAP
            // stays available for cleanup (see the UAPI docs).
            if vm.is_unusable() {
                dev_dbg!(vm.dev(), "VM_BIND map op on unusable VM\n");
                return Err(EINVAL);
            }

            let map_flags = match VmMapFlags::try_from(op.flags & !VmBindOpType::MASK) {
                Ok(flags) => flags,
                Err(_) => {
                    dev_dbg!(vm.dev(), "VM_BIND op invalid map flags {:#x}\n", op.flags);
                    return Err(EINVAL);
                }
            };
            let bo = crate::gem::lookup_handle(file, op.bo_handle).map_err(|_| {
                dev_dbg!(vm.dev(), "VM_BIND op invalid BO handle {}\n", op.bo_handle);
                EINVAL
            })?;
            // Validate the BO window before mapping.
            let bo_size = u64::from_safe_cast(bo.size());
            if op.size > bo_size || op.bo_offset > bo_size - op.size {
                dev_dbg!(vm.dev(), "VM_BIND op BO range out of bounds\n");
                return Err(EINVAL);
            }
            vm.map_bo_range(&bo, op.bo_offset, op.size, op.va, map_flags)
        }
        Ok(VmBindOpType::Unmap) => {
            // Unmap must not carry map-specific flags or BO references.
            if op.flags & !VmBindOpType::MASK != 0 || op.bo_handle != 0 || op.bo_offset != 0 {
                dev_dbg!(
                    vm.dev(),
                    "VM_BIND UNMAP carries flags/BO refs: flags={:#x} bo_handle={} bo_offset={}\n",
                    op.flags,
                    op.bo_handle,
                    op.bo_offset
                );
                return Err(EINVAL);
            }
            vm.unmap_range(op.va, op.size)
        }
        Err(_) => {
            dev_dbg!(
                vm.dev(),
                "VM_BIND op type {:#x} not supported\n",
                op.flags & VmBindOpType::MASK
            );
            Err(EINVAL)
        }
    }
}

#[repr(transparent)]
struct VmBindOp(uapi::drm_panthor_vm_bind_op);

impl core::ops::Deref for VmBindOp {
    type Target = uapi::drm_panthor_vm_bind_op;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

// TODO: use zerocopy FromBytes when these traits are derived for bindings.
// SAFETY: `VmBindOp` contains only integers, so any bit pattern is valid;
// the `#[repr(transparent)]` wrapper has the same layout as the UAPI struct.
unsafe impl FromBytes for VmBindOp {}
