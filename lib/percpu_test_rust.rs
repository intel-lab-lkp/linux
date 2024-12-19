// SPDX-License-Identifier: GPL-2.0
//! A simple self test for the rust per-CPU API.
use kernel::{
    define_per_cpu, percpu::cpu_guard::*, percpu::*, pr_info, prelude::*, unsafe_get_per_cpu_ref,
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

impl kernel::Module for PerCpuTestModule {
    fn init(_module: &'static ThisModule) -> Result<Self, Error> {
        pr_info!("rust percpu test start\n");

        let mut native: i64 = 0;
        let mut pcpu: PerCpuRef<i64> = unsafe { unsafe_get_per_cpu_ref!(PERCPU, CpuGuard::new()) };

        native += -1;
        *pcpu += -1;
        assert!(native == *pcpu && native == -1);

        native += 1;
        *pcpu += 1;
        assert!(native == *pcpu && native == 0);

        let mut unative: u64 = 0;
        let mut upcpu: PerCpuRef<u64> =
            unsafe { unsafe_get_per_cpu_ref!(UPERCPU, CpuGuard::new()) };

        unative += 1;
        *upcpu += 1;
        assert!(unative == *upcpu && unative == 1);

        unative = unative.wrapping_add((-1i64) as u64);
        *upcpu = upcpu.wrapping_add((-1i64) as u64);
        assert!(unative == *upcpu && unative == 0);

        unative = unative.wrapping_add((-1i64) as u64);
        *upcpu = upcpu.wrapping_add((-1i64) as u64);
        assert!(unative == *upcpu && unative == (-1i64) as u64);

        unative = 0;
        *upcpu = 0;

        unative = unative.wrapping_sub(1);
        *upcpu = upcpu.wrapping_sub(1);
        assert!(unative == *upcpu && unative == (-1i64) as u64);
        assert!(unative == *upcpu && unative == u64::MAX);

        pr_info!("rust percpu test done\n");

        // Return Err to unload the module
        Result::Err(EINVAL)
    }
}

