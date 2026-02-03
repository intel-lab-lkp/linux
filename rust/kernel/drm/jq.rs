// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2025, 2026 Red Hat Inc.:
//   - Philipp Stanner <pstanner@redhat.com>

//! DrmJobqueue. A load balancer, dependency manager and timeout handler for
//! GPU job submissions.

use crate::{prelude::*, types::ARef};
use core::sync::atomic::{AtomicU32, Ordering};
use kernel::list::*;
use kernel::revocable::Revocable;
use kernel::sync::{
    new_spinlock, Arc, DmaFence, DmaFenceCb, DmaFenceCbFunc, DmaFenceCtx, SpinLock,
};
use kernel::workqueue::{self, impl_has_work, new_work, Work, WorkItem};

#[pin_data]
struct Dependency {
    #[pin]
    links: ListLinks,
    fence: ARef<DmaFence<i32>>,
}

impl Dependency {
    fn new(fence: ARef<DmaFence<i32>>) -> Result<ListArc<Self>> {
        ListArc::pin_init(
            try_pin_init!(Self {
                links <- ListLinks::new(),
                fence,
            }),
            GFP_KERNEL,
        )
    }
}

impl_list_arc_safe! {
    impl ListArcSafe<0> for Dependency { untracked; }
}
impl_list_item! {
    impl ListItem<0> for Dependency { using ListLinks { self.links }; }
}
// Callback item for the dependency fences to wake / progress the jobqueue.
struct DependencyWaker<T: 'static + Send> {
    jobq: Arc<Revocable<SpinLock<InnerJobqueue<T>>>>,
    // Scary raw pointer! See justification at the unsafe block below.
    //
    // What would be the alternatives to the rawpointer? I can see two:
    //   1. Refcount the jobs and have the dependency callbacks take a reference.
    //      That would require then, however, to guard the jobs with a SpinLock.
    //      That SpinLock would just exist, however, to satisfy the Rust compiler.
    //      From a kernel-engineering perspective, that would be undesirable,
    //      because the  only thing within a job that might be accessed by multiple
    //      CPUs in parallel is `Job::nr_of_deps`. It's certainly conceivable
    //      that some userspace applications with a great many dependencies would
    //      then suffer from lock contention, just to modify an integer.
    //  2.  Clever Hackyness just to avoid an unsafe that's provably correct:
    //      We could replace this rawpointer with a Arc<AtomicU32>, the Job<T>
    //      holding another reference. Would work. But is that worth it?
    //      Share your opinion on-list :)
    job: *const Job<T>,
}

impl<T: 'static + Send> DependencyWaker<T> {
    fn new(jobq: Arc<Revocable<SpinLock<InnerJobqueue<T>>>>, job: *const Job<T>) -> Self {
        Self { jobq, job }
    }
}

impl<T: 'static + Send> DmaFenceCbFunc for DependencyWaker<T> {
    fn callback(cb: Pin<KBox<DmaFenceCb<Self>>>)
    where
        Self: Sized,
    {
        let jq_guard = cb.data.jobq.try_access();
        if jq_guard.is_none() {
            return;
        }
        let outer_jq = jq_guard.unwrap();

        // SAFETY:
        // `job` is only needed to modify the dependency counter within the job.
        // That counter is atomic, so concurrent modifications are safe.
        //
        // As for the life time: Jobs that have pending dependencies are held by
        // `InnerJobqueue::waiting_jobs`. As long as any of these dependency
        // callbacks here are active, a job can by definition not move to the
        // `InnerJobqueue::running_jobs` list and can, thus, not be freed.
        //
        // In case `Jobqueue` drops, the revocable-check above will guard against
        // UAF. Moreover, jobqueue will deregister all of those dma_fence
        // callbacks and thereby cleanly decouple itself. The dma_fences that
        // these callbacks are registered on can, after all, outlive the jobqueue.
        let job: &Job<T> = unsafe { &*cb.data.job };

        let old_nr_of_deps = job.nr_of_deps.fetch_sub(1, Ordering::Relaxed);
        // If counter == 0, a new job somewhere in the queue just got ready.
        // Run all ready jobs.
        if old_nr_of_deps == 1 {
            let mut jq = outer_jq.lock();
            jq.check_start_submit_worker(cb.data.jobq.clone());
        }

        // TODO remove the Dependency from the job's dep list, so that when
        // `Jobqueue` gets dropped it won't try to deregister callbacks for
        // already-signalled fences.
    }
}

