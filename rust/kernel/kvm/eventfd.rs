// SPDX-License-Identifier: GPL-2.0
#![allow(unsafe_op_in_unsafe_fn)]

//! KVM eventfd support - use eventfd objects to signal various KVM events.
//!
//! This is a Rust reimplementation of `virt/kvm/eventfd.c`, providing
//! irqfd (interrupt injection via eventfd) and ioeventfd (MMIO/PIO write
//! to eventfd signal) functionality with full C API compatibility.
//!
//! Copyright 2009 Novell.  All Rights Reserved.
//! Copyright 2010 Red Hat, Inc. and/or its affiliates.
//! Author: Gregory Haskins <ghaskins@novell.com>

use core::ptr;

/// Read a pointer value with a compiler barrier, analogous to rcu_dereference().
///
/// On architectures supported by Rust for Linux (x86, arm64), a compiler
/// barrier is sufficient for rcu_dereference semantics (no address-dependency
/// issues).  When Rust gains first-class support for alpha or other weakly
/// ordered architectures, this must be replaced with a proper barrier.
///
/// # Safety
/// Caller must hold the appropriate RCU/SRCU read lock and ensure `p` is valid.
#[inline(always)]
unsafe fn rcu_dereference_raw<T>(p: *const *mut T) -> *mut T {
    // SAFETY: Caller ensures p is valid and appropriate read lock is held.
    unsafe { core::ptr::read_volatile(p) }
}

use crate::bindings;
use crate::error::Result;
use crate::ffi::{c_int, c_uint, c_void};

// Constants

// These must match the UAPI definitions in include/uapi/linux/kvm.h.
// TODO: Use bindgen-generated constants when available.
const KVM_IOEVENTFD_FLAG_DATAMATCH: u32 = 1 << 0;
const KVM_IOEVENTFD_FLAG_PIO: u32 = 1 << 1;
const KVM_IOEVENTFD_FLAG_DEASSIGN: u32 = 1 << 2;
const KVM_IOEVENTFD_FLAG_VIRTIO_CCW_NOTIFY: u32 = 1 << 3;
const KVM_IOEVENTFD_VALID_FLAG_MASK: u32 = (1 << 5) - 1;

/// `WQ_PERCPU` flag - per-CPU workqueue, not exported as a bindgen constant.
const WQ_PERCPU: u32 = 1 << 8;

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
#[repr(C)]
struct KvmIrqfdPt {
    irqfd: *mut bindings::kvm_kernel_irqfd,
    kvm: *mut bindings::kvm,
    pt: bindings::poll_table,
    ret: c_int,
}

// Helper wrappers for unsafe FFI calls

/// # Safety
/// Caller must ensure `ctx` is a valid eventfd_ctx pointer.
#[inline(always)]
unsafe fn eventfd_ctx_put(ctx: *mut bindings::eventfd_ctx) {
    // SAFETY: Caller guarantees ctx is valid.
    unsafe { bindings::eventfd_ctx_put(ctx) }
}

/// # Safety
/// Caller must ensure `ctx` is a valid eventfd_ctx pointer.
#[inline(always)]
unsafe fn eventfd_signal(ctx: *mut bindings::eventfd_ctx) {
    // SAFETY: Caller guarantees ctx is valid.
    unsafe { bindings::eventfd_signal(ctx) }
}

#[inline(always)]
unsafe fn eventfd_ctx_fdget(fd: c_int) -> Result<*mut bindings::eventfd_ctx> {
    // SAFETY: FFI call, fd is a valid file descriptor.
    let ptr = unsafe { bindings::eventfd_ctx_fdget(fd) };
    let ptr = crate::error::from_err_ptr(ptr)?;
    Ok(ptr as *mut _)
}

/// # Safety
/// Caller must ensure `file` is a valid file pointer.
#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe fn eventfd_ctx_fileget(file: *mut bindings::file) -> Result<*mut bindings::eventfd_ctx> {
    // SAFETY: Caller guarantees file is valid.
    let ptr = unsafe { bindings::eventfd_ctx_fileget(file) };
    let ptr = crate::error::from_err_ptr(ptr)?;
    Ok(ptr as *mut _)
}

/// # Safety
/// Caller must hold proper locks.
#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
#[inline(always)]
unsafe fn kvm_set_irq(
    kvm: *mut bindings::kvm,
    source_id: c_int,
    irq: u32,
    level: c_int,
    line_status: bool,
) {
    // SAFETY: Caller guarantees kvm is valid and proper locks held.
    unsafe {
        bindings::kvm_set_irq(kvm, source_id, irq, level, line_status);
    }
}

/// # Safety
/// Caller must ensure `ctx`, `wait`, and `cnt` are valid pointers.
#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
#[inline(always)]
unsafe fn eventfd_ctx_remove_wait_queue(
    ctx: *mut bindings::eventfd_ctx,
    wait: *mut bindings::wait_queue_entry,
    cnt: *mut u64,
) -> c_int {
    // SAFETY: FFI call with valid pointers.
    unsafe { bindings::eventfd_ctx_remove_wait_queue(ctx, wait, cnt) }
}

