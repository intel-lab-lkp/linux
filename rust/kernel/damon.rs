// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 KylinSoft Corporation.
// Author: Enze Li <lienze@kylinos.cn>

//! DAMON (Data Access MONitor) abstractions.
//!
//! C header: [`include/linux/damon.h`](srctree/include/linux/damon.h)

use core::ptr::NonNull;

use kernel::bindings;
use kernel::error::code::*;
use kernel::error::Error;
use kernel::error::Result;

/// Return if DAMON is ready to be used.
pub fn damon_initialized() -> bool {
    // SAFETY: It is a simple pure query function.
    unsafe { bindings::damon_initialized() }
}

/// DAMON operations.
pub struct OpsID {
    ops_id: bindings::damon_ops_id,
}

impl OpsID {
    /// Monitoring operations for virtual address spaces.
    pub const VADDR: Self = Self {
        ops_id: bindings::damon_ops_id_DAMON_OPS_VADDR,
    };
}

/// Represents a monitoring target.
pub struct Target {
    target: NonNull<bindings::damon_target>,
}

impl Target {
    /// Construct a damon_target struct.
    pub fn damon_new_target() -> Result<Self> {
        // SAFETY: damon_new_target returns a valid pointer to new allocated
        // damon_target or NULL if allocation failure, which will be checked
        // by the subsequent NonNull::new.
        let raw = unsafe { bindings::damon_new_target() };
        let t = NonNull::new(raw).ok_or(ENOMEM)?;
        Ok(Self { target: t })
    }

    /// Set the PID of monitoring target.
    pub fn damon_set_target_pid(&mut self, pid: i32) -> Result {
        // SAFETY: slef.target is a valid, non-null 'damon_target *' by the
        // Target invariant, and the '&mut self' borrow keeps it alive for
        // the duration of the call.  The returned errno is checked below.
        let ret = unsafe { bindings::damon_set_target_pid(self.target.as_ptr(), pid) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(())
        }
    }
}

impl Drop for Target {
    fn drop(&mut self) {
        let t = self.target.as_ptr();

        // SAFETY: t is a valid, non-null 'damon_target *' by the Target
        // invariant, and release the pid refcount taken by find_get_pid
        // with a matching put_pid, then free the target once in Drop.
        unsafe {
            if !(*t).pid.is_null() {
                bindings::put_pid((*t).pid);
            }
            bindings::damon_free_target(t)
        };
    }
}

/// Target access pattern of the given scheme.
pub struct DamosAccessPattern {
    damos_access_pattern: bindings::damos_access_pattern,
}

impl DamosAccessPattern {
    /// Create a new damos_access_pattern.
    pub const fn new(
        min_sz_region: usize,
        max_sz_region: usize,
        min_nr_accesses: u32,
        max_nr_accesses: u32,
        min_age_region: u32,
        max_age_region: u32,
    ) -> Self {
        Self {
            damos_access_pattern: bindings::damos_access_pattern {
                min_sz_region,
                max_sz_region,
                min_nr_accesses,
                max_nr_accesses,
                min_age_region,
                max_age_region,
            },
        }
    }
}

/// Controls the aggressiveness of the given scheme.
pub struct DamosQuota {
    damos_quota: bindings::damos_quota,
}

impl DamosQuota {
    /// Create a zero-initialized damos_quota sturct.
    pub fn zeroed() -> Self {
        Self {
            // SAFETY: All-zero is a valid bit pattern for damos_quota.
            damos_quota: unsafe { core::mem::zeroed() },
        }
    }
}

/// Controls when a given scheme should be activated.
pub struct DamosWatermarks {
    damos_watermarks: bindings::damos_watermarks,
}

impl DamosWatermarks {
    /// Create a zero-initialized damos_watermarks sturct.
    pub fn zeroed() -> Self {
        Self {
            // SAFETY: All-zero is a valid bit pattern for damos_watermarks.
            damos_watermarks: unsafe { core::mem::zeroed() },
        }
    }
}

/// Represents an action of a Data Access Monitoring-based Operation Scheme.
pub struct DamosAction {
    /// Reclaim the region.
    damos_action: bindings::damos_action,
}

impl DamosAction {
    /// PAGEOUT action.
    pub const PAGEOUT: Self = Self {
        damos_action: bindings::damos_action_DAMOS_PAGEOUT,
    };
}

/// Represents a Data Access Monitoring-based Operation Scheme.
pub struct Damos {
    damos: NonNull<bindings::damos>,
}

impl Damos {
    /// Create new Damos.
    pub fn new(
        pattern: &mut DamosAccessPattern,
        action: DamosAction,
        apply_interval_us: usize,
        quota: &mut DamosQuota,
        wmarks: &mut DamosWatermarks,
        target_nid: i32,
    ) -> Result<Self> {
        // SAFETY: All pointer arguments come from local variables that
        // stay alive during the call; the rest are just numbers.  The
        // return value is checked for NULL right after.
        let ptr = unsafe {
            bindings::damon_new_scheme(
                &mut pattern.damos_access_pattern,
                action.damos_action,
                apply_interval_us,
                &mut quota.damos_quota,
                &mut wmarks.damos_watermarks,
                target_nid,
            )
        };
        NonNull::new(ptr).map(|p| Self { damos: p }).ok_or(ENOMEM)
    }
}

