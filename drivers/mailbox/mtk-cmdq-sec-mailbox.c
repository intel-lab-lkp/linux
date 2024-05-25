// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox_controller.h>
#include <linux/of_platform.h>
#include <linux/sched/clock.h>
#include <linux/timer.h>

#include <linux/mailbox/mtk-cmdq-mailbox.h>
#include <linux/mailbox/mtk-cmdq-sec-mailbox.h>

#define CMDQ_THR_EXEC_CNT_PA		(0x28)

#define CMDQ_TIMEOUT_DEFAULT		(1000)

#define CMDQ_WFE_CMD(event)		(0x2000000080008001ULL | ((u64)(event) << 32))
#define CMDQ_EOC_CMD			(0x4000000000000001ULL)
#define CMDQ_JUMP_CMD(addr, shift)	(0x1000000100000000ULL | ((addr) >> (shift)))

struct cmdq_sec_task {
	struct cmdq_task		task;

	/* secure CMDQ */
	bool				reset_exec;
	u32				wait_cookie;
	s32				scenario;
	u64				trigger;
	u64				exec_time;
	struct work_struct		exec_work;
};

struct cmdq_sec_thread {
	struct cmdq_thread		thread;

	/* secure CMDQ */
	struct device			*dev;
	u32				idx;
	struct timer_list		timeout;
	u32				timeout_ms;
	struct work_struct		timeout_work;
	u32				wait_cookie;
	u32				next_cookie;
	u32				task_cnt;
	struct workqueue_struct		*task_exec_wq;
};

/**
 * struct cmdq_sec_context - CMDQ secure context structure.
 * @tgid: tgid of process context.
 * @state: state of inter-world communicatiom.
 * @iwc_msg: buffer for inter-world communicatiom message.
 * @tee_ctx: context structure for tee vendor.
 *
 * Note it is not global data, each process has its own cmdq_sec_context.
 */
struct cmdq_sec_context {
	u32				tgid;
	enum cmdq_iwc_state_enum	state;
	void				*iwc_msg;
	struct cmdq_sec_tee_context	tee_ctx;
};

/**
 * struct cmdq_sec_shared_mem - shared memory between normal and secure world
 * @va: virtual address of share memory.
 * @pa: physical address of share memory.
 * @size: size of share memory.
 *
 */
struct cmdq_sec_shared_mem {
	void				*va;
	dma_addr_t			pa;
	u32				size;
};

struct cmdq_sec {
	struct device			dev;
	const struct gce_sec_plat	*pdata;
	void __iomem			*base;
	phys_addr_t			base_pa;
	struct cmdq_sec_thread		*sec_thread;
	struct cmdq_pkt			clt_pkt;

	atomic_t			path_res;
	struct cmdq_sec_shared_mem	*shared_mem;
	struct cmdq_sec_context		*context;

	struct workqueue_struct		*timeout_wq;
	u64				sec_invoke;
	u64				sec_done;

	struct mbox_client		notify_clt;
	struct mbox_chan		*notify_chan;
	bool				notify_run;
	struct work_struct		irq_notify_work;
	struct workqueue_struct		*notify_wq;
	/* mutex for cmdq_sec_thread excuting cmdq_sec_task */
	struct mutex			exec_lock;
};

static atomic_t cmdq_path_res = ATOMIC_INIT(0);

static int cmdq_sec_task_submit(struct cmdq_sec *cmdq, struct cmdq_sec_task *sec_task,
				const u32 iwc_cmd, const u32 thrd_idx);

u16 cmdq_sec_get_eof_event_id(struct mbox_chan *chan)
{
	struct cmdq_thread *thread = chan->con_priv;
	struct cmdq_sec_thread *sec_thread = container_of(thread, struct cmdq_sec_thread, thread);
	struct cmdq_sec *cmdq = container_of(sec_thread->dev, struct cmdq_sec, dev);

	return (u16)cmdq->pdata->cmdq_event;
}
EXPORT_SYMBOL_GPL(cmdq_sec_get_eof_event_id);

dma_addr_t cmdq_sec_get_exec_cnt_addr(struct mbox_chan *chan)
{
	struct cmdq_thread *thread = chan->con_priv;
	struct cmdq_sec_thread *sec_thread = container_of(thread, struct cmdq_sec_thread, thread);
	struct cmdq_sec *cmdq = container_of(sec_thread->dev, struct cmdq_sec, dev);

	if (!cmdq->shared_mem) {
		dev_err(&cmdq->dev, "%s share memory not ready!", __func__);
		return 0;
	}

	dev_dbg(&cmdq->dev, "%s %d: thread:%u gce:%#lx",
		__func__, __LINE__, sec_thread->idx,
			(unsigned long)cmdq->base_pa);

	return cmdq->base_pa + CMDQ_THR_BASE +
		CMDQ_THR_SIZE * sec_thread->idx + CMDQ_THR_EXEC_CNT_PA;
}
EXPORT_SYMBOL_GPL(cmdq_sec_get_exec_cnt_addr);