/// A jobqueue Job.
///
/// You can stuff your data in it. The job will be borrowed back to your driver
/// once the time has come to run it.
///
/// Jobs are consumed by [`Jobqueue::submit_job`] by value (ownership transfer).
/// You can set multiple [`DmaFence`] as dependencies for a job. It will only
/// get run once all dependency fences have been signaled.
///
/// Jobs cost credits. Jobs will only be run if there are is enough capacity in
/// the jobqueue for the job's credits. It is legal to specify jobs costing 0
/// credits, effectively disabling that mechanism.
#[pin_data]
pub struct Job<T: 'static + Send> {
    cost: u32,
    #[pin]
    pub data: T,
    done_fence: Option<ARef<DmaFence<i32>>>,
    hardware_fence: Option<ARef<DmaFence<i32>>>,
    nr_of_deps: AtomicU32,
    dependencies: List<Dependency>,
}

impl<T: 'static + Send> Job<T> {
    /// Create a new job that can be submitted to [`Jobqueue`].
    ///
    /// Jobs contain driver data that will later be made available to the driver's
    /// run_job() callback in which the job gets pushed to the GPU.
    pub fn new(cost: u32, data: impl PinInit<T>) -> Result<Pin<KBox<Self>>> {
        let job = pin_init!(Self {
            cost,
            data <- data,
            done_fence: None,
            hardware_fence: None,
            nr_of_deps: AtomicU32::new(0),
            dependencies <- List::<Dependency>::new(),
        });

        KBox::pin_init(job, GFP_KERNEL)
    }

    /// Add a callback to the job. When the job gets submitted, all added callbacks will be
    /// registered on the [`DmaFence`] the jobqueue returns for that job.
    // TODO is callback a good name? We could call it "consequences" for example.
    pub fn add_callback() -> Result {
        Ok(())
    }

    /// Add a [`DmaFence`] or a [`DoneFence`] as this job's dependency. The job
    /// will only be executed after that dependency has been finished.
    pub fn add_dependency(&mut self, fence: ARef<DmaFence<i32>>) -> Result {
        let dependency = Dependency::new(fence)?;

        self.dependencies.push_back(dependency);
        self.nr_of_deps.fetch_add(1, Ordering::Relaxed);

        Ok(())
    }

    /// Check if there are dependencies for this job. Register the jobqueue
    /// waker if yes.
    fn arm_deps(&mut self, jobq: Arc<Revocable<SpinLock<InnerJobqueue<T>>>>) {
        let job_ptr = &raw const *self;
        let mut cursor = self.dependencies.cursor_front();

        while let Some(dep) = cursor.peek_next() {
            let waker = DependencyWaker::new(jobq.clone(), job_ptr);
            if dep.fence.register_callback(waker).is_err() {
                // TODO precise error check
                // The fence raced or was already signaled. But the hardware_fence
                // waker is not yet registered. Thus, it's OK to just decrement
                // the dependency count.
                self.nr_of_deps.fetch_sub(1, Ordering::Relaxed);
                // TODO this dependency must be removed from the list so that
                // `Jobqueue::drop()` doesn't try to deregister the callback.
            }

            cursor.move_next();
        }
    }
}

#[pin_data]
struct JobWrap<T: 'static + Send> {
    #[pin]
    links: ListLinks,
    inner: Pin<KBox<Job<T>>>,
}

impl<T: 'static + Send> JobWrap<T> {
    fn new(job: Pin<KBox<Job<T>>>) -> Result<ListArc<Self>> {
        ListArc::pin_init(
            try_pin_init!(Self {
                links <- ListLinks::new(),
                inner: job,
            }),
            GFP_KERNEL,
        )
    }
}

impl_list_arc_safe! {
    impl{T: Send} ListArcSafe<0> for JobWrap<T> { untracked; }
}
impl_list_item! {
    impl{T: Send} ListItem<0> for JobWrap<T> { using ListLinks { self.links }; }
}

struct InnerJobqueue<T: 'static + Send> {
    capacity: u32,
    waiting_jobs: List<JobWrap<T>>,
    running_jobs: List<JobWrap<T>>,
    submit_worker_active: bool,
    run_job: fn(&Pin<&mut Job<T>>) -> ARef<DmaFence<i32>>,
}

// SAFETY: We use `List` with effectively a `UniqueArc`, so it can be `Send` when elements are `Send`.
unsafe impl<T: 'static + Send> Send for InnerJobqueue<T> {}

