// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include <linux/delay.h>

#include "hinic3_eqs.h"
#include "hinic3_hwdev.h"
#include "hinic3_mbox.h"
#include "hinic3_hwif.h"
#include "hinic3_csr.h"

#define AEQ_CTRL_0_INTR_IDX_MASK      GENMASK(9, 0)
#define AEQ_CTRL_0_DMA_ATTR_MASK      GENMASK(17, 12)
#define AEQ_CTRL_0_PCI_INTF_IDX_MASK  GENMASK(22, 20)
#define AEQ_CTRL_0_INTR_MODE_MASK     BIT(31)
#define AEQ_CTRL_0_SET(val, member)  \
	FIELD_PREP(AEQ_CTRL_0_##member##_MASK, val)

#define AEQ_CTRL_1_LEN_MASK           GENMASK(20, 0)
#define AEQ_CTRL_1_ELEM_SIZE_MASK     GENMASK(25, 24)
#define AEQ_CTRL_1_PAGE_SIZE_MASK     GENMASK(31, 28)
#define AEQ_CTRL_1_SET(val, member)  \
	FIELD_PREP(AEQ_CTRL_1_##member##_MASK, val)

#define CEQ_CTRL_0_INTR_IDX_MASK      GENMASK(9, 0)
#define CEQ_CTRL_0_DMA_ATTR_MASK      GENMASK(17, 12)
#define CEQ_CTRL_0_LIMIT_KICK_MASK    GENMASK(23, 20)
#define CEQ_CTRL_0_PCI_INTF_IDX_MASK  GENMASK(25, 24)
#define CEQ_CTRL_0_PAGE_SIZE_MASK     GENMASK(30, 27)
#define CEQ_CTRL_0_INTR_MODE_MASK     BIT(31)
#define CEQ_CTRL_0_SET(val, member)   \
	FIELD_PREP(CEQ_CTRL_0_##member##_MASK, val)

#define CEQ_CTRL_1_LEN_MASK           GENMASK(19, 0)
#define CEQ_CTRL_1_SET(val, member)  \
	FIELD_PREP(CEQ_CTRL_1_##member##_MASK, val)

#define EQ_ELEM_DESC_TYPE_MASK        GENMASK(6, 0)
#define EQ_ELEM_DESC_SRC_MASK         BIT(7)
#define EQ_ELEM_DESC_SIZE_MASK        GENMASK(15, 8)
#define EQ_ELEM_DESC_WRAPPED_MASK     BIT(31)
#define EQ_ELEM_DESC_GET(val, member)  \
	FIELD_GET(EQ_ELEM_DESC_##member##_MASK, val)

#define EQ_CI_SIMPLE_INDIR_CI_MASK       GENMASK(20, 0)
#define EQ_CI_SIMPLE_INDIR_ARMED_MASK    BIT(21)
#define EQ_CI_SIMPLE_INDIR_AEQ_IDX_MASK  GENMASK(31, 30)
#define EQ_CI_SIMPLE_INDIR_CEQ_IDX_MASK  GENMASK(31, 24)
#define EQ_CI_SIMPLE_INDIR_SET(val, member)  \
	FIELD_PREP(EQ_CI_SIMPLE_INDIR_##member##_MASK, val)

#define EQ_CI_SIMPLE_INDIR_REG_ADDR(eq)  \
	(((eq)->type == HINIC3_AEQ) ?  \
	 HINIC3_CSR_AEQ_CI_SIMPLE_INDIR_ADDR :  \
	 HINIC3_CSR_CEQ_CI_SIMPLE_INDIR_ADDR)

#define EQ_PROD_IDX_REG_ADDR(eq)  \
	(((eq)->type == HINIC3_AEQ) ?  \
	 HINIC3_CSR_AEQ_PROD_IDX_ADDR : HINIC3_CSR_CEQ_PROD_IDX_ADDR)

#define HINIC3_EQ_HI_PHYS_ADDR_REG(type, pg_num)  \
	((u32)((type == HINIC3_AEQ) ?  \
	       HINIC3_AEQ_HI_PHYS_ADDR_REG(pg_num) :  \
	       HINIC3_CEQ_HI_PHYS_ADDR_REG(pg_num)))

#define HINIC3_EQ_LO_PHYS_ADDR_REG(type, pg_num)  \
	((u32)((type == HINIC3_AEQ) ?  \
	       HINIC3_AEQ_LO_PHYS_ADDR_REG(pg_num) :  \
	       HINIC3_CEQ_LO_PHYS_ADDR_REG(pg_num)))

#define HINIC3_EQ_MAX_PAGES(eq)  \
	((eq)->type == HINIC3_AEQ ?  \
	 HINIC3_AEQ_MAX_PAGES : HINIC3_CEQ_MAX_PAGES)

#define HINIC3_TASK_PROCESS_EQE_LIMIT  1024
#define HINIC3_EQ_UPDATE_CI_STEP       64
#define HINIC3_EQS_WQ_NAME             "hinic3_eqs"

#define EQ_MSIX_RESEND_TIMER_CLEAR     1

#define EQ_VALID_SHIFT    31
#define EQ_WRAPPED(eq)    ((u32)(eq)->wrapped << EQ_VALID_SHIFT)

#define EQ_WRAPPED_SHIFT  20
#define EQ_CONS_IDX(eq)   \
	((eq)->cons_idx | ((u32)(eq)->wrapped << EQ_WRAPPED_SHIFT))

#define CEQE_TYPE_MASK   GENMASK(25, 23)
#define CEQE_TYPE(type)  FIELD_GET(CEQE_TYPE_MASK, type)

#define CEQE_DATA_MASK   GENMASK(25, 0)
#define CEQE_DATA(data)  ((data) & CEQE_DATA_MASK)

static const struct hinic3_aeq_elem *get_curr_aeq_elem(const struct hinic3_eq *eq)
{
	return get_q_element(&eq->qpages, eq->cons_idx, NULL);
}

static const __be32 *get_curr_ceq_elem(const struct hinic3_eq *eq)
{
	return get_q_element(&eq->qpages, eq->cons_idx, NULL);
}

int hinic3_aeq_register_cb(struct hinic3_hwdev *hwdev, enum hinic3_aeq_type event,
			   hinic3_aeq_event_cb hwe_cb)
{
	struct hinic3_aeqs *aeqs;
	unsigned long *cb_state;

	aeqs = hwdev->aeqs;
	cb_state = &aeqs->aeq_cb_state[event];
	aeqs->aeq_cb[event] = hwe_cb;
	set_bit(HINIC3_AEQ_CB_REG, cb_state);
	return 0;
}

void hinic3_aeq_unregister_cb(struct hinic3_hwdev *hwdev, enum hinic3_aeq_type event)
{
	struct hinic3_aeqs *aeqs;
	unsigned long *cb_state;

	aeqs = hwdev->aeqs;
	cb_state = &aeqs->aeq_cb_state[event];
	clear_bit(HINIC3_AEQ_CB_REG, cb_state);
	/* Ensure handler can observe our intent to unregister. */
	mb();
	while (test_bit(HINIC3_AEQ_CB_RUNNING, cb_state))
		usleep_range(EQ_USLEEP_LOW_BOUND, EQ_USLEEP_HIG_BOUND);

	aeqs->aeq_cb[event] = NULL;
}

int hinic3_ceq_register_cb(struct hinic3_hwdev *hwdev, enum hinic3_ceq_event event,
			   hinic3_ceq_event_cb callback)
{
	struct hinic3_ceqs *ceqs;

	ceqs = hwdev->ceqs;
	ceqs->ceq_cb[event] = callback;
	set_bit(HINIC3_CEQ_CB_REG, &ceqs->ceq_cb_state[event]);
	return 0;
}

void hinic3_ceq_unregister_cb(struct hinic3_hwdev *hwdev, enum hinic3_ceq_event event)
{
	struct hinic3_ceqs *ceqs;

	ceqs = hwdev->ceqs;
	clear_bit(HINIC3_CEQ_CB_REG, &ceqs->ceq_cb_state[event]);
	/* Ensure handler can observe our intent to unregister. */
	mb();
	while (test_bit(HINIC3_CEQ_CB_RUNNING, &ceqs->ceq_cb_state[event]))
		usleep_range(EQ_USLEEP_LOW_BOUND, EQ_USLEEP_HIG_BOUND);

	ceqs->ceq_cb[event] = NULL;
}

/* Set consumer index in the hw. */
static void set_eq_cons_idx(struct hinic3_eq *eq, u32 arm_state)
{
	u32 addr = EQ_CI_SIMPLE_INDIR_REG_ADDR(eq);
	u32 eq_wrap_ci, val;

	eq_wrap_ci = EQ_CONS_IDX(eq);
	val = EQ_CI_SIMPLE_INDIR_SET(arm_state, ARMED);
	if (eq->type == HINIC3_AEQ) {
		val = val |
			EQ_CI_SIMPLE_INDIR_SET(eq_wrap_ci, CI) |
			EQ_CI_SIMPLE_INDIR_SET(eq->q_id, AEQ_IDX);
	} else {
		val = val |
			EQ_CI_SIMPLE_INDIR_SET(eq_wrap_ci, CI) |
			EQ_CI_SIMPLE_INDIR_SET(eq->q_id, CEQ_IDX);
	}

	hinic3_hwif_write_reg(eq->hwdev->hwif, addr, val);
}

static struct hinic3_ceqs *ceq_to_ceqs(const struct hinic3_eq *eq)
{
	return container_of(eq, struct hinic3_ceqs, ceq[eq->q_id]);
}

static void ceq_event_handler(struct hinic3_ceqs *ceqs, u32 ceqe)
{
	enum hinic3_ceq_event event = CEQE_TYPE(ceqe);
	struct hinic3_hwdev *hwdev = ceqs->hwdev;
	u32 ceqe_data = CEQE_DATA(ceqe);

	if (event >= HINIC3_MAX_CEQ_EVENTS) {
		dev_warn(hwdev->dev, "Ceq unknown event:%d, ceqe date: 0x%x\n",
			 event, ceqe_data);
		return;
	}

	set_bit(HINIC3_CEQ_CB_RUNNING, &ceqs->ceq_cb_state[event]);
	/* Ensure unregister sees we are running. */
	mb();
	if (ceqs->ceq_cb[event] &&
	    test_bit(HINIC3_CEQ_CB_REG, &ceqs->ceq_cb_state[event]))
		ceqs->ceq_cb[event](hwdev, ceqe_data);

	clear_bit(HINIC3_CEQ_CB_RUNNING, &ceqs->ceq_cb_state[event]);
}

static struct hinic3_aeqs *aeq_to_aeqs(const struct hinic3_eq *eq)
{
	return container_of(eq, struct hinic3_aeqs, aeq[eq->q_id]);
}

static void aeq_event_handler(struct hinic3_aeqs *aeqs, u32 aeqe,
			      const struct hinic3_aeq_elem *aeqe_pos)
{
	struct hinic3_hwdev *hwdev = aeqs->hwdev;
	u8 data[HINIC3_AEQE_DATA_SIZE], size;
	enum hinic3_aeq_type event;
	hinic3_aeq_event_cb hwe_cb;
	unsigned long *cb_state;

	if (EQ_ELEM_DESC_GET(aeqe, SRC))
		return;

	event = EQ_ELEM_DESC_GET(aeqe, TYPE);
	if (event >= HINIC3_MAX_AEQ_EVENTS) {
		dev_warn(hwdev->dev, "Aeq unknown event:%d\n", event);
		return;
	}

	memcpy(data, aeqe_pos->aeqe_data, HINIC3_AEQE_DATA_SIZE);
	cmdq_buf_swab32(data, HINIC3_AEQE_DATA_SIZE);
	size = EQ_ELEM_DESC_GET(aeqe, SIZE);
	cb_state = &aeqs->aeq_cb_state[event];
	set_bit(HINIC3_AEQ_CB_RUNNING, cb_state);
	/* Ensure unregister sees we are running. */
	mb();
	hwe_cb = aeqs->aeq_cb[event];
	if (hwe_cb && test_bit(HINIC3_AEQ_CB_REG, cb_state))
		hwe_cb(aeqs->hwdev, data, size);
	clear_bit(HINIC3_AEQ_CB_RUNNING, cb_state);
}

static int aeq_irq_handler(struct hinic3_eq *eq)
{
	const struct hinic3_aeq_elem *aeqe_pos;
	struct hinic3_aeqs *aeqs;
	u32 i, eqe_cnt = 0;
	u32 aeqe;

	aeqs = aeq_to_aeqs(eq);
	for (i = 0; i < HINIC3_TASK_PROCESS_EQE_LIMIT; i++) {
		aeqe_pos = get_curr_aeq_elem(eq);
		aeqe = be32_to_cpu(aeqe_pos->desc);
		/* HW updates wrapped bit, when it adds eq element event */
		if (EQ_ELEM_DESC_GET(aeqe, WRAPPED) == eq->wrapped)
			return 0;

		/* Prevent speculative reads from element */
		dma_rmb();
		aeq_event_handler(aeqs, aeqe, aeqe_pos);
		eq->cons_idx++;
		if (eq->cons_idx == eq->eq_len) {
			eq->cons_idx = 0;
			eq->wrapped = !eq->wrapped;
		}

		if (++eqe_cnt >= HINIC3_EQ_UPDATE_CI_STEP) {
			eqe_cnt = 0;
			set_eq_cons_idx(eq, HINIC3_EQ_NOT_ARMED);
		}
	}

	return -EAGAIN;
}

static int ceq_irq_handler(struct hinic3_eq *eq)
{
	struct hinic3_ceqs *ceqs;
	u32 ceqe, eqe_cnt = 0;
	__be32 ceqe_raw;
	u32 i;

	ceqs = ceq_to_ceqs(eq);
	for (i = 0; i < HINIC3_TASK_PROCESS_EQE_LIMIT; i++) {
		ceqe_raw = *get_curr_ceq_elem(eq);
		ceqe = be32_to_cpu(ceqe_raw);

		/* HW updates wrapped bit, when it adds eq element event */
		if (EQ_ELEM_DESC_GET(ceqe, WRAPPED) == eq->wrapped)
			return 0;

		ceq_event_handler(ceqs, ceqe);
		eq->cons_idx++;
		if (eq->cons_idx == eq->eq_len) {
			eq->cons_idx = 0;
			eq->wrapped = !eq->wrapped;
		}

		if (++eqe_cnt >= HINIC3_EQ_UPDATE_CI_STEP) {
			eqe_cnt = 0;
			set_eq_cons_idx(eq, HINIC3_EQ_NOT_ARMED);
		}
	}

	return -EAGAIN;
}

static void reschedule_eq_handler(struct hinic3_eq *eq)
{
	if (eq->type == HINIC3_AEQ) {
		struct hinic3_aeqs *aeqs = aeq_to_aeqs(eq);

		queue_work_on(WORK_CPU_UNBOUND, aeqs->workq, &eq->aeq_work);
	} else {
		tasklet_schedule(&eq->ceq_tasklet);
	}
}

static int eq_irq_handler(struct hinic3_eq *eq)
{
	int err;

	if (eq->type == HINIC3_AEQ)
		err = aeq_irq_handler(eq);
	else
		err = ceq_irq_handler(eq);

	set_eq_cons_idx(eq, err ? HINIC3_EQ_NOT_ARMED :
			HINIC3_EQ_ARMED);

	return err;
}

static void eq_irq_work(struct work_struct *work)
{
	struct hinic3_eq *eq = container_of(work, struct hinic3_eq, aeq_work);
	int err;

	err = eq_irq_handler(eq);
	if (err)
		reschedule_eq_handler(eq);
}

static void ceq_tasklet(ulong ceq_data)
{
	struct hinic3_eq *eq = (struct hinic3_eq *)ceq_data;
	int err;

	err = eq_irq_handler(eq);
	if (err)
		reschedule_eq_handler(eq);
}

static irqreturn_t aeq_interrupt(int irq, void *data)
{
	struct hinic3_eq *aeq = data;
	struct hinic3_aeqs *aeqs = aeq_to_aeqs(aeq);
	struct hinic3_hwdev *hwdev = aeq->hwdev;
	struct workqueue_struct *workq;

	/* clear resend timer cnt register */
	workq = aeqs->workq;
	hinic3_misx_intr_clear_resend_bit(hwdev, aeq->eq_irq.msix_entry_idx,
					  EQ_MSIX_RESEND_TIMER_CLEAR);
	queue_work_on(WORK_CPU_UNBOUND, workq, &aeq->aeq_work);
	return IRQ_HANDLED;
}

static irqreturn_t ceq_interrupt(int irq, void *data)
{
	struct hinic3_eq *ceq = data;

	/* clear resend timer counters */
	hinic3_misx_intr_clear_resend_bit(ceq->hwdev,
					  ceq->eq_irq.msix_entry_idx,
					  EQ_MSIX_RESEND_TIMER_CLEAR);
	tasklet_schedule(&ceq->ceq_tasklet);
	return IRQ_HANDLED;
}

static int hinic3_set_ceq_ctrl_reg(struct hinic3_hwdev *hwdev, u16 q_id,
				   u32 ctrl0, u32 ctrl1)
{
	struct comm_cmd_ceq_ctrl_reg ceq_ctrl;
	u32 out_size = sizeof(ceq_ctrl);
	int err;

	memset(&ceq_ctrl, 0, sizeof(ceq_ctrl));
	ceq_ctrl.func_id = hinic3_global_func_id(hwdev);
	ceq_ctrl.q_id = q_id;
	ceq_ctrl.ctrl0 = ctrl0;
	ceq_ctrl.ctrl1 = ctrl1;

	err = hinic3_send_mbox_to_mgmt(hwdev, HINIC3_MOD_COMM,
				       COMM_MGMT_CMD_SET_CEQ_CTRL_REG,
				       &ceq_ctrl, sizeof(ceq_ctrl),
				       &ceq_ctrl, &out_size, 0);
	if (err || !out_size || ceq_ctrl.head.status) {
		dev_err(hwdev->dev, "Failed to set ceq %u ctrl reg, err: %d status: 0x%x, out_size: 0x%x\n",
			q_id, err, ceq_ctrl.head.status, out_size);
		return -EFAULT;
	}

	return 0;
}

static int set_eq_ctrls(struct hinic3_eq *eq)
{
	struct hinic3_hwif *hwif = eq->hwdev->hwif;
	struct irq_info *eq_irq = &eq->eq_irq;
	struct hinic3_queue_pages *qpages;
	u8 pci_intf_idx, elem_size;
	u32 mask, ctrl0, ctrl1;
	u32 page_size_val;
	int err;

	qpages = &eq->qpages;
	page_size_val = ilog2(qpages->page_size / HINIC3_MIN_PAGE_SIZE);
	pci_intf_idx = hwif->attr.pci_intf_idx;

	if (eq->type == HINIC3_AEQ) {
		/* set ctrl0 using read-modify-write */
		mask = AEQ_CTRL_0_INTR_IDX_MASK |
		       AEQ_CTRL_0_DMA_ATTR_MASK |
		       AEQ_CTRL_0_PCI_INTF_IDX_MASK |
		       AEQ_CTRL_0_INTR_MODE_MASK;
		ctrl0 = hinic3_hwif_read_reg(hwif, HINIC3_CSR_AEQ_CTRL_0_ADDR);
		ctrl0 = (ctrl0 & ~mask) |
			AEQ_CTRL_0_SET(eq_irq->msix_entry_idx, INTR_IDX) |
			AEQ_CTRL_0_SET(0, DMA_ATTR) |
			AEQ_CTRL_0_SET(pci_intf_idx, PCI_INTF_IDX) |
			AEQ_CTRL_0_SET(HINIC3_INTR_MODE_ARMED, INTR_MODE);
		hinic3_hwif_write_reg(hwif, HINIC3_CSR_AEQ_CTRL_0_ADDR, ctrl0);

		/* HW expects log2(number of 32 byte units). */
		elem_size = qpages->elem_size_shift - 5;
		ctrl1 = AEQ_CTRL_1_SET(eq->eq_len, LEN) |
			AEQ_CTRL_1_SET(elem_size, ELEM_SIZE) |
			AEQ_CTRL_1_SET(page_size_val, PAGE_SIZE);
		hinic3_hwif_write_reg(hwif, HINIC3_CSR_AEQ_CTRL_1_ADDR, ctrl1);
	} else {
		ctrl0 = CEQ_CTRL_0_SET(eq_irq->msix_entry_idx, INTR_IDX) |
			CEQ_CTRL_0_SET(0, DMA_ATTR) |
			CEQ_CTRL_0_SET(0, LIMIT_KICK) |
			CEQ_CTRL_0_SET(pci_intf_idx, PCI_INTF_IDX) |
			CEQ_CTRL_0_SET(page_size_val, PAGE_SIZE) |
			CEQ_CTRL_0_SET(HINIC3_INTR_MODE_ARMED, INTR_MODE);

		ctrl1 = CEQ_CTRL_1_SET(eq->eq_len, LEN);

		/* set ceq ctrl reg through mgmt cpu */
		err = hinic3_set_ceq_ctrl_reg(eq->hwdev, eq->q_id, ctrl0, ctrl1);
		if (err)
			return err;
	}

	return 0;
}

static void ceq_elements_init(struct hinic3_eq *eq, u32 init_val)
{
	__be32 *ceqe;
	u32 i;

	for (i = 0; i < eq->eq_len; i++) {
		ceqe = get_q_element(&eq->qpages, i, NULL);
		*ceqe = cpu_to_be32(init_val);
	}

	wmb();    /* Write the init values */
}

static void aeq_elements_init(struct hinic3_eq *eq, u32 init_val)
{
	struct hinic3_aeq_elem *aeqe;
	u32 i;

	for (i = 0; i < eq->eq_len; i++) {
		aeqe = get_q_element(&eq->qpages, i, NULL);
		aeqe->desc = cpu_to_be32(init_val);
	}

	wmb();    /* Write the init values */
}

static void eq_elements_init(struct hinic3_eq *eq, u32 init_val)
{
	if (eq->type == HINIC3_AEQ)
		aeq_elements_init(eq, init_val);
	else
		ceq_elements_init(eq, init_val);
}

static int alloc_eq_pages(struct hinic3_eq *eq)
{
	struct hinic3_hwif *hwif = eq->hwdev->hwif;
	struct hinic3_queue_pages *qpages;
	dma_addr_t page_paddr;
	u32 reg, init_val;
	u16 pg_idx;
	int err;

	qpages = &eq->qpages;
	err = queue_pages_alloc(eq->hwdev, qpages, HINIC3_MIN_PAGE_SIZE);
	if (err)
		return err;

	for (pg_idx = 0; pg_idx < qpages->num_pages; pg_idx++) {
		page_paddr = qpages->pages[pg_idx].align_paddr;
		reg = HINIC3_EQ_HI_PHYS_ADDR_REG(eq->type, pg_idx);
		hinic3_hwif_write_reg(hwif, reg, upper_32_bits(page_paddr));
		reg = HINIC3_EQ_LO_PHYS_ADDR_REG(eq->type, pg_idx);
		hinic3_hwif_write_reg(hwif, reg, lower_32_bits(page_paddr));
	}

	init_val = EQ_WRAPPED(eq);
	eq_elements_init(eq, init_val);
	return 0;
}

static void eq_calc_page_size_and_num(struct hinic3_eq *eq, u32 elem_size)
{
	u32 max_pages, min_page_size, page_size, total_size;

	/* No need for complicated arithmetics. All values must be power of 2.
	 * Multiplications give power of 2 and divisions give power of 2 without
	 * remainder.
	 */
	max_pages = HINIC3_EQ_MAX_PAGES(eq);
	min_page_size = HINIC3_MIN_PAGE_SIZE;
	total_size = eq->eq_len * elem_size;

	if (total_size <= max_pages * min_page_size)
		page_size = min_page_size;
	else
		page_size = total_size / max_pages;

	queue_pages_init(&eq->qpages, eq->eq_len, page_size, elem_size);
}

static int request_eq_irq(struct hinic3_eq *eq, struct irq_info *entry)
{
	int err;

	if (eq->type == HINIC3_AEQ) {
		INIT_WORK(&eq->aeq_work, eq_irq_work);
		snprintf(eq->irq_name, sizeof(eq->irq_name),
			 "hinic3_aeq%u@pci:%s", eq->q_id,
			 pci_name(eq->hwdev->pdev));
		err = request_irq(entry->irq_id, aeq_interrupt, 0UL,
				  eq->irq_name, eq);
	} else {
		tasklet_init(&eq->ceq_tasklet, ceq_tasklet, (ulong)eq);
		snprintf(eq->irq_name, sizeof(eq->irq_name),
			 "hinic3_ceq%u@pci:%s", eq->q_id,
			 pci_name(eq->hwdev->pdev));
		err = request_irq(entry->irq_id, ceq_interrupt, 0UL,
				  eq->irq_name, eq);
	}

	return err;
}

static void reset_eq(struct hinic3_eq *eq)
{
	/* clear eq_len to force eqe drop in hardware */
	if (eq->type == HINIC3_AEQ)
		hinic3_hwif_write_reg(eq->hwdev->hwif,
				      HINIC3_CSR_AEQ_CTRL_1_ADDR, 0);
	else
		hinic3_set_ceq_ctrl_reg(eq->hwdev, eq->q_id, 0, 0);

	hinic3_hwif_write_reg(eq->hwdev->hwif, EQ_PROD_IDX_REG_ADDR(eq), 0);
}

static int init_eq(struct hinic3_eq *eq, struct hinic3_hwdev *hwdev, u16 q_id,
		   u32 q_len, enum hinic3_eq_type type, struct irq_info *entry)
{
	u32 elem_size;
	int err;

	eq->hwdev = hwdev;
	eq->q_id = q_id;
	eq->type = type;
	eq->eq_len = q_len;

	/* Indirect access should set q_id first */
	hinic3_hwif_write_reg(hwdev->hwif, HINIC3_EQ_INDIR_IDX_ADDR(eq->type),
			      eq->q_id);

	reset_eq(eq);

	eq->cons_idx = 0;
	eq->wrapped = 0;

	elem_size = (type == HINIC3_AEQ) ? HINIC3_AEQE_SIZE : HINIC3_CEQE_SIZE;
	eq_calc_page_size_and_num(eq, elem_size);

	err = alloc_eq_pages(eq);
	if (err) {
		dev_err(hwdev->dev, "Failed to allocate pages for eq\n");
		return err;
	}

	eq->eq_irq.msix_entry_idx = entry->msix_entry_idx;
	eq->eq_irq.irq_id = entry->irq_id;

	err = set_eq_ctrls(eq);
	if (err) {
		dev_err(hwdev->dev, "Failed to set ctrls for eq\n");
		goto err_undo_pages_alloc;
	}

	set_eq_cons_idx(eq, HINIC3_EQ_ARMED);

	err = request_eq_irq(eq, entry);
	if (err) {
		dev_err(hwdev->dev,
			"Failed to request irq for the eq, err: %d\n", err);
		goto err_undo_pages_alloc;
	}

	hinic3_set_msix_state(hwdev, entry->msix_entry_idx,
			      HINIC3_MSIX_DISABLE);

	return 0;

err_undo_pages_alloc:
	queue_pages_free(hwdev, &eq->qpages);
	return err;
}

static void remove_eq(struct hinic3_eq *eq)
{
	struct irq_info *entry = &eq->eq_irq;

	hinic3_set_msix_state(eq->hwdev, entry->msix_entry_idx,
			      HINIC3_MSIX_DISABLE);
	synchronize_irq(entry->irq_id);
	free_irq(entry->irq_id, eq);
	/* Indirect access should set q_id first */
	hinic3_hwif_write_reg(eq->hwdev->hwif,
			      HINIC3_EQ_INDIR_IDX_ADDR(eq->type),
			      eq->q_id);

	if (eq->type == HINIC3_AEQ) {
		cancel_work_sync(&eq->aeq_work);
		/* clear eq_len to avoid hw access host memory */
		hinic3_hwif_write_reg(eq->hwdev->hwif,
				      HINIC3_CSR_AEQ_CTRL_1_ADDR, 0);
	} else {
		tasklet_kill(&eq->ceq_tasklet);
		hinic3_set_ceq_ctrl_reg(eq->hwdev, eq->q_id, 0, 0);
	}

	/* update consumer index to avoid invalid interrupt */
	eq->cons_idx = hinic3_hwif_read_reg(eq->hwdev->hwif,
					    EQ_PROD_IDX_REG_ADDR(eq));
	set_eq_cons_idx(eq, HINIC3_EQ_NOT_ARMED);
	queue_pages_free(eq->hwdev, &eq->qpages);
}

int hinic3_aeqs_init(struct hinic3_hwdev *hwdev, u16 num_aeqs,
		     struct irq_info *msix_entries)
{
	struct hinic3_aeqs *aeqs;
	u16 i, q_id;
	int err;

	aeqs = kzalloc(sizeof(*aeqs), GFP_KERNEL);
	if (!aeqs)
		return -ENOMEM;

	hwdev->aeqs = aeqs;
	aeqs->hwdev = hwdev;
	aeqs->num_aeqs = num_aeqs;
	aeqs->workq = alloc_workqueue(HINIC3_EQS_WQ_NAME, WQ_MEM_RECLAIM,
				      HINIC3_MAX_AEQS);
	if (!aeqs->workq) {
		dev_err(hwdev->dev, "Failed to initialize aeq workqueue\n");
		err = -ENOMEM;
		goto err_create_work;
	}

	for (q_id = 0; q_id < num_aeqs; q_id++) {
		err = init_eq(&aeqs->aeq[q_id], hwdev, q_id, HINIC3_DEFAULT_AEQ_LEN,
			      HINIC3_AEQ, &msix_entries[q_id]);
		if (err) {
			dev_err(hwdev->dev, "Failed to init aeq %u\n",
				q_id);
			goto err_init_aeq;
		}
	}
	for (q_id = 0; q_id < num_aeqs; q_id++)
		hinic3_set_msix_state(hwdev, msix_entries[q_id].msix_entry_idx,
				      HINIC3_MSIX_ENABLE);

	return 0;

err_init_aeq:
	for (i = 0; i < q_id; i++)
		remove_eq(&aeqs->aeq[i]);

	destroy_workqueue(aeqs->workq);

err_create_work:
	kfree(aeqs);
	return err;
}

void hinic3_aeqs_free(struct hinic3_hwdev *hwdev)
{
	struct hinic3_aeqs *aeqs = hwdev->aeqs;
	enum hinic3_aeq_type aeq_event;
	struct hinic3_eq *eq;
	u16 q_id;

	for (q_id = 0; q_id < aeqs->num_aeqs; q_id++) {
		eq = aeqs->aeq + q_id;
		remove_eq(eq);
		hinic3_free_irq(hwdev, eq->eq_irq.irq_id);
	}

	for (aeq_event = 0; aeq_event < HINIC3_MAX_AEQ_EVENTS; aeq_event++)
		hinic3_aeq_unregister_cb(hwdev, aeq_event);

	destroy_workqueue(aeqs->workq);

	kfree(aeqs);
}

int hinic3_ceqs_init(struct hinic3_hwdev *hwdev, u16 num_ceqs,
		     struct irq_info *msix_entries)
{
	struct hinic3_ceqs *ceqs;
	u16 i, q_id;
	int err;

	ceqs = kzalloc(sizeof(*ceqs), GFP_KERNEL);
	if (!ceqs)
		return -ENOMEM;

	hwdev->ceqs = ceqs;
	ceqs->hwdev = hwdev;
	ceqs->num_ceqs = num_ceqs;

	for (q_id = 0; q_id < num_ceqs; q_id++) {
		err = init_eq(&ceqs->ceq[q_id], hwdev, q_id, HINIC3_DEFAULT_CEQ_LEN,
			      HINIC3_CEQ, &msix_entries[q_id]);
		if (err) {
			dev_err(hwdev->dev, "Failed to init ceq %u\n",
				q_id);
			goto err_init_ceq;
		}
	}
	for (q_id = 0; q_id < num_ceqs; q_id++)
		hinic3_set_msix_state(hwdev, msix_entries[q_id].msix_entry_idx,
				      HINIC3_MSIX_ENABLE);

	return 0;

err_init_ceq:
	for (i = 0; i < q_id; i++)
		remove_eq(&ceqs->ceq[i]);

	kfree(ceqs);
	return err;
}

void hinic3_ceqs_free(struct hinic3_hwdev *hwdev)
{
	struct hinic3_ceqs *ceqs = hwdev->ceqs;
	enum hinic3_ceq_event ceq_event;
	struct hinic3_eq *eq;
	u16 q_id;

	for (q_id = 0; q_id < ceqs->num_ceqs; q_id++) {
		eq = ceqs->ceq + q_id;
		remove_eq(eq);
		hinic3_free_irq(hwdev, eq->eq_irq.irq_id);
	}

	for (ceq_event = 0; ceq_event < HINIC3_MAX_CEQ_EVENTS; ceq_event++)
		hinic3_ceq_unregister_cb(hwdev, ceq_event);

	kfree(ceqs);
}
