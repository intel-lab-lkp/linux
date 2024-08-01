/*
 * CGEB driver
 *
 * Copyright (c) 2024 Mary Strodl
 *
 * Based on code from Congatec AG and Sascha Hauer
 *
 * CGEB is a BIOS interface found on congatech modules. It consists of
 * code found in the BIOS memory map which is called in a ioctl like
 * fashion. This file contains the basic driver for this interface
 * which provides access to the GCEB interface and registers the child
 * devices like I2C busses and watchdogs.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/connector.h>
#include <linux/mfd/congatec-cgeb.h>
#include <linux/completion.h>

#include <generated/autoconf.h>

#define CGOS_BOARD_MAX_SIZE_ID_STRING 16

#define CGEB_VERSION_MAJOR 1

#define CGEB_GET_VERSION_MAJOR(v) (((unsigned long)(v)) >> 24)

/* CGEB Low Descriptor located in 0xc0000-0xfffff */
#define CGEB_LD_MAGIC "$CGEBLD$"

#pragma pack(push,4)

struct cgeb_low_desc {
	char magic[8];          /* descriptor magic string */
	u16 size;               /* size of this descriptor */
	u16 reserved;
	char bios_name[8];      /* BIOS name and revision "ppppRvvv" */
	u32 hi_desc_phys_addr;  /* phys addr of the high descriptor, can be 0 */
};

/* CGEB High Descriptor located in 0xfff00000-0xffffffff */
#ifdef CONFIG_X86_64
#define CGEB_HD_MAGIC "$CGEBQD$"
#else
#define CGEB_HD_MAGIC "$CGEBHD$"
#endif

struct cgeb_high_desc {
	char magic[8];          /* descriptor magic string */
	u16 size;               /* size of this descriptor */
	u16 reserved;
	u32 data_size;          /* CGEB data area size */
	u32 code_size;          /* CGEB code area size */
	u32 entry_rel;          /* CGEB entry point relative to start */
};

struct cgeb_far_ptr {
	void __user *off;
	u16 seg;
	u16 pad;
};

struct cgeb_fps {
	u32 size;               /* size of the parameter structure */
	u32 fct;                /* function number */
	struct cgeb_far_ptr data;       /* CGEB data area */
	void __user *cont;      /* private continuation pointer */
	void __user *subfps;    /* private sub function parameter
				 * structure pointer
				 */
	void __user *subfct;    /* sub function pointer */
	u32 status;             /* result codes of the function */
	u32 unit;               /* unit number or type */
	u32 pars[4];            /* input parameters */
	u32 rets[2];            /* return parameters */
	void __user *iptr;      /* input pointer */
	void __user *optr;      /* output pointer */
};

/* continuation status codes */
#define CGEB_SUCCESS            0
#define CGEB_NEXT               1
#define CGEB_DELAY              2
#define CGEB_NOIRQS             3

#define CGEB_DBG_STR        0x100
#define CGEB_DBG_HEX        0x101
#define CGEB_DBG_DEC        0x102

struct cgeb_map_mem {
	void *phys;             /* physical address */
	u32 size;               /* size in bytes */
	struct cgeb_far_ptr virt;
};

struct cgeb_map_mem_list {
	u32 count;              /* number of memory map entries */
	struct cgeb_map_mem entries[];
};

struct cgeb_boardinfo {
	u32 size;
	u32 flags;
	u32 classes;
	u32 primary_class;
	char board[CGOS_BOARD_MAX_SIZE_ID_STRING];
	/* optional */
	char vendor[CGOS_BOARD_MAX_SIZE_ID_STRING];
};

struct cgeb_i2c_info {
	u32 size;
	u32 type;
	u32 frequency;
	u32 maxFrequency;
};

#pragma pack(pop)

/* I2C Types */
#define CGEB_I2C_TYPE_UNKNOWN 0
#define CGEB_I2C_TYPE_PRIMARY 1
#define CGEB_I2C_TYPE_SMB     2
#define CGEB_I2C_TYPE_DDC     3
#define CGEB_I2C_TYPE_BC      4

struct cgeb_board_data {
	void __user *data;
	void __user *code;
	size_t code_size;
	u16 ds;
	struct cgeb_map_mem_list *map_mem;
	void __user *map_mem_user;
	struct platform_device **devices;
	int num_devices;

