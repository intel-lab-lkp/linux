// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 *    Zheng Yang <zhengyang@rock-chips.com>
 *    Yakir Yang <ykk@rock-chips.com>
 * Copyright (C) StarFive Technology Co., Ltd.
 */

#include <linux/irq.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#include <drm/bridge/inno_hdmi.h>
#include <drm/display/drm_hdmi_state_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>

#include "inno-hdmi.h"

u8 hdmi_readb(struct inno_hdmi *hdmi, u16 offset)
{
	return readl_relaxed(hdmi->regs + (offset) * 0x04);
}
EXPORT_SYMBOL_GPL(hdmi_readb);

void hdmi_writeb(struct inno_hdmi *hdmi, u16 offset, u32 val)
{
	writel_relaxed(val, hdmi->regs + (offset) * 0x04);
}
EXPORT_SYMBOL_GPL(hdmi_writeb);

void hdmi_modb(struct inno_hdmi *hdmi, u16 offset, u32 msk, u32 val)
{
	u8 temp = hdmi_readb(hdmi, offset) & ~msk;

	temp |= val & msk;
	hdmi_writeb(hdmi, offset, temp);
}
EXPORT_SYMBOL_GPL(hdmi_modb);

void inno_hdmi_i2c_init(struct inno_hdmi *hdmi, unsigned long long rate)
{
	unsigned long long ddc_bus_freq = rate >> 2;

	do_div(ddc_bus_freq, HDMI_SCL_RATE);

	hdmi_writeb(hdmi, DDC_BUS_FREQ_L, ddc_bus_freq & 0xFF);
	hdmi_writeb(hdmi, DDC_BUS_FREQ_H, (ddc_bus_freq >> 8) & 0xFF);

	/* Clear the EDID interrupt flag and mute the interrupt */
	hdmi_writeb(hdmi, HDMI_INTERRUPT_MASK1, 0);
	hdmi_writeb(hdmi, HDMI_INTERRUPT_STATUS1, m_INT_EDID_READY);
}
EXPORT_SYMBOL_GPL(inno_hdmi_i2c_init);

static irqreturn_t inno_hdmi_i2c_irq(struct inno_hdmi *hdmi)
{
	struct inno_hdmi_i2c *i2c = hdmi->i2c;
	u8 stat;

	stat = hdmi_readb(hdmi, HDMI_INTERRUPT_STATUS1);
	if (!(stat & m_INT_EDID_READY))
		return IRQ_NONE;

	/* Clear HDMI EDID interrupt flag */
	hdmi_writeb(hdmi, HDMI_INTERRUPT_STATUS1, m_INT_EDID_READY);

	complete(&i2c->cmp);

	return IRQ_HANDLED;
}

