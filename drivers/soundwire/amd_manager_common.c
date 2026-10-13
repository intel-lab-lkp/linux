// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * SoundWire AMD Manager driver
 *
 * Copyright 2026 Advanced Micro Devices, Inc.
 */

#include <linux/completion.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/pm_runtime.h>
#include <linux/wait.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "bus.h"
#include "amd_init.h"
#include "amd_manager.h"

#define DRV_NAME "amd_sdw_manager"

#define to_amd_sdw(b)	container_of(b, struct amd_sdw_manager, bus)

static void amd_sdw_ctl_word_prep(u32 *lower_word, u32 *upper_word, struct sdw_msg *msg,
				  int cmd_offset)
{
	u32 upper_data;
	u32 lower_data = 0;
	u16 addr;
	u8 upper_addr, lower_addr;
	u8 data = 0;

	addr = msg->addr + cmd_offset;
	upper_addr = (addr & 0xFF00) >> 8;
	lower_addr = addr & 0xFF;

	if (msg->flags == SDW_MSG_FLAG_WRITE)
		data = msg->buf[cmd_offset];

	upper_data = FIELD_PREP(AMD_SDW_MCP_CMD_DEV_ADDR, msg->dev_num);
	upper_data |= FIELD_PREP(AMD_SDW_MCP_CMD_COMMAND, msg->flags + 2);
	upper_data |= FIELD_PREP(AMD_SDW_MCP_CMD_REG_ADDR_HIGH, upper_addr);
	lower_data |= FIELD_PREP(AMD_SDW_MCP_CMD_REG_ADDR_LOW, lower_addr);
	lower_data |= FIELD_PREP(AMD_SDW_MCP_CMD_REG_DATA, data);

	*upper_word = upper_data;
	*lower_word = lower_data;
}

static enum sdw_command_response
amd_program_scp_addr(struct amd_sdw_manager *amd_manager, struct sdw_msg *msg)
{
	struct sdw_msg scp_msg = {0};
	u64 response_buf[2] = {0};
	u32 upper_data = 0, lower_data = 0;
	int index;

	scp_msg.dev_num = msg->dev_num;
	scp_msg.addr = SDW_SCP_ADDRPAGE1;
	scp_msg.buf = &msg->addr_page1;
	scp_msg.flags = SDW_MSG_FLAG_WRITE;
	amd_sdw_ctl_word_prep(&lower_data, &upper_data, &scp_msg, 0);
	if (acp_sdw_send_cmd_get_resp(amd_manager, lower_data, upper_data, &response_buf[0])) {
		dev_err_ratelimited(amd_manager->dev,
				    "SCP_addrpage command timeout for Slave %d\n", msg->dev_num);
		return SDW_CMD_TIMEOUT;
	}
	scp_msg.addr = SDW_SCP_ADDRPAGE2;
	scp_msg.buf = &msg->addr_page2;
	amd_sdw_ctl_word_prep(&lower_data, &upper_data, &scp_msg, 0);
	if (acp_sdw_send_cmd_get_resp(amd_manager, lower_data, upper_data, &response_buf[1])) {
		dev_err_ratelimited(amd_manager->dev,
				    "SCP_addrpage command timeout for Slave %d\n", msg->dev_num);
		return SDW_CMD_TIMEOUT;
	}

	for (index = 0; index < 2; index++) {
		if (!(response_buf[index] & AMD_SDW_MCP_RESP_ACK)) {
			if (response_buf[index] & AMD_SDW_MCP_RESP_NACK) {
				dev_err_ratelimited(amd_manager->dev,
						    "SCP_addrpage NACKed for Slave %d\n",
						    msg->dev_num);
				return SDW_CMD_FAIL;
			}
			dev_dbg_ratelimited(amd_manager->dev, "SCP_addrpage ignored for Slave %d\n",
					    msg->dev_num);
			return SDW_CMD_IGNORED;
		}
	}
	return SDW_CMD_OK;
}