dma_addr_t cmdq_sec_get_cookie_addr(struct mbox_chan *chan)
{
	struct cmdq_thread *thread = chan->con_priv;
	struct cmdq_sec_thread *sec_thread = container_of(thread, struct cmdq_sec_thread, thread);
	struct cmdq_sec *cmdq = container_of(sec_thread->dev, struct cmdq_sec, dev);

	if (!cmdq->shared_mem) {
		dev_err(&cmdq->dev, "%s share memory not ready!", __func__);
		return 0;
	}

	dev_dbg(&cmdq->dev, "%s %d: thread:%u gce:%#lx",
		__func__, __LINE__, sec_thread->idx,
			(unsigned long)cmdq->base_pa);

	return cmdq->shared_mem->pa +
		CMDQ_SEC_SHARED_THR_CNT_OFFSET + sec_thread->idx * sizeof(u32);
}
EXPORT_SYMBOL_GPL(cmdq_sec_get_cookie_addr);

static u32 cmdq_sec_get_cookie(struct cmdq_sec *cmdq, u32 idx)
{
	return *(u32 *)(cmdq->shared_mem->va +
		CMDQ_SEC_SHARED_THR_CNT_OFFSET + idx * sizeof(u32));
}

static void cmdq_sec_task_done(struct cmdq_sec_task *sec_task, int sta)
{
	struct cmdq_cb_data data;

	data.sta = sta;
	data.pkt = sec_task->task.pkt;

	pr_debug("%s sec_task:%p pkt:%p err:%d",
		 __func__, sec_task, sec_task->task.pkt, sta);

	mbox_chan_received_data(sec_task->task.thread->chan, &data);

	list_del_init(&sec_task->task.list_entry);
	kfree(sec_task);
}

static bool cmdq_sec_irq_handler(struct cmdq_sec_thread *sec_thread,
				 const u32 cookie, const int err)
{
	struct cmdq_sec_task *sec_task;
	struct cmdq_task *task, *temp, *cur_task = NULL;
	struct cmdq_sec *cmdq = container_of(sec_thread->dev, struct cmdq_sec, dev);
	unsigned long flags;
	int done;

	spin_lock_irqsave(&sec_thread->thread.chan->lock, flags);
	if (sec_thread->wait_cookie <= cookie)
		done = cookie - sec_thread->wait_cookie + 1;
	else if (sec_thread->wait_cookie == (cookie + 1) % CMDQ_MAX_COOKIE_VALUE)
		done = 0;
	else
		done = CMDQ_MAX_COOKIE_VALUE - sec_thread->wait_cookie + 1 + cookie + 1;

	list_for_each_entry_safe(task, temp, &sec_thread->thread.task_busy_list, list_entry) {
		if (!done)
			break;

		sec_task = container_of(task, struct cmdq_sec_task, task);
		cmdq_sec_task_done(sec_task, err);

		if (sec_thread->task_cnt)
			sec_thread->task_cnt -= 1;

		done--;
	}

	cur_task = list_first_entry_or_null(&sec_thread->thread.task_busy_list,
					    struct cmdq_task, list_entry);
	if (err && cur_task) {
		spin_unlock_irqrestore(&sec_thread->thread.chan->lock, flags);

		sec_task = container_of(cur_task, struct cmdq_sec_task, task);

		/* for error task, cancel, callback and done */
		cmdq_sec_task_submit(cmdq, sec_task, CMD_CMDQ_IWC_CANCEL_TASK,
				     sec_thread->idx);

		cmdq_sec_task_done(sec_task, err);

		spin_lock_irqsave(&sec_thread->thread.chan->lock, flags);

		task = list_first_entry_or_null(&sec_thread->thread.task_busy_list,
						struct cmdq_task, list_entry);
		if (cur_task == task)
			cmdq_sec_task_done(sec_task, err);
		else
			dev_err(&cmdq->dev, "task list changed");

		/*
		 * error case stop all task for secure,
		 * since secure tdrv always remove all when cancel
		 */
		while (!list_empty(&sec_thread->thread.task_busy_list)) {
			cur_task = list_first_entry(&sec_thread->thread.task_busy_list,
						    struct cmdq_task, list_entry);

			sec_task = container_of(cur_task, struct cmdq_sec_task, task);
			cmdq_sec_task_done(sec_task, -ECONNABORTED);
		}
	} else if (err) {
		dev_dbg(&cmdq->dev, "error but all task done, check notify callback");
	}

	if (list_empty(&sec_thread->thread.task_busy_list)) {
		sec_thread->wait_cookie = 0;
		sec_thread->next_cookie = 0;
		sec_thread->task_cnt = 0;
		__raw_writel(0, (void __iomem *)cmdq->shared_mem->va +
			     CMDQ_SEC_SHARED_THR_CNT_OFFSET +
			     sec_thread->idx * sizeof(u32));
		spin_unlock_irqrestore(&sec_thread->thread.chan->lock, flags);
		del_timer(&sec_thread->timeout);
		return true;
	}

	sec_thread->wait_cookie = cookie % CMDQ_MAX_COOKIE_VALUE + 1;

	mod_timer(&sec_thread->timeout, jiffies + msecs_to_jiffies(sec_thread->timeout_ms));
	spin_unlock_irqrestore(&sec_thread->thread.chan->lock, flags);

	return false;
}

