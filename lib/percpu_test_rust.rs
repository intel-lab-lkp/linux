// SPDX-License-Identifier: GPL-2.0
//! A simple self test for the rust per-CPU API.

use core::ffi::c_void;

use kernel::{
    bindings::{on_each_cpu, smp_processor_id},
    define_per_cpu,
    percpu::{cpu_guard::*, *},
    pr_info,
    prelude::*,
    unsafe_get_per_cpu,
};

module! {
    type: PerCpuTestModule,
    name: "percpu_test_rust",
    author: "Mitchell Levy",
    description: "Test code to exercise the Rust Per CPU variable API",
    license: "GPL v2",
}

struct PerCpuTestModule;

define_per_cpu!(PERCPU: i64 = 0);
define_per_cpu!(UPERCPU: u64 = 0);

macro_rules! make_optimization_test {
    ($ty:ty) => {
        let mut test: DynamicPerCpu<$ty> = DynamicPerCpu::new(GFP_KERNEL).unwrap();
        {
            let _ = CpuGuard::new();
            // SAFETY: No other usage of `test`
            unsafe { test.get(CpuGuard::new()) }.with(|val: &mut $ty| *val = 10);
            test.num().add(1);
            // SAFETY: No other usage of `test`
            unsafe { test.get(CpuGuard::new()) }.with(|val: &mut $ty| assert_eq!(*val, 11));
            test.num().add(10);
            // SAFETY: No other usage of `test`
            unsafe { test.get(CpuGuard::new()) }.with(|val: &mut $ty| assert_eq!(*val, 21));
            test.num().sub(5);
            // SAFETY: No other usage of `test`
            unsafe { test.get(CpuGuard::new()) }.with(|val: &mut $ty| assert_eq!(*val, 16));
        }
    };
}

impl kernel::Module for PerCpuTestModule {
    fn init(_module: &'static ThisModule) -> Result<Self, Error> {
        pr_info!("rust percpu test start\n");

        let mut native: i64 = 0;
        // SAFETY: PERCPU is properly defined
        let mut pcpu: StaticPerCpu<i64> = unsafe { unsafe_get_per_cpu!(PERCPU) };
        // SAFETY: We only have one PerCpu that points at PERCPU
        unsafe { pcpu.get(CpuGuard::new()) }.with(|val: &mut i64| {
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
        // SAFETY: UPERCPU is properly defined
        let mut upcpu: StaticPerCpu<u64> = unsafe { unsafe_get_per_cpu!(UPERCPU) };

        // SAFETY: We only have one PerCpu pointing at UPERCPU
        unsafe { upcpu.get(CpuGuard::new()) }.with(|val: &mut u64| {
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

        pr_info!("rust static percpu test done\n");

        pr_info!("rust dynamic percpu test start\n");
        let mut test: DynamicPerCpu<u64> = DynamicPerCpu::new(GFP_KERNEL).unwrap();

        // SAFETY: No prerequisites for on_each_cpu.
        unsafe {
            on_each_cpu(Some(inc_percpu), (&raw mut test) as *mut c_void, 0);
            on_each_cpu(Some(inc_percpu), (&raw mut test) as *mut c_void, 0);
            on_each_cpu(Some(inc_percpu), (&raw mut test) as *mut c_void, 0);
            on_each_cpu(Some(inc_percpu), (&raw mut test) as *mut c_void, 1);
            on_each_cpu(Some(check_percpu), (&raw mut test) as *mut c_void, 1);
        }

        pr_info!("rust dynamic percpu test done\n");

        pr_info!("rust numeric optimizations test start\n");

        make_optimization_test!(u8);
        make_optimization_test!(u16);
        make_optimization_test!(u32);
        make_optimization_test!(u64);
        make_optimization_test!(usize);

        make_optimization_test!(i8);
        make_optimization_test!(i16);
        make_optimization_test!(i32);
        make_optimization_test!(i64);
        make_optimization_test!(isize);

        pr_info!("rust numeric optimizations test done\n");

        // Return Err to unload the module
        Result::Err(EINVAL)
    }
}

extern "C" fn inc_percpu(info: *mut c_void) {
    // SAFETY: We know that info is a void *const DynamicPerCpu<u64> and DynamicPerCpu<u64> is Send.
    let mut pcpu = unsafe { (*(info as *const DynamicPerCpu<u64>)).clone() };
    // SAFETY: smp_processor_id has no preconditions
    pr_info!("Incrementing on {}\n", unsafe { smp_processor_id() });

    // SAFETY: We don't have multiple clones of pcpu in scope
    unsafe { pcpu.get(CpuGuard::new()) }.with(|val: &mut u64| *val += 1);
}

extern "C" fn check_percpu(info: *mut c_void) {
    // SAFETY: We know that info is a void *const DynamicPerCpu<u64> and DynamicPerCpu<u64> is Send.
    let mut pcpu = unsafe { (*(info as *const DynamicPerCpu<u64>)).clone() };
    // SAFETY: smp_processor_id has no preconditions
    pr_info!("Asserting on {}\n", unsafe { smp_processor_id() });

    // SAFETY: We don't have multiple clones of pcpu in scope
    unsafe { pcpu.get(CpuGuard::new()) }.with(|val: &mut u64| assert!(*val == 4));
}
