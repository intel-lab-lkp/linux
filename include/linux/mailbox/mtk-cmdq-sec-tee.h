/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __MTK_CMDQ_SEC_TEE_H__
#define __MTK_CMDQ_SEC_TEE_H__

#include <linux/types.h>
#include <linux/delay.h>
#include <linux/tee_drv.h>

/**
 * struct cmdq_sec_tee_context - context for tee vendor
 * @uuid: Universally Unique Identifier of secure world.
 * @tee_context: basic tee context.
 * @session: session handle.
 * @shared_mem: shared memory.
 */
struct cmdq_sec_tee_context {
	u8			uuid[TEE_IOCTL_UUID_LEN];
	struct tee_context	*tee_context;
	u32			session;
	struct tee_shm		*shared_mem;
};

/**
 * cmdq_sec_setup_tee_context() - setup the uuid for the tee context to communicate with
 * @tee:	context for tee vendor
 *
 * Return: 0 for success; else the error code is returned
 *
 */
void cmdq_sec_setup_tee_context(struct cmdq_sec_tee_context *tee);

/**
 * cmdq_sec_init_context() - initialize the tee context
 * @tee:	context for tee vendor
 *
 * Return: 0 for success; else the error code is returned
 *
 */
int cmdq_sec_init_context(struct cmdq_sec_tee_context *tee);

/**
 * cmdq_sec_deinit_context() - de-initialize the tee context
 * @tee:	context for tee vendor
 *
 * Return: 0 for success; else the error code is returned
 *
 */
int cmdq_sec_deinit_context(struct cmdq_sec_tee_context *tee);

/**
 * cmdq_sec_allocate_wsm() - allocate the world share memory to pass message to tee
 * @tee:	context for tee vendor
 * @wsm_buffer:	world share memory buffer with parameters pass to tee
 * @size:	size to allocate
 *
 * Return: 0 for success; else the error code is returned
 *
 */
int cmdq_sec_allocate_wsm(struct cmdq_sec_tee_context *tee, void **wsm_buffer, u32 size);

/**
 * cmdq_sec_free_wsm() - free the world share memory
 * @tee:	context for tee vendor
 * @wsm_buffer:	world share memory buffer with parameters pass to tee
 *
 * Return: 0 for success; else the error code is returned
 *
 */
int cmdq_sec_free_wsm(struct cmdq_sec_tee_context *tee, void **wsm_buffer);

/**
 * cmdq_sec_open_session() - open session to the tee context
 * @tee:	context for tee vendor
 * @wsm_buffer:	world share memory buffer with parameters pass to tee
 *
 * Return: 0 for success; else the error code is returned
 *
 */
int cmdq_sec_open_session(struct cmdq_sec_tee_context *tee, void *wsm_buffer);

/**
 * cmdq_sec_close_session() - close session to the tee context
 * @tee:	context for tee vendor
 *
 * Return: 0 for success; else the error code is returned
 *
 */
int cmdq_sec_close_session(struct cmdq_sec_tee_context *tee);

/**
 * cmdq_sec_execute_session() - execute session to the tee context
 * @tee:	context for tee vendor
 * @cmd:	tee invoke cmd id
 * @timeout_ms:	timeout ms to current tee invoke cmd
 *
 * Return: 0 for success; else the error code is returned
 *
 */
int cmdq_sec_execute_session(struct cmdq_sec_tee_context *tee, u32 cmd, s32 timeout_ms);

#endif	/* __MTK_CMDQ_SEC_TEE_H__ */