static void cmdq_sec_irq_notify_work(struct work_struct *work_item)
{
	struct cmdq_sec *cmdq = container_of(work_item, struct cmdq_sec, irq_notify_work);
	int i;

	mutex_lock(&cmdq->exec_lock);

	for (i = 0; i <= cmdq->pdata->secure_thread_nr; i++) {
		struct cmdq_sec_thread *sec_thread = &cmdq->sec_thread[i];
		u32 cookie = cmdq_sec_get_cookie(cmdq, sec_thread->idx);

		if (cookie < sec_thread->wait_cookie || !sec_thread->task_cnt)
			continue;

		cmdq_sec_irq_handler(sec_thread, cookie, 0);
	}

	mutex_unlock(&cmdq->exec_lock);
}

static void cmdq_sec_irq_notify_callback(struct mbox_client *cl, void *mssg)
{
	struct cmdq_cb_data *data = (struct cmdq_cb_data *)mssg;
	struct cmdq_sec *cmdq = container_of(data->pkt, struct cmdq_sec, clt_pkt);

	if (work_pending(&cmdq->irq_notify_work)) {
		dev_dbg(&cmdq->dev, "%s last notify callback working", __func__);
		return;
	}

	queue_work(cmdq->notify_wq, &cmdq->irq_notify_work);
}

static int cmdq_sec_irq_notify_start(struct cmdq_sec *cmdq)
{
	int err;
	dma_addr_t dma_addr;
	u64 *inst = NULL;

	if (cmdq->notify_run)
		return 0;

	cmdq->notify_clt.dev = cmdq->pdata->mbox->dev;
	cmdq->notify_clt.rx_callback = cmdq_sec_irq_notify_callback;
	cmdq->notify_clt.tx_block = false;
	cmdq->notify_clt.knows_txdone = true;
	cmdq->notify_chan = mbox_request_channel(&cmdq->notify_clt, 0);
	if (IS_ERR(cmdq->notify_chan)) {
		dev_err(&cmdq->dev, "failed to request channel\n");
		return -ENODEV;
	}

	cmdq->clt_pkt.va_base = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!cmdq->clt_pkt.va_base)
		return -ENOMEM;

	cmdq->clt_pkt.buf_size = PAGE_SIZE;

	dma_addr = dma_map_single(cmdq->pdata->mbox->dev, cmdq->clt_pkt.va_base,
				  cmdq->clt_pkt.buf_size, DMA_TO_DEVICE);
	if (dma_mapping_error(cmdq->pdata->mbox->dev, dma_addr)) {
		dev_err(cmdq->pdata->mbox->dev, "dma map failed, size=%lu\n", PAGE_SIZE);
		kfree(cmdq->clt_pkt.va_base);
		return -ENOMEM;
	}
	cmdq->clt_pkt.pa_base = dma_addr;

	INIT_WORK(&cmdq->irq_notify_work, cmdq_sec_irq_notify_work);

	/* generate irq notify loop command */
	inst = (u64 *)cmdq->clt_pkt.va_base;
	*inst = CMDQ_WFE_CMD(cmdq->pdata->cmdq_event);
	inst++;
	*inst = CMDQ_EOC_CMD;
	inst++;
	*inst = CMDQ_JUMP_CMD(cmdq->clt_pkt.pa_base, cmdq->pdata->shift);
	inst++;
	cmdq->clt_pkt.cmd_buf_size += CMDQ_INST_SIZE * 3;
	cmdq->clt_pkt.loop = true;

	dma_sync_single_for_device(cmdq->pdata->mbox->dev,
				   cmdq->clt_pkt.pa_base,
				   cmdq->clt_pkt.cmd_buf_size,
				   DMA_TO_DEVICE);
	err = mbox_send_message(cmdq->notify_chan, &cmdq->clt_pkt);
	mbox_client_txdone(cmdq->notify_chan, 0);
	if (err < 0) {
		dev_err(&cmdq->dev, "%s failed:%d", __func__, err);
		dma_unmap_single(cmdq->pdata->mbox->dev, cmdq->clt_pkt.pa_base,
				 cmdq->clt_pkt.buf_size, DMA_TO_DEVICE);
		kfree(cmdq->clt_pkt.va_base);
		mbox_free_channel(cmdq->notify_chan);

		return err;
	}

	cmdq->notify_run = true;
	dev_dbg(&cmdq->dev, "%s success!", __func__);

	return 0;
}