	#ifdef CONFIG_X86_64
	void (*entry)(void*, struct cgeb_fps *, struct cgeb_fps *, void*);
	#else
	/*
	 * entry points to a bimodal C style function that expects a far pointer
	 * to a fps. If cs is 0 then it does a near return, otherwise a far
	 * return. If we ever need a far return then we must not pass cs at all.
	 * parameters are removed by the caller.
	 */
	void __attribute__((regparm(0)))(*entry)(unsigned short,
			  struct cgeb_fps *, unsigned short);
	#endif
};

struct cgeb_call_user {
	void *optr;
	size_t size;
	void *callback_data;
	int (*callback)(void __user *, void *, void *);
};

enum cgeb_msg_type {
	CGEB_MSG_ACK = 0,
	CGEB_MSG_ERROR,
	CGEB_MSG_FPS,
	CGEB_MSG_MAPPED,
	CGEB_MSG_MAP,
	CGEB_MSG_CODE,
	CGEB_MSG_ALLOC,
	CGEB_MSG_ALLOC_CODE,
	CGEB_MSG_FREE,
	CGEB_MSG_MUNMAP,
	CGEB_MSG_CALL,
	CGEB_MSG_PING,
};

struct cgeb_msg {
	enum cgeb_msg_type type;
	union {
		struct cgeb_msg_mapped {
			void *virt;
		} mapped;
		struct cgeb_msg_fps {
			size_t optr_size;
			void *optr;
			struct cgeb_fps fps;
		} fps;
		struct cgeb_msg_code {
			size_t length;
			uint32_t entry_rel;
			void *data;
		} code;
		struct cgeb_msg_map {
			uint32_t phys;
			size_t size;
		} map;
	};
};

static char cgeb_helper_path[PATH_MAX] = "/sbin/cgeb-helper";

static struct cb_id cgeb_cn_id = {
	.idx = CN_IDX_CGEB,
	.val = CN_VAL_CGEB
};

enum cgeb_request_state {
	CGEB_REQ_IDLE = 0,
	CGEB_REQ_ACTIVE,
	CGEB_REQ_DONE,
};

static DEFINE_MUTEX(cgeb_lock);
struct cgeb_request {
	struct completion done;
	struct cgeb_msg *out;
	enum cgeb_request_state busy;
	int ack;
	int (*callback)(struct cgeb_msg*, void*);
	void *user;
};

#define CGEB_REQUEST_MAX 16
static struct cgeb_request cgeb_requests[CGEB_REQUEST_MAX];

struct cgeb_after_alloc_data {
	void *kernel;
	void __user **user;
	size_t length;
};

struct cgeb_map_data {
	void __user *user_list;
	struct cgeb_board_data *board;
};

static int cgeb_helper_start(void)
{
	char *envp[] = {
		NULL,
	};

	char *argv[] = {
		cgeb_helper_path,
		NULL,
	};

	return call_usermodehelper(cgeb_helper_path, argv, envp, UMH_WAIT_EXEC);
}

