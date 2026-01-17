// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Google LLC.

use kernel::{
    alloc::kvec::{KVVec, KVec},
    error::code::*,
    prelude::*,
    security,
    str::{CStr, CString},
    sync::{Arc, Mutex},
    task::Kuid,
};

use crate::{error::BinderError, node::NodeRef, process::Process};

kernel::sync::global_lock! {
    // SAFETY: We call `init` in the module initializer, so it's initialized before first use.
    pub(crate) unsafe(uninit) static CONTEXTS: Mutex<ContextList> = ContextList {
        contexts: KVVec::new(),
    };
}

pub(crate) struct ContextList {
    contexts: KVVec<Arc<Context>>,
}

pub(crate) fn get_all_contexts() -> Result<KVec<Arc<Context>>> {
    let lock = CONTEXTS.lock();
    let mut ctxs = KVec::with_capacity(lock.contexts.len(), GFP_KERNEL)?;
    for ctx in lock.contexts.iter() {
        ctxs.push(ctx.clone(), GFP_KERNEL)?;
    }
    Ok(ctxs)
}

/// This struct keeps track of the processes using this context, and which process is the context
/// manager.
struct Manager {
    node: Option<NodeRef>,
    uid: Option<Kuid>,
    all_procs: KVVec<Arc<Process>>,
}

/// There is one context per binder file (/dev/binder, /dev/hwbinder, etc)
#[pin_data]
pub(crate) struct Context {
    #[pin]
    manager: Mutex<Manager>,
    pub(crate) name: CString,
}

impl Context {
    pub(crate) fn new(name: &CStr) -> Result<Arc<Self>> {
        let name = CString::try_from(name)?;
        let ctx = Arc::pin_init(
            try_pin_init!(Context {
                name,
                manager <- kernel::new_mutex!(Manager {
                    all_procs: KVVec::new(),
                    node: None,
                    uid: None,
                }, "Context::manager"),
            }),
            GFP_KERNEL,
        )?;

        CONTEXTS.lock().contexts.push(ctx.clone(), GFP_KERNEL)?;

        Ok(ctx)
    }

    /// Called when the file for this context is unlinked.
    ///
    /// No-op if called twice.
    pub(crate) fn deregister(&self) {
        // Safe removal using retain
        CONTEXTS.lock().contexts.retain(|c| {
            let p1 = Arc::as_ptr(c);
            let p2 = self as *const Context;
            p1 != p2
        });
    }

    pub(crate) fn register_process(self: &Arc<Self>, proc: Arc<Process>) -> Result {
        if !Arc::ptr_eq(self, &proc.ctx) {
            pr_err!("Context::register_process called on the wrong context.");
            return Err(EINVAL);
        }
        self.manager
            .lock()
            .all_procs
            .push(proc, GFP_KERNEL)
            .map_err(Error::from)
    }

    pub(crate) fn deregister_process(self: &Arc<Self>, proc: &Process) {
        if !Arc::ptr_eq(self, &proc.ctx) {
            pr_err!("Context::deregister_process called on the wrong context.");
            return;
        }
        // Safe removal using retain
        self.manager.lock().all_procs.retain(|p| {
            let p1 = Arc::as_ptr(p);
            let p2 = proc as *const Process;
            p1 != p2
        });
    }

    pub(crate) fn set_manager_node(&self, node_ref: NodeRef) -> Result {
        let mut manager = self.manager.lock();
        if manager.node.is_some() {
            pr_warn!("BINDER_SET_CONTEXT_MGR already set");
            return Err(EBUSY);
        }
        security::binder_set_context_mgr(&node_ref.node.owner.cred)?;

        // If the context manager has been set before, ensure that we use the same euid.
        let caller_uid = Kuid::current_euid();
        if let Some(ref uid) = manager.uid {
            if *uid != caller_uid {
                return Err(EPERM);
            }
        }

        manager.node = Some(node_ref);
        manager.uid = Some(caller_uid);
        Ok(())
    }

    pub(crate) fn unset_manager_node(&self) {
        let node_ref = self.manager.lock().node.take();
        drop(node_ref);
    }

    pub(crate) fn get_manager_node(&self, strong: bool) -> Result<NodeRef, BinderError> {
        self.manager
            .lock()
            .node
            .as_ref()
            .ok_or_else(BinderError::new_dead)?
            .clone(strong)
            .map_err(BinderError::from)
    }

    pub(crate) fn for_each_proc<F>(&self, mut func: F)
    where
        F: FnMut(&Process),
    {
        let lock = self.manager.lock();
        for proc in &lock.all_procs {
            func(&proc);
        }
    }

    pub(crate) fn get_all_procs(&self) -> Result<KVec<Arc<Process>>> {
        let lock = self.manager.lock();
        let mut procs = KVec::with_capacity(lock.all_procs.len(), GFP_KERNEL)?;
        for proc in lock.all_procs.iter() {
            procs.push(Arc::clone(proc), GFP_KERNEL)?;
        }
        Ok(procs)
    }

    pub(crate) fn get_procs_with_pid(&self, pid: i32) -> Result<KVec<Arc<Process>>> {
        let orig = self.get_all_procs()?;
        let mut backing = KVec::with_capacity(orig.len(), GFP_KERNEL)?;
        for proc in orig.into_iter().filter(|proc| proc.task.pid() == pid) {
            backing.push(proc, GFP_KERNEL)?;
        }
        Ok(backing)
    }
}