static int cmdq_sec_session_init(struct cmdq_sec_context *context)
{
	int err = 0;

	if (context->state >= IWC_SES_OPENED) {
		pr_debug("session opened:%u", context->state);
		return 0;
	}

	if (context->state == IWC_INIT) {
		err = cmdq_sec_init_context(&context->tee_ctx);
		if (err)
			return err;
		context->state = IWC_CONTEXT_INITED;
	}

	if (context->state == IWC_CONTEXT_INITED) {
		if (context->iwc_msg) {
			pr_err("iwcMessage not NULL:%p", context->iwc_msg);
			return -EINVAL;
		}

		err = cmdq_sec_allocate_wsm(&context->tee_ctx, &context->iwc_msg,
					    sizeof(struct iwc_cmdq_message_t));
		if (err)
			return err;

		context->state = IWC_WSM_ALLOCATED;
	}

	if (context->state == IWC_WSM_ALLOCATED) {
		err = cmdq_sec_open_session(&context->tee_ctx, context->iwc_msg);
		if (err)
			return err;

		context->state = IWC_SES_OPENED;
	}

	return 0;
}

static int cmdq_sec_fill_iwc_msg(struct cmdq_sec_context *context,
				 struct cmdq_sec_task *sec_task, u32 thrd_idx)
{
	struct iwc_cmdq_message_t *iwc_msg = NULL;
	struct cmdq_sec_data *data = (struct cmdq_sec_data *)sec_task->task.pkt->sec_data;
	u32 size = 0, *instr;

	iwc_msg = (struct iwc_cmdq_message_t *)context->iwc_msg;

	if (sec_task->task.pkt->cmd_buf_size + 4 * CMDQ_INST_SIZE > CMDQ_TZ_CMD_BLOCK_SIZE) {
		pr_err("sec_task:%p size:%zu > %u",
		       sec_task, sec_task->task.pkt->cmd_buf_size, CMDQ_TZ_CMD_BLOCK_SIZE);
		return -EFAULT;
	}

	if (thrd_idx == CMDQ_INVALID_THREAD) {
		iwc_msg->command.cmd_size = 0;
		iwc_msg->command.metadata.addr_list_length = 0;
		return -EINVAL;
	}

	iwc_msg->command.thread = thrd_idx;
	iwc_msg->command.scenario = sec_task->scenario;
	size = sec_task->task.pkt->cmd_buf_size;
	memcpy(iwc_msg->command.va_base, sec_task->task.pkt->va_base, size);
	iwc_msg->command.cmd_size += size;

	instr = &iwc_msg->command.va_base[iwc_msg->command.cmd_size / 4 - 4];
	/* Remove IRQ_EN in EOC */
	if (*(u64 *)instr == CMDQ_EOC_CMD)
		instr[0] = 0;
	else
		pr_err("%s %d: find EOC failed: %#x %#x",
		       __func__, __LINE__, instr[1], instr[0]);

	iwc_msg->command.wait_cookie = sec_task->wait_cookie;
	iwc_msg->command.reset_exec = sec_task->reset_exec;

	if (data->addr_metadata_cnt) {
		iwc_msg->command.metadata.addr_list_length = data->addr_metadata_cnt;
		memcpy(iwc_msg->command.metadata.addr_list,
		       (u32 *)(unsigned long)data->addr_metadatas,
		       data->addr_metadata_cnt * sizeof(struct iwc_cmdq_addr_metadata_t));
	}

	iwc_msg->command.normal_task_handle = (unsigned long)sec_task->task.pkt;

	return 0;
}