static int cgeb_request(struct cgeb_msg msg, struct cgeb_msg *out,
			int (*callback)(struct cgeb_msg*, void*), void *user)
{
	static int seq;
	struct cn_msg *wrapper;
	struct cgeb_request *req;
	int err, retries = 0;

	wrapper = (struct cn_msg*) kzalloc(sizeof(*wrapper) + sizeof(msg),
					   GFP_KERNEL);
	if (!wrapper)
		return -ENOMEM;

	memset(wrapper, 0, sizeof(*wrapper));
	memcpy(&wrapper->id, &cgeb_cn_id, sizeof(cgeb_cn_id));

	wrapper->len = sizeof(msg);
	wrapper->ack = get_random_u32();
	memcpy(wrapper + 1, &msg, sizeof(msg));

	mutex_lock(&cgeb_lock);

	req = &cgeb_requests[seq];

	if (req->busy) {
		mutex_unlock(&cgeb_lock);
		err = -EBUSY;
		goto out;
	}
	wrapper->seq = seq;
	req->busy = CGEB_REQ_ACTIVE;
	req->ack = wrapper->ack;
	req->out = out;
	req->callback = callback;
	req->user = user;

	err = cn_netlink_send(wrapper, 0, 0, GFP_KERNEL);
	if (err == -ESRCH) {
		err = cgeb_helper_start();
		if (err) {
			pr_err("failed to execute %s\n", cgeb_helper_path);
			pr_err("make sure that the cgeb helper is installed and"
			       " executable\n");
		} else {
			do {
				err = cn_netlink_send(wrapper, 0, 0,
						      GFP_KERNEL);
				if (err == -ENOBUFS)
					err = 0;
				if (err == -ESRCH)
					msleep(30);
			} while (err == -ESRCH && ++retries < 5);
		}
	} else if (err == -ENOBUFS)
		err = 0;

	kfree(wrapper);

	if (++seq >= CGEB_REQUEST_MAX)
		seq = 0;

	mutex_unlock(&cgeb_lock);

	if (err)
		goto out;

	/* Wait for a response to the request */
	err = wait_for_completion_interruptible_timeout(
		&req->done, msecs_to_jiffies(20000));
	if (err == 0) {
		pr_err("CGEB: Timed out running request of type %d!\n",
		       msg.type);
		err = -ETIMEDOUT;
	} else if (err > 0)
		err = 0;

	if (err)
		goto out;

	mutex_lock(&cgeb_lock);

	if (req->busy != CGEB_REQ_DONE) {
		pr_err("CGEB: BUG: Request is in a bad state?\n");
		err = -EINVAL;
	}

	req->busy = CGEB_REQ_IDLE;
	mutex_unlock(&cgeb_lock);
out:
	return err;
}

static void cgeb_cn_callback(struct cn_msg *msg, struct netlink_skb_parms *nsp)
{
	struct cgeb_request *req;
	if (!capable(CAP_SYS_ADMIN))
		return;

	if (msg->seq >= CGEB_REQUEST_MAX) {
		pr_err("CGEB: Impossible sequence number: %d! Ignoring\n",
		       msg->seq);
		return;
	}

	mutex_lock(&cgeb_lock);
	req = &cgeb_requests[msg->seq];

	if (!req->busy || req->ack != msg->ack) {
		pr_err("CGEB: Bad response to request %d! Ignoring\n",
		       msg->seq);
		mutex_unlock(&cgeb_lock);
		return;
	}

	if (msg->len != sizeof(*req->out)) {
		pr_err("CGEB: Bad response size for request %d!\n", msg->seq);
		mutex_unlock(&cgeb_lock);
		return;
	}

	pr_debug("Got a response to request %d!\n", msg->seq);

	memcpy(req->out, &msg->data, sizeof(*req->out));

	req->busy = CGEB_REQ_DONE;
	mutex_unlock(&cgeb_lock);

	if (req->callback)
		req->callback(req->out, req->user);

	complete(&req->done);

	return;
}

static int cgeb_copy_to_user(struct cgeb_msg *resp, void *user)
{
	struct cgeb_high_desc *high_desc;
	unsigned long uncopied;
	high_desc = user;
	uncopied = copy_to_user(resp->code.data, high_desc,
				high_desc->code_size);
	if (uncopied) {
		pr_err("CGEB: Couldn't copy code into userspace! %ld\n",
		       uncopied);
		return -ENOMEM;
	}
	return 0;
}

static int cgeb_upload_code(struct cgeb_high_desc *high_desc,
			    struct cgeb_board_data *board)
{
	struct cgeb_msg req = {0}, resp;
	size_t len = high_desc->code_size;
	int ret = 0;

	req.type = CGEB_MSG_ALLOC_CODE;
	req.code.length = len;
	pr_debug("CGEB: Allocating memory for code\n");
	ret = cgeb_request(req, &resp, cgeb_copy_to_user, high_desc);
	if (ret)
		goto out;
	if (resp.type != CGEB_MSG_CODE) {
		pr_err("CGEB: Bad response type for alloc: %d\n", resp.type);
		ret = -EINVAL;
		goto out;
	}

	board->code = resp.code.data;
	board->code_size = len;

	req.type = CGEB_MSG_CODE;
	req.code.data = resp.code.data;
	req.code.entry_rel = high_desc->entry_rel;
	req.code.length = len;