static int amd_prep_msg(struct amd_sdw_manager *amd_manager, struct sdw_msg *msg)
{
	int ret;

	if (msg->page) {
		ret = amd_program_scp_addr(amd_manager, msg);
		if (ret) {
			msg->len = 0;
			return ret;
		}
	}
	switch (msg->flags) {
	case SDW_MSG_FLAG_READ:
	case SDW_MSG_FLAG_WRITE:
		break;
	default:
		dev_err(amd_manager->dev, "Invalid msg cmd: %d\n", msg->flags);
		return -EINVAL;
	}
	return 0;
}

static enum sdw_command_response amd_sdw_fill_msg_resp(struct amd_sdw_manager *amd_manager,
						       struct sdw_msg *msg, u64 response,
						       int offset)
{
	if (response & AMD_SDW_MCP_RESP_ACK) {
		if (msg->flags == SDW_MSG_FLAG_READ)
			msg->buf[offset] = FIELD_GET(AMD_SDW_MCP_RESP_RDATA, response);
	} else {
		if (response & AMD_SDW_MCP_RESP_NACK) {
			dev_err_ratelimited(amd_manager->dev,
					    "command response NACK received for Slave %d\n",
					    msg->dev_num);
			return SDW_CMD_FAIL;
		}
		dev_dbg_ratelimited(amd_manager->dev, "command is ignored for Slave %d\n",
				    msg->dev_num);
		return SDW_CMD_IGNORED;
	}
	return SDW_CMD_OK;
}

/* AMD SoundWire bus helper shared across all ACP revisions. */
static unsigned int _amd_sdw_xfer_msg(struct amd_sdw_manager *amd_manager, struct sdw_msg *msg,
				      int cmd_offset)
{
	u64 response;
	u32 upper_data = 0, lower_data = 0;

	amd_sdw_ctl_word_prep(&lower_data, &upper_data, msg, cmd_offset);
	if (acp_sdw_send_cmd_get_resp(amd_manager, lower_data, upper_data, &response)) {
		dev_err_ratelimited(amd_manager->dev, "command timeout for Slave %d\n",
				    msg->dev_num);
		return SDW_CMD_TIMEOUT;
	}
	return amd_sdw_fill_msg_resp(amd_manager, msg, response, cmd_offset);
}

/* AMD SoundWire bus helper shared across all ACP revisions. */
enum sdw_command_response amd_sdw_xfer_msg(struct sdw_bus *bus, struct sdw_msg *msg)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	int ret, i;

	ret = amd_prep_msg(amd_manager, msg);
	if (ret)
		return SDW_CMD_FAIL_OTHER;
	for (i = 0; i < msg->len; i++) {
		ret = _amd_sdw_xfer_msg(amd_manager, msg, i);
		if (ret)
			return ret;
	}
	return SDW_CMD_OK;
}

/* AMD SoundWire bus helper shared across all ACP revisions. */
void amd_sdw_fill_slave_status(struct amd_sdw_manager *amd_manager, u16 index, u32 status)
{
	switch (status) {
	case SDW_SLAVE_ATTACHED:
	case SDW_SLAVE_UNATTACHED:
	case SDW_SLAVE_ALERT:
		amd_manager->status[index] = status;
		break;
	default:
		amd_manager->status[index] = SDW_SLAVE_RESERVED;
		break;
	}
}

/* AMD SoundWire bus helper shared across all ACP revisions. */
void amd_sdw_process_ping_status(u64 response, struct amd_sdw_manager *amd_manager)
{
	u64 slave_stat;
	u32 val;
	u16 dev_index;

	/* slave status response */
	slave_stat = FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_0_3, response);
	slave_stat |= FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_4_11, response) << 8;
	dev_dbg(amd_manager->dev, "slave_stat:0x%llx\n", slave_stat);
	for (dev_index = 0; dev_index <= SDW_MAX_DEVICES; ++dev_index) {
		val = (slave_stat >> (dev_index * 2)) & AMD_SDW_MCP_SLAVE_STATUS_MASK;
		dev_dbg(amd_manager->dev, "val:0x%x\n", val);
		amd_sdw_fill_slave_status(amd_manager, dev_index, val);
	}
}