static int cmdq_sec_session_send(struct cmdq_sec_context *context,
				 struct cmdq_sec_task *sec_task, const u32 iwc_cmd,
				 const u32 thrd_idx, struct cmdq_sec *cmdq)
{
	int err = 0;
	u64 cost;
	struct iwc_cmdq_message_t *iwc_msg = NULL;

	iwc_msg = (struct iwc_cmdq_message_t *)context->iwc_msg;

	memset(iwc_msg, 0, sizeof(*iwc_msg));
	iwc_msg->cmd = iwc_cmd;
	iwc_msg->cmdq_id = cmdq->pdata->hwid;
	iwc_msg->command.thread = thrd_idx;

	switch (iwc_cmd) {
	case CMD_CMDQ_IWC_SUBMIT_TASK:
		err = cmdq_sec_fill_iwc_msg(context, sec_task, thrd_idx);
		if (err)
			return err;
		break;
	case CMD_CMDQ_IWC_CANCEL_TASK:
		iwc_msg->cancel_task.wait_cookie = sec_task->wait_cookie;
		iwc_msg->cancel_task.thread = thrd_idx;
		break;
	case CMD_CMDQ_IWC_PATH_RES_ALLOCATE:
		if (!cmdq->shared_mem || !cmdq->shared_mem->va) {
			dev_err(&cmdq->dev, "%s %d: shared_mem is NULL", __func__, __LINE__);
			return -EFAULT;
		}
		iwc_msg->path_resource.size = cmdq->shared_mem->size;
		iwc_msg->path_resource.share_memoy_pa = cmdq->shared_mem->pa;
		iwc_msg->path_resource.use_normal_irq = 1;
		break;
	default:
		break;
	}

	cmdq->sec_invoke = sched_clock();
	dev_dbg(&cmdq->dev, "%s execute cmdq:%p sec_task:%p command:%u thread:%u cookie:%d",
		__func__, cmdq, sec_task, iwc_cmd, thrd_idx,
		sec_task ? sec_task->wait_cookie : -1);

	/* send message */
	err = cmdq_sec_execute_session(&context->tee_ctx, iwc_cmd, CMDQ_TIMEOUT_DEFAULT);

	cmdq->sec_done = sched_clock();
	cost = div_u64(cmdq->sec_done - cmdq->sec_invoke, 1000000);
	if (cost >= CMDQ_TIMEOUT_DEFAULT)
		dev_err(&cmdq->dev, "%s execute timeout cmdq:%p sec_task:%p cost:%lluus",
			__func__, cmdq, sec_task, cost);
	else
		dev_dbg(&cmdq->dev, "%s execute done cmdq:%p sec_task:%p cost:%lluus",
			__func__, cmdq, sec_task, cost);

	if (err)
		return err;

	context->state = IWC_SES_ON_TRANSACTED;
	return 0;
}

static int cmdq_sec_session_reply(const u32 iwc_cmd, struct iwc_cmdq_message_t *iwc_msg,
				  struct cmdq_sec_task *sec_task)
{
	if (iwc_msg->rsp >= 0)
		return iwc_msg->rsp;

	if (iwc_cmd == CMD_CMDQ_IWC_SUBMIT_TASK) {
		struct iwc_cmdq_sec_status_t *sec_status = &iwc_msg->sec_status;
		int i;

		/* print submit fail case status */
		pr_err("last sec status: step:%u status:%d args:%#x %#x %#x %#x dispatch:%s\n",
		       sec_status->step, sec_status->status, sec_status->args[0],
			   sec_status->args[1], sec_status->args[2], sec_status->args[3],
			   sec_status->dispatch);

		for (i = 0; i < sec_status->inst_index; i += 2)
			pr_err("instr %d: %08x %08x\n", i / 2,
			       sec_status->sec_inst[i], sec_status->sec_inst[i + 1]);
	} else if (iwc_cmd == CMD_CMDQ_IWC_CANCEL_TASK) {
		struct iwc_cmdq_cancel_task_t *cancel = &iwc_msg->cancel_task;

		/* print cancel task fail case status */
		if ((cancel->err_instr[1] >> 24) == CMDQ_CODE_WFE)
			pr_err("secure error inst event:%u value:%d\n",
			       cancel->err_instr[1], cancel->reg_value);

		pr_err("cancel_task inst:%08x %08x aee:%d reset:%d pc:0x%08x\n",
		       cancel->err_instr[1], cancel->err_instr[0],
			   cancel->throw_aee, cancel->has_reset, cancel->pc);
	}

	return iwc_msg->rsp;
}

static int cmdq_sec_task_submit(struct cmdq_sec *cmdq, struct cmdq_sec_task *sec_task,
				const u32 iwc_cmd, const u32 thrd_idx)
{
	struct cmdq_sec_context *context;
	int err = 0;

	if (!cmdq->context) {
		context = kzalloc(sizeof(*cmdq->context), GFP_ATOMIC);
		if (!context)
			return -ENOMEM;

		cmdq->context = context;
		cmdq->context->state = IWC_INIT;
		cmdq->context->tgid = current->tgid;
	}

	if (cmdq->context->state == IWC_INIT)
		cmdq_sec_setup_tee_context(&cmdq->context->tee_ctx);

	err = cmdq_sec_session_init(cmdq->context);
	if (err) {
		dev_err(&cmdq->dev, "%s %d: cmdq_sec_session_init fail: %d",
			__func__, __LINE__, err);
		return err;
	}

	err = cmdq_sec_irq_notify_start(cmdq);
	if (err) {
		dev_err(&cmdq->dev, "%s %d: cmdq_sec_irq_notify_start fail: %d",
			__func__, __LINE__, err);
		return err;
	}

	err = cmdq_sec_session_send(cmdq->context, sec_task, iwc_cmd, thrd_idx, cmdq);
	if (err) {
		dev_err(&cmdq->dev, "%s %d: iwc_cmd:%d err:%d sec_task:%p thread:%u gce:%#lx",
			__func__, __LINE__, iwc_cmd, err, sec_task, thrd_idx,
			(unsigned long)cmdq->base_pa);
		return err;
	}

	err = cmdq_sec_session_reply(iwc_cmd, cmdq->context->iwc_msg, sec_task);
	if (err) {
		dev_err(&cmdq->dev, "%s %d: cmdq_sec_session_reply fail: %d",
			__func__, __LINE__, err);
		return err;
	}

	return 0;
}