impl<T: 'static + Send> InnerJobqueue<T> {
    fn new(capacity: u32, run_job: fn(&Pin<&mut Job<T>>) -> ARef<DmaFence<i32>>) -> Self {
        let waiting_jobs = List::<JobWrap<T>>::new();
        let running_jobs = List::<JobWrap<T>>::new();

        Self {
            capacity,
            waiting_jobs,
            running_jobs,
            submit_worker_active: false,
            run_job,
        }
    }

    fn has_waiting_jobs(&self) -> bool {
        !self.waiting_jobs.is_empty()
    }

    fn has_capacity_left(&self, cost: u32) -> bool {
        let cost = cost as i64;
        let capacity = self.capacity as i64;

        if capacity - cost >= 0 {
            return true;
        }

        false
    }

    fn check_start_submit_worker(&mut self, outer: Arc<Revocable<SpinLock<Self>>>) {
        if self.submit_worker_active {
            return;
        }
        self.submit_worker_active = true;

        // TODO the work item should likely be moved into the JQ struct, since
        // only ever 1 worker needs to run at a time. But if we do it that way,
        // how can we store a reference to the JQ? We obviously can't store it
        // in the JQ itself because circular dependency -> memory leak.
        let submit_work = SubmitWorker::new(outer).unwrap(); // TODO error
        let _ = workqueue::system().enqueue(submit_work); // TODO error
    }
}

// Callback item for the hardware fences to wake / progress the jobqueue.
struct HwFenceWaker<T: 'static + Send> {
    jobq: Arc<Revocable<SpinLock<InnerJobqueue<T>>>>,
    // Another scary raw pointer!
    // This one is necessary so that a) a job can be removed from `InnerJobqueue::running_jobs`,
    // and b) its done_fence be accessed and signaled.
    //
    // What would be the alternatives to this rawpointer? Two come to mind:
    //   1. Refcount the job. Then the job would have to be locked to satisfy Rust.
    //      Locking it is not necessary, however. See the below safety comment
    //      for details.
    //   2. Clever hacky tricks: We could assign a unique ID per job and store it
    //      in this callback. Then, we could find the associated job via iterating
    //      over `jobq.running_jobs`. So to access a job and signal its done_fence,
    //      we'd have to do a list iteration, which is undesirable performance-wise.
    //      Moreover, the unique ID parent would have to be stored in `Jobqueue`,
    //      requiring us to generate jobs on the jobqueue object.
    job: *const JobWrap<T>,
}

impl<T: 'static + Send> HwFenceWaker<T> {
    fn new(jobq: Arc<Revocable<SpinLock<InnerJobqueue<T>>>>, job: *const JobWrap<T>) -> Self {
        Self { jobq, job }
    }
}

impl<T: 'static + Send> DmaFenceCbFunc for HwFenceWaker<T> {
    fn callback(cb: Pin<KBox<DmaFenceCb<Self>>>)
    where
        Self: Sized,
    {
        // This prevents against deadlock. See Jobqueue's drop() for details.
        let jq_guard = cb.data.jobq.try_access();
        if jq_guard.is_none() {
            // The JQ itself will signal all done_fences with an error when it drops.
            return;
        }
        let jq_guard = jq_guard.unwrap();

        let mut jobq = jq_guard.lock();

        // SAFETY:
        // We need the job to remove it from `InnerJobqueue::running_jobs` and to
        // access its done_fence. There is always only one hardware_fence waker
        // callback per job. It's the only party which will remove the job from
        // the running_jobs list. This callback only exists once all Dependency
        // callbacks have already ran. As for the done_fence, the DmaFence
        // implementation guarantees synchronization and correctness. Thus,
        // unlocked access is safe.
        //
        // As for the life time: Only when this callback here has ran will a job
        // be removed from the running_jobs list and, thus, be dropped.
        // `InnerJobqueue`, which owns running_jobs, can only drop once
        // `Jobqueue` got dropped. The latter will deregister all hardware fence
        // callbacks while dropping, thereby preventing UAF through dma_fence
        // callbacks on jobs.
        let job: &JobWrap<T> = unsafe { &*cb.data.job };

        jobq.capacity += job.inner.cost;
        let _ = job.inner.done_fence.as_ref().expect("done_fence not present").signal(); // TODO err

        // SAFETY: This callback function gets registered only once per job,
        // and the registering party (`run_all_ready_jobs()`) adds the job to
        // the list.
        //
        // This is the only reference (incl. refcount) to this job. Thus, it
        // may be removed only after all accesses above have been performed.
        unsafe { jobq.running_jobs.remove(job) };

        // Run more ready jobs if there's capacity.
        jobq.check_start_submit_worker(cb.data.jobq.clone());
    }
}