	pr_debug("CGEB: Uploading code\n");
	ret = cgeb_request(req, &resp, NULL, NULL);

	if (ret)
		goto out;

	/* Do stuff with response */
	if (resp.type != CGEB_MSG_ACK) {
		pr_err("CGEB: Failed to upload code! Got non-ack response!\n");
		ret = -EINVAL;
	}

out:
	return ret;
}

static unsigned short get_data_segment(void)
{
	unsigned short ret;

#ifdef CONFIG_X86_64
	ret = 0;
#else
	asm volatile("mov %%ds, %0\n"
			  : "=r"(ret)
			  :
			  : "memory"
	);
#endif

	return ret;
}

static int cgeb_after_call(struct cgeb_msg *resp, void *user)
{
	int ret = 0;
	int alloc_size;
	struct cgeb_call_user *data = user;
	if (!resp->fps.fps.optr)
		return ret;

	switch(resp->fps.fps.status) {
	case CGEB_NEXT:
	case CGEB_NOIRQS:
	case CGEB_DELAY:
	case CGEB_DBG_HEX:
	case CGEB_DBG_DEC:
		/* These lead to continuations, we don't need their memory */
		return ret;

	/* Everything else we could need */
	case CGEB_DBG_STR:
		data->size = alloc_size = strnlen_user(resp->fps.fps.optr, 1023);
		if (alloc_size > 1023) {
			data->size = 1023;
			alloc_size = data->size + 1;
		}
		/* Special case, because these come from program memory */
		data->optr = kzalloc(alloc_size, GFP_KERNEL);
		if (!data->optr)
			return -ENOMEM;
	}

	ret = copy_from_user(data->optr, resp->fps.fps.optr, data->size);

	if (ret) {
		pr_err("CGEB: Couldn't copy optr out of userspace! %d\n", ret);
		ret = -ENOMEM;
	}

	if (resp->fps.fps.status == CGEB_SUCCESS && data->callback) {
		data->callback(resp->fps.fps.optr, data->optr,
			       data->callback_data);
	}

	return ret;
}

static int cgeb_after_alloc(struct cgeb_msg *resp, void *user)
{
	int ret;
	struct cgeb_after_alloc_data *data = user;

	ret = copy_to_user(resp->code.data, data->kernel, data->length);

	if (ret) {
		pr_err("CGEB: Couldn't copy iptr into userspace! %d\n", ret);

		ret = -ENOMEM;
	}

	*data->user = resp->code.data;

	return ret;
}

static int cgeb_get_user_ptr(void *kernel, void __user **user, size_t length)
{
	struct cgeb_msg req = {0}, resp;
	struct cgeb_after_alloc_data data;

	data.kernel = kernel;
	data.user = user;
	data.length = length;

	req.type = CGEB_MSG_ALLOC;
	req.code.length = length;
	return cgeb_request(req, &resp, cgeb_after_alloc, &data);
}

static void __user *cgeb_user_alloc(size_t length)
{
	int ret;
	struct cgeb_msg req = {0}, resp;
	req.type = CGEB_MSG_ALLOC;
	req.code.length = length;
	ret = cgeb_request(req, &resp, NULL, NULL);
	if (ret)
		return NULL;

	return resp.code.data;
}

static int cgeb_user_free(void __user *memory)
{
	struct cgeb_msg req = {0}, resp;

	req.type = CGEB_MSG_FREE;
	req.code.data = memory;
	return cgeb_request(req, &resp, NULL, NULL);
}

/*
 * cgeb_call - invoke CGEB BIOS call.
 *
 * @board:     board context data
 * @p:         CGEB parameters for this call
 * @fct:       CGEB function code
 * @return:    0 on success or negative error code on failure.
 *
 * Call the CGEB BIOS code with the given parameters.
 */
int cgeb_call(struct cgeb_board_data *board,
	      struct cgeb_function_parameters *p, cgeb_function_t fct)
{
	struct cgeb_fps *fps;
	struct cgeb_msg req = {0}, resp;
	struct cgeb_call_user user_data, user_data_clone;
	int i;
	int err;
	void __user *iptr_user = NULL;
	void __user *optr_user = NULL;