/// # Safety
/// Caller must ensure `ctx` and `cnt` are valid pointers.
#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
#[inline(always)]
unsafe fn eventfd_ctx_do_read(ctx: *mut bindings::eventfd_ctx, cnt: *mut u64) {
    // SAFETY: FFI call with valid pointers.
    unsafe { bindings::eventfd_ctx_do_read(ctx, cnt) }
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
#[inline(always)]
unsafe fn init_waitqueue_func_entry(
    wq_entry: *mut bindings::wait_queue_entry,
    func: bindings::wait_queue_func_t,
) {
    // SAFETY: FFI call, wq_entry is valid.
    unsafe { bindings::init_waitqueue_func_entry(wq_entry, func) }
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
#[inline(always)]
unsafe fn add_wait_queue_priority_exclusive(
    wq_head: *mut bindings::wait_queue_head,
    wq_entry: *mut bindings::wait_queue_entry,
) -> c_int {
    // SAFETY: FFI call.
    unsafe { bindings::add_wait_queue_priority_exclusive(wq_head, wq_entry) }
}

// Lockdep helpers - these wrap C macros that are not available via bindgen.
#[cfg(CONFIG_LOCKDEP)]
#[inline(always)]
unsafe fn spin_release(map: *mut bindings::lockdep_map, ip: core::ffi::c_ulong) {
    // SAFETY: Caller holds the corresponding lock; this is a lockdep annotation only.
    unsafe { bindings::rust_helper_spin_release(map, ip) }
}

#[cfg(CONFIG_LOCKDEP)]
#[inline(always)]
unsafe fn spin_acquire(
    map: *mut bindings::lockdep_map,
    subclass: c_int,
    trylock: c_int,
    ip: core::ffi::c_ulong,
) {
    // SAFETY: Caller holds the corresponding lock; this is a lockdep annotation only.
    unsafe { bindings::rust_helper_spin_acquire(map, subclass, trylock, ip) }
}

// RAII Guards

/// RAII Guard for SRCU read lock.
struct SrcuGuard {
    srcu: *mut bindings::srcu_struct,
    idx: core::ffi::c_int,
    _not_thread_safe: crate::types::NotThreadSafe,
}

impl SrcuGuard {
    /// # Safety
    /// Caller must ensure `srcu` is a valid pointer.
    unsafe fn new(srcu: *mut bindings::srcu_struct) -> Self {
        // SAFETY: Caller guarantees `srcu` is valid.
        let idx = unsafe { bindings::srcu_read_lock(srcu) };
        Self {
            srcu,
            idx,
            _not_thread_safe: crate::types::NotThreadSafe,
        }
    }
}

impl Drop for SrcuGuard {
    fn drop(&mut self) {
        // SAFETY: We hold the lock `idx` on `srcu`.
        unsafe { bindings::srcu_read_unlock(self.srcu, self.idx) };
    }
}

/// RAII Guard for mutex lock.
struct MutexGuard {
    lock: *mut bindings::mutex,
}

impl MutexGuard {
    /// # Safety
    /// Caller must ensure `lock` is a valid pointer.
    unsafe fn new(lock: *mut bindings::mutex) -> Self {
        // SAFETY: Caller guarantees `lock` is valid.
        unsafe { bindings::mutex_lock(lock) };
        Self { lock }
    }
}

impl Drop for MutexGuard {
    fn drop(&mut self) {
        // SAFETY: We hold the lock.
        unsafe { bindings::mutex_unlock(self.lock) };
    }
}

/// RAII Guard for spin_lock_irq.
struct SpinLockIrqGuard {
    lock: *mut bindings::spinlock_t,
}

impl SpinLockIrqGuard {
    /// # Safety
    /// Caller must ensure `lock` is a valid pointer.
    unsafe fn new(lock: *mut bindings::spinlock_t) -> Self {
        // SAFETY: Caller guarantees `lock` is valid.
        unsafe { bindings::spin_lock_irq(lock) };
        Self { lock }
    }
}

impl Drop for SpinLockIrqGuard {
    fn drop(&mut self) {
        // SAFETY: We hold the lock and IRQs are disabled.
        unsafe { bindings::spin_unlock_irq(self.lock) };
    }
}

/// RAII Guard for spin_lock_irqsave.
struct SpinLockIrqSaveGuard {
    lock: *mut bindings::spinlock_t,
    flags: core::ffi::c_ulong,
}

impl SpinLockIrqSaveGuard {
    /// # Safety
    /// Caller must ensure `lock` is a valid pointer.
    unsafe fn new(lock: *mut bindings::spinlock_t) -> Self {
        // SAFETY: Caller guarantees `lock` is valid.
        let flags = unsafe { bindings::rust_helper_spin_lock_irqsave(lock) };
        Self {
            lock,
            flags: flags as _,
        }
    }
}

impl Drop for SpinLockIrqSaveGuard {
    fn drop(&mut self) {
        // SAFETY: We hold the lock.
        unsafe { bindings::rust_helper_spin_unlock_irqrestore(self.lock, self.flags as _) };
    }
}

// irqfd - Interrupt injection via eventfd

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
static IRQFD_CLEANUP_WQ: core::sync::atomic::AtomicPtr<bindings::workqueue_struct> =
    core::sync::atomic::AtomicPtr::new(ptr::null_mut());

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe extern "C" fn irqfd_inject(work: *mut bindings::work_struct) {
    // SAFETY: work is embedded in kvm_kernel_irqfd at the inject field offset.
    let irqfd: *mut bindings::kvm_kernel_irqfd =
        crate::container_of!(work, bindings::kvm_kernel_irqfd, inject);
    let kvm = (*irqfd).kvm;

    if (*irqfd).resampler.is_null() {
        kvm_set_irq(
            kvm,
            bindings::KVM_USERSPACE_IRQ_SOURCE_ID as _,
            (*irqfd).gsi as u32,
            1,
            false,
        );
        kvm_set_irq(
            kvm,
            bindings::KVM_USERSPACE_IRQ_SOURCE_ID as _,
            (*irqfd).gsi as u32,
            0,
            false,
        );
    } else {
        kvm_set_irq(
            kvm,
            bindings::KVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID as _,
            (*irqfd).gsi as u32,
            1,
            false,
        );
    }
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe fn irqfd_resampler_notify(resampler: *mut bindings::kvm_kernel_irqfd_resampler) {
    // SAFETY: Caller holds SRCU read lock on kvm->irq_srcu. Use read_volatile for
    // RCU-safe pointer reads (equivalent to READ_ONCE / list_for_each_entry_srcu).
    let mut pos = unsafe { rcu_dereference_raw(&raw const (*resampler).list.next) };
    let head = &raw mut (*resampler).list;

    while pos != head {
        let irqfd: *mut bindings::kvm_kernel_irqfd =
            crate::container_of!(pos, bindings::kvm_kernel_irqfd, resampler_link).cast();
        eventfd_signal((*irqfd).resamplefd);
        pos = unsafe { rcu_dereference_raw(&raw const (*pos).next) };
    }
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe extern "C" fn irqfd_resampler_ack(kian: *mut bindings::kvm_irq_ack_notifier) {
    // SAFETY: kian is embedded in kvm_kernel_irqfd_resampler.
    let resampler: *mut bindings::kvm_kernel_irqfd_resampler =
        crate::container_of!(kian, bindings::kvm_kernel_irqfd_resampler, notifier);
    let kvm = (*resampler).kvm;

    kvm_set_irq(
        kvm,
        bindings::KVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID as i32,
        (*resampler).notifier.gsi,
        0,
        false,
    );

    // SAFETY: kvm->irq_srcu is valid.
    let _srcu_guard = unsafe { SrcuGuard::new(&raw mut (*kvm).irq_srcu) };
    irqfd_resampler_notify(resampler);
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe fn irqfd_resampler_shutdown(irqfd: *mut bindings::kvm_kernel_irqfd) {
    let resampler = (*irqfd).resampler;
    let kvm = (*resampler).kvm;

    // SAFETY: kvm->irqfds.resampler_lock is valid.
    let _mutex_guard = unsafe { MutexGuard::new(&raw mut (*kvm).irqfds.resampler_lock) };

    unsafe { bindings::list_del_rcu(&raw mut (*irqfd).resampler_link) };

    if unsafe { bindings::list_empty(&(*resampler).list) } != 0 {
        unsafe { bindings::list_del_rcu(&raw mut (*resampler).link) };
        unsafe { bindings::kvm_unregister_irq_ack_notifier(kvm, &raw mut (*resampler).notifier) };
        kvm_set_irq(
            kvm,
            bindings::KVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID as i32,
            (*resampler).notifier.gsi,
            0,
            false,
        );
        unsafe { bindings::kfree(resampler as *const c_void) };
    } else {
        unsafe { bindings::synchronize_srcu_expedited(&raw mut (*kvm).irq_srcu) };
    }

    // _mutex_guard dropped here
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe extern "C" fn irqfd_shutdown(work: *mut bindings::work_struct) {
    // SAFETY: work is embedded in kvm_kernel_irqfd at the shutdown field offset.
    let irqfd: *mut bindings::kvm_kernel_irqfd =
        crate::container_of!(work, bindings::kvm_kernel_irqfd, shutdown);
    let kvm = (*irqfd).kvm;
    let mut cnt: u64 = 0;

    // SAFETY: synchronize_srcu_expedited ensures irqfd is fully initialized.
    unsafe { bindings::synchronize_srcu_expedited(&raw mut (*kvm).irq_srcu) };

    // SAFETY: Unhook from wait-queue to prevent further events.
    unsafe {
        eventfd_ctx_remove_wait_queue((*irqfd).eventfd, &raw mut (*irqfd).wait, &raw mut cnt)
    };

    // SAFETY: Block until all outstanding events complete.
    unsafe { bindings::flush_work(&raw mut (*irqfd).inject) };

    if !(*irqfd).resampler.is_null() {
        irqfd_resampler_shutdown(irqfd);
        eventfd_ctx_put((*irqfd).resamplefd);
    }

    #[cfg(CONFIG_HAVE_KVM_IRQ_BYPASS)]
    {
        unsafe { bindings::irq_bypass_unregister_consumer(&raw mut (*irqfd).consumer) };
    }

    eventfd_ctx_put((*irqfd).eventfd);
    // SAFETY: irqfd was allocated with kzalloc, now freeing.
    unsafe { bindings::kfree(irqfd as *const c_void) };
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe fn irqfd_is_active(irqfd: *mut bindings::kvm_kernel_irqfd) -> bool {
    // Assert that either irqfds.lock or SRCU is held, matching C's lockdep_assert_once().
    unsafe { bindings::lockdep_assert_irqfd_access((*irqfd).kvm) };
    // SAFETY: The caller guarantees `irqfd` is a valid pointer to `kvm_kernel_irqfd`.
    unsafe { bindings::list_empty(&(*irqfd).list) == 0 }
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe fn irqfd_deactivate(irqfd: *mut bindings::kvm_kernel_irqfd) {
    unsafe { bindings::lockdep_assert_held_irqfds_lock((*irqfd).kvm) };
    // SAFETY: The caller guarantees `irqfd` is valid and `irqfds.lock` is held.
    if !unsafe { irqfd_is_active(irqfd) } {
        // SAFETY: This is a kernel BUG condition that should never be reached.
        panic!("BUG: irqfd is not active during deactivation!");
    }
    // SAFETY: `irqfd` is valid, and the caller holds the `irqfds.lock` which protects `list`.
    unsafe { bindings::list_del_init(&raw mut (*irqfd).list) };

    // SAFETY: IRQFD_CLEANUP_WQ is initialized during kvm_irqfd_init, and `shutdown` work is valid.
    unsafe {
        bindings::queue_work(
            IRQFD_CLEANUP_WQ.load(core::sync::atomic::Ordering::Acquire),
            &raw mut (*irqfd).shutdown,
        )
    };
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe extern "C" fn irqfd_wakeup(
    wait: *mut bindings::wait_queue_entry,
    _mode: c_uint,
    _sync: c_int,
    key: *mut c_void,
) -> c_int {
    // SAFETY: wait is embedded in kvm_kernel_irqfd.
    let irqfd: *mut bindings::kvm_kernel_irqfd =
        crate::container_of!(wait, bindings::kvm_kernel_irqfd, wait);
    let flags = (key as usize) as bindings::__poll_t;
    let kvm = (*irqfd).kvm;
    let mut ret: c_int = 0;

    if (flags & bindings::POLLIN) != 0 {
        let mut cnt: u64 = 0;
        eventfd_ctx_do_read((*irqfd).eventfd, &raw mut cnt);

        // SAFETY: kvm->irq_srcu is valid.
        let _srcu_guard = unsafe { SrcuGuard::new(&raw mut (*kvm).irq_srcu) };

        // Read irq_entry under seqcount protection to avoid torn reads
        // when kvm_irq_routing_update() writes concurrently on another CPU.
        let mut irq = loop {
            let seq = unsafe { bindings::read_seqcount_begin(&raw mut (*irqfd).irq_entry_sc) };
            let entry = unsafe { core::ptr::read(&(*irqfd).irq_entry) };
            if unsafe { bindings::read_seqcount_retry(&raw mut (*irqfd).irq_entry_sc, seq) } == 0 {
                break entry;
            }
        };

        let deactivating = !irqfd_is_active(irqfd);
        if deactivating
            || unsafe {
                bindings::kvm_arch_set_irq_inatomic(
                    &raw mut irq,
                    kvm,
                    bindings::KVM_USERSPACE_IRQ_SOURCE_ID as i32,
                    1,
                    false,
                )
            } == -(bindings::EWOULDBLOCK as c_int)
        {
            // SAFETY: FFI call.
            unsafe { bindings::schedule_work(&raw mut (*irqfd).inject) };
        }

        // _srcu_guard is dropped here
        ret = 1;
    }

    if (flags & bindings::POLLHUP) != 0 {
        // SAFETY: Taking irqfds.lock is safe here per original C comment.
        // SAFETY: kvm is valid, irqfds.lock is valid.
        let _spin_guard = unsafe { SpinLockIrqSaveGuard::new(&raw mut (*kvm).irqfds.lock) };

        if irqfd_is_active(irqfd) {
            irqfd_deactivate(irqfd);
        }

        // _spin_guard dropped here
    }

    ret
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe fn irqfd_update(kvm: *mut bindings::kvm, irqfd: *mut bindings::kvm_kernel_irqfd) {
    unsafe { bindings::lockdep_assert_held_irqfds_lock(kvm) };
    let mut entries: [bindings::kvm_kernel_irq_routing_entry; bindings::KVM_NR_IRQCHIPS as usize] =
        unsafe { core::mem::zeroed() };

    let n_entries = unsafe { bindings::kvm_irq_map_gsi(kvm, entries.as_mut_ptr(), (*irqfd).gsi) };

    unsafe { bindings::write_seqcount_begin(&raw mut (*irqfd).irq_entry_sc) };

    if n_entries == 1 {
        unsafe { ptr::write(&raw mut (*irqfd).irq_entry, entries[0]) };
    } else {
        (*irqfd).irq_entry.type_ = 0;
    }

    unsafe { bindings::write_seqcount_end(&raw mut (*irqfd).irq_entry_sc) };
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe extern "C" fn kvm_irqfd_register(
    _file: *mut bindings::file,
    wqh: *mut bindings::wait_queue_head,
    pt: *mut bindings::poll_table,
) {
    let p = crate::container_of!(pt, KvmIrqfdPt, pt);
    let irqfd = (*p).irqfd;
    let kvm = (*p).kvm;

    // SAFETY: Take irqfds.lock to protect routing and list.
    let _spin_guard = unsafe { SpinLockIrqGuard::new(&raw mut (*kvm).irqfds.lock) };

    // Initialize routing before registering with eventfd.
    irqfd_update(kvm, irqfd);

    // Initialize wait queue entry with our custom wakeup handler.
    init_waitqueue_func_entry(&raw mut (*irqfd).wait, Some(irqfd_wakeup));

    // Temporarily lie to lockdep about holding irqfds.lock to avoid a
    // false positive: add_wait_queue_priority_exclusive takes wqh->lock
    // internally, and lockdep would see irqfds.lock -> wqh->lock, which
    // matches the reverse order in irqfd_wakeup(EPOLLHUP).
    // SAFETY: `kvm` is a valid pointer, and `irqfds.lock` is held.
    unsafe { bindings::rust_helper_kvm_irqfds_spin_release(kvm) };

    // Add as priority waiter on eventfd.
    (*p).ret = add_wait_queue_priority_exclusive(wqh, &raw mut (*irqfd).wait);

    // SAFETY: `kvm` is valid, lockdep state is restored after
    // releasing wait queue lock.
    unsafe { bindings::rust_helper_kvm_irqfds_spin_acquire(kvm) };

    if (*p).ret == 0 {
        // SAFETY: Adding irqfd to kvm's irqfd list.
        unsafe { bindings::list_add_tail(&raw mut (*irqfd).list, &raw mut (*kvm).irqfds.items) };
    }

    // _spin_guard dropped here
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe fn kvm_irqfd_assign(kvm: *mut bindings::kvm, args: *mut bindings::kvm_irqfd) -> c_int {
    if !unsafe { bindings::kvm_arch_intc_initialized(kvm) } {
        return -(bindings::EAGAIN as c_int);
    }

    if !unsafe { bindings::kvm_arch_irqfd_allowed(kvm, args) } {
        return -(bindings::EINVAL as c_int);
    }

    // Allocate irqfd.
    let irqfd: *mut bindings::kvm_kernel_irqfd = unsafe {
        bindings::kzalloc(
            core::mem::size_of::<bindings::kvm_kernel_irqfd>(),
            bindings::GFP_KERNEL_ACCOUNT,
        )
    } as *mut _;

    if irqfd.is_null() {
        return -(bindings::ENOMEM as c_int);
    }
    let irqfd_guard =
        crate::types::ScopeGuard::new(move || unsafe { bindings::kfree(irqfd as *const c_void) });

    // Initialize fields.
    (*irqfd).kvm = kvm;
    (*irqfd).gsi = (*args).gsi as i32;
    unsafe { bindings::INIT_LIST_HEAD(&raw mut (*irqfd).list) };

    // Init inject work.
    unsafe {
        crate::bindings::init_work_with_key(
            &raw mut (*irqfd).inject,
            Some(irqfd_inject),
            false,
            b"irqfd::inject\0".as_ptr() as *const _,
            crate::sync::static_lock_class!().as_ptr() as *mut _,
        )
    };

    // Init shutdown work.
    unsafe {
        crate::bindings::init_work_with_key(
            &raw mut (*irqfd).shutdown,
            Some(irqfd_shutdown),
            false,
            b"irqfd::shutdown\0".as_ptr() as *const _,
            crate::sync::static_lock_class!().as_ptr() as *mut _,
        )
    };

    unsafe {
        bindings::seqcount_spinlock_init(
            &raw mut (*irqfd).irq_entry_sc,
            &raw mut (*kvm).irqfds.lock,
        )
    };

    // Get eventfd from fd.
    let f = unsafe { bindings::fdget((*args).fd) };
    if unsafe { bindings::fd_empty(f) } != 0 {
        return -(bindings::EBADF as c_int);
    }
    let _f_guard = crate::types::ScopeGuard::new(move || unsafe { bindings::fdput(f) });

    let file = unsafe { bindings::fd_file(f) };
    let eventfd = match unsafe { eventfd_ctx_fileget(file) } {
        Ok(ctx) => ctx,
        Err(e) => return e.to_errno() as c_int,
    };
    (*irqfd).eventfd = eventfd;
    let eventfd_guard = crate::types::ScopeGuard::new(move || unsafe { eventfd_ctx_put(eventfd) });

    let resamplefd_guard = crate::types::ScopeGuard::new(move || unsafe {
        if !(*irqfd).resamplefd.is_null() {
            eventfd_ctx_put((*irqfd).resamplefd);
        }
    });

    let resampler_guard = crate::types::ScopeGuard::new(move || unsafe {
        if !(*irqfd).resampler.is_null() {
            irqfd_resampler_shutdown(irqfd);
        }
    });

    // Handle RESAMPLE flag.
    if ((*args).flags & bindings::KVM_IRQFD_FLAG_RESAMPLE) != 0 {
        let resamplefd = match unsafe { eventfd_ctx_fdget((*args).resamplefd as i32) } {
            Ok(ctx) => ctx,
            Err(e) => return e.to_errno() as c_int,
        };
        (*irqfd).resamplefd = resamplefd;
        unsafe { bindings::INIT_LIST_HEAD(&raw mut (*irqfd).resampler_link) };

        // SAFETY: kvm->irqfds.resampler_lock is valid.
        let _mutex_guard = unsafe { MutexGuard::new(&raw mut (*kvm).irqfds.resampler_lock) };

        // Search for existing resampler for this gsi.
        let mut found = false;
        let mut head = (*kvm).irqfds.resampler_list.next;
        while head != &raw mut (*kvm).irqfds.resampler_list {
            let resampler: *mut bindings::kvm_kernel_irqfd_resampler =
                crate::container_of!(head, bindings::kvm_kernel_irqfd_resampler, link);
            if (*resampler).notifier.gsi == (*irqfd).gsi as u32 {
                (*irqfd).resampler = resampler;
                found = true;
                break;
            }
            head = (*head).next;
        }

        if !found {
            let resampler: *mut bindings::kvm_kernel_irqfd_resampler = unsafe {
                bindings::kzalloc(
                    core::mem::size_of::<bindings::kvm_kernel_irqfd_resampler>(),
                    bindings::GFP_KERNEL_ACCOUNT,
                )
            } as *mut _;

            if resampler.is_null() {
                // _mutex_guard dropped here
                return -(bindings::ENOMEM as c_int);
            }

            (*resampler).kvm = kvm;
            unsafe { bindings::INIT_LIST_HEAD(&raw mut (*resampler).list) };
            (*resampler).notifier.gsi = (*irqfd).gsi as u32;
            (*resampler).notifier.irq_acked = Some(irqfd_resampler_ack);
            unsafe { bindings::INIT_LIST_HEAD(&raw mut (*resampler).link) };

            unsafe {
                bindings::list_add_rcu(
                    &raw mut (*resampler).link,
                    &raw mut (*kvm).irqfds.resampler_list,
                )
            };
            unsafe { bindings::kvm_register_irq_ack_notifier(kvm, &raw mut (*resampler).notifier) };
            (*irqfd).resampler = resampler;
        }

        unsafe {
            bindings::list_add_rcu(
                &raw mut (*irqfd).resampler_link,
                &raw mut (*(*irqfd).resampler).list,
            )
        };
        unsafe { bindings::synchronize_srcu_expedited(&raw mut (*kvm).irq_srcu) };

        // _mutex_guard dropped here
    }

    // Take SRCU read lock to ensure stable routing and prevent premature freeing.
    // SAFETY: kvm->irq_srcu is valid.
    let srcu_guard = unsafe { SrcuGuard::new(&raw mut (*kvm).irq_srcu) };

    // Register irqfd with eventfd via poll mechanism.
    let mut irqfd_pt = KvmIrqfdPt {
        pt: unsafe { core::mem::zeroed() },
        irqfd,
        kvm,
        ret: 0,
    };
    unsafe { bindings::init_poll_funcptr(&raw mut irqfd_pt.pt, Some(kvm_irqfd_register)) };

    // Perform the poll to trigger registration.
    let events = unsafe { bindings::vfs_poll(bindings::fd_file(f), &raw mut irqfd_pt.pt) };

    if irqfd_pt.ret != 0 {
        drop(srcu_guard);
        return irqfd_pt.ret;
    }

    if (events as u32 & bindings::POLLIN) != 0 {
        // SAFETY: FFI call.
        unsafe { bindings::schedule_work(&raw mut (*irqfd).inject) };
    }

    #[cfg(CONFIG_HAVE_KVM_IRQ_BYPASS)]
    {
        if unsafe { bindings::kvm_arch_has_irq_bypass() } {
            (*irqfd).consumer.add_producer = Some(bindings::kvm_arch_irq_bypass_add_producer);
            (*irqfd).consumer.del_producer = Some(bindings::kvm_arch_irq_bypass_del_producer);
            (*irqfd).consumer.stop = Some(bindings::kvm_arch_irq_bypass_stop);
            (*irqfd).consumer.start = Some(bindings::kvm_arch_irq_bypass_start);
            let ret = unsafe {
                bindings::irq_bypass_register_consumer(&raw mut (*irqfd).consumer, (*irqfd).eventfd)
            };
            if ret != 0 {
                crate::pr_info!(
                    "irq bypass consumer (eventfd {:?}) registration fails: {}\n",
                    (*irqfd).eventfd,
                    ret
                );
            }
        }
    }

    resampler_guard.dismiss();
    resamplefd_guard.dismiss();
    eventfd_guard.dismiss();
    irqfd_guard.dismiss();
    _f_guard.dismiss();

    drop(srcu_guard);
    // fd is released after SRCU unlock, matching C's CLASS(fd) cleanup order.
    unsafe { bindings::fdput(f) };
    0
}

#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
unsafe fn kvm_irqfd_deassign(kvm: *mut bindings::kvm, args: *mut bindings::kvm_irqfd) -> c_int {
    let eventfd = match unsafe { eventfd_ctx_fdget((*args).fd as i32) } {
        Ok(ctx) => ctx,
        Err(e) => return e.to_errno() as c_int,
    };

    // SAFETY: kvm is valid, lock is valid. Called from syscall context (IRQs enabled),
    // use spin_lock_irq to match C kvm_irqfd_deassign.
    {
        let _spin_guard = unsafe { SpinLockIrqGuard::new(&raw mut (*kvm).irqfds.lock) };

        // Iterate and deactivate matching irqfds.
        let mut pos = (*kvm).irqfds.items.next;
        let head = &raw mut (*kvm).irqfds.items;
        while pos != head {
            let irqfd: *mut bindings::kvm_kernel_irqfd =
                crate::container_of!(pos, bindings::kvm_kernel_irqfd, list);
            let next = (*pos).next;
            if (*irqfd).eventfd == eventfd && (*irqfd).gsi == (*args).gsi as i32 {
                irqfd_deactivate(irqfd);
            }
            pos = next;
        }
        // _spin_guard drops here, unlocking before sleep-capable calls.
    }

    eventfd_ctx_put(eventfd);

    // SAFETY: Flush to guarantee no more interrupts on this gsi.
    // SAFETY: IRQFD_CLEANUP_WQ is initialized.
    unsafe {
        bindings::flush_workqueue(IRQFD_CLEANUP_WQ.load(core::sync::atomic::Ordering::Acquire))
    };

    0
}

// Lock class keys removed in favor of static_lock_class!()

// ioeventfd - Translate PIO/MMIO writes to eventfd signals

/// Rust representation of the C `struct _ioeventfd`.
/// Layout-compatible with the C version.
#[repr(C)]
struct Ioeventfd {
    list: bindings::list_head,
    addr: u64,
    length: c_int,
    eventfd: *mut bindings::eventfd_ctx,
    datamatch: u64,
    dev: bindings::kvm_io_device,
    bus_idx: u8,
    wildcard: bool,
}

impl Ioeventfd {
    fn datamatch(&self) -> Option<u64> {
        if self.wildcard {
            None
        } else {
            Some(self.datamatch)
        }
    }
}

#[used]
static IOEVENTFD_OPS: bindings::kvm_io_device_ops = bindings::kvm_io_device_ops {
    read: None,
    write: Some(ioeventfd_write),
    destructor: Some(ioeventfd_destructor),
};

#[inline(always)]
unsafe fn ioeventfd_in_range(
    p: &Ioeventfd,
    addr: bindings::gpa_t,
    len: c_int,
    val: *const c_void,
) -> bool {
    if addr != p.addr {
        return false;
    }
    if p.length == 0 {
        return true;
    }
    if len != p.length {
        return false;
    }
    if let Some(datamatch) = p.datamatch() {
        // KVM guarantees val is naturally aligned; assert to match C's BUG_ON.
        debug_assert!(
            (val as usize) % (len as usize) == 0,
            "ioeventfd_in_range: val pointer not aligned to len"
        );
        let _val: u64 = match len {
            1 => unsafe { (val as *const u8).read() as u64 },
            2 => unsafe { (val as *const u16).read() as u64 },
            4 => unsafe { (val as *const u32).read() as u64 },
            8 => unsafe { (val as *const u64).read() as u64 },
            _ => return false,
        };

        _val == datamatch
    } else {
        true
    }
}

unsafe extern "C" fn ioeventfd_write(
    _vcpu: *mut bindings::kvm_vcpu,
    this: *mut bindings::kvm_io_device,
    addr: bindings::gpa_t,
    len: c_int,
    val: *const c_void,
) -> c_int {
    let p: *const Ioeventfd = crate::container_of!(this, Ioeventfd, dev);

    if !ioeventfd_in_range(&*p, addr, len, val) {
        return -(bindings::EOPNOTSUPP as c_int);
    }

    eventfd_signal((*p).eventfd);
    0
}

unsafe extern "C" fn ioeventfd_destructor(this: *mut bindings::kvm_io_device) {
    let p: *mut Ioeventfd = crate::container_of!(this, Ioeventfd, dev);

    // SAFETY: p is valid, delete from list, release eventfd reference and free.
    unsafe { bindings::list_del(&raw mut (*p).list) };
    unsafe { eventfd_ctx_put((*p).eventfd) };
    unsafe { bindings::kfree(p as *const c_void) };
}

unsafe fn ioeventfd_check_collision(kvm: *mut bindings::kvm, p: &Ioeventfd) -> bool {
    let mut pos = (*kvm).ioeventfds.next;
    let head = &raw mut (*kvm).ioeventfds;

    while pos != head {
        let _p: *const Ioeventfd = crate::container_of!(pos, Ioeventfd, list);

        if (*_p).bus_idx == p.bus_idx
            && (*_p).addr == p.addr
            && ((*_p).length == 0
                || p.length == 0
                || ((*_p).length == p.length
                    && ((*_p).wildcard || p.wildcard || (*_p).datamatch == p.datamatch)))
        {
            return true;
        }
        pos = (*pos).next;
    }

    false
}

fn ioeventfd_bus_from_flags(flags: u32) -> bindings::kvm_bus {
    if (flags & KVM_IOEVENTFD_FLAG_PIO) != 0 {
        bindings::kvm_bus_KVM_PIO_BUS
    } else if (flags & KVM_IOEVENTFD_FLAG_VIRTIO_CCW_NOTIFY) != 0 {
        bindings::kvm_bus_KVM_VIRTIO_CCW_NOTIFY_BUS
    } else {
        bindings::kvm_bus_KVM_MMIO_BUS
    }
}

unsafe fn kvm_assign_ioeventfd_idx(
    kvm: *mut bindings::kvm,
    bus_idx: bindings::kvm_bus,
    args: *const bindings::kvm_ioeventfd,
) -> c_int {
    let eventfd = match unsafe { eventfd_ctx_fdget((*args).fd) } {
        Ok(ctx) => ctx,
        Err(e) => return e.to_errno() as c_int,
    };
    let eventfd_guard = crate::types::ScopeGuard::new(move || unsafe { eventfd_ctx_put(eventfd) });

    let p: *mut Ioeventfd = unsafe {
        bindings::kzalloc(
            core::mem::size_of::<Ioeventfd>(),
            bindings::GFP_KERNEL_ACCOUNT,
        )
    } as *mut _;

    if p.is_null() {
        return -(bindings::ENOMEM as c_int);
    }
    let p_guard =
        crate::types::ScopeGuard::new(move || unsafe { bindings::kfree(p as *const c_void) });

    unsafe { bindings::INIT_LIST_HEAD(&raw mut (*p).list) };
    (*p).addr = (*args).addr;
    (*p).bus_idx = bus_idx as u8;
    (*p).length = (*args).len as i32;
    (*p).eventfd = eventfd;

    if ((*args).flags & KVM_IOEVENTFD_FLAG_DATAMATCH) != 0 {
        (*p).datamatch = (*args).datamatch;
    } else {
        (*p).wildcard = true;
    }

    // SAFETY: kvm->slots_lock is valid.
    let _mutex_guard = unsafe { MutexGuard::new(&raw mut (*kvm).slots_lock) };

    if unsafe { ioeventfd_check_collision(kvm, &*p) } {
        // _mutex_guard dropped here
        return -(bindings::EEXIST as c_int);
    }

    // Initialize the io device.
    (*p).dev.ops = &IOEVENTFD_OPS;

    let ret = unsafe {
        bindings::kvm_io_bus_register_dev(kvm, bus_idx, (*p).addr, (*p).length, &raw mut (*p).dev)
    };
    if ret < 0 {
        // _mutex_guard dropped here
        return ret;
    }

    let bus = unsafe { bindings::kvm_get_bus(kvm, bus_idx) };
    if !bus.is_null() {
        unsafe { (*bus).ioeventfd_count += 1 };
    }
    unsafe { bindings::list_add_tail(&raw mut (*p).list, &raw mut (*kvm).ioeventfds) };

    // _mutex_guard dropped here

    p_guard.dismiss();
    eventfd_guard.dismiss();

    0
}

unsafe fn kvm_deassign_ioeventfd_idx(
    kvm: *mut bindings::kvm,
    bus_idx: bindings::kvm_bus,
    args: *const bindings::kvm_ioeventfd,
) -> c_int {
    let eventfd = match unsafe { eventfd_ctx_fdget((*args).fd) } {
        Ok(ctx) => ctx,
        Err(e) => return e.to_errno() as c_int,
    };

    let wildcard = ((*args).flags & KVM_IOEVENTFD_FLAG_DATAMATCH) == 0;

    let mut ret: c_int = -(bindings::ENOENT as c_int);

    {
        // SAFETY: kvm->slots_lock is valid.
        let _mutex_guard = unsafe { MutexGuard::new(&raw mut (*kvm).slots_lock) };

        let mut pos = (*kvm).ioeventfds.next;
        let head = &raw mut (*kvm).ioeventfds;

        while pos != head {
            let p: *mut Ioeventfd = crate::container_of!(pos, Ioeventfd, list);
            let next = (*pos).next;

            if (*p).bus_idx != bus_idx as u8
                || (*p).eventfd != eventfd
                || (*p).addr != (*args).addr
                || (*p).length != (*args).len as i32
                || (*p).wildcard != wildcard
            {
                pos = next;
                continue;
            }

            if !(*p).wildcard && (*p).datamatch != (*args).datamatch {
                pos = next;
                continue;
            }

            // SAFETY: kvm_io_bus_unregister_dev will trigger the iodevice destructor,
            // which implicitly performs list_del and kfree(p). Using `next` above avoids UAF.
            unsafe { bindings::kvm_io_bus_unregister_dev(kvm, bus_idx, &raw mut (*p).dev) };
            let bus = unsafe { bindings::kvm_get_bus(kvm, bus_idx) };
            if !bus.is_null() {
                unsafe { (*bus).ioeventfd_count -= 1 };
            }
            ret = 0;
            break;
        }
        // _mutex_guard drops here, unlocking before eventfd_ctx_put.
    }

    eventfd_ctx_put(eventfd);

    ret
}

unsafe fn kvm_deassign_ioeventfd(
    kvm: *mut bindings::kvm,
    args: *const bindings::kvm_ioeventfd,
) -> c_int {
    let bus_idx = ioeventfd_bus_from_flags((*args).flags);
    let ret = unsafe { kvm_deassign_ioeventfd_idx(kvm, bus_idx, args) };

    if (*args).len == 0 && bus_idx == bindings::kvm_bus_KVM_MMIO_BUS {
        unsafe { kvm_deassign_ioeventfd_idx(kvm, bindings::kvm_bus_KVM_FAST_MMIO_BUS, args) };
    }

    ret
}

unsafe fn kvm_assign_ioeventfd(
    kvm: *mut bindings::kvm,
    args: *const bindings::kvm_ioeventfd,
) -> c_int {
    let bus_idx = ioeventfd_bus_from_flags((*args).flags);

    match (*args).len {
        0 | 1 | 2 | 4 | 8 => {}
        _ => return -(bindings::EINVAL as c_int),
    }

    // Check for range overflow.
    if (*args).addr.wrapping_add((*args).len as u64) < (*args).addr {
        return -(bindings::EINVAL as c_int);
    }

    // Check for unknown flags.
    // Use the kernel's valid flag mask to stay in sync with UAPI header.
    if (*args).flags & !KVM_IOEVENTFD_VALID_FLAG_MASK != 0 {
        return -(bindings::EINVAL as c_int);
    }

    // No length can't be combined with DATAMATCH.
    if (*args).len == 0 && ((*args).flags & KVM_IOEVENTFD_FLAG_DATAMATCH) != 0 {
        return -(bindings::EINVAL as c_int);
    }

    let ret = unsafe { kvm_assign_ioeventfd_idx(kvm, bus_idx, args) };
    if ret != 0 {
        return ret;
    }

    // Also register on fast MMIO bus for zero-length ioeventfd.
    if (*args).len == 0 && bus_idx == bindings::kvm_bus_KVM_MMIO_BUS {
        let fast_ret =
            unsafe { kvm_assign_ioeventfd_idx(kvm, bindings::kvm_bus_KVM_FAST_MMIO_BUS, args) };
        if fast_ret < 0 {
            unsafe { kvm_deassign_ioeventfd_idx(kvm, bus_idx, args) };
            return fast_ret;
        }
    }

    0
}

// Exported C API - these are the public symbols called from KVM core

/// Main entry point for `KVM_IRQFD` ioctl.
#[no_mangle]
pub unsafe extern "C" fn kvm_irqfd(
    kvm: *mut bindings::kvm,
    args: *mut bindings::kvm_irqfd,
) -> c_int {
    #[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
    {
        if (*args).flags & !(bindings::KVM_IRQFD_FLAG_DEASSIGN | bindings::KVM_IRQFD_FLAG_RESAMPLE)
            != 0
        {
            return -(bindings::EINVAL as c_int);
        }

        if (*args).flags & bindings::KVM_IRQFD_FLAG_DEASSIGN != 0 {
            return kvm_irqfd_deassign(kvm, args);
        }

        kvm_irqfd_assign(kvm, args)
    }

    #[cfg(not(CONFIG_HAVE_KVM_IRQCHIP))]
    {
        let _ = (kvm, args);
        -(bindings::EINVAL as c_int)
    }
}

/// Main entry point for `KVM_IOEVENTFD` ioctl.
#[no_mangle]
pub unsafe extern "C" fn kvm_ioeventfd(
    kvm: *mut bindings::kvm,
    args: *mut bindings::kvm_ioeventfd,
) -> c_int {
    if (*args).flags & KVM_IOEVENTFD_FLAG_DEASSIGN != 0 {
        kvm_deassign_ioeventfd(kvm, args)
    } else {
        kvm_assign_ioeventfd(kvm, args)
    }
}

/// Initialize eventfd state for a KVM VM.
#[no_mangle]
pub unsafe extern "C" fn kvm_eventfd_init(kvm: *mut bindings::kvm) {
    #[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
    {
        unsafe {
            crate::bindings::__spin_lock_init(
                &raw mut (*kvm).irqfds.lock,
                b"irqfds.lock\0".as_ptr() as *const _,
                crate::sync::static_lock_class!().as_ptr() as *mut _,
            )
        };
        unsafe { bindings::INIT_LIST_HEAD(&raw mut (*kvm).irqfds.items) };
        unsafe { bindings::INIT_LIST_HEAD(&raw mut (*kvm).irqfds.resampler_list) };
        unsafe {
            crate::bindings::__mutex_init(
                &raw mut (*kvm).irqfds.resampler_lock,
                b"irqfds.resampler_lock\0".as_ptr() as *const _,
                crate::sync::static_lock_class!().as_ptr() as *mut _,
            )
        };
    }

    unsafe { bindings::INIT_LIST_HEAD(&raw mut (*kvm).ioeventfds) };
}

/// Release all irqfds on VM exit.
#[no_mangle]
#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
pub unsafe extern "C" fn kvm_irqfd_release(kvm: *mut bindings::kvm) {
    {
        // SAFETY: Caller guarantees kvm is valid.
        let _spin_guard = unsafe { SpinLockIrqGuard::new(&raw mut (*kvm).irqfds.lock) };

        let mut pos = (*kvm).irqfds.items.next;
        let head = &raw mut (*kvm).irqfds.items;
        while pos != head {
            let irqfd: *mut bindings::kvm_kernel_irqfd =
                crate::container_of!(pos, bindings::kvm_kernel_irqfd, list);
            let next = (*pos).next;
            irqfd_deactivate(irqfd);
            pos = next;
        }
        // _spin_guard drops here, unlocking before flush_workqueue.
    }

    // SAFETY: IRQFD_CLEANUP_WQ is initialized.
    unsafe {
        bindings::flush_workqueue(IRQFD_CLEANUP_WQ.load(core::sync::atomic::Ordering::Acquire))
    };
}

/// Update irq routing for all irqfds.
#[no_mangle]
#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
pub unsafe extern "C" fn kvm_irq_routing_update(kvm: *mut bindings::kvm) {
    // SAFETY: Caller guarantees kvm is valid.
    let _spin_guard = unsafe { SpinLockIrqGuard::new(&raw mut (*kvm).irqfds.lock) };

    let mut pos = (*kvm).irqfds.items.next;
    let head = &raw mut (*kvm).irqfds.items;
    while pos != head {
        let irqfd: *mut bindings::kvm_kernel_irqfd =
            crate::container_of!(pos, bindings::kvm_kernel_irqfd, list);

        #[cfg(CONFIG_HAVE_KVM_IRQ_BYPASS)]
        {
            let mut old = unsafe { core::ptr::read(&(*irqfd).irq_entry) };
            irqfd_update(kvm, irqfd);
            if !(*irqfd).producer.is_null() {
                unsafe {
                    bindings::kvm_arch_update_irqfd_routing(
                        irqfd,
                        &raw mut old,
                        &raw mut (*irqfd).irq_entry,
                    )
                };
            }
        }

        #[cfg(not(CONFIG_HAVE_KVM_IRQ_BYPASS))]
        {
            irqfd_update(kvm, irqfd);
        }

        pos = (*pos).next;
    }

    // _spin_guard dropped here
}

/// Notify irqfd resamplers for a given irqchip/pin.
#[no_mangle]
#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
pub unsafe extern "C" fn kvm_notify_irqfd_resampler(
    kvm: *mut bindings::kvm,
    irqchip: c_uint,
    pin: c_uint,
) -> bool {
    // SAFETY: kvm->irq_srcu is valid.
    let _srcu_guard = unsafe { SrcuGuard::new(&raw mut (*kvm).irq_srcu) };
    let gsi = unsafe { bindings::kvm_irq_map_chip_pin(kvm, irqchip, pin) };

    if gsi != -1 {
        let mut pos = (*kvm).irqfds.resampler_list.next;
        let head = &raw mut (*kvm).irqfds.resampler_list;
        while pos != head {
            let resampler: *mut bindings::kvm_kernel_irqfd_resampler =
                crate::container_of!(pos, bindings::kvm_kernel_irqfd_resampler, link);
            if (*resampler).notifier.gsi == gsi as u32 {
                irqfd_resampler_notify(resampler);
                // _srcu_guard dropped on return
                return true;
            }
            pos = (*pos).next;
        }
    }
    // _srcu_guard dropped here

    false
}

/// Check if an IRQ has a notifier.
#[no_mangle]
pub unsafe extern "C" fn kvm_irq_has_notifier(
    kvm: *mut bindings::kvm,
    irqchip: c_uint,
    pin: c_uint,
) -> bool {
    // SAFETY: kvm->irq_srcu is valid.
    let _srcu_guard = unsafe { SrcuGuard::new(&raw mut (*kvm).irq_srcu) };
    let gsi = unsafe { bindings::kvm_irq_map_chip_pin(kvm, irqchip, pin) };

    if gsi != -1 {
        // SAFETY: SRCU read lock held; use read_volatile for RCU-safe hlist traversal
        // (equivalent to hlist_for_each_entry_srcu / hlist_first_rcu / hlist_next_rcu).
        let mut node =
        unsafe { rcu_dereference_raw(&raw const (*kvm).irq_ack_notifier_list.first) };
        while !node.is_null() {
            let kian: *mut bindings::kvm_irq_ack_notifier =
                crate::container_of!(node, bindings::kvm_irq_ack_notifier, link);
            if (*kian).gsi == gsi as u32 {
                // _srcu_guard dropped on return
                return true;
            }
            node = unsafe { rcu_dereference_raw(&raw const (*node).next) };
        }
    }

    // _srcu_guard dropped here
    false
}

/// Notify that a GSI has been acknowledged.
#[no_mangle]
pub unsafe extern "C" fn kvm_notify_acked_gsi(kvm: *mut bindings::kvm, gsi: c_int) {
    // SAFETY: Caller holds SRCU read lock on kvm->irq_srcu. Use read_volatile for
    // RCU-safe hlist traversal (equivalent to hlist_for_each_entry_srcu).
    let mut node = unsafe { rcu_dereference_raw(&raw const (*kvm).irq_ack_notifier_list.first) };
    while !node.is_null() {
        let kian: *mut bindings::kvm_irq_ack_notifier =
            crate::container_of!(node, bindings::kvm_irq_ack_notifier, link);
        if (*kian).gsi == gsi as u32 {
            if let Some(acked) = (*kian).irq_acked {
                unsafe { acked(kian) };
            }
        }
        node = unsafe { rcu_dereference_raw(&raw const (*node).next) };
    }
}

/// Notify that an IRQ has been acknowledged (irqchip/pin -> GSI lookup).
#[no_mangle]
pub unsafe extern "C" fn kvm_notify_acked_irq(
    kvm: *mut bindings::kvm,
    irqchip: c_uint,
    pin: c_uint,
) {
    // SAFETY: Calling C tracepoint helper is safe.
    unsafe { bindings::rust_helper_trace_kvm_ack_irq(irqchip, pin) };
    // SAFETY: `kvm` is valid, and srcu read lock ensures routing table is protected.
    // SAFETY: kvm->irq_srcu is valid.
    let _srcu_guard = unsafe { SrcuGuard::new(&raw mut (*kvm).irq_srcu) };
    // SAFETY: `kvm` is valid, lookup does not escape.
    let gsi = unsafe { bindings::kvm_irq_map_chip_pin(kvm, irqchip, pin) };
    if gsi != -1 {
        kvm_notify_acked_gsi(kvm, gsi);
    }
    // SAFETY: Releases the srcu read lock acquired above.
    // _srcu_guard dropped here
}

/// Register an IRQ ack notifier.
#[no_mangle]
pub unsafe extern "C" fn kvm_register_irq_ack_notifier(
    kvm: *mut bindings::kvm,
    kian: *mut bindings::kvm_irq_ack_notifier,
) {
    // SAFETY: Caller guarantees kvm is valid.
    let _mutex_guard = unsafe { MutexGuard::new(&raw mut (*kvm).irq_lock) };
    unsafe {
        bindings::rust_helper_hlist_add_head_rcu(
            &raw mut (*kian).link,
            &raw mut (*kvm).irq_ack_notifier_list,
        )
    };
    // _mutex_guard dropped here
    unsafe { bindings::kvm_arch_post_irq_ack_notifier_list_update(kvm) };
}

/// Unregister an IRQ ack notifier.
#[no_mangle]
pub unsafe extern "C" fn kvm_unregister_irq_ack_notifier(
    kvm: *mut bindings::kvm,
    kian: *mut bindings::kvm_irq_ack_notifier,
) {
    // SAFETY: Caller guarantees kvm is valid.
    let _mutex_guard = unsafe { MutexGuard::new(&raw mut (*kvm).irq_lock) };
    unsafe { bindings::rust_helper_hlist_del_init_rcu(&raw mut (*kian).link) };
    // _mutex_guard dropped here
    unsafe { bindings::synchronize_srcu_expedited(&raw mut (*kvm).irq_srcu) };
    unsafe { bindings::kvm_arch_post_irq_ack_notifier_list_update(kvm) };
}

/// Global init: allocate the host-wide irqfd cleanup workqueue.
#[no_mangle]
#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
pub unsafe extern "C" fn kvm_irqfd_init() -> c_int {
    // SAFETY: alloc_workqueue creates a workqueue.
    let wq = unsafe {
        bindings::rust_helper_alloc_workqueue(
            b"kvm-irqfd-cleanup\0".as_ptr() as *const _,
            WQ_PERCPU,
            0,
        )
    };
    if wq.is_null() {
        return -(bindings::ENOMEM as c_int);
    }
    // SAFETY: Written once during init, read afterwards.
    IRQFD_CLEANUP_WQ.store(wq, core::sync::atomic::Ordering::Release);
    0
}

/// Global exit: destroy the irqfd cleanup workqueue.
#[no_mangle]
#[cfg(CONFIG_HAVE_KVM_IRQCHIP)]
pub unsafe extern "C" fn kvm_irqfd_exit() {
    // SAFETY: IRQFD_CLEANUP_WQ was initialized in kvm_irqfd_init.
    unsafe {
        bindings::destroy_workqueue(IRQFD_CLEANUP_WQ.load(core::sync::atomic::Ordering::Acquire))
    };
}