static int sdw_master_read_amd_prop(struct sdw_bus *bus)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	struct fwnode_handle *link;
	struct sdw_master_prop *prop;
	u32 quirk_mask = 0;
	u32 wake_en_mask = 0;
	u32 power_mode_mask = 0;
	char name[32];

	prop = &bus->prop;
	/* Find manager handle */
	snprintf(name, sizeof(name), "mipi-sdw-link-%d-subproperties", bus->link_id);
	link = device_get_named_child_node(bus->dev, name);
	if (!link) {
		dev_err(bus->dev, "Manager node %s not found\n", name);
		return -EIO;
	}
	fwnode_property_read_u32(link, "amd-sdw-enable", &quirk_mask);
	if (!(quirk_mask & AMD_SDW_QUIRK_MASK_BUS_ENABLE))
		prop->hw_disabled = true;
	prop->quirks = SDW_MASTER_QUIRKS_CLEAR_INITIAL_CLASH |
		       SDW_MASTER_QUIRKS_CLEAR_INITIAL_PARITY;

	fwnode_property_read_u32(link, "amd-sdw-wakeup-enable", &wake_en_mask);
	amd_manager->wake_en_mask = wake_en_mask;
	fwnode_property_read_u32(link, "amd-sdw-power-mode", &power_mode_mask);
	amd_manager->power_mode_mask = power_mode_mask;

	fwnode_handle_put(link);

	return 0;
}

/* AMD SoundWire bus helper shared across all ACP revisions. */
int amd_prop_read(struct sdw_bus *bus)
{
	sdw_master_read_prop(bus);
	sdw_master_read_amd_prop(bus);
	return 0;
}

static int amd_sdw_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct amd_sdw_manager *amd_manager = snd_soc_dai_get_drvdata(dai);
	struct sdw_amd_dai_runtime *dai_runtime;
	struct sdw_stream_config sconfig;
	int ch, dir;
	int ret;

	dai_runtime = amd_manager->dai_runtime_array[dai->id];
	if (!dai_runtime)
		return -EIO;

	ch = params_channels(params);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		dir = SDW_DATA_DIR_RX;
	else
		dir = SDW_DATA_DIR_TX;
	dev_dbg(amd_manager->dev, "dir:%d dai->id:0x%x\n", dir, dai->id);

	sconfig.direction = dir;
	sconfig.ch_count = ch;
	sconfig.frame_rate = params_rate(params);
	sconfig.type = dai_runtime->stream_type;

	sconfig.bps = snd_pcm_format_width(params_format(params));

	/* Port configuration */
	struct sdw_port_config *pconfig __free(kfree) = kzalloc_obj(*pconfig);
	if (!pconfig)
		return -ENOMEM;

	pconfig->num = dai->id;
	pconfig->ch_mask = (1 << ch) - 1;
	ret = sdw_stream_add_master(&amd_manager->bus, &sconfig,
				    pconfig, 1, dai_runtime->stream);
	if (ret)
		dev_err(amd_manager->dev, "add manager to stream failed:%d\n", ret);

	return ret;
}

static int amd_sdw_hw_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct amd_sdw_manager *amd_manager = snd_soc_dai_get_drvdata(dai);
	struct sdw_amd_dai_runtime *dai_runtime;
	int ret;

	dai_runtime = amd_manager->dai_runtime_array[dai->id];
	if (!dai_runtime)
		return -EIO;

	ret = sdw_stream_remove_master(&amd_manager->bus, dai_runtime->stream);
	if (ret < 0)
		dev_err(dai->dev, "remove manager from stream %s failed: %d\n",
			dai_runtime->stream->name, ret);
	return ret;
}

