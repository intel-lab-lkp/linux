// SPDX-License-Identifier: GPL-2.0+
/*
 * OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface framework
 *
 * Author: Parthiban Veerasooran <parthiban.veerasooran@microchip.com>
 */

#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/oa_tc6.h>

static int oa_tc6_spi_transfer(struct spi_device *spi, u8 *ptx, u8 *prx,
			       u16 len)
{
	struct spi_transfer xfer = {
		.tx_buf = ptx,
		.rx_buf = prx,
		.len = len,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(spi, &msg);
}

static bool oa_tc6_get_parity(u32 p)
{
	bool parity = true;

	/* This function returns an odd parity bit */
	while (p) {
		parity = !parity;
		p = p & (p - 1);
	}
	return parity;
}

static void oa_tc6_prepare_ctrl_buf(struct oa_tc6 *tc6, u32 addr, u32 val[],
				    u8 len, bool wnr, u8 *buf, bool ctrl_prot)
{
	u32 hdr;

	/* Prepare the control header with the required details */
	hdr = FIELD_PREP(CTRL_HDR_DNC, 0) |
	      FIELD_PREP(CTRL_HDR_WNR, wnr) |
	      FIELD_PREP(CTRL_HDR_AID, 0) |
	      FIELD_PREP(CTRL_HDR_MMS, addr >> 16) |
	      FIELD_PREP(CTRL_HDR_ADDR, addr) |
	      FIELD_PREP(CTRL_HDR_LEN, len - 1);
	hdr |= FIELD_PREP(CTRL_HDR_P, oa_tc6_get_parity(hdr));
	*(u32 *)buf = cpu_to_be32(hdr);

	if (wnr) {
		for (u8 i = 0; i < len; i++) {
			u16 pos;

			if (!ctrl_prot) {
				/* Send the value to be written followed by the
				 * header.
				 */
				pos = (i + 1) * TC6_HDR_SIZE;
				*(u32 *)&buf[pos] = cpu_to_be32(val[i]);
			} else {
				/* If protected then send complemented value
				 * also followed by actual value.
				 */
				pos = TC6_HDR_SIZE + (i * (TC6_HDR_SIZE * 2));
				*(u32 *)&buf[pos] = cpu_to_be32(val[i]);
				pos = (i + 1) * (TC6_HDR_SIZE * 2);
				*(u32 *)&buf[pos] = cpu_to_be32(~val[i]);
			}
		}
	}
}

static int oa_tc6_check_control(struct oa_tc6 *tc6, u8 *ptx, u8 *prx, u8 len,
				bool wnr, bool ctrl_prot)
{
	/* 1st 4 bytes of rx chunk data can be discarded */
	u32 rx_hdr = *(u32 *)&prx[TC6_HDR_SIZE];
	u32 tx_hdr = *(u32 *)ptx;
	u32 rx_data_complement;
	u32 tx_data;
	u32 rx_data;
	u16 pos1;
	u16 pos2;

	/* If tx hdr and echoed hdr are not equal then there might be an issue
	 * with the connection between SPI host and MAC-PHY. Here this case is
	 * considered as MAC-PHY is not connected.
	 */
	if (tx_hdr != rx_hdr)
		return -ENODEV;

	if (wnr) {
		if (!ctrl_prot) {
			/* In case of ctrl write, both tx data & echoed
			 * data are compared for the error.
			 */
			pos1 = TC6_HDR_SIZE;
			pos2 = TC6_HDR_SIZE * 2;
			for (u8 i = 0; i < len; i++) {
				tx_data = *(u32 *)&ptx[pos1 + (i * TC6_HDR_SIZE)];
				rx_data = *(u32 *)&prx[pos2 + (i * TC6_HDR_SIZE)];
				if (tx_data != rx_data)
					return -ENODEV;
			}
			return 0;
		}
	} else {
		if (!ctrl_prot)
			return 0;
	}

	/* In case of ctrl read or ctrl write in protected mode, the rx data and
	 * the complement of rx data are compared for the error.
	 */
	pos1 = TC6_HDR_SIZE * 2;
	pos2 = TC6_HDR_SIZE * 3;
	for (u8 i = 0; i < len; i++) {
		rx_data = *(u32 *)&prx[pos1 + (i * TC6_HDR_SIZE * 2)];
		rx_data_complement = *(u32 *)&prx[pos2 + (i * TC6_HDR_SIZE * 2)];
		if (rx_data != ~rx_data_complement)
			return -ENODEV;
	}

	return 0;
}

int oa_tc6_perform_ctrl(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len,
			bool wnr, bool ctrl_prot)
{
	u8 *tx_buf;
	u8 *rx_buf;
	u16 size;
	u16 pos;
	int ret;

	if (ctrl_prot)
		size = (TC6_HDR_SIZE * 2) + (len * (TC6_HDR_SIZE * 2));
	else
		size = (TC6_HDR_SIZE * 2) + (len * TC6_HDR_SIZE);

	tx_buf = kzalloc(size, GFP_KERNEL);
	if (!tx_buf)
		return -ENOMEM;

	rx_buf = kzalloc(size, GFP_KERNEL);
	if (!rx_buf) {
		ret = -ENOMEM;
		goto err_rx_buf_kzalloc;
	}

	/* Prepare control command */
	oa_tc6_prepare_ctrl_buf(tc6, addr, val, len, wnr, tx_buf, ctrl_prot);

	/* Perform SPI transfer */
	ret = oa_tc6_spi_transfer(tc6->spi, tx_buf, rx_buf, size);
	if (ret)
		goto err_ctrl;

	/* In case of reset write, the echoed control command doesn't have any
	 * valid data. So no need to check for error.
	 */
	if (addr != OA_TC6_RESET) {
		/* Check echoed/received control reply */
		ret = oa_tc6_check_control(tc6, tx_buf, rx_buf, len, wnr,
					   ctrl_prot);
		if (ret)
			goto err_ctrl;
	}

	if (!wnr) {
		/* Copy read data from the rx data in case of ctrl read */
		for (u8 i = 0; i < len; i++) {
			if (!ctrl_prot) {
				pos = (TC6_HDR_SIZE * 2) + (i * TC6_HDR_SIZE);
				val[i] = be32_to_cpu(*(u32 *)&rx_buf[pos]);
			} else {
				pos = (TC6_HDR_SIZE * 2) +
				       (i * (TC6_HDR_SIZE * 2));
				val[i] = be32_to_cpu(*(u32 *)&rx_buf[pos]);
			}
		}
	}

err_ctrl:
	kfree(rx_buf);
err_rx_buf_kzalloc:
	kfree(tx_buf);
	return ret;
}

static int oa_tc6_handler(void *data)
{
	struct oa_tc6 *tc6 = data;
	u32 regval;
	int ret;

	while (likely(!kthread_should_stop())) {
		wait_event_interruptible(tc6->tc6_wq, tc6->int_flag ||
					 kthread_should_stop());
		if (tc6->int_flag) {
			tc6->int_flag = false;
			ret = oa_tc6_perform_ctrl(tc6, OA_TC6_STS0, &regval, 1,
						  false, false);
			if (ret) {
				dev_err(&tc6->spi->dev, "Failed to read STS0\n");
				continue;
			}
			/* Check for reset complete interrupt status */
			if (regval & RESETC) {
				regval = RESETC;
				/* SPI host should write RESETC bit with one to
				 * clear the reset interrupt status.
				 */
				ret = oa_tc6_perform_ctrl(tc6, OA_TC6_STS0,
							  &regval, 1, true,
							  false);
				if (ret) {
					dev_err(&tc6->spi->dev,
						"Failed to write STS0\n");
					continue;
				}
				complete(&tc6->rst_complete);
			}
		}
	}
	return 0;
}

static irqreturn_t macphy_irq(int irq, void *dev_id)
{
	struct oa_tc6 *tc6 = dev_id;

	/* Wake tc6 task to perform interrupt action */
	tc6->int_flag = true;
	wake_up_interruptible(&tc6->tc6_wq);

	return IRQ_HANDLED;
}

static int oa_tc6_sw_reset(struct oa_tc6 *tc6)
{
	long timeleft;
	u32 regval;
	int ret;

	/* Perform software reset with both protected and unprotected control
	 * commands because the driver doesn't know the current status of the
	 * MAC-PHY.
	 */
	regval = SW_RESET;
	reinit_completion(&tc6->rst_complete);
	ret = oa_tc6_perform_ctrl(tc6, OA_TC6_RESET, &regval, 1, true, false);
	if (ret) {
		dev_err(&tc6->spi->dev, "RESET register write failed\n");
		return ret;
	}

	ret = oa_tc6_perform_ctrl(tc6, OA_TC6_RESET, &regval, 1, true, true);
	if (ret) {
		dev_err(&tc6->spi->dev, "RESET register write failed\n");
		return ret;
	}
	timeleft = wait_for_completion_interruptible_timeout(&tc6->rst_complete,
							     msecs_to_jiffies(1));
	if (timeleft <= 0) {
		dev_err(&tc6->spi->dev, "MAC-PHY reset failed\n");
		return -ENODEV;
	}

	return 0;
}

int oa_tc6_write_register(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len)
{
	return oa_tc6_perform_ctrl(tc6, addr, val, len, true, tc6->ctrl_prot);
}
EXPORT_SYMBOL_GPL(oa_tc6_write_register);

int oa_tc6_read_register(struct oa_tc6 *tc6, u32 addr, u32 val[], u8 len)
{
	return oa_tc6_perform_ctrl(tc6, addr, val, len, false, tc6->ctrl_prot);
}
EXPORT_SYMBOL_GPL(oa_tc6_read_register);

int oa_tc6_configure(struct oa_tc6 *tc6, u8 cps, bool ctrl_prot, bool tx_cut_thr,
		     bool rx_cut_thr)
{
	u32 regval;
	int ret;

	/* Read and configure the IMASK0 register for unmasking the interrupts */
	ret = oa_tc6_read_register(tc6, OA_TC6_IMASK0, &regval, 1);
	if (ret)
		return ret;

	regval &= TXPEM & TXBOEM & TXBUEM & RXBOEM & LOFEM & HDREM;
	ret = oa_tc6_write_register(tc6, OA_TC6_IMASK0, &regval, 1);
	if (ret)
		return ret;

	/* Configure the CONFIG0 register with the required configurations */
	regval = SYNC;
	if (ctrl_prot)
		regval |= PROTE;
	if (tx_cut_thr)
		regval |= TXCTE;
	if (rx_cut_thr)
		regval |= RXCTE;
	regval |= FIELD_PREP(CPS, ilog2(cps) / ilog2(2));

	ret = oa_tc6_write_register(tc6, OA_TC6_CONFIG0, &regval, 1);
	if (ret)
		return ret;

	tc6->cps = cps;
	tc6->ctrl_prot = ctrl_prot;
	tc6->tx_cut_thr = tx_cut_thr;
	tc6->rx_cut_thr = rx_cut_thr;

	return 0;
}
EXPORT_SYMBOL_GPL(oa_tc6_configure);

struct oa_tc6 *oa_tc6_init(struct spi_device *spi)
{
	struct oa_tc6 *tc6;
	int ret;

	if (!spi)
		return NULL;

	tc6 = kzalloc(sizeof(*tc6), GFP_KERNEL);
	if (!tc6)
		return NULL;

	tc6->spi = spi;

	/* Used for triggering the OA TC6 task */
	init_waitqueue_head(&tc6->tc6_wq);

	init_completion(&tc6->rst_complete);

	/* This task performs the SPI transfer */
	tc6->tc6_task = kthread_run(oa_tc6_handler, tc6, "OA TC6 Task");
	if (IS_ERR(tc6->tc6_task))
		goto err_tc6_task;

	/* Set the highest priority to the tc6 task as it is time critical */
	sched_set_fifo(tc6->tc6_task);

	/* Register MAC-PHY interrupt service routine */
	ret = devm_request_irq(&spi->dev, spi->irq, macphy_irq, 0, "macphy int",
			       tc6);
	if ((ret != -ENOTCONN) && ret < 0) {
		dev_err(&spi->dev, "Error attaching macphy irq %d\n", ret);
		goto err_macphy_irq;
	}

	/* Perform MAC-PHY software reset */
	if (oa_tc6_sw_reset(tc6))
		goto err_macphy_reset;

	return tc6;

err_macphy_reset:
	devm_free_irq(&tc6->spi->dev, tc6->spi->irq, tc6);
err_macphy_irq:
	kthread_stop(tc6->tc6_task);
err_tc6_task:
	kfree(tc6);
	return NULL;
}
EXPORT_SYMBOL_GPL(oa_tc6_init);

int oa_tc6_deinit(struct oa_tc6 *tc6)
{
	int ret;

	devm_free_irq(&tc6->spi->dev, tc6->spi->irq, tc6);
	ret = kthread_stop(tc6->tc6_task);
	if (!ret)
		kfree(tc6);
	return ret;
}
EXPORT_SYMBOL_GPL(oa_tc6_deinit);
