// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Red Hat Inc.
 * Author: Philipp Stanner <pstanner@redhat.com>
 */

//! DRM's job dependency manager and load balancer for GPUs with firmware
//! scheduling, i.e., GPUs that spawn one ring per user context.
//!
//! JobQueue abstracts the entire handling of [`dma_buf::FenceContext`] and its
//! associated fences for the user.
//!
//! Jobs created on the JobQueue take over ownership of userspace provided data
//! and prepare all memory allocations. A later job submission sets up the fence
//! and performs no fallible operations. A fence sequence number always
//! corresponds 1:1 with the job sequence number.
//!
//! Before jobs are submitted, dependencies in the form of [`dma_buf::Fence`]
//! can be set on them. Jobs get submitted by the JobQueue in process context
//! through a work item in FIFO order. Only jobs whose dependency fences have
//! all been signaled get submitted. If the front job's dependency is not yet
//! fullfilled, the JobQueue pauses until that's the case.
//!
//! The driver tells the JobQueue which jobs are finished through
//! [`JobQueue::complete_jobs_up_to_seqno`].
//!
//! This is pure Rust code that does not directly abstract C.

use crate::prelude::*;

use core::{
    convert::Infallible,
    ops::Deref, //
};

use kernel::{
    dma_buf::dma_fence::{
        DriverFence,
        DriverFenceAllocation,
        Fence,
        FenceContext,
        FenceCallback,
        FenceCallbackRegistration,
        FenceContextOps, //
    },
    list::*,
    str::CString,
    sync::{
        aref::ARef,
        SpinLockIrq,
        new_spinlock_irq,
        atomic::{
            Atomic,
            Relaxed, //
        }, //
    }, //
    workqueue::{
        self,
        new_scoped_work,
//        ScopedQueue,
        ScopedWork,
        ScopedWorkItem,
        ScopedWorkRef,
    }, //
};

struct DependencyWaker<'b, T: JobQueueOps + Send + Sync> {
    nr_of_deps: &'b Atomic<u32>,
    jq: &'b JobQueue<'b, T>,
}

impl<'b, T: JobQueueOps + Send + Sync> FenceCallback for DependencyWaker<'b, T> {
    fn on_signal(&mut self) {
        // Last dependency kicks off the worker.
        if self.nr_of_deps.fetch_sub(1, Relaxed) == 1 {
            // FIXME:
            // I suspect that this is still the same deadlock-danger versus JQ's
            // drop() that we solved previously with Revocable.
            //
            // If JQ drops, it takes the JQ-lock to deregister its waker callbacks
            // on all external dependency Fences, thereby taking the fence lock.
            // If one of those fences signal at that moment, first the fence lock
            // will be taken, and then the JQ lock here -> deadlock.
            //
            // So we likely want to add the Revocable-solution from the first
            // drafts again.
            let mut jq = self.jq.inner.lock();
            jq.as_mut().check_start_submit_worker();
        }
    }
}

/// A JobQueue job before it has been submitted.
///
/// This is only useful for registering dependencies on and for submitting it.
pub struct Job<'a, T: JobQueueOps + Send + Sync> {
    inner: ListArc<JobInternal<'a, T>>,
}

impl<'a, T: JobQueueOps + Send + Sync> Job<'a, T> {
    /// Add `fence` as a dependency for this job.
    ///
    /// A JobQueue will only push a job once all dependency fences have been
    /// signaled.
    pub fn add_dependency(&'a mut self, fence: &Fence) -> Result {
        let nr_of_deps = &raw const self.inner.nr_of_deps;

        let waker = DependencyWaker {
            // SAFETY: `nr_of_deps` lives as long as the job.
            // TODO RFC: Is there a better way to do it? The mutable-immutable references
            // otherwise run into conflict when `nr_of_deps` is used below.
            nr_of_deps: unsafe { &*nr_of_deps },
            jq: self.inner.jq,
        };

        let job = &mut self.inner;

        job.dependencies().reserve(1, GFP_KERNEL)?;

        // If the registration below gets registered successfully, a signaling
        // fence will decrement the atomic. That could race. Thus, increment in
        // advance. Ordering is ensured thanks to the dma_fence backend.
        job.nr_of_deps.fetch_add(1, Relaxed);

        // SAFETY: The `waker` is not forgotten.
        let registration = unsafe { FenceCallbackRegistration::new(fence, waker) };
        match KBox::pin_init(registration, GFP_KERNEL) {
            Err(e) => {
                let _ = job.nr_of_deps.fetch_sub(1, Relaxed);
                return Err(e);
            },
            Ok(dep) => {
                if let Err(e) = job.dependencies().push_within_capacity(dep) {
                    // Impossible because of the reservation.
                    pr_err!("Job::add_dependency(): reserved memory not available.\n");
                    return Err(e.into());
                }
            },
        }

        Ok(())
    }
}