static int amd_set_sdw_stream(struct snd_soc_dai *dai, void *stream, int direction)
{
	struct amd_sdw_manager *amd_manager = snd_soc_dai_get_drvdata(dai);
	struct sdw_amd_dai_runtime *dai_runtime;

	dai_runtime = amd_manager->dai_runtime_array[dai->id];
	if (stream) {
		/* first paranoia check */
		if (dai_runtime) {
			dev_err(dai->dev, "dai_runtime already allocated for dai %s\n",	dai->name);
			return -EINVAL;
		}

		/* allocate and set dai_runtime info */
		dai_runtime = kzalloc_obj(*dai_runtime);
		if (!dai_runtime)
			return -ENOMEM;

		dai_runtime->stream_type = SDW_STREAM_PCM;
		dai_runtime->bus = &amd_manager->bus;
		dai_runtime->stream = stream;
		amd_manager->dai_runtime_array[dai->id] = dai_runtime;
	} else {
		/* second paranoia check */
		if (!dai_runtime) {
			dev_err(dai->dev, "dai_runtime not allocated for dai %s\n", dai->name);
			return -EINVAL;
		}

		/* for NULL stream we release allocated dai_runtime */
		kfree(dai_runtime);
		amd_manager->dai_runtime_array[dai->id] = NULL;
	}
	return 0;
}

static int amd_pcm_set_sdw_stream(struct snd_soc_dai *dai, void *stream, int direction)
{
	return amd_set_sdw_stream(dai, stream, direction);
}

static void *amd_get_sdw_stream(struct snd_soc_dai *dai, int direction)
{
	struct amd_sdw_manager *amd_manager = snd_soc_dai_get_drvdata(dai);
	struct sdw_amd_dai_runtime *dai_runtime;

	dai_runtime = amd_manager->dai_runtime_array[dai->id];
	if (!dai_runtime)
		return ERR_PTR(-EINVAL);

	return dai_runtime->stream;
}

static const struct snd_soc_dai_ops amd_sdw_dai_ops = {
	.hw_params = amd_sdw_hw_params,
	.hw_free = amd_sdw_hw_free,
	.set_stream = amd_pcm_set_sdw_stream,
	.get_stream = amd_get_sdw_stream,
};

static const struct snd_soc_component_driver amd_sdw_dai_component = {
	.name = "soundwire",
};

static int amd_sdw_register_dais(struct amd_sdw_manager *amd_manager)
{
	struct sdw_amd_dai_runtime **dai_runtime_array;
	struct snd_soc_dai_driver *dais;
	struct snd_soc_pcm_stream *stream;
	struct device *dev;
	int i, num_dais;

	dev = amd_manager->dev;
	num_dais = amd_manager->num_dout_ports + amd_manager->num_din_ports;
	dais = devm_kcalloc(dev, num_dais, sizeof(*dais), GFP_KERNEL);
	if (!dais)
		return -ENOMEM;

	dai_runtime_array = devm_kcalloc(dev, num_dais,
					 sizeof(struct sdw_amd_dai_runtime *),
					 GFP_KERNEL);
	if (!dai_runtime_array)
		return -ENOMEM;
	amd_manager->dai_runtime_array = dai_runtime_array;
	for (i = 0; i < num_dais; i++) {
		dais[i].name = devm_kasprintf(dev, GFP_KERNEL, "SDW%d Pin%d", amd_manager->instance,
					      i);
		if (!dais[i].name)
			return -ENOMEM;
		if (i < amd_manager->num_dout_ports)
			stream = &dais[i].playback;
		else
			stream = &dais[i].capture;

		stream->channels_min = 2;
		stream->channels_max = 2;
		stream->rates = SNDRV_PCM_RATE_48000;
		stream->formats = SNDRV_PCM_FMTBIT_S16_LE;

		dais[i].ops = &amd_sdw_dai_ops;
		dais[i].id = i;
	}

	return devm_snd_soc_register_component(dev, &amd_sdw_dai_component,
					       dais, num_dais);
}

static void amd_sdw_update_slave_status_work(struct work_struct *work)
{
	struct amd_sdw_manager *amd_manager =
		container_of(work, struct amd_sdw_manager, amd_sdw_work);

	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		amd_acp63_sdw_update_slave_status_work(amd_manager);
		break;
	default:
		dev_err(amd_manager->dev, "unexpected acp_rev 0x%x\n", amd_manager->acp_rev);
		return;
	}
}