static void cmdq_sec_task_exec_work(struct work_struct *work_item)
{
	struct cmdq_sec_task *sec_task = container_of(work_item,
						      struct cmdq_sec_task, exec_work);
	struct cmdq_sec_thread *sec_thread = container_of(sec_task->task.thread,
							 struct cmdq_sec_thread, thread);
	struct cmdq_sec *cmdq = container_of(sec_thread->dev, struct cmdq_sec, dev);
	struct cmdq_sec_data *data;
	unsigned long flags;
	int err;

	dev_dbg(&cmdq->dev, "%s gce:%#lx sec_task:%p pkt:%p thread:%u",
		__func__, (unsigned long)cmdq->base_pa,
		sec_task, sec_task->task.pkt, sec_thread->idx);

	if (!sec_task->task.pkt->sec_data) {
		dev_err(&cmdq->dev, "pkt:%p without sec_data", sec_task->task.pkt);
		return;
	}
	data = (struct cmdq_sec_data *)sec_task->task.pkt->sec_data;

	mutex_lock(&cmdq->exec_lock);

	spin_lock_irqsave(&sec_thread->thread.chan->lock, flags);
	if (!sec_thread->task_cnt) {
		mod_timer(&sec_thread->timeout, jiffies +
			  msecs_to_jiffies(sec_thread->timeout_ms));
		sec_thread->wait_cookie = 1;
		sec_thread->next_cookie = 1;
		sec_thread->task_cnt = 0;
		__raw_writel(0, (void __iomem *)cmdq->shared_mem->va +
			     CMDQ_SEC_SHARED_THR_CNT_OFFSET + sec_thread->idx * sizeof(u32));
	}

	sec_task->reset_exec = sec_thread->task_cnt ? false : true;
	sec_task->wait_cookie = sec_thread->next_cookie;
	sec_thread->next_cookie = (sec_thread->next_cookie + 1) % CMDQ_MAX_COOKIE_VALUE;
	list_add_tail(&sec_task->task.list_entry, &sec_thread->thread.task_busy_list);
	sec_thread->task_cnt += 1;
	spin_unlock_irqrestore(&sec_thread->thread.chan->lock, flags);
	sec_task->trigger = sched_clock();

	if (!atomic_cmpxchg(&cmdq_path_res, 0, 1)) {
		err = cmdq_sec_task_submit(cmdq, NULL, CMD_CMDQ_IWC_PATH_RES_ALLOCATE,
					   CMDQ_INVALID_THREAD);
		if (err) {
			atomic_set(&cmdq_path_res, 0);
			goto task_end;
		}
	}

	if (sec_thread->task_cnt > CMDQ_MAX_TASK_IN_SECURE_THREAD) {
		dev_err(&cmdq->dev, "task_cnt:%u cannot more than %u sec_task:%p thread:%u",
			sec_thread->task_cnt, CMDQ_MAX_TASK_IN_SECURE_THREAD,
			sec_task, sec_thread->idx);
		err = -EMSGSIZE;
		goto task_end;
	}

	err = cmdq_sec_task_submit(cmdq, sec_task, CMD_CMDQ_IWC_SUBMIT_TASK,
				   sec_thread->idx);
	if (err)
		dev_err(&cmdq->dev, "cmdq_sec_task_submit err:%d sec_task:%p thread:%u",
			err, sec_task, sec_thread->idx);

task_end:
	if (err) {
		struct cmdq_cb_data cb_data;

		cb_data.sta = err;
		cb_data.pkt = sec_task->task.pkt;
		mbox_chan_received_data(sec_thread->thread.chan, &cb_data);

		spin_lock_irqsave(&sec_thread->thread.chan->lock, flags);
		if (!sec_thread->task_cnt)
			dev_err(&cmdq->dev, "thread:%u task_cnt:%u cannot below zero",
				sec_thread->idx, sec_thread->task_cnt);
		else
			sec_thread->task_cnt -= 1;

		sec_thread->next_cookie = (sec_thread->next_cookie - 1 +
			CMDQ_MAX_COOKIE_VALUE) % CMDQ_MAX_COOKIE_VALUE;
		list_del(&sec_task->task.list_entry);
		dev_dbg(&cmdq->dev, "gce:%#lx err:%d sec_task:%p pkt:%p",
			(unsigned long)cmdq->base_pa, err, sec_task, sec_task->task.pkt);
		dev_dbg(&cmdq->dev, "thread:%u task_cnt:%u wait_cookie:%u next_cookie:%u",
			sec_thread->idx, sec_thread->task_cnt,
			sec_thread->wait_cookie, sec_thread->next_cookie);
		spin_unlock_irqrestore(&sec_thread->thread.chan->lock, flags);

		kfree(sec_task);
	}

	mutex_unlock(&cmdq->exec_lock);
}