/// Push a job immediately.
///
/// Returns true if the hardware_fence raced, false otherwise.
fn run_job<T: 'static + Send>(
    driver_cb: fn(&Pin<&mut Job<T>>) -> ARef<DmaFence<i32>>,
    waker: HwFenceWaker<T>,
    job: Pin<&mut Job<T>>,
) -> bool {
    let hardware_fence = driver_cb(&job);

    // If a GPU is very fast (or is processing jobs synchronously or sth.) it
    // could be that the hw_fence is already signaled. In case that happens, we
    // signal the done_fence for userspace & Co. immediately.

    // TODO catch for exact error (currently backend only ever errors if it raced.
    // But still, robustness, you know.
    if hardware_fence.register_callback(waker).is_err() {
        // TODO: Print into log in case of error.
        let _ = job.done_fence.as_ref().expect("done_fence not present").signal();
        return true;
    }

    *job.project().hardware_fence = Some(hardware_fence);

    false
}

// Submits all ready jobs as long as there's capacity.
fn run_all_ready_jobs<T: 'static + Send>(
    jobq: &mut InnerJobqueue<T>,
    outer_jobq: Arc<Revocable<SpinLock<InnerJobqueue<T>>>>,
    driver_cb: fn(&Pin<&mut Job<T>>) -> ARef<DmaFence<i32>>,
) {
    let mut cursor = jobq.waiting_jobs.cursor_front();

    while let Some(job) = cursor.peek_next() {
        if job.inner.nr_of_deps.load(Ordering::Relaxed) > 0 {
            return;
        }

        let cost = job.inner.cost as i64;
        if jobq.capacity as i64 - cost < 0 {
            return;
        }

        let runnable_job = job.remove();
        // To obtain a mutable reference to the list element, we need to cast
        // into a UniqueArc. unwrap() cannot fire because by the jobqueue design
        // a job is only ever in the waiting_jobs OR running_jobs list.
        let mut unique_job = Arc::<JobWrap<T>>::into_unique_or_drop(runnable_job.into_arc()).unwrap();
        let job_ptr: *const JobWrap<T> = &raw const *unique_job;

        let runnable_inner_job /* &mut Pin<KBox<Job<T>>> */ = unique_job.as_mut().project().inner;

        let hw_fence_waker = HwFenceWaker::new(outer_jobq.clone(), job_ptr);
        if !run_job(driver_cb, hw_fence_waker, runnable_inner_job.as_mut()) {
            // run_job() didn't run the job immediately (because the
            // hw_fence did not race). Subtract the credits.
            jobq.capacity -= cost as u32;
        }

        // We gave up our ownership above. And we couldn't clone the Arc, because
        // we needed a UniqueArc for the mutable access. So turn it back now.
        let running_job = ListArc::from(unique_job);
        jobq.running_jobs.push_back(running_job);
    }
}

#[pin_data]
struct SubmitWorker<T: 'static + Send> {
    jobq: Arc<Revocable<SpinLock<InnerJobqueue<T>>>>,
    #[pin]
    work: Work<SubmitWorker<T>>,
}

impl<T: Send> SubmitWorker<T> {
    fn new(
        jobq: Arc<Revocable<SpinLock<InnerJobqueue<T>>>>,
    ) -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(Self {
            jobq,
            work <- new_work!("Jobqueue::SubmitWorker")}),
            GFP_KERNEL,
        )
    }
}

impl_has_work! {
    impl{T: Send} HasWork<Self> for SubmitWorker<T> { self.work }
}

impl<T: Send> WorkItem for SubmitWorker<T> {
    type Pointer = Arc<SubmitWorker<T>>;

    fn run(this: Arc<SubmitWorker<T>>) {
        let outer_jobq_copy = this.jobq.clone();

        let guard = this.jobq.try_access();
        if guard.is_none() {
            // Can never happen. JQ gets only revoked when it drops, and we hold
            // a reference.
            return;
        }
        let jobq = guard.unwrap();

        let mut jobq = jobq.lock();
        let run_job = jobq.run_job;

        run_all_ready_jobs(&mut jobq, outer_jobq_copy, run_job);
        jobq.submit_worker_active = false;
    }
}

