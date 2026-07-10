# SPDX-License-Identifier: GPL-2.0-only
config DRM_XE_JOB_TIMEOUT_MAX
	int "Hard upper limit for job timeout (ms)"
	default 10000 # milliseconds
	help
	  Absolute upper bound (in milliseconds) for the per-engine-class job
	  timeout. This is the maximum value that can be written to the sysfs
	  job_timeout_ms knob, regardless of privileges. To raise this ceiling,
	  increase this value and rebuild the kernel.
config DRM_XE_JOB_TIMEOUT_MIN
	int "Hard lower limit for job timeout (ms)"
	default 1 # milliseconds
	help
	  Absolute lower bound (in milliseconds) for the per-engine-class job
	  timeout. This is the minimum value that can be written to the sysfs
	  job_timeout_ms knob, regardless of privileges.

	  Note: the job timeout default (5000 ms) is hardcoded in the driver
	  and is not configurable here. Use the sysfs job_timeout_ms knob at
	  runtime to change the engine-class default.
config DRM_XE_TIMESLICE_MAX
	int "Hard upper limit for timeslice duration (us)"
	default 10000000 # microseconds
	help
	  Absolute upper bound (in microseconds) for the timeslice duration.
	  This caps both the sysfs timeslice_duration_us knob and the value
	  accepted via the DRM_XE_EXEC_QUEUE_SET_PROPERTY_TIMESLICE UAPI for
	  processes with CAP_SYS_NICE when DRM_XE_ENABLE_SCHEDTIMEOUT_LIMIT
	  is enabled.
config DRM_XE_TIMESLICE_MIN
	int "Hard lower limit for timeslice duration (us)"
	default 1 # microseconds
	help
	  Absolute lower bound (in microseconds) for the timeslice duration.
	  This caps both the sysfs timeslice_duration_us knob and the value
	  accepted via the DRM_XE_EXEC_QUEUE_SET_PROPERTY_TIMESLICE UAPI for
	  processes with CAP_SYS_NICE when DRM_XE_ENABLE_SCHEDTIMEOUT_LIMIT
	  is enabled.
config DRM_XE_PREEMPT_TIMEOUT
	int "Default preempt timeout (us, jiffy granularity)"
	default 640000 # microseconds
	help
	  Initial per-engine-class preemption timeout (in microseconds). This
	  is the value the driver programs at boot; it can be changed at
	  runtime via the sysfs preempt_timeout_us knob.

	  This is how long the driver waits for the current context to reach
	  an arbitration point and yield the GPU voluntarily when a
	  higher-priority context becomes runnable. If the context does not
	  yield before the timer expires, the HW is reset to allow the
	  higher-priority context to execute.

	  The range userspace may write via sysfs is bounded by
	  DRM_XE_PREEMPT_TIMEOUT_MIN and DRM_XE_PREEMPT_TIMEOUT_MAX.
config DRM_XE_PREEMPT_TIMEOUT_MAX
	int "Hard upper limit for preempt timeout (us)"
	default 10000000 # microseconds
	help
	  Absolute upper bound (in microseconds) for the per-engine-class
	  preemption timeout. This is the maximum value that can be written to
	  the sysfs preempt_timeout_us knob, regardless of privileges.
config DRM_XE_PREEMPT_TIMEOUT_MIN
	int "Hard lower limit for preempt timeout (us)"
	default 1 # microseconds
	help
	  Absolute lower bound (in microseconds) for the per-engine-class
	  preemption timeout. This is the minimum value that can be written to
	  the sysfs preempt_timeout_us knob, regardless of privileges.
config DRM_XE_ENABLE_SCHEDTIMEOUT_LIMIT
	bool "Default configuration of limitation on scheduler timeout"
	default y
	help
	  Configures the enablement of limitation on scheduler timeout
	  to apply to applicable user. For elevated user, all above MIN
	  and MAX values will apply when this configuration is enable to
	  apply limitation. By default limitation is applied.

config DRM_XE_BO_DEFRAG_RECLAIM_BACKOFF_THRESHOLD
	int "BO defrag reclaim backoff threshold"
	default 2
	range 1 1000
	help
	  Once this many BOs are tracked on the device defrag list (i.e. were
	  backed with a sub-optimal page order), request that the TTM pool backs
	  off from aggressive reclaim at the beneficial order during populate,
	  so that allocations make forward progress instead of stalling.

config DRM_XE_BO_DEFRAG_SIZE_LIMIT
	int "Maximum bytes to defrag per work run"
	default 33554432
	range 0 1073741824
	help
	  Maximum number of bytes the defrag worker will process in a single run
	  before yielding and rescheduling itself. Set to less than large page
	  size (2M) to disable defrag. A defrag move synchronously reallocates
	  and re-copies a BO's backing store, which is not free. If a large
	  number of BOs become eligible at once, processing them all in one
	  worker run would hold things up for a long, unbounded stretch.
	  Instead, cap the work done per run and requeue, spreading the defrag
	  effort out over time.

config DRM_XE_BO_DEFRAG_INTERVAL_MS
	int "Default delay before defrag worker run (ms)"
	default 25
	range 1 1000
	help
	  Default delay before (re)running the defrag worker, in milliseconds.

config DRM_XE_BO_DEFRAG_INTERVAL_MAX_MS
	int "Upper bound for defrag worker interval (ms)"
	default 15000
	range 100 1000000
	help
	  Upper bound for the (exponentially backed off) defrag worker interval,
	  in milliseconds, so repeated failures don't push the retry arbitrarily
	  far out. 15000 ms = 15 seconds.