	if ((p->optr && !p->optr_size) || (p->iptr && !p->iptr_size) ||
	    (!p->optr && p->optr_size) || (!p->iptr && p->iptr_size)) {
		pr_warn("CGEB: called with impossible iptr/optr\n");
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));
	fps = &req.fps.fps;

	fps->size = sizeof(fps);
	fps->fct = fct;
	fps->data.off = board->data;
	fps->data.seg = board->ds;
	fps->data.pad = 0;
	fps->status = 0;
	fps->cont = fps->subfct = fps->subfps = 0;
	fps->unit = p->unit;
	for (i = 0; i < 4; i++)
		fps->pars[i] = p->pars[i];
	if (p->iptr) {
		if (!p->iptr_size)
			return -EINVAL;
		err = cgeb_get_user_ptr(p->iptr, &iptr_user, p->iptr_size);
		fps->iptr = iptr_user;
		if (err)
			return err;
	}
	req.fps.optr_size = p->optr_size;
	user_data.optr = p->optr;
	user_data.size = p->optr_size;

	user_data.callback = p->callback;
	user_data.callback_data = p->callback_data;

	while (1) {
		pr_debug("CGEB: CGEB: ->  size %02x, fct %02x, data %04x:%p\n",
			 fps->size, fps->fct, fps->data.seg, fps->data.off);

		user_data_clone = user_data;
		req.type = CGEB_MSG_CALL;
		req.fps.fps = *fps;
		err = cgeb_request(req, &resp, cgeb_after_call,
				   &user_data_clone);
		if (err)
			goto out;
		if (resp.type != CGEB_MSG_FPS) {
			err = -EINVAL;
			goto out;
		}
		fps = &resp.fps.fps;
		/* Don't allocate another optr */
		req.fps.optr_size = 0;

		if (resp.fps.optr) {
			optr_user = resp.fps.optr;
		}

		/* DBG_STR causes an allocation, don't overwrite our original */
		if (fps->status != CGEB_DBG_STR)
			user_data = user_data_clone;

		pr_debug("CGEB: CGEB: <-  size %02x, fct %02x, data %04x:%p\n",
			 fps->size, fps->fct, fps->data.seg, fps->data.off);

		switch (fps->status) {
		case CGEB_SUCCESS:
			goto out;
		case CGEB_NEXT:
			break;  /* simply call again */
		case CGEB_NOIRQS:
			__set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(0);
			break;
		case CGEB_DELAY:
			usleep_range(fps->rets[0], fps->rets[0] + 1000);
			break;
		case CGEB_DBG_STR:
			if (fps->optr) {
				pr_debug("CGEB (dbg): %s\n", (char *)user_data_clone.optr);
				kfree(user_data_clone.optr);
			}
			break;
		case CGEB_DBG_HEX:
			pr_debug("CGEB (dbg): 0x%08x\n", fps->rets[0]);
			break;
		case CGEB_DBG_DEC:
			pr_debug("CGEB (dbg): %d\n", fps->rets[0]);
			break;

		default:
			/* unknown continuation code */
			err = -EINVAL;
			goto out;
		}
	}
out:
	if (iptr_user)
		cgeb_user_free(iptr_user);
	if (optr_user)
		cgeb_user_free(optr_user);
	for (i = 0; i < 2; i++)
		p->rets[i] = fps->rets[i];
	p->optr = user_data.optr;

	return err;
}
EXPORT_SYMBOL_GPL(cgeb_call);

/*
 * cgeb_call_simple - convenience wrapper for cgeb_call
 *
 * @board:     board context data
 * @p:         CGEB parameters for this call
 * @fct:       CGEB function code
 * @return:    0 on success or negative error code on failure.
 *
 * Call the CGEB BIOS code with the given parameters.
 */
int cgeb_call_simple(struct cgeb_board_data *board,
		     cgeb_function_t fct, u32 unit,
		     void *optr, size_t optr_size, u32 *result)
{
	struct cgeb_function_parameters p;
	unsigned int ret;

	memset(&p, 0, sizeof(p));
	p.optr_size = optr_size;
	p.unit = unit;
	p.optr = optr;

	ret = cgeb_call(board, &p, fct);

	if (result)
		*result = p.rets[0];

	return ret;
}
EXPORT_SYMBOL_GPL(cgeb_call_simple);