void amd_sdw_update_slave_status(u32 status_change_0to7, u32 status_change_8to11,
				 struct amd_sdw_manager *amd_manager)
{
	u64 slave_stat;
	u32 val;
	int dev_index;

	if (status_change_0to7 == AMD_SDW_SLAVE_0_ATTACHED)
		memset(amd_manager->status, 0, sizeof(amd_manager->status));
	slave_stat = status_change_0to7;
	slave_stat |= FIELD_GET(AMD_SDW_MCP_SLAVE_STATUS_8TO_11, status_change_8to11) << 32;
	dev_dbg(amd_manager->dev, "status_change_0to7:0x%x status_change_8to11:0x%x\n",
		status_change_0to7, status_change_8to11);
	if (slave_stat) {
		for (dev_index = 0; dev_index <= SDW_MAX_DEVICES; ++dev_index) {
			if (slave_stat & AMD_SDW_MCP_SLAVE_STATUS_VALID_MASK(dev_index)) {
				val = (slave_stat >> AMD_SDW_MCP_SLAVE_STAT_SHIFT_MASK(dev_index)) &
				      AMD_SDW_MCP_SLAVE_STATUS_MASK;
				amd_sdw_fill_slave_status(amd_manager, dev_index, val);
			}
		}
	}
}

static void amd_sdw_irq_thread(struct work_struct *work)
{
	struct amd_sdw_manager *amd_manager =
			container_of(work, struct amd_sdw_manager, amd_sdw_irq_thread);

	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		amd_acp63_sdw_irq_thread(amd_manager);
		break;
	default:
		dev_err(amd_manager->dev, "unexpected acp_rev 0x%x\n", amd_manager->acp_rev);
		return;
	}
}

int amd_sdw_manager_start(struct amd_sdw_manager *amd_manager)
{
	struct sdw_master_prop *prop;
	int ret;

	prop = &amd_manager->bus.prop;
	if (!prop->hw_disabled) {
		ret = acp_sdw_clk_init_ctrl(amd_manager);
		if (ret)
			return ret;
		ret = acp_init_sdw_manager(amd_manager);
		if (ret)
			return ret;
		acp_enable_sdw_interrupts(amd_manager);
		ret = acp_enable_sdw_manager(amd_manager);
		if (ret)
			return ret;
		acp_sdw_set_frameshape(amd_manager);
	}
	/* Enable runtime PM */
	pm_runtime_set_autosuspend_delay(amd_manager->dev, AMD_SDW_MASTER_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(amd_manager->dev);
	pm_runtime_mark_last_busy(amd_manager->dev);
	pm_runtime_set_active(amd_manager->dev);
	pm_runtime_enable(amd_manager->dev);
	return 0;
}

/*
 * AMD SoundWire clock-stop sequence shared across all ACP revisions.
 * Differences between revisions in this path are minimal; a switch/case
 * on acp_rev is sufficient to accommodate any variant-specific additions
 * needed for future platforms.
 */
int amd_sdw_clock_stop(struct amd_sdw_manager *amd_manager)
{
	u32 val;
	int ret;

	ret = sdw_bus_prep_clk_stop(&amd_manager->bus);
	if (ret < 0 && ret != -ENODATA) {
		dev_err(amd_manager->dev, "prepare clock stop failed %d", ret);
		return 0;
	}
	ret = sdw_bus_clk_stop(&amd_manager->bus);
	if (ret < 0 && ret != -ENODATA) {
		dev_err(amd_manager->dev, "bus clock stop failed %d", ret);
		return 0;
	}

	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL, val,
				 (val & AMD_SDW_CLK_STOP_DONE), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "SDW%x clock stop failed\n", amd_manager->instance);
		return 0;
	}

	amd_manager->clk_stopped = true;
	if (amd_manager->wake_en_mask)
		writel(0x01, amd_manager->acp_mmio + ACP_SW_WAKE_EN(amd_manager->instance));

	dev_dbg(amd_manager->dev, "SDW%x clock stop successful\n", amd_manager->instance);
	return 0;
}

