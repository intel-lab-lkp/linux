// SPDX-License-Identifier: GPL-2.0 or MIT

use kernel::{
    drm::{
        self,
        gem::BaseObject,
        Registered, //
    },
    prelude::*,
    sizes::SizeConstants,
    transmute::FromBytes,
    uaccess::{
        UserSlice,
        UserSliceReader, //
    },
    uapi, //
};

use crate::{
    driver::{
        TyrDrmDevice,
        TyrDrmDriver,
        TyrDrmRegistrationData, //
    },
    pool::VmPool,
    vm::{
        UserVaRequest,
        Vm,
        VmMapFlags,
        VmSpec, //
    }, //
};

#[pin_data(PinnedDrop)]
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
            vm_pool <- VmPool::new()?,
        })
    }
}

#[pinned_drop]
impl PinnedDrop for TyrDrmFileData<'_> {
    fn drop(self: Pin<&mut Self>) {
        let proj = self.project();
        while let Some(vm) = proj.vm_pool.pop_first() {
            vm.kill();
        }
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
            dev_err!(
                ddev.as_ref(),
                "Invalid VM create flags: {:#x}\n",
                vmcreate.flags
            );
            return Err(EINVAL);
        }

        let ret: Result<u32, Error> = file.inner_with(|fd| {
            let vm = Vm::new(
                fd.reg.pdev.as_ref(),
                ddev,
                fd.reg.mmu.as_arc_borrow(),
                &fd.reg.gpu_info,
                VmSpec::User {
                    user_va: UserVaRequest::from_uapi(vmcreate.user_va_range),
                },
            )?;
            vmcreate.user_va_range = vm.layout.user.end;

            let id = fd.vm_pool.add(vm.as_arc_borrow()).inspect_err(|_| {
                vm.kill();
            })?;
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
            dev_err!(
                ddev.as_ref(),
                "Invalid VM destroy pad: {:#x}\n",
                vmdestroy.pad
            );
            return Err(EINVAL);
        }

        let ret: Result<u32, Error> = file.inner_with(|fd| {
            let vm = fd.vm_pool.remove(vmdestroy.id)?;
            vm.kill();
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
            dev_err!(
                ddev.as_ref(),
                "Invalid VM_BIND flags: {:#x}\n",
                vmbind.flags
            );
            return Err(EINVAL);
        }

        if vmbind.flags & async_flag != 0 {
            dev_err!(ddev.as_ref(), "Async VM_BIND not supported\n");
            return Err(ENOTSUPP);
        }

        let count = vmbind.ops.count as usize;
        if count == 0 {
            return Ok(0);
        }

        let size_of_op = size_of::<VmBindOp>();
        // Stride versions the UAPI struct: reject only undersized strides.
        if size_of_op > vmbind.ops.stride as usize {
            dev_err!(
                ddev.as_ref(),
                "Invalid VM_BIND op stride {}\n",
                vmbind.ops.stride
            );
            return Err(EINVAL);
        }
        let stride = vmbind.ops.stride as usize;

        let total_len = stride.checked_mul(count).ok_or_else(|| {
            dev_err!(ddev.as_ref(), "VM_BIND ops length overflow\n");
            EINVAL
        })?;
        let mut reader =
            UserSlice::new(UserPtr::from_addr(vmbind.ops.array as usize), total_len).reader();
        let mut ops = KVec::new();
        for _ in 0..count {
            ops.push(reader.read::<VmBindOp>()?, GFP_KERNEL)?;
            read_padding_zero(&mut reader, stride - size_of_op)?;
        }

        let ret: Result<u32, Error> = file.inner_with(|fd| {
            let vm = fd.vm_pool.get(vmbind.vm_id).ok_or_else(|| {
                dev_err!(ddev.as_ref(), "Invalid VM_BIND vm_id: {}\n", vmbind.vm_id);
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
        file.inner_with(|fd| {
            let vm = fd.vm_pool.get(vmgetstate.vm_id).ok_or_else(|| {
                dev_err!(
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
        _reg_data: &TyrDrmRegistrationData<'_>,
        bocreate: &mut uapi::drm_panthor_bo_create,
        file: &TyrDrmFile,
    ) -> Result<u32> {
        if bocreate.size == 0
            || bocreate.pad != 0
            || bocreate.flags & !uapi::drm_panthor_bo_flags_DRM_PANTHOR_BO_NO_MMAP != 0
            || bocreate.exclusive_vm_id != 0
        {
            dev_err!(
                ddev.as_ref(),
                "Invalid BO_CREATE params: size={}, pad={}, flags={:#x}, exclusive_vm_id={}\n",
                bocreate.size,
                bocreate.pad,
                bocreate.flags,
                bocreate.exclusive_vm_id
            );
            return Err(EINVAL);
        }

        let size = usize::try_from(bocreate.size).map_err(|_| {
            dev_err!(
                ddev.as_ref(),
                "BO_CREATE size {:#x} too large\n",
                bocreate.size
            );
            EINVAL
        })?;
        let bo = crate::gem::new_object(ddev, size, bocreate.flags)?;
        bocreate.handle = bo.create_handle(file)?;
        bocreate.size = bo.size() as u64;

        Ok(0)
    }

    pub(crate) fn bo_mmap_offset(
        ddev: &TyrDrmDevice<Registered>,
        _reg_data: &TyrDrmRegistrationData<'_>,
        bommap: &mut uapi::drm_panthor_bo_mmap_offset,
        file: &TyrDrmFile,
    ) -> Result<u32> {
        if bommap.pad != 0 {
            dev_err!(
                ddev.as_ref(),
                "BO mmap offset pad not zero: {}\n",
                bommap.pad
            );
            return Err(EINVAL);
        }

        let bo = crate::gem::lookup_handle(file, bommap.handle).inspect_err(|_| {
            dev_err!(ddev.as_ref(), "Invalid BO mmap handle: {}\n", bommap.handle);
        })?;
        if bo.create_flags() & uapi::drm_panthor_bo_flags_DRM_PANTHOR_BO_NO_MMAP != 0 {
            dev_err!(ddev.as_ref(), "BO mmap offset on NO_MMAP object\n");
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
    if vm.is_unusable() {
        dev_err!(vm.dev(), "VM_BIND on destroyed VM\n");
        return Err(EINVAL);
    }

    if op.size == 0 {
        return Ok(());
    }

    if op.syncs.count != 0 {
        dev_err!(vm.dev(), "VM_BIND op syncs not supported\n");
        return Err(EINVAL);
    }

    let end = match op.va.checked_add(op.size) {
        Some(end) => end,
        None => {
            dev_err!(vm.dev(), "VM_BIND op VA range overflow\n");
            return Err(EINVAL);
        }
    };
    if op.va < vm.layout.user.start || end > vm.layout.user.end {
        dev_err!(
            vm.dev(),
            "VM_BIND op VA range {:#x}..{:#x} outside user range\n",
            op.va,
            end
        );
        return Err(EINVAL);
    }

    if (op.va | op.size | op.bo_offset) & (u64::SZ_4K - 1) != 0 {
        dev_err!(vm.dev(), "VM_BIND op not GPU-page-aligned\n");
        return Err(EINVAL);
    }

    const TYPE_MASK: u32 =
        uapi::drm_panthor_vm_bind_op_flags_DRM_PANTHOR_VM_BIND_OP_TYPE_MASK as u32;
    const TYPE_MAP: u32 = uapi::drm_panthor_vm_bind_op_flags_DRM_PANTHOR_VM_BIND_OP_TYPE_MAP as u32;
    const TYPE_UNMAP: u32 =
        uapi::drm_panthor_vm_bind_op_flags_DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP as u32;

    match op.flags & TYPE_MASK {
        TYPE_MAP => {
            let map_flags = match VmMapFlags::try_from(op.flags & !TYPE_MASK) {
                Ok(flags) => flags,
                Err(_) => {
                    dev_err!(vm.dev(), "VM_BIND op invalid map flags {:#x}\n", op.flags);
                    return Err(EINVAL);
                }
            };
            let bo = crate::gem::lookup_handle(file, op.bo_handle).map_err(|_| {
                dev_err!(vm.dev(), "VM_BIND op invalid BO handle {}\n", op.bo_handle);
                EINVAL
            })?;
            // Validate the BO window before mapping.
            let bo_size = bo.size() as u64;
            if op.size > bo_size || op.bo_offset > bo_size - op.size {
                dev_err!(vm.dev(), "VM_BIND op BO range out of bounds\n");
                return Err(EINVAL);
            }
            vm.map_bo_range(&bo, op.bo_offset, op.size, op.va, map_flags)
        }
        TYPE_UNMAP => {
            // Unmap must not carry map-specific flags or BO references.
            if op.flags & !TYPE_MASK != 0 || op.bo_handle != 0 || op.bo_offset != 0 {
                dev_err!(
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
        _ => {
            dev_err!(vm.dev(), "VM_BIND op type {:#x} not supported\n", op.flags);
            Err(EINVAL)
        }
    }
}

/// Reads `len` bytes of array padding, rejecting any nonzero byte with `E2BIG`.
fn read_padding_zero(reader: &mut UserSliceReader, len: usize) -> Result {
    let mut buf = [0u8; 64];
    let mut remaining = len;
    while remaining > 0 {
        let chunk = remaining.min(buf.len());
        reader.read_slice(&mut buf[..chunk])?;
        if buf[..chunk].iter().any(|&b| b != 0) {
            return Err(E2BIG);
        }
        remaining -= chunk;
    }
    Ok(())
}

#[repr(transparent)]
struct VmBindOp(uapi::drm_panthor_vm_bind_op);

impl core::ops::Deref for VmBindOp {
    type Target = uapi::drm_panthor_vm_bind_op;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

// SAFETY: `VmBindOp` contains only integers, so any bit pattern is valid;
// the `#[repr(transparent)]` wrapper has the same layout as the UAPI struct.
unsafe impl FromBytes for VmBindOp {}