static void *cgeb_find_magic(void *_mem, size_t len, char *magic)
{
	u32 magic0 = ((u32 *)magic)[0];
	u32 magic1 = ((u32 *)magic)[1];
	int i = 0;

	while (i < len) {
		u32 *mem = _mem + i;

		if (mem[0] == magic0 && mem[1] == magic1)
			return mem;
		i += 16;
	}

	return NULL;
}


static int cgeb_overwrite_map(struct cgeb_msg *req, void *user)
{
	struct cgeb_board_data *board = (struct cgeb_board_data*) user;
	struct cgeb_map_mem_list *pmm = board->map_mem;
	int err;
	err = copy_to_user(board->map_mem_user, board->map_mem,
			   sizeof(pmm->entries[0]) * pmm->count + sizeof(*pmm));
	if (err) {
		pr_err("CGEB: Couldn't copy map_mem into userspace!\n");
		return -ENOMEM;
	}
	return 0;
}

static void cgeb_unmap_memory(struct cgeb_board_data *board)
{
	struct cgeb_map_mem_list *pmm;
	struct cgeb_map_mem *pmme;
	struct cgeb_msg req = {0}, res;
	unsigned long i;

	if (!board->map_mem)
		return;

	req.type = CGEB_MSG_MUNMAP;

	pmm = board->map_mem;
	pmme = pmm->entries;
	pr_debug("CGEB: Unmapping %d pages\n", pmm->count);
	for (i = 0; i < pmm->count; i++, pmme++) {
		pmme->virt.seg = 0;
		if (pmme->virt.off) {
			req.code.data = pmme->virt.off;
			req.code.length = pmme->size;
			pmme->virt.off = 0;
			cgeb_request(req, &res, cgeb_overwrite_map, board);
		}
	}
	board->map_mem_user = NULL;
	kfree(board->map_mem);
}

static int cgeb_deref_map(void __user *userspace, void *optr, void *user)
{
	struct cgeb_board_data *board = (struct cgeb_board_data*) user;
	struct cgeb_map_mem_list *list = optr;
	size_t size;
	int uncopied;
	size = sizeof(*list) + sizeof(list->entries[0]) * list->count;

	board->map_mem = (struct cgeb_map_mem_list*) kzalloc(size, GFP_KERNEL);
	if (!board->map_mem) {
		pr_err("Couldn't allocate map_mem\n");
		return -ENOMEM;
	}

	board->map_mem_user = userspace;
	uncopied = copy_from_user(board->map_mem, board->map_mem_user, size);
	if (uncopied) {
		pr_err("Couldn't copy map_mem from map_mem_user!\n");
		kfree(board->map_mem);
		board->map_mem = NULL;
		return -ENOMEM;
	}
	return 0;
}


static int cgeb_map_memory(struct cgeb_board_data *board)
{
	struct cgeb_map_mem_list *pmm;
	struct cgeb_map_mem *pmme;
	struct cgeb_msg req = {0}, res;
	struct cgeb_function_parameters fps = {0};
	int i;
	int ret;

	fps.optr = &board->map_mem;
	fps.optr_size = sizeof(*board->map_mem);
	fps.callback = cgeb_deref_map;
	fps.callback_data = board;
	ret = cgeb_call(board, &fps, CgebMapGetMem);
	if (ret)
		return ret;

	if (!board->map_mem)
		return 0;

	pmm = board->map_mem;
	pmme = pmm->entries;

	req.type = CGEB_MSG_MAP;

	pr_debug("CGEB: Memory Map with %d entries\n", pmm->count);

	for (i = 0; i < pmm->count; i++, pmme++) {
		pr_debug("CGEB: Memory map entry phys=%p, size=%08x\n",
				pmme->phys, pmme->size);
		if (pmme->phys && pmme->size) {
			/* We only want to look at the lower 32 bits */
			req.map.phys = (size_t) pmme->phys & 0xffffffff;
			req.map.size = pmme->size;
			ret = cgeb_request(req, &res, NULL, NULL);
			if (ret)
				return ret;
			if (res.type != CGEB_MSG_MAPPED) {
				pr_err("CGEB: Invalid map response!\n");
				return -EINVAL;
			}
			pmme->virt.off = res.mapped.virt;
		} else {
			pmme->virt.off = 0;
		}

		pmme->virt.seg = (pmme->virt.off) ? board->ds : 0;

		pr_debug("CGEB:   Map phys %p, size %08x, virt %04x:%p\n",
			pmme->phys, pmme->size, pmme->virt.seg,
			pmme->virt.off);
	}

	cgeb_request(req, &res, cgeb_overwrite_map, board);
	return cgeb_call_simple(board, CgebMapChanged, 0, NULL, 0, NULL);
}