#[pin_data]
#[repr(C)]
struct JobInternal<'b, T: JobQueueOps + Send + Sync> {
    #[pin]
    links: ListLinks,
    fence_allocation: ListArcField<Option<DriverFenceAllocation<'b, DriverDataWrapper<T>>>, 0>,
    fence: ListArcField<Option<DriverFence<'b, DriverDataWrapper<T>>>>,
    dependencies: ListArcField<KVec<Pin<KBox<FenceCallbackRegistration<DependencyWaker<'b, T>>>>>>,
    nr_of_deps: Atomic<u32>,
    jq: &'b JobQueue<'b, T>,
}

impl_list_arc_safe! {
    impl{'b, T: JobQueueOps + Send + Sync} ListArcSafe<0> for JobInternal<'b, T> { untracked; }
}
impl_list_item! {
    impl{'b, T: JobQueueOps + Send + Sync} ListItem<0> for JobInternal<'b, T> { using ListLinks { self.links }; }
}


impl<'b, T: JobQueueOps + Send + Sync> JobInternal<'b, T> {
//    const LIST_JOB_ID: u64 = 0xe7e445f2bb9e97fc;

    kernel::list::define_list_arc_field_getter! {
        pub(crate) fn fence_allocation(&mut self<0>) -> &mut Option<DriverFenceAllocation<'b, DriverDataWrapper<T>>> { fence_allocation }
        pub(crate) fn fence(&mut self<0>) -> &mut Option<DriverFence<'b, DriverDataWrapper<T>>> { fence }
        pub(crate) fn dependencies(&mut self<0>) -> &mut KVec<Pin<KBox<FenceCallbackRegistration<DependencyWaker<'b, T>>>>> { dependencies }
    }
}

#[pin_data]
struct SubmitWorker<'a, T: JobQueueOps + Send + Sync> {
    #[pin]
    jq: &'a JobQueue<'a, T>,
}

impl<T: JobQueueOps + Send + Sync> ScopedWorkItem for SubmitWorker<'_, T> {
    fn run(work: &ScopedWorkRef<Self>) {
        let fctx_data = work.jq.fctx.deref().deref();
        let mut jq = work.jq.inner.lock();
        let mut jq = jq.as_mut().project();

        let mut cursor = jq.waiting_jobs.cursor_front();

        while let Some(job) = cursor.peek_next() {
            if job.nr_of_deps.load(Relaxed) > 0 {
                break;
            }

            let mut job = job.remove();
            let runnable_job = JobRunnable {
                seqno: job.fence().as_ref().expect("fence not present").as_fence().seqno(),
                data: job.fence().as_ref().expect("fence not present").deref(),
            };

            if !T::run_job(fctx_data, runnable_job) {
                // There was no capacity left. The driver will invoke the JobQueue
                // later again.
                jq.waiting_jobs.push_front(job);
                break;
            }

            jq.running_jobs.push_back(job);
        }

        *jq.submit_worker_active = false;
    }
}

#[pin_data]
struct JobQueueInner<'a, T: JobQueueOps + Send + Sync> {
    submit_worker_active: bool,
    #[pin]
    submit_worker: ScopedWork<SubmitWorker<'a, T>>,
    #[pin]
    waiting_jobs: List<JobInternal<'a, T>>,
    #[pin]
    running_jobs: List<JobInternal<'a, T>>,
}

impl<T: JobQueueOps + Send + Sync> JobQueueInner<'_, T> {
    fn check_start_submit_worker(self: Pin<&mut Self>) {
        if self.submit_worker_active {
            return;
        }

        // TODO this shouldn't be the system wq
        // SAFETY: The worker is carried by the JobQueue and, thus, cannot be
        // forgotten.
        unsafe { workqueue::system_dfl().enqueue_scoped(&*self.submit_worker) };

        *self.project().submit_worker_active = true;
    }

}

