// SPDX-License-Identifier: GPL-2.0

//! Robust stress test for Rust workqueue API.

use kernel::prelude::*;
use kernel::sync::Arc;
use kernel::time::msecs_to_jiffies;
use kernel::workqueue::{self, new_work, Work, WorkItem};

#[pin_data]
struct TestItem {
    #[pin]
    work: Work<TestItem>,
    value: i32,
    #[pin]
    delayed_work: workqueue::DelayedWork<TestItem>,
}

kernel::impl_has_work! {
    impl HasWork<Self> for TestItem { self.work }
}

// SAFETY: The `delayed_work` field is at a fixed offset and is valid for the lifetime of
// `TestItem`.
unsafe impl workqueue::HasDelayedWork<Self> for TestItem {}

impl WorkItem for TestItem {
    type Pointer = Arc<TestItem>;

    fn run(this: Arc<TestItem>) {
        pr_info!(
            "Rust workqueue test: Work item running (value: {})\n",
            this.value
        );
    }
}

/// Helper to get Arc strong count for verification in tests.
/// This uses internal layout knowledge of Arc.
fn get_arc_count<T: Sized>(arc: &Arc<T>) -> i32 {
    // SAFETY: ArcInner has refcount as its first field. Arc points to data at DATA_OFFSET.
    unsafe {
        let ptr = Arc::as_ptr(arc);
        let inner_ptr = (ptr as *const u8).sub(Arc::<T>::DATA_OFFSET);
        // The first field of ArcInner is Refcount, which is a transparent wrapper around
        // refcount_t. In the kernel, refcount_t is an atomic_t (i32).
        let refcount_ptr = inner_ptr as *const i32;
        // We use a relaxed load to get the current value.
        core::ptr::read_volatile(refcount_ptr)
    }
}

struct RustWorkqueueTest;

impl kernel::Module for RustWorkqueueTest {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust workqueue test: starting robust verification\n");

        // 1. Basic Lifecycle with Refcount Validation
        {
            let test_item = Arc::pin_init(
                pin_init!(TestItem {
                    work <- new_work!("TestItem::work"),
                    value: 42,
                    delayed_work <- workqueue::new_delayed_work!("TestItem::delayed_work"),
                }),
                GFP_KERNEL,
            )?;

            let initial_count = get_arc_count(&test_item);
            pr_info!("Initial Arc strong count: {}\n", initial_count);

            // Enqueue
            let enqueued_item = test_item.clone();

            if let Err(returned_item) = workqueue::system().enqueue(enqueued_item) {
                pr_warn!("Work already pending, unexpected!\n");
                let _ = returned_item;
            } else {
                pr_info!(
                    "Work enqueued successfully. Strong count: {}\n",
                    get_arc_count(&test_item)
                );
            }

            // Cancel immediately (best effort)
            if let Some(reclaimed) = test_item.work.cancel() {
                let count_after_cancel = get_arc_count(&test_item);
                pr_info!(
                    "Success: Work cancelled and Arc reclaimed. Strong count: {}\n",
                    count_after_cancel
                );

                // VALIDATION: Reclamation must restore the refcount (minus the clone we just
                // reclaimed)
                if count_after_cancel != initial_count + 1 {
                    pr_err!(
                        "ERROR: Refcount mismatch after cancel! Expected {}, got {}\n",
                        initial_count + 1,
                        count_after_cancel
                    );
                    return Err(ENXIO);
                }
                drop(reclaimed);
                if get_arc_count(&test_item) != initial_count {
                    pr_err!(
                        "ERROR: Refcount mismatch after drop! Expected {}, got {}\n",
                        initial_count,
                        get_arc_count(&test_item)
                    );
                    return Err(ENXIO);
                }
            } else {
                pr_info!("Work already running or finished, could not reclaim via cancel().\n");
            }
        }

        // 2. Stress Testing: Enqueue/Cancel Sync Loop
        {
            pr_info!("Starting stress test (1000 iterations)...\n");
            let test_item = Arc::pin_init(
                pin_init!(TestItem {
                    work <- new_work!("TestItem::work"),
                    value: 99,
                    delayed_work <- workqueue::new_delayed_work!("TestItem::delayed_work"),
                }),
                GFP_KERNEL,
            )?;

            for i in 0..1000 {
                let _ = workqueue::system().enqueue(test_item.clone());
                // Use cancel_sync to ensure determinism for the next iteration
                let _ = test_item.work.cancel_sync();

                if i % 250 == 0 {
                    pr_info!("Stress test progress: {}/1000\n", i);
                }
            }

            if get_arc_count(&test_item) != 1 {
                pr_err!(
                    "ERROR: Refcount leak detected after stress test! count: {}\n",
                    get_arc_count(&test_item)
                );
                return Err(ENXIO);
            } else {
                pr_info!("Stress test completed successfully. No refcount leaks.\n");
            }
        }

        // 3. Delayed Work Cancellation Test
        {
            let test_item = Arc::pin_init(
                pin_init!(TestItem {
                    work <- new_work!("TestItem::work"),
                    value: 7,
                    delayed_work <- workqueue::new_delayed_work!("TestItem::delayed_work"),
                }),
                GFP_KERNEL,
            )?;

            let initial_count = get_arc_count(&test_item);

            // Schedule with a long delay
            let to_enqueue = test_item.clone();
            let res = workqueue::system()
                .enqueue_delayed(to_enqueue, msecs_to_jiffies(5000));
            if let Err(returned) = res {
                pr_warn!("Delayed work already pending, returned item.\n");
                drop(returned);
            } else {
                pr_info!("Delayed work enqueued. count: {}\n", get_arc_count(&test_item));
            }

            if test_item.delayed_work.is_pending() {
                pr_info!("Delayed work is pending as expected.\n");
            }

            if let Some(reclaimed) = test_item.delayed_work.cancel() {
                pr_info!("Success: Delayed work reclaimed. No leak.\n");
                drop(reclaimed);
            } else {
                pr_warn!("Notice: Delayed work not reclaimed (running or never enqueued).\n");
            }

            let final_count = get_arc_count(&test_item);
            if final_count != initial_count {
                pr_err!(
                    "ERROR: Refcount leak after delayed cancel! expected {}, got {}\n",
                    initial_count,
                    final_count
                );
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