static int cmdq_sec_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct cmdq_pkt *pkt = (struct cmdq_pkt *)data;
	struct cmdq_sec_data *sec_data = (struct cmdq_sec_data *)pkt->sec_data;
	struct cmdq_thread *thread = (struct cmdq_thread *)chan->con_priv;
	struct cmdq_sec_thread *sec_thread = container_of(thread, struct cmdq_sec_thread, thread);
	struct cmdq_sec_task *sec_task;

	if (!sec_data)
		return -EINVAL;

	sec_task = kzalloc(sizeof(*sec_task), GFP_ATOMIC);
	if (!sec_task)
		return -ENOMEM;

	sec_task->task.pkt = pkt;
	sec_task->task.thread = thread;
	sec_task->scenario = sec_data->scenario;

	INIT_WORK(&sec_task->exec_work, cmdq_sec_task_exec_work);
	queue_work(sec_thread->task_exec_wq, &sec_task->exec_work);

	return 0;
}

static void cmdq_sec_thread_timeout(struct timer_list *t)
{
	struct cmdq_sec_thread *sec_thread = from_timer(sec_thread, t, timeout);
	struct cmdq_sec *cmdq = container_of(sec_thread->dev, struct cmdq_sec, dev);

	if (!work_pending(&sec_thread->timeout_work))
		queue_work(cmdq->timeout_wq, &sec_thread->timeout_work);
}

static void cmdq_sec_task_timeout_work(struct work_struct *work_item)
{
	struct cmdq_sec_thread *sec_thread = container_of(work_item,
							  struct cmdq_sec_thread, timeout_work);
	struct cmdq_sec *cmdq = container_of(sec_thread->dev, struct cmdq_sec, dev);
	struct cmdq_task *task;
	struct cmdq_sec_task *sec_task;
	unsigned long flags;
	u64 duration;
	u32 cookie;

	mutex_lock(&cmdq->exec_lock);

	spin_lock_irqsave(&sec_thread->thread.chan->lock, flags);
	if (list_empty(&sec_thread->thread.task_busy_list)) {
		dev_err(&cmdq->dev, "thread:%u task_list is empty", sec_thread->idx);
		spin_unlock_irqrestore(&sec_thread->thread.chan->lock, flags);
		goto done;
	}

	task = list_first_entry(&sec_thread->thread.task_busy_list,
				struct cmdq_task, list_entry);
	sec_task = container_of(task, struct cmdq_sec_task, task);
	duration = div_u64(sched_clock() - sec_task->trigger, 1000000);
	if (duration < sec_thread->timeout_ms) {
		mod_timer(&sec_thread->timeout, jiffies +
			  msecs_to_jiffies(sec_thread->timeout_ms - duration));
		spin_unlock_irqrestore(&sec_thread->thread.chan->lock, flags);
		goto done;
	}

	cookie = cmdq_sec_get_cookie(cmdq, sec_thread->idx);
	spin_unlock_irqrestore(&sec_thread->thread.chan->lock, flags);

	dev_err(&cmdq->dev, "%s duration:%llu cookie:%u thread:%u",
		__func__, duration, cookie, sec_thread->idx);
	cmdq_sec_irq_handler(sec_thread, cookie, -ETIMEDOUT);

done:
	mutex_unlock(&cmdq->exec_lock);
}

static int cmdq_sec_mbox_startup(struct mbox_chan *chan)
{
	struct cmdq_thread *thread = (struct cmdq_thread *)chan->con_priv;
	struct cmdq_sec_thread *sec_thread = container_of(thread,
							  struct cmdq_sec_thread, thread);
	char name[20];

	snprintf(name, sizeof(name), "task_exec_wq_%u", sec_thread->idx);
	sec_thread->task_exec_wq = create_singlethread_workqueue(name);

	return 0;
}

static int cmdq_sec_mbox_flush(struct mbox_chan *chan, unsigned long timeout)
{
	struct cmdq_thread *thread = (struct cmdq_thread *)chan->con_priv;
	struct cmdq_sec_thread *sec_thread = container_of(thread,
							  struct cmdq_sec_thread, thread);
	struct cmdq_sec *cmdq = container_of(sec_thread->dev, struct cmdq_sec, dev);
	u32 cookie = 0;

	mutex_lock(&cmdq->exec_lock);

	if (list_empty(&thread->task_busy_list)) {
		mutex_unlock(&cmdq->exec_lock);
		return 0;
	}

	cookie = cmdq_sec_get_cookie(cmdq, sec_thread->idx);
	if (cookie >= sec_thread->wait_cookie && sec_thread->task_cnt > 0)
		cmdq_sec_irq_handler(sec_thread, cookie, -ECONNABORTED);

	mutex_unlock(&cmdq->exec_lock);
	return 0;
}

