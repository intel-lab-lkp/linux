// SPDX-License-Identifier: GPL-2.0

//! Rust prcl sample.

use core::ptr::null_mut;
use kernel::bindings;
use kernel::damon;
use kernel::debugfs::Scope;
use kernel::prelude::*;
use kernel::sync::atomic::{Atomic, Relaxed};
use kernel::uaccess::UserSliceReader;

module! {
    type: RustPrcl,
    name: "rust_prcl",
    authors: ["Enze Li <lienze@kylinos.cn>"],
    description: "DAMON-based proactive reclamation module for rust edition",
    license: "GPL",
}

struct RustPrcl {
    _data: Pin<KBox<Scope<ModuleData>>>,
}

struct ModuleData {
    enabled: Atomic<bool>,
    target_pid: Atomic<i32>,
}

impl Drop for ModuleData {
    fn drop(&mut self) {
        if self.enabled.load(Relaxed) {
            damon_sample_rust_prcl_stop();
        }
    }
}

static CTX: Atomic<*mut bindings::damon_ctx> = Atomic::new(null_mut());

fn damon_sample_rust_prcl_start(target_pid: i32) -> Result {
    let ctx = damon::DamonCtx::damon_new_ctx()?;
    ctx.damon_select_ops(damon::OpsID::VADDR)?;

    let mut target = damon::Target::damon_new_target()?;
    target.damon_set_target_pid(target_pid)?;
    ctx.damon_add_target(target);

    let mut pattern =
        damon::DamosAccessPattern::new(bindings::PAGE_SIZE, usize::MAX, 0, 0, 50, u32::MAX);

    let mut quota = damon::DamosQuota::zeroed();
    let mut watermarks = damon::DamosWatermarks::zeroed();
    let scheme = damon::Damos::new(
        &mut pattern,
        damon::DamosAction::PAGEOUT,
        0,
        &mut quota,
        &mut watermarks,
        bindings::NUMA_NO_NODE,
    )?;

    ctx.damon_add_scheme(scheme);

    let ret = ctx.damon_start();
    match ret {
        Ok(_) => {
            CTX.store(ctx.into_raw(), Relaxed);
        }
        Err(e) => {
            pr_err!("damon_start failed, error: {:?}\n", e);
            return Err(e);
        }
    }
    Ok(())
}

fn damon_sample_rust_prcl_stop() {
    pr_info!("stop\n");
    let raw = CTX.xchg(null_mut(), Relaxed);
    if !raw.is_null() {
        // SAFETY: raw came from into_raw() at damon_start(), and xchg()
        // makes its sole remaining owner, so moving it back once is safe.
        let _ctx = unsafe { damon::DamonCtx::from_raw(raw) };
        // _ctx drops at the end of this scope, it runs damon_stop(1) and
        // damon_destroy_ctx().
    }
}

fn damon_sample_rust_parse_bool(s: &str) -> Result<bool> {
    let s = s.trim();
    if s.eq_ignore_ascii_case("y") {
        Ok(true)
    } else if s.eq_ignore_ascii_case("n") {
        Ok(false)
    } else {
        Err(EINVAL)
    }
}

fn enabled_read(data: &ModuleData, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
    writeln!(f, "{}", if data.enabled.load(Relaxed) { 'Y' } else { 'N' })
}

fn enabled_write(data: &ModuleData, reader: &mut UserSliceReader) -> Result {
    let mut buf = [0u8; 32];
    if reader.len() > buf.len() {
        return Err(EINVAL);
    }
    let n = reader.len();
    reader.read_slice(&mut buf[..n])?;

    let s = core::str::from_utf8(&buf[..n]).map_err(|_| EINVAL)?;
    let enabled = damon_sample_rust_parse_bool(s)?;

    let is_enabled = data.enabled.load(Relaxed);
    if enabled == is_enabled {
        return Ok(());
    }

    if !damon::damon_initialized() {
        return Ok(());
    }

    data.enabled.store(enabled, Relaxed);

    if enabled {
        let target_pid = data.target_pid.load(Relaxed);
        if let Err(e) = damon_sample_rust_prcl_start(target_pid) {
            data.enabled.store(false, Relaxed);
            return Err(e);
        }
    } else {
        damon_sample_rust_prcl_stop();
    }

    Ok(())
}

impl kernel::Module for RustPrcl {
    fn init(_module: &'static kernel::ThisModule) -> Result<Self> {
        let data = KBox::pin_init(
            Scope::dir(
                ModuleData {
                    enabled: Atomic::new(false),
                    target_pid: Atomic::new(0),
                },
                c"rust_prcl",
                |data, dir| {
                    dir.read_write_callback_file(c"enabled", data, &enabled_read, &enabled_write);
                    dir.read_write_file(c"target_pid", &data.target_pid);
                },
            ),
            GFP_KERNEL,
        )?;

        Ok(Self { _data: data })
    }
}