static void cgeb_munmap(void __user *ptr, size_t size)
{
	struct cgeb_msg req = {0}, res;

	req.type = CGEB_MSG_MUNMAP;
	req.code.data = ptr;
	req.code.length = size;
	cgeb_request(req, &res, NULL, NULL);
}

static struct cgeb_board_data *cgeb_open(resource_size_t base, u32 len)
{
	u32 dw;
	struct cgeb_boardinfo pbi;
	struct cgeb_low_desc *low_desc;
	struct cgeb_high_desc *high_desc = NULL;
	u32 high_desc_phys;
	u32 high_desc_len;
	void __iomem *pcur, *high_desc_virt;
	int ret;

	struct cgeb_board_data *board;

	board = kzalloc(sizeof(*board), GFP_KERNEL);
	if (!board)
		return NULL;

	pcur = ioremap_cache(base, len);
	if (!pcur)
		goto err_kfree;

	/* look for the CGEB descriptor */
	low_desc = cgeb_find_magic(pcur, len, CGEB_LD_MAGIC);
	if (!low_desc)
		goto err_kfree;

	pr_debug("CGEB: Found CGEB_LD_MAGIC\n");

	if (low_desc->size < sizeof(struct cgeb_low_desc) - sizeof(long))
		goto err_kfree;

	if (low_desc->size >= sizeof(struct cgeb_low_desc)
			&& low_desc->hi_desc_phys_addr)
		high_desc_phys = low_desc->hi_desc_phys_addr;
	else
		high_desc_phys = 0xfff00000;

	high_desc_len = (unsigned int) -(int)high_desc_phys;

	pr_debug("CGEB: Looking for CGEB hi desc between phys 0x%x and 0x%x\n",
		high_desc_phys, -1);

	high_desc_virt = ioremap_cache(high_desc_phys, high_desc_len);
	if (!high_desc_virt)
		goto err_kfree;

	pr_debug("CGEB: Looking for CGEB hi desc between virt 0x%p and 0x%p\n",
		high_desc_virt, high_desc_virt + high_desc_len - 1);

	high_desc = cgeb_find_magic(high_desc_virt, high_desc_len,
					CGEB_HD_MAGIC);
	if (!high_desc)
		goto err_iounmap;

	pr_debug("CGEB: Found CGEB_HD_MAGIC\n");

	if (high_desc->size < sizeof(struct cgeb_high_desc))
		goto err_iounmap;

	pr_debug("CGEB: data_size %u, code_size %u, entry_rel %u\n",
		high_desc->data_size, high_desc->code_size, high_desc->entry_rel);

	ret = cgeb_upload_code(high_desc, board);
	if (ret) {
		pr_err("CGEB: Couldn't upload code to helper: %d\n", ret);
		goto err_munmap;
	}

	board->ds = get_data_segment();

	ret = cgeb_call_simple(board, CgebGetCgebVersion, 0, NULL, 0, &dw);
	if (ret)
		goto err_munmap;

	if (CGEB_GET_VERSION_MAJOR(dw) != CGEB_VERSION_MAJOR)
		goto err_munmap;

	pr_debug("CGEB: BIOS interface revision: %d.%d\n",
			dw >> 16, dw & 0xffff);

	if (high_desc->data_size)
		dw = high_desc->data_size;
	else
		ret = cgeb_call_simple(board, CgebGetDataSize, 0, NULL, 0, &dw);

	if (!ret && dw) {
		board->data = cgeb_user_alloc(high_desc->data_size);
		if (!board->data)
			goto err_munmap;
	}

	ret = cgeb_call_simple(board, CgebOpen, 0, NULL, 0, NULL);
	if (ret)
		goto err_free_data;

	pr_debug("CGEB: Mapping memory\n");
	ret = cgeb_map_memory(board);
	if (ret)
		goto err_free_map;
	pr_debug("CGEB: Memory is mapped, getting board info\n");