static int amd_resume_child_device(struct device *dev, void *data)
{
	struct sdw_slave *slave = dev_to_sdw_dev(dev);
	int ret;

	if (!slave->probed) {
		dev_dbg(dev, "skipping device, no probed driver\n");
		return 0;
	}
	if (!slave->dev_num_sticky) {
		dev_dbg(dev, "skipping device, never detected on bus\n");
		return 0;
	}
	ret = pm_request_resume(dev);
	if (ret < 0) {
		dev_err(dev, "pm_request_resume failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static int __maybe_unused amd_pm_prepare(struct device *dev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(dev);
	struct sdw_bus *bus = &amd_manager->bus;
	int ret;

	if (bus->prop.hw_disabled) {
		dev_dbg(bus->dev, "SoundWire manager %d is disabled, ignoring\n",
			bus->link_id);
		return 0;
	}
	/*
	 * When multiple peripheral devices connected over the same link, if SoundWire manager
	 * device is not in runtime suspend state, observed that device alerts are missing
	 * without pm_prepare on AMD platforms in clockstop mode0.
	 */
	if (amd_manager->power_mode_mask) {
		ret = pm_runtime_resume(dev);
		if (ret < 0) {
			dev_err(bus->dev, "pm_runtime_resume failed: %d\n", ret);
			return 0;
		}
	}
	/* To force peripheral devices to system level suspend state, resume the devices
	 * from runtime suspend state first. Without that unable to dispatch the alert
	 * status to peripheral driver during system level resume as they are in runtime
	 * suspend state.
	 */
	ret = device_for_each_child(bus->dev, NULL, amd_resume_child_device);
	if (ret < 0)
		dev_err(dev, "amd_resume_child_device failed: %d\n", ret);
	return 0;
}

int amd_sdw_host_wake_enable_by_rev(struct amd_sdw_manager *amd_manager, bool enable)
{
	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		return acp_sdw_host_wake_enable(amd_manager, enable);
	default:
		dev_err(amd_manager->dev, "unexpected acp_rev 0x%x\n", amd_manager->acp_rev);
		return -EINVAL;
	}
	return 0;
}

static int __maybe_unused amd_suspend(struct device *dev)
{
	return acp_sdw_suspend(dev);
}

static int __maybe_unused amd_suspend_runtime(struct device *dev)
{
	return acp_sdw_suspend_runtime(dev);
}

static int __maybe_unused amd_resume_runtime(struct device *dev)
{
	return acp_sdw_resume_runtime(dev);
}

static int amd_sdw_manager_probe(struct platform_device *pdev)
{
	const struct acp_sdw_pdata *pdata = pdev->dev.platform_data;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct sdw_master_prop *prop;
	struct amd_sdw_manager *amd_manager;
	int ret;

	amd_manager = devm_kzalloc(dev, sizeof(struct amd_sdw_manager), GFP_KERNEL);
	if (!amd_manager)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOMEM;

	amd_manager->acp_mmio = devm_ioremap(dev, res->start, resource_size(res));
	if (!amd_manager->acp_mmio) {
		dev_err(dev, "mmio not found\n");
		return -ENOMEM;
	}
	amd_manager->ping_slave_stat = U32_MAX;
	amd_manager->instance = pdata->instance;
	amd_manager->acp_sdw_lock = pdata->acp_sdw_lock;
	amd_manager->acp_rev = pdata->acp_rev;
	amd_manager->dev = dev;
	amd_manager->bus.clk_stop_timeout = 200;
	amd_manager->bus.link_id = amd_manager->instance;
	ret = amd_sdw_hw_init_ops(amd_manager);
	if (ret) {
		dev_err(dev, "Failed to initialize hw ops (%d)\n", ret);
		return ret;
	}

	/*
	 * Due to BIOS compatibility, the two links are exposed within
	 * the scope of a single controller. If this changes, the
	 * controller_id will have to be updated with drv_data
	 * information.
	 */
	amd_manager->bus.controller_id = 0;
	dev_dbg(dev, "acp_rev:0x%x\n", amd_manager->acp_rev);
	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		switch (amd_manager->instance) {
		case ACP_SDW0:
			amd_manager->num_dout_ports = AMD_ACP63_SDW0_MAX_TX_PORTS;
			amd_manager->num_din_ports = AMD_ACP63_SDW0_MAX_RX_PORTS;
			break;
		case ACP_SDW1:
			amd_manager->num_dout_ports = AMD_ACP63_SDW1_MAX_TX_PORTS;
			amd_manager->num_din_ports = AMD_ACP63_SDW1_MAX_RX_PORTS;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		amd_manager->num_dout_ports = AMD_ACP70_SDW_MAX_TX_PORTS;
		amd_manager->num_din_ports = AMD_ACP70_SDW_MAX_RX_PORTS;
		break;
	default:
		dev_err(dev, "unsupported ACP revision: 0x%x\n", amd_manager->acp_rev);
		return -EINVAL;
	}
	amd_manager->max_ports = amd_manager->num_dout_ports + amd_manager->num_din_ports;

	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		amd_manager->mmio = amd_manager->acp_mmio +
				    (amd_manager->instance * ACP63_SDW_MANAGER_REG_OFFSET);
		amd_manager->bus.ops = &amd_acp63_sdw_ops;
		amd_manager->bus.port_ops = &amd_acp63_sdw_port_ops;
		amd_manager->bus.compute_params = &amd_acp63_sdw_compute_params;
		amd_manager->port_offset_map = devm_kcalloc(dev, amd_manager->max_ports,
							    sizeof(int), GFP_KERNEL);
		if (!amd_manager->port_offset_map)
			return -ENOMEM;
		break;
	default:
		dev_err(dev, "unsupported ACP revision: 0x%x\n", amd_manager->acp_rev);
		return -EINVAL;
	}

	prop = &amd_manager->bus.prop;
	prop->mclk_freq = AMD_SDW_BUS_BASE_FREQ;

	ret = sdw_bus_master_add(&amd_manager->bus, dev, dev->fwnode);
	if (ret) {
		dev_err(dev, "Failed to register SoundWire manager(%d)\n", ret);
		return ret;
	}
	ret = amd_sdw_register_dais(amd_manager);
	if (ret) {
		dev_err(dev, "CPU DAI registration failed\n");
		sdw_bus_master_delete(&amd_manager->bus);
		return ret;
	}
	dev_set_drvdata(dev, amd_manager);
	INIT_WORK(&amd_manager->amd_sdw_irq_thread, amd_sdw_irq_thread);
	INIT_WORK(&amd_manager->amd_sdw_work, amd_sdw_update_slave_status_work);
	return 0;
}

