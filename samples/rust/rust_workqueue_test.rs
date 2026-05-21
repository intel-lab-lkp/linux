// SPDX-License-Identifier: GPL-2.0

//! Robust stress test for Rust workqueue API.

use kernel::prelude::*;
use kernel::sync::Arc;
use kernel::time::msecs_to_jiffies;
use kernel::workqueue::{self, new_work, Work, WorkItem};

#[pin_data]
struct TestWorkItem {
    #[pin]
    work: Work<TestWorkItem>,
    value: i32,
}

kernel::impl_has_work! {
    impl HasWork<Self> for TestWorkItem { self.work }
}

impl WorkItem for TestWorkItem {
    type Pointer = Arc<TestWorkItem>;

    fn run(this: Arc<TestWorkItem>) {
        pr_info!(
            "Rust workqueue test: Work item running (value: {})\n",
            this.value
        );
    }
}

#[pin_data]
struct TestDelayedWorkItem {
    #[pin]
    delayed_work: workqueue::DelayedWork<TestDelayedWorkItem>,
    value: i32,
}

kernel::impl_has_delayed_work! {
    impl HasDelayedWork<Self> for TestDelayedWorkItem { self.delayed_work }
}

impl WorkItem for TestDelayedWorkItem {
    type Pointer = Arc<TestDelayedWorkItem>;

    fn run(this: Arc<TestDelayedWorkItem>) {
        pr_info!(
            "Rust workqueue test: Delayed work item running (value: {})\n",
            this.value
        );
    }
}

struct RustWorkqueueTest;

impl kernel::Module for RustWorkqueueTest {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust workqueue test: starting robust verification (v3)\n");

        // 1. Basic Lifecycle with Refcount Validation (Standard Work)
        {
            let work_item = Arc::pin_init(
                pin_init!(TestWorkItem {
                    work <- new_work!("TestWorkItem::work"),
                    value: 42,
                }),
                GFP_KERNEL,
            )?;

            let initial_count = workqueue::arc_count(&work_item);
            pr_info!("Initial Arc strong count: {}\n", initial_count);

            // Enqueue
            let enqueued_item = work_item.clone();

            if let Err(returned_item) = workqueue::system().enqueue(enqueued_item) {
                pr_warn!("Work already pending, unexpected!\n");
                let _ = returned_item;
            } else {
                pr_info!(
                    "Work enqueued successfully. Strong count: {}\n",
                    workqueue::arc_count(&work_item)
                );
            }

            // Cancel immediately
            if let Some(reclaimed) = work_item.work.cancel() {
                let count_after_cancel = workqueue::arc_count(&work_item);
                pr_info!(
                    "Success: Work cancelled and Arc reclaimed. Strong count: {}\n",
                    count_after_cancel
                );

                if count_after_cancel != initial_count + 1 {
                    pr_err!(
                        "ERROR: Refcount mismatch after cancel! Expected {}, got {}\n",
                        initial_count + 1,
                        count_after_cancel
                    );
                    return Err(ENXIO);
                }
                drop(reclaimed);
                if workqueue::arc_count(&work_item) != initial_count {
                    pr_err!("ERROR: Refcount mismatch after drop!\n");
                    return Err(ENXIO);
                }
            } else {
                pr_info!("Work already running or finished.\n");
            }
        }

        // 2. Stress Testing: Enqueue/Cancel Sync Loop
        {
            pr_info!("Starting stress test (1000 iterations)...\n");
            let work_item = Arc::pin_init(
                pin_init!(TestWorkItem {
                    work <- new_work!("TestWorkItem::work"),
                    value: 99,
                }),
                GFP_KERNEL,
            )?;

            for i in 0..1000 {
                let _ = workqueue::system().enqueue(work_item.clone());
                let _ = work_item.work.cancel_sync();
                if i % 250 == 0 {
                    pr_info!("Stress test progress: {}/1000\n", i);
                }
            }

            if workqueue::arc_count(&work_item) != 1 {
                pr_err!("ERROR: Refcount leak detected after stress test!\n");
                return Err(ENXIO);
            } else {
                pr_info!("Stress test completed successfully.\n");
            }
        }

        // 3. Delayed Work Cancellation Test
        {
            let delayed_item = Arc::pin_init(
                pin_init!(TestDelayedWorkItem {
                    delayed_work <- workqueue::new_delayed_work!("TestDWorkItem::delayed_work"),
                    value: 7,
                }),
                GFP_KERNEL,
            )?;

            let initial_count = workqueue::arc_count(&delayed_item);

            // Schedule with a long delay (5 seconds)
            if let Err(returned) =
                workqueue::system().enqueue_delayed(delayed_item.clone(), msecs_to_jiffies(5000))
            {
                drop(returned);
            } else {
                pr_info!(
                    "Delayed work enqueued. count: {}\n",
                    workqueue::arc_count(&delayed_item)
                );
            }

            if let Some(reclaimed) = delayed_item.delayed_work.cancel() {
                pr_info!("Success: Delayed work reclaimed. No leak.\n");
                drop(reclaimed);
            }

            if workqueue::arc_count(&delayed_item) != initial_count {
                pr_err!("ERROR: Refcount leak after delayed cancel!\n");
                return Err(ENXIO);
            }
        }

        pr_info!("Rust workqueue test: all robust checks passed\n");
        Ok(RustWorkqueueTest)
    }
}

impl Drop for RustWorkqueueTest {
    fn drop(&mut self) {
        pr_info!("Rust workqueue test: exit\n");
    }
}

module! {
    type: RustWorkqueueTest,
    name: "rust_workqueue_test",
    authors: ["Aakash Bollineni"],
    description: "Robust stress test for Rust workqueue API",
    license: "GPL",
}