	ret = cgeb_call_simple(board, CgebBoardGetInfo, 0, &pbi,
			       sizeof(pbi), NULL);
	if (ret)
		goto err_free_map;

	pr_info("CGEB: Board name: %c%c%c%c\n",
			pbi.board[0], pbi.board[1],
			pbi.board[2], pbi.board[3]);

	iounmap(high_desc_virt);

	return board;

err_free_map:
	cgeb_unmap_memory(board);
err_free_data:
	cgeb_user_free(board->data);
err_munmap:
	cgeb_munmap(board->code, board->code_size);
err_iounmap:
	iounmap(high_desc_virt);
err_kfree:
	kfree(board);
	return NULL;
}

static void cgeb_close(struct cgeb_board_data *board)
{
	cgeb_call_simple(board, CgebClose, 0, NULL, 0, NULL);

	cgeb_unmap_memory(board);

	cgeb_user_free(board->data);

	cgeb_munmap(board->code, board->code_size);
}

static unsigned long cgeb_i2c_get_type(struct cgeb_board_data *brd, int unit)
{
	struct cgeb_i2c_info info;
	int ret;

	ret = cgeb_call_simple(brd, CgebI2CGetInfo, unit, &info,
			       sizeof(info), NULL);
	if (ret)
		return ret;
	return info.type;
}

static struct cgeb_board_data *cgeb_board;

static int __init cgeb_init(void)
{
	struct cgeb_board_data *board;
	resource_size_t base;
	int i, ret, req;
	struct cgeb_pdata pdata;
	u32 i2c_count, watchdog_count;
	int num_devices = 0;

	for (req = 0; req < CGEB_REQUEST_MAX; ++req) {
		init_completion(&cgeb_requests[req].done);
	}

	ret = cn_add_callback(&cgeb_cn_id, "cgeb", cgeb_cn_callback);
	if (ret)
		return ret;

	pr_debug("CGEB: Opening board\n");
	for (base = 0xf0000; base >= 0xc0000; base -= 0x10000) {
		board = cgeb_open(base, 0x10000);
		if (board)
			break;
	}

	if (!board) {
		ret = -ENODEV;
		goto err_no_board;
	}

	cgeb_board = board;

	pdata.board = board;

	cgeb_call_simple(board, CgebI2CCount, 0, NULL, 0, &i2c_count);
	cgeb_call_simple(board, CgebWDogCount, 0, NULL, 0, &watchdog_count);

	board->num_devices = i2c_count + watchdog_count;
	board->devices = kzalloc(sizeof(void *) * (board->num_devices),
			 GFP_KERNEL);
	if (!board->devices) {
		ret = -ENOMEM;
		goto err_out;
	}

	for (i = 0; i < i2c_count; i++) {
		ret = cgeb_i2c_get_type(board, i);
		if (ret != CGEB_I2C_TYPE_PRIMARY)
			continue;

		 pdata.unit = i;

		 board->devices[num_devices] =
			platform_device_register_data(NULL, "cgeb-i2c", i,
					&pdata, sizeof(pdata));
		 num_devices++;
	}

	for (i = 0; i < watchdog_count; i++) {
		board->devices[num_devices] =
			platform_device_register_data(NULL, "cgeb-watchdog", i,
					&pdata, sizeof(pdata));
		pdata.unit = i;

		num_devices++;
	}

	return 0;
err_out:
	cgeb_close(board);
	kfree(board);

err_no_board:
	cn_del_callback(&cgeb_cn_id);

	return ret;
}

static void __exit cgeb_exit(void)
{
	struct cgeb_board_data *board = cgeb_board;
	int i;

	for (i = 0; i < board->num_devices; i++)
		if (board->devices[i])
			platform_device_unregister(board->devices[i]);

	cgeb_close(board);

	kfree(board->devices);
	kfree(board);
	cn_del_callback(&cgeb_cn_id);
}

module_init(cgeb_init);
module_exit(cgeb_exit);

MODULE_AUTHOR("Mary Strodl <mstrodl@csh.rit.edu>");
MODULE_DESCRIPTION("CGEB driver");
MODULE_LICENSE("GPL");
