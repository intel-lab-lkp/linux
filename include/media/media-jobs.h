/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Media jobs framework
 *
 * Copyright 2025 Ideas on Board Oy
 *
 * Author: Daniel Scally <dan.scally@ideasonboard.com>
 */

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#ifndef _MEDIA_JOBS_H
#define _MEDIA_JOBS_H

struct media_device;
struct media_entity;
struct media_job;
struct media_job_dep;

/**
 * define MEDIA_JOBS_FL_STEP_ANYWHERE - \
 *    Flag a media job step as able to run anytime
 *
 * This flag informs the framework that a job step does not need a particular
 * position in the list of job steps and can be placed anywhere.
 */
#define MEDIA_JOBS_FL_STEP_ANYWHERE			BIT(0)

/**
 * define MEDIA_JOBS_FL_STEP_FROM_FRONT - \
 *    Flag a media job step as needing to be placed near the start of the list
 *
 * This flag informs the framework that a job step needs to be placed at a set
 * position from the start of the list of job steps.
 */
#define MEDIA_JOBS_FL_STEP_FROM_FRONT			BIT(1)

/**
 * define MEDIA_JOBS_FL_STEP_FROM_BACK - \
 *    Flag a media job step as needing to be placed near the end of the list
 *
 * This flag informs the framework that a job step needs to be placed at a set
 * position from the end of the list of job steps.
 */
#define MEDIA_JOBS_FL_STEP_FROM_BACK			BIT(2)

/**
 * enum media_job_types - Type of media job
 *
 * @MEDIA_JOB_TYPE_PIPELINE_PULSE:	A data event moving through the media
 *					pipeline
 *
 * This enumeration details different types of media jobs. The type can be used
 * to differentiate between which steps and dependencies a driver needs to add
 * to a job when it is created.
 */
enum media_job_types {
	MEDIA_JOB_TYPE_PIPELINE_PULSE,
};

/**
 * struct media_job_scheduler - A job scheduler for a particular media device
 *
 * @mdev:		Media device this scheduler is for
 * @list:		List head to attach to the global list of schedulers
 * @kref:		Reference counter
 * @lock:		Lock to protect access to the scheduler
 * @setup_funcs:	List of &struct media_job_setup_func to populate jobs
 * @pending:		List of &struct media_jobs created but not yet queued
 * @queue:		List of &struct media_jobs queued to the scheduler
 * @work:		Work item to run the jobs
 * @async_wq:		Workqueue to run the work on
 *
 * This struct is the main job scheduler struct - drivers wanting to use this
 * framework should acquire an instance through media_jobs_get_scheduler() and
 * subsequently populate it with job setup functions.
 */
struct media_job_scheduler {
	struct media_device *mdev;
	struct list_head list;
	struct kref kref;

	spinlock_t lock; /* Synchronise access to the struct's lists */
	struct list_head setup_funcs;
	struct list_head pending;
	struct list_head queue;
	struct work_struct work;
	struct workqueue_struct *async_wq;
};

/**
 * struct media_job_setup_func - A function to populate a media job with steps
 *				 and dependencies
 *
 * @list:	The list object to attach to the scheduler
 * @type:	The &enum media_job_types that this function populates a job for
 * @job_setup:	Function pointer to the driver's job setup function
 * @data:	Pointer to the driver data for use with @job_setup
 *
 * This struct holds data about the functions a driver registers with the jobs
 * framework in order to populate a new job with steps and dependencies.
 */
struct media_job_setup_func {
	struct list_head list;
	enum media_job_types type;
	int (*job_setup)(struct media_job *job, void *data);
	void *data;
};

/**
 * struct media_job - A representation of a job to be run through the pipeline
 *
 * @lock:	Lock to protect access to the job's lists
 * @list:	List head to attach the job to &struct media_job_scheduler in
 *		either the pending or queue lists
 * @steps:	List of &struct media_job_step to run the job
 * @deps:	List of &struct media_job_dep to check that the job can be
 *		queued
 * @sched:	Pointer to the media job scheduler
 * @type:	The type of the job
 *
 * This struct holds lists of steps that need to be performed to carry out a
 * job in the pipeline. A separate list of dependencies allows the queueing of
 * the job to be delayed until all drivers are ready to carry it out.
 */