static void cmdq_sec_mbox_shutdown(struct mbox_chan *chan)
{
	cmdq_sec_mbox_flush(chan, 0);
}

static const struct mbox_chan_ops cmdq_sec_mbox_chan_ops = {
	.send_data = cmdq_sec_mbox_send_data,
	.startup = cmdq_sec_mbox_startup,
	.shutdown = cmdq_sec_mbox_shutdown,
	.flush = cmdq_sec_mbox_flush,
};

struct cmdq_sec_mailbox cmdq_sec_mbox = {
	.ops = &cmdq_sec_mbox_chan_ops,
};
EXPORT_SYMBOL_GPL(cmdq_sec_mbox);

static int cmdq_sec_probe(struct platform_device *pdev)
{
	int i;
	struct cmdq_sec *cmdq;
	struct device *dev = &pdev->dev;
	struct resource *res;

	cmdq = devm_kzalloc(dev, sizeof(*cmdq), GFP_KERNEL);
	if (!cmdq)
		return -ENOMEM;

	cmdq->dev = pdev->dev;
	cmdq->pdata = (struct gce_sec_plat *)pdev->dev.platform_data;
	if (!cmdq->pdata) {
		dev_err(dev, "no valid gce platform data!\n");
		return -EINVAL;
	}

	cmdq->base = cmdq->pdata->base;
	res = platform_get_resource(to_platform_device(cmdq->pdata->mbox->dev),
				    IORESOURCE_MEM, 0);
	if (IS_ERR(cmdq->base)) {
		dev_err(dev, "devm_platform_get_and_ioremap_resource failed!\n");
		return PTR_ERR(cmdq->base);
	}

	cmdq->base_pa = res->start;

	cmdq->sec_thread = devm_kcalloc(dev, cmdq->pdata->secure_thread_nr,
					sizeof(*cmdq->sec_thread), GFP_KERNEL);
	if (!cmdq->sec_thread)
		return -ENOMEM;

	mutex_init(&cmdq->exec_lock);
	for (i = 0; i < cmdq->pdata->secure_thread_nr; i++) {
		u32  idx = i + cmdq->pdata->secure_thread_min;

		cmdq->sec_thread[i].dev = &cmdq->dev;
		cmdq->sec_thread[i].idx = idx;
		cmdq->sec_thread[i].thread.base = cmdq->base + CMDQ_THR_BASE + CMDQ_THR_SIZE * idx;
		cmdq->sec_thread[i].timeout_ms = CMDQ_TIMEOUT_DEFAULT;
		INIT_LIST_HEAD(&cmdq->sec_thread[i].thread.task_busy_list);
		cmdq->pdata->mbox->chans[idx].con_priv = (void *)&cmdq->sec_thread[i].thread;
		dev_dbg(dev, "re-assign chans[%d] as secure thread\n", idx);
		timer_setup(&cmdq->sec_thread[i].timeout, cmdq_sec_thread_timeout, 0);
		INIT_WORK(&cmdq->sec_thread[i].timeout_work, cmdq_sec_task_timeout_work);
	}

	cmdq->notify_wq = create_singlethread_workqueue("mtk_cmdq_sec_notify_wq");
	cmdq->timeout_wq = create_singlethread_workqueue("mtk_cmdq_sec_timeout_wq");

	cmdq->shared_mem = devm_kzalloc(dev, sizeof(*cmdq->shared_mem), GFP_KERNEL);
	if (!cmdq->shared_mem)
		return -ENOMEM;

	cmdq->shared_mem->va = dma_alloc_coherent(dev, PAGE_SIZE,
						  &cmdq->shared_mem->pa, GFP_KERNEL);
	cmdq->shared_mem->size = PAGE_SIZE;

	platform_set_drvdata(pdev, cmdq);

	return 0;
}

static int cmdq_sec_remove(struct platform_device *pdev)
{
	struct cmdq_sec *cmdq = platform_get_drvdata(pdev);

	if (cmdq->context)
		cmdq_sec_free_wsm(&cmdq->context->tee_ctx, &cmdq->context->iwc_msg);

	return 0;
}

static struct platform_driver cmdq_sec_drv = {
	.probe = cmdq_sec_probe,
	.remove = cmdq_sec_remove,
	.driver = {
		.name = "mtk-cmdq-sec",
	},
};

static int __init cmdq_sec_init(void)
{
	return platform_driver_register(&cmdq_sec_drv);
}

static void __exit cmdq_sec_exit(void)
{
	platform_driver_unregister(&cmdq_sec_drv);
}

module_init(cmdq_sec_init);
module_exit(cmdq_sec_exit);

MODULE_LICENSE("GPL");