/// A JobQueue.
///
/// # Examples
///
/// ```
/// use core::ops::Deref;
///
/// use kernel::{
///     str::CString,
///     dma_buf::*,
///     drm::{
///         JobQueue,
///         JobQueueOps,
///         JobRunnable,
///     },
/// };
///
/// let driver_name = CString::try_from_fmt(fmt!("dummy_driver"))?;
/// let timeline_name = CString::try_from_fmt(fmt!("dummy_timeline"))?;
///
/// #[pin_data]
/// struct JqData {}
///
/// impl JqData {
///     fn new() -> impl PinInit<Self> {
///         pin_init!(Self {})
///     }
/// }
///
/// struct JobData {
///     data: CString,
/// }
///
/// impl JobQueueOps for JqData {
///     type JobDataType = JobData;
///
///     fn run_job<'a>(&self, job: JobRunnable<'a, JobData>) -> bool {
///
///         true
///     }
/// }
///
/// let jq_data = JqData::new();
/// let jq = KBox::pin_init(JobQueue::new(0, driver_name, timeline_name, jq_data), GFP_KERNEL)?;
///
/// struct Callback {}
///
/// impl FenceCallback for Callback {
///     fn on_signal(&mut self) {
///
///     }
/// }
///
/// let data = CString::try_from_fmt(fmt!("hello there!"))?;
/// let data = JobData { data };
///
/// // TODO RFC:
/// // This is the problem where self-references come into play.
/// // Jobs need a reference to the JobQueue due to their DependencyWaker
/// // callbacks. At the same time, the JobQueue owns the jobs.
/// // So the following outcommented code would not compile.
/// // Gary is working on a solution for that.
///
/// //let job = jq.new_job(data)?;
/// //jq.submit(job);
///
/// //drop(jq);
///
/// Ok::<(), Error>(())
/// ```
#[pin_data(PinnedDrop)]
pub struct JobQueue<'a, T: JobQueueOps + Send + Sync> {
    #[pin]
    inner: SpinLockIrq<JobQueueInner<'a, T>>,
    #[pin]
    fctx: FenceContext<DriverDataWrapper<T>>,
}

// SAFETY: The JobQueue is locked and pinned.
unsafe impl<T: JobQueueOps + Send + Sync> Sync for JobQueue<'_, T> {}
// SAFETY: The JobQueue is locked and pinned.
unsafe impl<T: JobQueueOps + Send + Sync> Send for JobQueue<'_, T> {}

/// Object passed to the driver to run a job.
pub struct JobRunnable<'a, T> {
    /// The job's sequence number.
    pub seqno: u64,
    /// The associated job data.
    pub data: &'a T,
}

/// Ops for the JobQueue.
pub trait JobQueueOps {
    /// The type of the data passed over in [`JobQueue::new_job`].
    type JobDataType: Send + Sync;

    /// Instructs the driver to run this job.
    ///
    /// Returns true if the job was run, false if there was no capacity.
    /// In case of an other error, the driver's callback implementation likely
    /// wants to trigger termination of the ring and, thus, dropping of the
    /// respective [`JobQueue`].
    fn run_job<'a>(&self, job: JobRunnable<'a, Self::JobDataType>) -> bool;
}

#[pin_data]
struct DriverDataWrapper<T: JobQueueOps + Send + Sync> {
    #[pin]
    data: T,
}

impl<T: JobQueueOps + Send + Sync> Deref for DriverDataWrapper<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.data
    }
}

impl<T: JobQueueOps + Send + Sync> DriverDataWrapper<T> {
    fn new<E>(data: impl PinInit<T, E>) -> impl PinInit<Self, E>
        where
            Error: From<E>,
            E: From<Infallible>,
    {
        try_pin_init!(Self {
            data <- data,
        }? E)
    }
}

impl<T: JobQueueOps + Send + Sync> FenceContextOps for DriverDataWrapper<T> {
    type FenceDataType = T::JobDataType;
}

