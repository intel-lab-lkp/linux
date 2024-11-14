/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#if !defined(_GPU_SCHED_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _GPU_SCHED_TRACE_H_

#include <linux/stringify.h>
#include <linux/types.h>
#include <linux/trace_seq.h>
#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM gpu_scheduler
#define TRACE_INCLUDE_FILE gpu_scheduler_trace


/**
 * DOC: uAPI trace events
 *
 * ``drm_sched_job``, ``drm_run_job``, ``drm_sched_process_job``,
 * and ``drm_sched_job_wait_dep`` are considered stable uAPI.
 *
 * Common trace events attributes:
 *
 * * ``id``    - this is &drm_sched_job->id. It uniquely idenfies a job
 *   inside a &struct drm_gpu_scheduler.
 *
 * * ``dev``   - the dev_name() of the device running the job.
 *
 * * ``ring``  - the hardware ring running the job. Together with ``dev`` it
 *   uniquely identifies where the job is going to be executed.
 *
 * * ``fence`` - the &dma_fence.context and the &dma_fence.seqno of
 *   &drm_sched_fence.finished
 *
 */

#ifndef __TRACE_EVENT_GPU_SCHEDULER_PRINT_FN
#define __TRACE_EVENT_GPU_SCHEDULER_PRINT_FN
/* Similar to trace_print_array_seq but for fences. */
static inline const char *__print_dma_fence_array(struct trace_seq *p, const void *buf, int count)
{
	const char *ret = trace_seq_buffer_ptr(p);
	u64 *fences = (u64 *) buf;
	const char *prefix = "";

	trace_seq_putc(p, '{');
	for (int i = 0; i < count; i++) {
		u64 context = fences[2 * i], seqno = fences[2 * i + 1];

		trace_seq_printf(p, "%s(context:%llu, seqno:%lld)",
				 prefix, context, seqno);
		prefix = ",";
	}
	trace_seq_putc(p, '}');
	trace_seq_putc(p, 0);

	return ret;
}
#endif

DECLARE_EVENT_CLASS(drm_sched_job,
	    TP_PROTO(struct drm_sched_job *sched_job, struct drm_sched_entity *entity,
		     unsigned int dep_count),
	    TP_ARGS(sched_job, entity, dep_count),
	    TP_STRUCT__entry(
			     __string(name, sched_job->sched->name)
			     __field(uint64_t, id)
			     __field(u32, job_count)
			     __field(int, hw_job_count)
			     __string(dev, dev_name(sched_job->sched->dev))
			     __field(uint64_t, fence_context)
			     __field(uint64_t, fence_seqno)
			     __field(int, n_deps)
			     __dynamic_array(u64, deps, dep_count * 2)
			     __field(u64, client_id)
			     ),

	    TP_fast_assign(
			   unsigned long idx;
			   struct dma_fence *fence;
			   u64 *dyn_arr;
			   __entry->id = sched_job->id;
			   __assign_str(name);
			   __entry->job_count = spsc_queue_count(&entity->job_queue);
			   __entry->hw_job_count = atomic_read(
				   &sched_job->sched->credit_count);
			   __assign_str(dev);
			   __entry->fence_context = sched_job->s_fence->finished.context;
			   __entry->fence_seqno = sched_job->s_fence->finished.seqno;
			   __entry->n_deps = dep_count;
			   if (dep_count) {
				dyn_arr = __get_dynamic_array(deps);
				xa_for_each(&sched_job->dependencies, idx, fence) {
					dyn_arr[2 * idx] = fence->context;
					dyn_arr[2 * idx + 1] = fence->seqno;
				}
			   }
			   __entry->client_id = sched_job->s_fence->drm_client_id;
			   ),
	    TP_printk("dev=%s, id=%llu, fence=(context:%llu, seqno:%lld), ring=%s, job count:%u, hw job count:%d, dependencies:%s, client_id:%lld",
		      __get_str(dev), __entry->id,
		      __entry->fence_context, __entry->fence_seqno, __get_str(name),
		      __entry->job_count, __entry->hw_job_count,
		      __print_dma_fence_array(p, __get_dynamic_array(deps), __entry->n_deps),
		      __entry->client_id)
);

DEFINE_EVENT(drm_sched_job, drm_sched_job,
	    TP_PROTO(struct drm_sched_job *sched_job, struct drm_sched_entity *entity,
		     unsigned int dep_count),
	    TP_ARGS(sched_job, entity, dep_count)
);

DEFINE_EVENT(drm_sched_job, drm_run_job,
	    TP_PROTO(struct drm_sched_job *sched_job, struct drm_sched_entity *entity,
	    	     unsigned int dep_count),
	    TP_ARGS(sched_job, entity, 0)
);

TRACE_EVENT(drm_sched_process_job,
	    TP_PROTO(struct drm_sched_fence *fence),
	    TP_ARGS(fence),
	    TP_STRUCT__entry(
		    __field(uint64_t, fence_context)
		    __field(uint64_t, fence_seqno)
		    ),

	    TP_fast_assign(
		    __entry->fence_context = fence->finished.context;
		    __entry->fence_seqno = fence->finished.seqno;
		    ),
	    TP_printk("fence=(context:%llu, seqno:%lld) signaled",
		      __entry->fence_context, __entry->fence_seqno)
);

TRACE_EVENT(drm_sched_job_wait_dep,
	    TP_PROTO(struct drm_sched_job *sched_job, struct dma_fence *fence),
	    TP_ARGS(sched_job, fence),
	    TP_STRUCT__entry(
			     __field(uint64_t, fence_context)
			     __field(uint64_t, fence_seqno)
			     __field(uint64_t, id)
			     __field(struct dma_fence *, fence)
			     __field(uint64_t, ctx)
			     __field(uint64_t, seqno)
			     ),

	    TP_fast_assign(
			   __entry->fence_context = sched_job->s_fence->finished.context;
			   __entry->fence_seqno = sched_job->s_fence->finished.seqno;
			   __entry->id = sched_job->id;
			   __entry->fence = fence;
			   __entry->ctx = fence->context;
			   __entry->seqno = fence->seqno;
			   ),
	    TP_printk("fence=(context:%llu, seqno:%lld), id=%llu, dependencies:{(context:%llu, seqno:%lld)}",
		      __entry->fence_context, __entry->fence_seqno, __entry->id,
		      __entry->ctx, __entry->seqno)
);

#endif

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../drivers/gpu/drm/scheduler
#include <trace/define_trace.h>
