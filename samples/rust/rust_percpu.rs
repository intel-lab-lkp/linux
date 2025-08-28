// SPDX-License-Identifier: GPL-2.0
//! A simple demonstration of the rust per-CPU API.

use core::cell::RefCell;
use core::ffi::c_void;

use kernel::{
    bindings::on_each_cpu,
    cpu::CpuId,
    define_per_cpu, get_static_per_cpu,
    percpu::{cpu_guard::*, *},
    pr_info,
    prelude::*,
};

module! {
    type: PerCpuMod,
    name: "rust_percpu",
    authors: ["Mitchell Levy"],
    description: "Sample to demonstrate the Rust per-CPU API",
    license: "GPL v2",
}

struct PerCpuMod;

define_per_cpu!(PERCPU: i64 = 0);
define_per_cpu!(UPERCPU: u64 = 0);
define_per_cpu!(CHECKED: RefCell<u64> = RefCell::new(0));

impl kernel::Module for PerCpuMod {
    fn init(_module: &'static ThisModule) -> Result<Self, Error> {
        pr_info!("rust percpu test start\n");

        let mut native: i64 = 0;
        let mut pcpu: StaticPerCpu<i64> = get_static_per_cpu!(PERCPU);

        // SAFETY: We only have one PerCpu that points at PERCPU
        unsafe { pcpu.get_mut(CpuGuard::new()) }.with(|val: &mut i64| {
            pr_info!("The contents of pcpu are {}\n", val);

            native += -1;
            *val += -1;
            pr_info!("Native: {}, *pcpu: {}\n", native, val);
            assert!(native == *val && native == -1);

            native += 1;
            *val += 1;
            pr_info!("Native: {}, *pcpu: {}\n", native, val);
            assert!(native == *val && native == 0);
        });

        let mut unative: u64 = 0;
        let mut upcpu: StaticPerCpu<u64> = get_static_per_cpu!(UPERCPU);

        // SAFETY: We only have one PerCpu pointing at UPERCPU
        unsafe { upcpu.get_mut(CpuGuard::new()) }.with(|val: &mut u64| {
            unative += 1;
            *val += 1;
            pr_info!("Unative: {}, *upcpu: {}\n", unative, val);
            assert!(unative == *val && unative == 1);

            unative = unative.wrapping_add((-1i64) as u64);
            *val = val.wrapping_add((-1i64) as u64);
            pr_info!("Unative: {}, *upcpu: {}\n", unative, val);
            assert!(unative == *val && unative == 0);

            unative = unative.wrapping_add((-1i64) as u64);
            *val = val.wrapping_add((-1i64) as u64);
            pr_info!("Unative: {}, *upcpu: {}\n", unative, val);
            assert!(unative == *val && unative == (-1i64) as u64);

            unative = 0;
            *val = 0;

            unative = unative.wrapping_sub(1);
            *val = val.wrapping_sub(1);
            pr_info!("Unative: {}, *upcpu: {}\n", unative, val);
            assert!(unative == *val && unative == (-1i64) as u64);
            assert!(unative == *val && unative == u64::MAX);
        });

        let mut checked_native: u64 = 0;
        let mut checked: StaticPerCpu<RefCell<u64>> = get_static_per_cpu!(CHECKED);
        checked.get(CpuGuard::new()).with(|val: &RefCell<u64>| {
            checked_native += 1;
            *val.borrow_mut() += 1;
            pr_info!(
                "Checked native: {}, *checked: {}\n",
                checked_native,
                val.borrow()
            );
            assert!(checked_native == *val.borrow() && checked_native == 1);

            checked_native = checked_native.wrapping_add((-1i64) as u64);
            val.replace_with(|old: &mut u64| old.wrapping_add((-1i64) as u64));
            pr_info!(
                "Checked native: {}, *checked: {}\n",
                checked_native,
                val.borrow()
            );
            assert!(checked_native == *val.borrow() && checked_native == 0);

            checked_native = checked_native.wrapping_add((-1i64) as u64);
            val.replace_with(|old: &mut u64| old.wrapping_add((-1i64) as u64));
            pr_info!(
                "Checked native: {}, *checked: {}\n",
                checked_native,
                val.borrow()
            );
            assert!(checked_native == *val.borrow() && checked_native == (-1i64) as u64);

            checked_native = 0;
            *val.borrow_mut() = 0;

            checked_native = checked_native.wrapping_sub(1);
            val.replace_with(|old: &mut u64| old.wrapping_sub(1));
            pr_info!(
                "Checked native: {}, *checked: {}\n",
                checked_native,
                val.borrow()
            );
            assert!(checked_native == *val.borrow() && checked_native == (-1i64) as u64);
            assert!(checked_native == *val.borrow() && checked_native == u64::MAX);
        });

        pr_info!("rust static percpu test done\n");

        pr_info!("rust dynamic percpu test start\n");
        let mut test: DynamicPerCpu<u64> = DynamicPerCpu::new_zero(GFP_KERNEL).unwrap();

        // SAFETY: No prerequisites for on_each_cpu.
        unsafe {
            on_each_cpu(Some(inc_percpu), (&raw mut test).cast(), 0);
            on_each_cpu(Some(inc_percpu), (&raw mut test).cast(), 0);
            on_each_cpu(Some(inc_percpu), (&raw mut test).cast(), 0);
            on_each_cpu(Some(inc_percpu), (&raw mut test).cast(), 1);
            on_each_cpu(Some(check_percpu), (&raw mut test).cast(), 1);
        }

        pr_info!("rust dynamic percpu test done\n");

        // Return Err to unload the module
        Result::Err(EINVAL)
    }
}

extern "C" fn inc_percpu(info: *mut c_void) {
    // SAFETY: We know that info is a void *const DynamicPerCpu<u64> and DynamicPerCpu<u64> is Send.
    let mut pcpu = unsafe { (*(info as *const DynamicPerCpu<u64>)).clone() };
    pr_info!("Incrementing on {}\n", CpuId::current().as_u32());

    // SAFETY: We don't have multiple clones of pcpu in scope
    unsafe { pcpu.get_mut(CpuGuard::new()) }.with(|val: &mut u64| *val += 1);
}

extern "C" fn check_percpu(info: *mut c_void) {
    // SAFETY: We know that info is a void *const DynamicPerCpu<u64> and DynamicPerCpu<u64> is Send.
    let mut pcpu = unsafe { (*(info as *const DynamicPerCpu<u64>)).clone() };
    pr_info!("Asserting on {}\n", CpuId::current().as_u32());

    // SAFETY: We don't have multiple clones of pcpu in scope
    unsafe { pcpu.get_mut(CpuGuard::new()) }.with(|val: &mut u64| assert!(*val == 4));
}
