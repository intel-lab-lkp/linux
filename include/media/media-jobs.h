/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Media jobs framework
 *
 * Copyright 2025 Ideas on Board Oy
 *
 * Author: Daniel Scally <dan.scally@ideasonboard.com>
 */

#ifndef _MEDIA_JOBS_H
#define _MEDIA_JOBS_H

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct media_device;
struct media_entity;
struct media_job;

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
 * @contributors:	List of &struct media_job_contributors
 * @pending:		List of &struct media_jobs created but not yet queued
 * @queue:		List of &struct media_jobs queued to the scheduler
 * @work:		Work item to run the jobs
 * @async_wq:		Workqueue to run the work on
 * @running:		Flag indicating whether the scheduler is running or not
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
	struct list_head contributors;
	struct list_head pending;
	struct list_head queue;

	struct work_struct work;
	struct workqueue_struct *async_wq;
	bool running;
};

/**
 * struct media_job_contributor_ops - Operations for a media job contributor
 *
 * @add_steps:	A function to ask the contributor to add its steps to the job
 * @ready:	A function to ask the contributor whether it's ready to run a job
 * @queue:	A function to tell the contributor that the job will be queued
 * @abort:	A function to tell the contributor that the job has been cancelled
 *
 * Media jobs have _contributors_ that may require certain conditions to be met
 * before running a job and may require certain steps to be taken on running
 * a job. For example, a video device may be a contributor and may require
 * buffers to have been queued before running a job, and upon running a job may
 * write the address of those buffers to hardware. These operations allow a
 * driver to define how the media jobs framework should check whether or not
 * those pre-conditions are met, what steps to take and how it should inform
 * the driver taking action based on the state of those preconditions.
 */
struct media_job_contributor_ops {
	int (*add_steps)(struct media_job *job, void *data);
	bool (*ready)(void *data);
	void (*queue)(void *data);
	void (*abort)(void *data);
};

/**
 * struct media_job_contributor - A representation of a contributor to a job
 *
 * @ops:	The list of operations that this contributor provides
 * @type:	The &enum media_job_types that this contributor is for
 * @data:	Pointer to the driver data for use with @ops
 *
 * @list:	The list object to attach to the scheduler
 *
 * This struct is a representation of a contributor to a media job. The type
 * field is used to specify what type of job it contributes to. The @ops member
 * defines callbacks into the drivers that allow the framework to check whether
 * the contributor is ready to queue a job, inform it that one has been queued,
 * abort a queued job and to populate a job with steps that need to be performed
 */
struct media_job_contributor {
	struct media_job_contributor_ops *ops;
	enum media_job_types type;
	void *data;

	struct list_head list;
};

/**
 * struct media_job - A representation of a job to be run through the pipeline
 *
 * @sched:	Pointer to the media job scheduler
 * @type:	The type of the job
 *
 * @lock:	Lock to protect access to the job's lists
 * @list:	List head to attach the job to &struct media_job_scheduler in
 *		either the pending or queue lists
 * @steps:	List of &struct media_job_step to run the job
 *
 * This struct holds lists of steps that need to be performed to carry out a
 * job in the pipeline.
 */
struct media_job {
	struct media_job_scheduler *sched;
	enum media_job_types type;

	spinlock_t lock; /* Synchronise access to the struct's lists */
	struct list_head list;
	struct list_head steps;
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
	u32 flags;
	unsigned int pos;
};

/**
 * media_jobs_try_queue_job - Try to queue a &struct media_job
 *
 * @sched:	Pointer to the job scheduler
 * @type:	The type of the media job
 *
 * Try to queue a media job with the scheduler. This function should be called
 * by the drivers whenever a precondition for a media job to be queued is met.
 * For example if a driver requires that a buffer be queued before running a job
 * then this function should be called when one is queued. The framework will
 * check to see if all the contributors to the given job type are ready to queue
 * one, and do so if so.
 *
 * To help reduce conditionals in drivers where a driver supports both the use
 * of the media jobs framework and operation without it, this function is a no
 * op if @sched is NULL.
 *
 * Return: 0 on success or a negative error number
 */
int media_jobs_try_queue_job(struct media_job_scheduler *sched,
			     enum media_job_types type);

/**
 * media_jobs_add_job_step - Add a step to a media job
 *
 * @job:	Pointer to the &struct media_job
 * @run_step:	Pointer to the function to run to execute the step
 * @data:	Pointer to the data to pass to @run_step
 * @flags:	One of the MEDIA_JOBS_FL_STEP_* flags
 * @pos:	A position indicator to use with @flags
 *
 * This function adds a step to the job and should be called from the .add_steps
 * callbacks for each contributors' operations. The @flags and @pos parameters
 * are used to determine the ordering of the steps within the job:
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
 * media_jobs_register_job_contributor - Registers a contributor for a type of
 *					 media job
 *
 * @sched:	Pointer to the media jobs scheduler
 * @ops:	Pointer to operations for this contributor
 * @data:	Data to pass to the ops functions
 * @type:	The type of job that this function should be called for
 *
 * Drivers that wish to utilise the framework need to use this function to
 * register contributors for a type of job. The contributor's operations are
 * used to populate the job with steps to perform and help the framework decide
 * when a job can be scheduled.
 *
 * Return: 0 on success or a negative error number
 */
int media_jobs_register_job_contributor(struct media_job_scheduler *sched,
					struct media_job_contributor_ops *ops,
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

#endif /* _MEDIA_JOBS_H */
