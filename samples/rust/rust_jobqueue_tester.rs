// SPDX-License-Identifier: GPL-2.0

//! Small example demonstrating how to use [`drm::Jobqueue`].

use kernel::prelude::*;
use kernel::sync::{DmaFenceCtx, DmaFence, Arc};
use kernel::drm::jq::{Job, Jobqueue};
use kernel::types::{ARef};
use kernel::time::{delay::fsleep, Delta};
use kernel::workqueue::{self, impl_has_work, new_work, Work, WorkItem};

module! {
    type: RustJobqueueTester,
    name: "rust_jobqueue_tester",
    authors: ["Philipp Stanner"],
    description: "Rust minimal sample",
    license: "GPL",
}

#[pin_data]
struct GPUWorker {
    hw_fence: ARef<DmaFence<i32>>,
    #[pin]
    work: Work<GPUWorker>,
}

impl GPUWorker {
    fn new(
        hw_fence: ARef<DmaFence<i32>>,
    ) -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {hw_fence, work <- new_work!("Jobqueue::GPUWorker")}),
            GFP_KERNEL,
        )
    }
}

impl_has_work! {
    impl HasWork<Self> for GPUWorker { self.work }
}

impl WorkItem for GPUWorker {
    type Pointer = Arc<GPUWorker>;

    fn run(this: Arc<GPUWorker>) {
        fsleep(Delta::from_secs(1));
        this.hw_fence.signal().unwrap();
    }
}

fn run_job(job: &Pin<&mut Job<Arc<DmaFenceCtx>>>) -> ARef<DmaFence<i32>> {
    let fence = job.data.as_arc_borrow().new_fence(42 as i32).unwrap();

    let gpu_worker = GPUWorker::new(fence.clone()).unwrap();
    let _ = workqueue::system().enqueue(gpu_worker);

    fence
}

struct RustJobqueueTester { }

impl kernel::Module for RustJobqueueTester {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust Jobqueue Tester (init)\n");
        pr_info!("Am I built-in? {}\n", !cfg!(MODULE));

        let dep_fctx = DmaFenceCtx::new()?;
        let hw_fctx = DmaFenceCtx::new()?;
        let jq = Jobqueue::new(1_000_000, run_job)?;


        pr_info!("Test 1: Test submitting two jobs without dependencies.\n");
        let job1 = Job::new(1, hw_fctx.clone())?;
        let job2 = Job::new(1, hw_fctx.clone())?;

        let fence1 = jq.submit_job(job1)?;
        let fence2 = jq.submit_job(job2)?;

        while !fence1.is_signaled() || !fence2.is_signaled() {
            fsleep(Delta::from_secs(2));
        }
        pr_info!("Test 1 succeeded.\n");


        pr_info!("Test 2: Test submitting a job with already-fullfilled dependency.\n");
        let mut job3 = Job::new(1, hw_fctx.clone())?;
        job3.add_dependency(fence1)?;

        let fence3 = jq.submit_job(job3)?;
        fsleep(Delta::from_secs(2));
        if !fence3.is_signaled() {
            pr_info!("Test 2 failed.\n");
            return Err(EAGAIN);
        }
        pr_info!("Test 2 succeeded.\n");


        pr_info!("Test 3: Test that a job with unfullfilled dependency gets never run.\n");
        let unsignaled_fence = dep_fctx.as_arc_borrow().new_fence(9001 as i32)?;

        let mut job4 = Job::new(1, hw_fctx.clone())?;
        job4.add_dependency(unsignaled_fence.clone())?;

        let blocked_job_fence = jq.submit_job(job4)?;
        fsleep(Delta::from_secs(2));
        if blocked_job_fence.is_signaled() {
            pr_info!("Test 3 failed.\n");
            return Err(EAGAIN);
        }
        pr_info!("Test 3 succeeded.\n");


        pr_info!("Test 4: Test whether Test 3's blocked job can be unblocked.\n");
        unsignaled_fence.signal()?;
        while !blocked_job_fence.is_signaled() {
            fsleep(Delta::from_secs(2));
        }
        pr_info!("Test 4 succeeded.\n");


        pr_info!("Test 5: Submit a bunch of unblocked jobs, then a blocked one, then an unblocked one.\n");
        let job1 = Job::new(1, hw_fctx.clone())?;
        let job2 = Job::new(1, hw_fctx.clone())?;
        let mut job3 = Job::new(1, hw_fctx.clone())?;
        let job4 = Job::new(1, hw_fctx.clone())?;
        let job5 = Job::new(1, hw_fctx.clone())?;

        let unsignaled_fence1 = dep_fctx.as_arc_borrow().new_fence(9001 as i32)?;
        let unsignaled_fence2 = dep_fctx.as_arc_borrow().new_fence(9001 as i32)?;
        let unsignaled_fence3 = dep_fctx.as_arc_borrow().new_fence(9001 as i32)?;
        job3.add_dependency(unsignaled_fence1.clone())?;
        job3.add_dependency(unsignaled_fence2.clone())?;
        job3.add_dependency(unsignaled_fence3.clone())?;

        let fence1 = jq.submit_job(job1)?;
        let fence2 = jq.submit_job(job2)?;
        let fence3 = jq.submit_job(job3)?;

        fsleep(Delta::from_secs(2));
        if fence3.is_signaled() || !fence1.is_signaled() || !fence2.is_signaled() {
            pr_info!("Test 5 failed.\n");
            return Err(EAGAIN);
        }

        unsignaled_fence1.signal()?;
        unsignaled_fence3.signal()?;
        fsleep(Delta::from_secs(2));
        if fence3.is_signaled() {
            pr_info!("Test 5 failed.\n");
            return Err(EAGAIN);
        }

        unsignaled_fence2.signal()?;
        fsleep(Delta::from_secs(2));
        if !fence3.is_signaled() {
            pr_info!("Test 5 failed.\n");
            return Err(EAGAIN);
        }

        let fence4 = jq.submit_job(job4)?;
        let fence5 = jq.submit_job(job5)?;

        fsleep(Delta::from_secs(2));

        if !fence4.is_signaled() || !fence5.is_signaled() {
            pr_info!("Test 5 failed.\n");
            return Err(EAGAIN);
        }
        pr_info!("Test 5 succeeded.\n");


        Ok(RustJobqueueTester { })
    }
}

impl Drop for RustJobqueueTester {
    fn drop(&mut self) {
        pr_info!("Rust Jobqueue Tester (exit)\n");
    }
}