impl<'a, T: JobQueueOps + Send + Sync> JobQueue<'a, T> {
    /// Create a new JobQueue.
    pub fn new<E>(
        initial_seqno: u64,
        // TODO: pass these as &CStr. Figure out how to get the lifetimes for PinInit right.
        driver_name: CString,
        timeline_name: CString,
        data: impl PinInit<T, E>,
    ) -> impl PinInit<Self, Error>
    where
        Error: From<E>,
        E: From<Infallible>,
    {
        try_pin_init!(&this in Self {
            inner <- new_spinlock_irq!(try_pin_init!(JobQueueInner {
                submit_worker_active: false,
                // SAFETY: `this` is valid. The lifetimes ensure that the reference remains valid.
                submit_worker <- new_scoped_work!("JobQueueSubmitWorker", Ok(SubmitWorker { jq: unsafe { &*this.as_ptr() } })),
                waiting_jobs: List::new(),
                running_jobs: List::new(),
            })),
            fctx <- FenceContext::new(initial_seqno, &driver_name, &timeline_name, DriverDataWrapper::new(data)),
        })
    }

    /// Create a new job.
    pub fn new_job(&'a self, data: T::JobDataType) -> Result<Job<'a, T>> {
        let fence_allocation = self.fctx.new_fence_allocation(data)?;
        let job = try_pin_init!(JobInternal {
            links <- ListLinks::new(),
            fence_allocation: ListArcField::new(Some(fence_allocation)),
            fence: ListArcField::new(None),
            nr_of_deps: Atomic::new(0),
            dependencies <- ListArcField::new(KVec::new()),
            jq: &self,
        });

        let inner = ListArc::pin_init(job, GFP_KERNEL)?;

        Ok(Job { inner })
    }

    // TODO RFC the lifetime ensures that only jobs can be submitted that were
    // actually created on this JQ, right?
    /// Submit a job that has previously been created with [`JobQueue::new_job()`].
    ///
    /// Returns a [`dma_buf::Fence`] on which the user can register callbacks.
    /// The fence will be signaled once the job is completed through
    /// [`JobQueue::complete_jobs_up_to_seqno`], which can happen immediately
    /// after the job has been submitted.
    // Mutable `self` so that the user is forced to ensure synchronization.
    // That's a bit better so that the fence sequence numbers stay ordered.
    pub fn submit(&mut self, job: Job<'a, T>) -> ARef<Fence> {
        let mut job = job.inner;
        let fence = job.fence_allocation().take().expect("fence_allocation not present").new_fence();

        let ret_fence = ARef::from(fence.as_fence());

        *job.fence() = Some(fence);

        let mut inner = self.inner.lock();

        inner.as_mut().project().waiting_jobs.push_back(job);
        inner.as_mut().check_start_submit_worker();

        ret_fence
    }

    /// Complete all the jobs in the queue's running_list, up to `seqno`.
    ///
    /// This signals the jobs' fences and drops both job and fence.
    pub fn complete_jobs_up_to_seqno(&self, seqno: u64, status: Result) {
        let mut jq = self.inner.lock();
        let mut jq = jq.as_mut().project();

        let mut cursor = jq.running_jobs.cursor_front();

        while let Some(job) = cursor.peek_next() {
            let mut job = job.remove();

            // TODO RFC I couldn't figure out how to access job.fence without removing first:
            // `if job.deref().fence.get_mut().unwrap().as_fence().seqno() > seqno {` ???
            if job.fence().as_mut().expect("fence not present").as_fence().seqno() > seqno {
                jq.running_jobs.push_front(job);
                return;
            }

            job.fence().take().expect("fence not present").signal(status);
        }
    }

    // RFC:
    // Do we want to provide a method like 
    //
    // pub fn complete_front_job() -> T
    //
    // that returns the user's data T so that it can be freed in a deferred
    // manner should T's drop() do something illegal in atomic context?
    //
    // That way the JobQueue would not have to handle dropping through its own
    // workqueue, which might be problematic synchronization-wise.
    //
    // I suppose it depends on how common the use-case is, i.e., what kind of
    // data a job typically carries.
    //
    // On the other hand, the job's data T with this implementation lives in
    // DriverFence's T, which already is being dropped with call_rcu(). So it
    // might be worth at least considering whether rcu_work() might address that?
}

#[pinned_drop]
impl<T: JobQueueOps + Send + Sync> PinnedDrop for JobQueue<'_, T> {
    fn drop(self: Pin<&mut Self>) {
        self.complete_jobs_up_to_seqno(u64::MAX, Err(ECANCELED));
        // TODO: Warn if JQ drops with remaining jobs. Or maybe not? It is being
        // proposed to drop things implicitly.
    }
}