/// A job load balancer, dependency manager and timeout handler for GPUs.
///
/// The JQ allows you to submit [`Job`]s. It will run all jobs whose dependecny
/// fences have been signalled, as long as there's capacity. Running jobs happens
/// by borrowing them back to your driver's run_job callback.
///
/// # Examples
///
/// ```
/// use kernel::sync::{DmaFenceCtx, DmaFence, Arc};
/// use kernel::drm::jq::{Job, Jobqueue};
/// use kernel::types::{ARef};
/// use kernel::time::{delay::fsleep, Delta};
///
/// let fctx = DmaFenceCtx::new()?;
///
/// fn run_job(job: &Pin<&mut Job<Arc<DmaFenceCtx>>>) -> ARef<DmaFence<i32>> {
///     let fence = job.data.as_arc_borrow().new_fence(42 as i32).unwrap();
///
///     // Our GPU is so damn fast that it executes each job immediately!
///     fence.signal();
///     fence
/// }
///
/// let jq1 = Jobqueue::new(1_000_000, run_job)?;
/// let jq2 = Jobqueue::new(1_000_000, run_job)?;
///
/// let job1 = Job::new(1, fctx.clone())?;
/// let job2 = Job::new(1, fctx.clone())?;
///
///
/// // Test normal submission of jobs without dependencies.
/// let fence1 = jq1.submit_job(job1)?;
/// let fence2 = jq1.submit_job(job2)?;
///
/// fsleep(Delta::from_secs(1));
/// assert_eq!(fence1.is_signaled(), true);
/// assert_eq!(fence2.is_signaled(), true);
///
///
/// // Test whether a job with a fullfilled dependency gets executed.
/// let mut job3 = Job::new(1, fctx.clone())?;
/// job3.add_dependency(fence1)?;
///
/// let fence3 = jq2.submit_job(job3)?;
/// fsleep(Delta::from_secs(1));
/// assert_eq!(fence3.is_signaled(), true);
///
///
/// // Test whether a job with an unfullfilled dependency does not get executed.
/// let unsignaled_fence = fctx.as_arc_borrow().new_fence(9001 as i32)?;
///
/// let mut job4 = Job::new(1, fctx.clone())?;
/// job4.add_dependency(unsignaled_fence.clone())?;
///
/// let blocked_job_fence = jq2.submit_job(job4)?;
/// fsleep(Delta::from_secs(1));
/// assert_eq!(blocked_job_fence.is_signaled(), false);
///
///
/// // Test whether job4 from above actually gets executed once its dep is met.
/// unsignaled_fence.signal()?;
/// fsleep(Delta::from_secs(1));
/// assert_eq!(blocked_job_fence.is_signaled(), true);
///
/// Ok::<(), Error>(())
/// ```
pub struct Jobqueue<T: 'static + Send> {
    inner: Arc<Revocable<SpinLock<InnerJobqueue<T>>>>,
    fctx: Arc<DmaFenceCtx>, // TODO currently has a separate lock shared with fences
    run_job: fn(&Pin<&mut Job<T>>) -> ARef<DmaFence<i32>>,
}