struct media_job {
	spinlock_t lock; /* Synchronise access to the struct's lists 6*/
	struct list_head list;
	struct list_head steps;
	struct list_head deps;
	struct media_job_scheduler *sched;
	enum media_job_types type;
};

/**
 * struct media_job_step - A holder for a function to run as part of a job
 *
 * @list:	List head to attach the job step to a &struct media_job.steps
 * @run_step:	The function to run to perform the step
 * @data:	Data to pass to the .run_step() function
 * @flags:	Flags to control how the step is ordered within the job's list
 *		of steps
 * @pos:	Position indicator to control how the step is ordered within the
 *		job's list of steps
 *
 * This struct defines a function that needs to be run as part of the execution
 * of a job in a media pipeline, along with information that help the scheduler
 * determine what order it should be ran in in reference to the other steps that
 * are part of the same job.
 */
struct media_job_step {
	struct list_head list;
	void (*run_step)(void *data);
	void *data;
	unsigned int flags;
	unsigned int pos;
};

/**
 * struct media_job_dep_ops - Operations to manage a media job dependency
 *
 * @check_dep:	A function to ask the driver whether the dependency is met
 * @clear_dep:	A function to tell the driver that the job has been queued
 * @reset_dep:	A function to tell the driver that the job has been cancelled
 *
 * Media jobs have dependencies, such as requiring buffers to be queued. These
 * operations allow a driver to define how the media jobs framework should check
 * whether or not those dependencies are met and how it should inform them that
 * it is taking action based on the state of those dependencies.
 */
struct media_job_dep_ops {
	bool (*check_dep)(void *data);
	void (*clear_dep)(void *data);
	void (*reset_dep)(void *data);
};

/**
 * struct media_job_dep - Representation of media job dependency
 *
 * @list:	List head to attach to a &struct media_job.deps
 * @ops:	A pointer to the dependency's operations functions
 * @met:	A flag to record whether or not the dependency is met
 * @data:	Data to pass to the dependency's operations
 *
 * This struct represents a dependency of a media job. The operations member
 * holds pointers to functions allowing the framework to interact with the
 * driver to check whether or not the dependency is met.
 */
struct media_job_dep {
	struct list_head list;
	struct media_job_dep_ops *ops;
	bool met;
	void *data;
};

/**
 * media_jobs_try_queue_job - Try to queue a &struct media_job
 *
 * @sched:	Pointer to the job scheduler
 * @type:	The type of the media job
 * @dep_ops:	A pointer to the dependency operations for this job
 * @dep_data:	A pointer to the dependency data for this job
 *
 * Try to queue a media job with the scheduler. This function should be called
 * by the drivers whenever a dependency for a media job is met - for example
 * when a buffer is queued to the driver. The framework will check to see if an
 * existing job on the scheduler's pending list shares the same type, dependency
 * operations and dependency data. If it does then that existing job will be
 * considered. If there is no extant job with those same parameters, a new job
 * is allocated and populated by calling the setup functions registered with
 * the framework.
 *
 * The function iterates over the dependencies that are registered with the job
 * and checks to see if they are met. If they're all met, they're cleared and
 * the job is placed onto the scheduler's queue.
 *
 * To help reduce conditionals in drivers where a driver supports both the use
 * of the media jobs framework and operation without it, this function is a no
 * op if @sched is NULL.
 *
 * Return: 0 on success or a negative error number
 */
int media_jobs_try_queue_job(struct media_job_scheduler *sched,
			     enum media_job_types type,
			     struct media_job_dep_ops *dep_ops, void *dep_data);