static irqreturn_t inno_hdmi_hardirq(int irq, void *dev_id)
{
	struct inno_hdmi *hdmi = dev_id;
	irqreturn_t ret = IRQ_NONE;
	u8 interrupt;

	if (hdmi->i2c)
		ret = inno_hdmi_i2c_irq(hdmi);

	interrupt = hdmi_readb(hdmi, HDMI_STATUS);
	if (interrupt & m_INT_HOTPLUG) {
		hdmi_modb(hdmi, HDMI_STATUS, m_INT_HOTPLUG, m_INT_HOTPLUG);
		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}

static irqreturn_t inno_hdmi_irq(int irq, void *dev_id)
{
	struct inno_hdmi *hdmi = dev_id;

	drm_helper_hpd_irq_event(hdmi->bridge.dev);

	return IRQ_HANDLED;
}

static int inno_hdmi_i2c_read(struct inno_hdmi *hdmi, struct i2c_msg *msgs)
{
	int length = msgs->len;
	u8 *buf = msgs->buf;
	int ret;

	ret = wait_for_completion_timeout(&hdmi->i2c->cmp, HZ / 10);
	if (!ret)
		return -EAGAIN;

	while (length--)
		*buf++ = hdmi_readb(hdmi, HDMI_EDID_FIFO_ADDR);

	return 0;
}

static int inno_hdmi_i2c_write(struct inno_hdmi *hdmi, struct i2c_msg *msgs)
{
	/*
	 * The DDC module only support read EDID message, so
	 * we assume that each word write to this i2c adapter
	 * should be the offset of EDID word address.
	 */
	if (msgs->len != 1 || (msgs->addr != DDC_ADDR && msgs->addr != DDC_SEGMENT_ADDR))
		return -EINVAL;

	reinit_completion(&hdmi->i2c->cmp);

	if (msgs->addr == DDC_SEGMENT_ADDR)
		hdmi->i2c->segment_addr = msgs->buf[0];
	if (msgs->addr == DDC_ADDR)
		hdmi->i2c->ddc_addr = msgs->buf[0];

	/* Set edid fifo first addr */
	hdmi_writeb(hdmi, HDMI_EDID_FIFO_OFFSET, 0x00);

	/* Set edid word address 0x00/0x80 */
	hdmi_writeb(hdmi, HDMI_EDID_WORD_ADDR, hdmi->i2c->ddc_addr);

	/* Set edid segment pointer */
	hdmi_writeb(hdmi, HDMI_EDID_SEGMENT_POINTER, hdmi->i2c->segment_addr);

	return 0;
}

static int inno_hdmi_i2c_xfer(struct i2c_adapter *adap,
			      struct i2c_msg *msgs, int num)
{
	struct inno_hdmi *hdmi = i2c_get_adapdata(adap);
	struct inno_hdmi_i2c *i2c = hdmi->i2c;
	int i, ret = 0;

	guard(mutex)(&i2c->lock);
	/* Clear the EDID interrupt flag and unmute the interrupt */
	hdmi_writeb(hdmi, HDMI_INTERRUPT_MASK1, m_INT_EDID_READY);
	hdmi_writeb(hdmi, HDMI_INTERRUPT_STATUS1, m_INT_EDID_READY);

	for (i = 0; i < num; i++) {
		DRM_DEV_DEBUG(hdmi->dev,
			      "xfer: num: %d/%d, len: %d, flags: %#x\n",
			      i + 1, num, msgs[i].len, msgs[i].flags);

		if (msgs[i].flags & I2C_M_RD)
			ret = inno_hdmi_i2c_read(hdmi, &msgs[i]);
		else
			ret = inno_hdmi_i2c_write(hdmi, &msgs[i]);

		if (ret < 0)
			break;
	}

	if (!ret)
		ret = num;

	/* Mute HDMI EDID interrupt */
	hdmi_writeb(hdmi, HDMI_INTERRUPT_MASK1, 0);

	return ret;
}

static u32 inno_hdmi_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm inno_hdmi_algorithm = {
	.master_xfer	= inno_hdmi_i2c_xfer,
	.functionality	= inno_hdmi_i2c_func,
};

static struct i2c_adapter *inno_hdmi_i2c_adapter(struct inno_hdmi *hdmi)
{
	struct i2c_adapter *adap;
	struct inno_hdmi_i2c *i2c;
	int ret;

	i2c = devm_kzalloc(hdmi->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return ERR_PTR(-ENOMEM);

	mutex_init(&i2c->lock);
	init_completion(&i2c->cmp);

	adap = &i2c->adap;
	adap->owner = THIS_MODULE;
	adap->dev.parent = hdmi->dev;
	adap->dev.of_node = hdmi->dev->of_node;
	adap->algo = &inno_hdmi_algorithm;
	strscpy(adap->name, "Inno HDMI", sizeof(adap->name));
	i2c_set_adapdata(adap, hdmi);

	ret = devm_i2c_add_adapter(hdmi->dev, adap);
	if (ret) {
		dev_warn(hdmi->dev, "cannot add %s I2C adapter\n", adap->name);
		return ERR_PTR(ret);
	}

	hdmi->i2c = i2c;

	DRM_DEV_DEBUG(hdmi->dev, "registered %s I2C bus driver\n", adap->name);

	return adap;
}

static int inno_bridge_attach(struct drm_bridge *bridge,
			      enum drm_bridge_attach_flags flags)
{
	struct inno_hdmi *hdmi = bridge->driver_private;

	if (flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)
		return drm_bridge_attach(bridge->encoder, hdmi->next_bridge,
					 bridge, flags);

	return 0;
}

static enum drm_connector_status
inno_hdmi_bridge_detect(struct drm_bridge *bridge)
{
	struct inno_hdmi *hdmi = bridge->driver_private;

	return (hdmi_readb(hdmi, HDMI_STATUS) & m_HOTPLUG) ?
		connector_status_connected : connector_status_disconnected;
}

static const struct drm_edid *inno_bridge_edid_read(struct drm_bridge *bridge,
						    struct drm_connector *connector)
{
	struct inno_hdmi *hdmi = bridge->driver_private;

	return drm_edid_read_ddc(connector, hdmi->bridge.ddc);
}

static enum drm_mode_status
inno_bridge_mode_valid(struct drm_bridge *bridge,
		       const struct drm_display_info *info,
		       const struct drm_display_mode *mode)
{
	struct inno_hdmi *hdmi = bridge->driver_private;

	enum drm_mode_status mode_status = MODE_OK;

	if (hdmi->plat_data->mode_valid)
		mode_status = hdmi->plat_data->mode_valid(hdmi, mode);

	return mode_status;
}

static int inno_hdmi_clear_infoframe(struct drm_bridge *bridge,
				     enum hdmi_infoframe_type type)
{
	struct inno_hdmi *hdmi = bridge->driver_private;
	int ret = 0;

	if (hdmi->plat_data->hdmi_clear_infoframe)
		ret = hdmi->plat_data->hdmi_clear_infoframe(hdmi, type);

	return ret;
}

static int inno_write_infoframe(struct drm_bridge *bridge,
				enum hdmi_infoframe_type type,
				const u8 *buffer, size_t len)
{
	struct inno_hdmi *hdmi = bridge->driver_private;
	int ret = 0;

	if (hdmi->plat_data->hdmi_write_infoframe)
		ret = hdmi->plat_data->hdmi_write_infoframe(hdmi, type, buffer, len);

	return ret;
}

static int inno_bridge_atomic_check(struct drm_bridge *bridge,
					   struct drm_bridge_state *bridge_state,
					   struct drm_crtc_state *crtc_state,
					   struct drm_connector_state *conn_state)
{
	return drm_atomic_helper_connector_hdmi_check(conn_state->connector,
							  bridge_state->base.state);
}

static const struct drm_bridge_funcs inno_bridge_funcs = {
	.atomic_check	  = inno_bridge_atomic_check,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.attach = inno_bridge_attach,
	.detect = inno_hdmi_bridge_detect,
	.edid_read = inno_bridge_edid_read,
	.mode_valid = inno_bridge_mode_valid,
	.hdmi_clear_infoframe = inno_hdmi_clear_infoframe,
	.hdmi_write_infoframe = inno_write_infoframe,
};

int inno_hdmi_bind(struct drm_device *drm, struct inno_hdmi *hdmi, struct drm_encoder	*encoder)
{
	int ret, irq;
	struct platform_device *pdev = to_platform_device(hdmi->dev);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		return ret;
	}

	hdmi->ddc = inno_hdmi_i2c_adapter(hdmi);
	if (IS_ERR(hdmi->ddc)) {
		ret = PTR_ERR(hdmi->ddc);
		hdmi->ddc = NULL;
		return ret;
	}

	hdmi->next_bridge = devm_drm_of_get_bridge(hdmi->dev, hdmi->dev->of_node, 1, 0);
	if (IS_ERR(hdmi->next_bridge))
		return dev_err_probe(hdmi->dev, PTR_ERR(hdmi->next_bridge),
				     "failed to acquire drm_bridge\n");

	hdmi->bridge.driver_private = hdmi;
	hdmi->bridge.funcs = &inno_bridge_funcs;
	hdmi->bridge.ddc = hdmi->ddc;
#ifdef CONFIG_OF
	hdmi->bridge.of_node = hdmi->dev->of_node;
#endif
	hdmi->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID |
					 DRM_BRIDGE_OP_HPD | DRM_BRIDGE_OP_HDMI;
	hdmi->bridge.type = DRM_MODE_CONNECTOR_HDMIA;

	hdmi->bridge.vendor = hdmi->plat_data->vendor;
	hdmi->bridge.product = hdmi->plat_data->product;

	devm_drm_bridge_add(hdmi->dev, &hdmi->bridge);

	/* Unmute hotplug interrupt */
	hdmi_modb(hdmi, HDMI_STATUS, m_MASK_INT_HOTPLUG, v_MASK_INT_HOTPLUG(1));

	ret = devm_request_threaded_irq(hdmi->dev, irq, inno_hdmi_hardirq,
					inno_hdmi_irq, IRQF_SHARED,
					dev_name(hdmi->dev), hdmi);
	return ret;
}
EXPORT_SYMBOL_GPL(inno_hdmi_bind);

MODULE_AUTHOR("Keith Zhao <keithzhao@starfivetech.com>");
MODULE_DESCRIPTION("INNO HDMI transmitter driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:inno-hdmi");