/// DAMON monitoring context.
pub struct DamonCtx {
    ctx: NonNull<bindings::damon_ctx>,
}

impl DamonCtx {
    /// Create a new DAMON monitoring context.
    pub fn damon_new_ctx() -> Result<Self> {
        // SAFETY: damon_new_ctx returns a valid pointer to new allocated
        // damon_ctx or NULL if allocation failure, which will be checked
        // by the subsequent NonNull::new.
        let raw = unsafe { bindings::damon_new_ctx() };
        let c = NonNull::new(raw).ok_or(ENOMEM)?;
        Ok(Self { ctx: c })
    }

    /// Select a monitoring operations to use with the context.
    pub fn damon_select_ops(&self, id: OpsID) -> Result {
        // SAFETY: self.ctx was created by a successful damon_new_ctx()
        // call, so it points to a valid damon_ctx. id.ops_id comes from a
        // known-good constant, so it is a value the C side understands.
        let ret = unsafe { bindings::damon_select_ops(self.ctx.as_ptr(), id.ops_id) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(())
        }
    }

    /// Add a new DAMON monitoring target.
    pub fn damon_add_target(&self, target: Target) {
        // SAFETY: Both arguments are valid: self.ctx is non-null by
        // construction, and target.target points to an unique owned
        // damon_target.  The C function takes ownership of the target
        // that is why rust slide should forget it.
        unsafe { bindings::damon_add_target(self.ctx.as_ptr(), target.target.as_ptr()) };
        core::mem::forget(target);
    }

    /// Add a scheme to context.
    pub fn damon_add_scheme(&self, scheme: Damos) {
        // SAFETY: self.ctx is a valid 'damon_ctx *' and scheme.damos is a
        // valid 'damos *', both by the type invariants of Ctx and Damos.
        // damon_add_scheme takes ownership of the scheme, so the rust
        // side should forget it to avoid a double free.
        unsafe { bindings::damon_add_scheme(self.ctx.as_ptr(), scheme.damos.as_ptr()) };
        core::mem::forget(scheme);
    }

    /// Starts the monitorings for a given group of contexts.
    pub fn damon_start(&self) -> Result {
        let mut ctxs = [self.ctx.as_ptr()];
        // SAFETY: ctxs is a stack-allocated array of length 1 whose only
        // element is a valid 'damon_ctx *'.  The pointer remains valid
        // for the duration of the call.  The length argument 1 matches the
        // array size, and true is a valid bool value for the exclusive
        // parameter.
        let ret = unsafe { bindings::damon_start(ctxs.as_mut_ptr(), 1, true) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(())
        }
    }

    /// Transfers ownership of the underlying damon_ctx to the caller.
    /// After this call, the DamonCtx is consumed and will not call
    /// damon_destroy_ctx on drop.  The caller becomes responsible for
    /// eventually calling damon_destroy_ctx.
    pub fn into_raw(self) -> *mut bindings::damon_ctx {
        let ptr = self.ctx.as_ptr();
        core::mem::forget(self);
        ptr
    }

    /// Reconstructs a DamonCtx from a raw pointer returned by into_raw.
    /// Note that the ptr must be the non-null pointer obtained from
    /// into_raw(), and returned once -- the caller must not use ptr
    /// afterwards, and no other owner may remain.
    pub unsafe fn from_raw(ptr: *mut bindings::damon_ctx) -> Self {
        // SAFETY: ptr is a valid, non-null 'damon_ctx *'.
        Self {
            ctx: unsafe { NonNull::new_unchecked(ptr) },
        }
    }

    /// Stops the monitorings for a given group of contexts.
    fn damon_stop(&self, nr_ctxs: i32) {
        let mut ctxs = [self.ctx.as_ptr()];
        // SAFETY: ctxs is a stack buffer whose element is a valid
        // 'damon_ctx *' by the DamonCtx invariant.
        unsafe { bindings::damon_stop(ctxs.as_mut_ptr(), nr_ctxs) };
    }

    /// Destroys the contexts and frees all associated resources.
    fn damon_destroy_ctx(&self) {
        // SAFETY: self.ctx is a valid, non-null 'damon_ctx *' by the
        // DamonCtx invariant, and it is only destroyed once from drop()
        // that execute after damon_stop().
        unsafe { bindings::damon_destroy_ctx(self.ctx.as_ptr()) };
    }
}

impl Drop for DamonCtx {
    fn drop(&mut self) {
        self.damon_stop(1);
        self.damon_destroy_ctx();
    }
}