/**
 * media_jobs_add_job_step - Add a step to a media job
 *
 * @job:	Pointer to the &struct media_job
 * @run_step:	Pointer to the function to run to execute the step
 * @data:	Pointer to the data to pass to @run_ste
 * @flags:	One of the MEDIA_JOBS_FL_STEP_* flags
 * @pos:	A position indicator to use with @flags
 *
 * This function adds a step to the job and should be called from the drivers'
 * job setup functions as registered with the framework through
 * media_jobs_add_job_setup_func(). The @flags and @pos parameters are used
 * to determine the ordering of the steps within the job:
 *
 * If @flags has the MEDIA_JOBS_FL_STEP_ANYWHERE bit set, the step is placed
 * after all steps with MEDIA_JOBS_FL_STEP_FROM_FRONT and before all steps with
 * MEDIA_JOBS_FL_STEP_FROM_BACK bit set, but otherwise in whatever order this
 * function is called.
 *
 * If @flags has the MEDIA_JOBS_FL_STEP_FROM_FRONT bit set then the step is
 * placed @pos steps from the front of the list. Attempting to place multiple
 * steps in the same position will result in an error.
 *
 * If @flags has the MEDIA_JOBS_FL_STEP_FROM_BACK bit set then the step is
 * placed @pos steps from the back of the list. Attempting to place multiple
 * steps in the same position will result in an error.
 *
 * Return: 0 on success or a negative error number
 */
int media_jobs_add_job_step(struct media_job *job, void (*run_step)(void *data),
			    void *data, unsigned int flags, unsigned int pos);

/**
 * media_jobs_add_job_dep - Add a dependency to a media job
 *
 * @job:	Pointer to the &struct media_job
 * @ops:	Pointer to the &struct media_job_dep_ops
 * @data:	Pointer to the data to pass to the dependency's operations
 *
 * This function adds a dependency to the job and should be called from the
 * drivers job setup functions as registered with the framework through the
 * media_jobs_add_job_setup_func() function.
 *
 * Return: 0 on success or a negative error number
 */
int media_jobs_add_job_dep(struct media_job *job, struct media_job_dep_ops *ops,
			   void *data);

/**
 * media_jobs_add_job_setup_func - Add a function that populates a media job
 *
 * @sched:	Pointer to the media jobs scheduler
 * @job_setup:	Pointer to the new job setup function
 * @data:	Data to pass to the job setup function
 * @type:	The type of job that this function should be called for
 *
 * Drivers that wish to utilise the framework need to use this function to
 * register a callback that adds job steps and dependencies when one is created.
 * The function must call media_jobs_add_job_step() and media_jobs_add_job_dep()
 * to populate the job.
 *
 * Return: 0 on success or a negative error number
 */
int media_jobs_add_job_setup_func(struct media_job_scheduler *sched,
				  int (*job_setup)(struct media_job *job, void *data),
				  void *data, enum media_job_types type);

/**
 * media_jobs_put_scheduler - Put a reference to the media jobs scheduler
 *
 * @sched:	Pointer to the media jobs scheduler
 *
 * This function puts a reference to the media jobs scheduler, and is intended
 * to be called in error and exit paths for consuming drivers
 */
void media_jobs_put_scheduler(struct media_job_scheduler *sched);

/**
 * media_jobs_get_scheduler - Get a media jobs scheduler
 *
 * @mdev:	Pointer to the media device associated with the scheduler
 *
 * This function gets a pointer to a &struct media_job_scheduler associated with
 * the media device passed to @mdev. If one is not available then it is
 * allocated and returned. This allows multiple drivers sharing a media graph to
 * work with the same media job scheduler.
 *
 * Return: 0 on success or a negative error number
 */
struct media_job_scheduler *media_jobs_get_scheduler(struct media_device *mdev);

/**
 * media_jobs_run_jobs - Run any media jobs that are ready in the queue
 *
 * @sched:	Pointer to the media job scheduler
 *
 * This function triggers the workqueue that processes any jobs that have been
 * queued, and should be called whenever the pipeline is ready to do so.
 *
 * To help reduce conditionals in drivers where a driver supports both the use
 * of the media jobs framework and operation without it, this function is a no
 * op if @sched is NULL.
 */
void media_jobs_run_jobs(struct media_job_scheduler *sched);

/**
 * media_jobs_cancel_jobs - cancel all waiting jobs
 *
 * @sched:	Pointer to the media job scheduler
 *
 * This function iterates over any pending and queued jobs, resets their
 * dependencies and frees the job
 *
 * To help reduce conditionals in drivers where a driver supports both the use
 * of the media jobs framework and operation without it, this function is a no
 * op if @sched is NULL.
 */
void media_jobs_cancel_jobs(struct media_job_scheduler *sched);

extern struct list_head media_job_schedulers;
extern struct mutex media_job_schedulers_lock;

#endif /* _MEDIA_JOBS_H */