impl<T: 'static + Send> Jobqueue<T> {
    /// Create a new [`Jobqueue`] with `capacity` space for jobs. `run_job` is
    /// your driver's callback which the jobqueue will call to push a submitted
    /// job to the hardware.
    ///
    /// If you don't want to use the capacity mechanism, set it to a value
    /// unequal 0 and instead set [`Job`]'s cost to 0.
    pub fn new(
        capacity: u32,
        run_job: fn(&Pin<&mut Job<T>>) -> ARef<DmaFence<i32>>,
    ) -> Result<Self> {
        if capacity == 0 {
            return Err(EINVAL);
        }

        let inner = Arc::pin_init(
            Revocable::new(new_spinlock!(InnerJobqueue::<T>::new(capacity, run_job))),
            GFP_KERNEL,
        )?;
        let fctx = DmaFenceCtx::new()?;

        Ok(Self {
            inner,
            fctx,
            run_job,
        })
    }

    /// Submit a job to the jobqueue.
    ///
    /// The jobqueue takes ownership over the job and later passes it back to the
    /// driver by reference through the driver's run_job callback. Jobs are
    /// passed back by reference instead of by value partially to allow for later
    /// adding a job resubmission mechanism to be added to [`Jobqueue`].
    ///
    /// Jobs get run and their done_fences get signalled in submission order.
    ///
    /// Returns the "done_fence" on success, which gets signalled once the
    /// hardware has completed the job and once the jobqueue is done with a job.
    // TODO: Return a DmaFence-wrapper that users cannot signal.
    pub fn submit_job(&self, mut job: Pin<KBox<Job<T>>>) -> Result<ARef<DmaFence<i32>>> {
        let job_cost = job.cost;
        // TODO: It would be nice if the done_fence's seqno actually matches the
        // submission order. To do that, however, we'd need to protect job
        // creation with InnerJobqueue's spinlock. Is that worth it?
        let done_fence = self.fctx.as_arc_borrow().new_fence(42 as i32)?;
        *job.as_mut().project().done_fence = Some(done_fence.clone());

        // TODO register job's callbacks on done_fence.

        let guard = self.inner.try_access();
        if guard.is_none() {
            // Can never happen. JQ gets only revoked when it drops.
            return Err(ENODEV);
        }
        let jobq = guard.unwrap();

        let mut jobq = jobq.lock();

        let had_waiting_jobs_already = jobq.has_waiting_jobs();

        // Check if there are dependencies and, if yes, register rewake
        // callbacks on their fences. Must be done under the JQ lock's protection
        // since the callbacks will access JQ data.
        // SAFETY: `job` was submitted perfectly validly above. We don't move
        // the contents; arm_deps() merely iterates over the dependency-list.
        // TODO: Supposedely this unsafe is unnecessary if you do some magic.
        let pure_job = unsafe { Pin::into_inner_unchecked(job.as_mut()) };
        pure_job.arm_deps(self.inner.clone());

        let wrapped_job = JobWrap::new(job)?;
        jobq.waiting_jobs.push_back(wrapped_job);

        if had_waiting_jobs_already {
            // Jobs waiting means that there is either currently no capacity
            // for more jobs, or the jobqueue is blocked by a job with
            // unfullfilled dependencies. Either the hardware fences' callbacks
            // or those of the dependency fences will pull in more jobs once
            // the conditions are met.
            return Ok(done_fence);
        } else if jobq.has_capacity_left(job_cost) {
            // This is the first waiting job. Wake the submit_worker if necessary.
            jobq.check_start_submit_worker(self.inner.clone());
        }

        // If the conditions for running now were not met, the callbacks registered
        // on the already running jobs' hardware fences will check if there's space
        // for the next job, guaranteeing progress.
        //
        // If no jobs were running, there was by definition still space and the
        // job will get pushed by the worker.
        //
        // If a job couldn't be pushed because there were unfinished dependencies,
        // then the hardware fences' callbacks mentioned above will detect that
        // and not yet push the job.
        //
        // Each dependency's fence has its own callback which checks:
        //   a) whether all other callbacks are fullfilled and if yes:
        //   b) whether there are now enough credits available.
        //
        // If a) and b) are fullfilled, the job gets pushed.
        //
        // If there are no jobs currently running, credits must be available by
        // definition.

        Ok(done_fence)
    }
}

impl<T: 'static + Send> Drop for Jobqueue<T> {
    fn drop(&mut self) {
        // The hardware and dependency fences might outlive the jobqueue.
        // So fence callbacks could very well still call into job queue code,
        // resulting in data UAF or, should the jobqueue code be unloaded,
        // even code UAF.
        //
        // Thus, the jobqueue needs to be cleanly decoupled from those fences
        // when it drops; in other words, it needs to deregister all its
        // fence callbacks.
        //
        // This, however, could easily deadlock when a hw_fence signals:
        //
        // Step     |   Jobqueue step               |   hw_fence step
        // ------------------------------------------------------------------
        // 1        |   JQ starts drop              |   fence signals
        // 2        |   JQ lock taken               |   fence lock taken
        // 3        |   Tries to take fence lock    |   Tries to take JQ lock
        // 4        |   ***DEADLOCK***              |   ***DEADLOCK***
        //
        // In order to prevent deadlock, we first have to revoke access to the
        // JQ so that all fence callbacks can't try to take the lock anymore,
        // and then deregister all JQ callbacks on the fences.
        self.inner.revoke();

        /*
        let guard = self.inner.lock();
        for job in self.inner.waiting_jobs {
            job.deregister_dep_fences();
        }
        for job in self.inner.running_jobs {
            job.deregister_hw_fence();
        }

        TODO: signall all remaining done_fences with an error.
        */
    }
}