static void amd_sdw_manager_remove(struct platform_device *pdev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(&pdev->dev);
	int ret;

	if (pm_runtime_enabled(&pdev->dev))
		pm_runtime_disable(&pdev->dev);
	acp_disable_sdw_interrupts(amd_manager);
	cancel_work_sync(&amd_manager->amd_sdw_irq_thread);
	cancel_work_sync(&amd_manager->amd_sdw_work);
	sdw_bus_master_delete(&amd_manager->bus);
	ret = acp_disable_sdw_manager(amd_manager);
	if (ret)
		dev_err(&pdev->dev, "Failed to disable device (%pe)\n", ERR_PTR(ret));
}

static const struct dev_pm_ops amd_pm = {
	.prepare = amd_pm_prepare,
	SET_SYSTEM_SLEEP_PM_OPS(amd_suspend, amd_resume_runtime)
	SET_RUNTIME_PM_OPS(amd_suspend_runtime, amd_resume_runtime, NULL)
};

static struct platform_driver amd_sdw_driver = {
	.probe	= &amd_sdw_manager_probe,
	.remove = &amd_sdw_manager_remove,
	.driver = {
		.name	= "amd_sdw_manager",
		.pm = &amd_pm,
	}
};
module_platform_driver(amd_sdw_driver);

MODULE_AUTHOR("Vijendar.Mukunda@amd.com");
MODULE_DESCRIPTION("AMD SoundWire driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:" DRV_NAME);
