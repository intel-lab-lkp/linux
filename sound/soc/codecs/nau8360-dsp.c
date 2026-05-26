// SPDX-License-Identifier: GPL-2.0-only
//
// The NAU83G60 Stereo Class-D Amplifier with DSP and I/V-sense driver.
//
// Copyright (C) 2026 Nuvoton Technology Corp.
// Author: David Lin <ctlin0@nuvoton.com>
//         Seven Lee <wtli@nuvoton.com>
//         John Hsu <kchsu0@nuvoton.com>
//         Neo Chang <ylchang2@nuvoton.com>

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#include "nau8360-dsp.h"
#include "nau8360.h"

#define NAU8360_DSP_IDLE_RETRY 10
const char *nau8360_def_firmwares[NAU8360_DSP_FW_NUM] = {
	NAU8360_DSP_FIRMWARE".l", NAU8360_DSP_FIRMWARE".r" };
const unsigned short nau8360_dsp_addr[NAU8360_DSP_FW_NUM] = {
	NAU8360_RF000_DSP_COMM, NAU8360_RF002_DSP_COMM };

static int nau8360_dsp_chan_kcs_setup(struct snd_soc_component *component,
	const char *fw_name, int dsp_addr);

#define NAU_DSP_CMD(_id, _msg, _setup, _reply) \
	[_id] = { \
		.cmd_id = _id, \
		.msg_param = _msg, \
		.setup_data = _setup, \
		.reply_data = _reply, \
	}

#define NAU_DSP_CMD_ID(_id) \
	[_id] = { \
		.cmd_id = _id, \
	}

#define __dsp_dbg_data(dev, prefix, val) \
	dsp_dbg(dev, prefix " %02x %02x %02x %02x", \
		(u8)((val) & 0xff), \
		(u8)(((val) >> 8) & 0xff), \
		(u8)(((val) >> 16) & 0xff), \
		(u8)(((val) >> 24) & 0xff))

#define payload_read(dev, val)  __dsp_dbg_data(dev, "[R]", val)
#define payload_write(dev, val) __dsp_dbg_data(dev, "[W]", val)

static const struct nau8360_cmd_info nau8360_dsp_cmd_table[] = {
	NAU_DSP_CMD(NAU8360_DSP_CMD_GET_COUNTER,      0, 0, 1),
	NAU_DSP_CMD(NAU8360_DSP_CMD_GET_FRAME_STATUS, 0, 0, 1),
	NAU_DSP_CMD(NAU8360_DSP_CMD_GET_REVISION,     0, 0, 1),
	NAU_DSP_CMD(NAU8360_DSP_CMD_GET_KCS_RSLTS,    1, 0, 1),
	NAU_DSP_CMD(NAU8360_DSP_CMD_GET_KCS_SETUP,    1, 0, 1),
	NAU_DSP_CMD(NAU8360_DSP_CMD_SET_KCS_SETUP,    1, 1, 1),
	NAU_DSP_CMD_ID(NAU8360_DSP_CMD_CLK_STOP),
	NAU_DSP_CMD_ID(NAU8360_DSP_CMD_CLK_RESTART),
};

static bool nau8360_dsp_commands(int cmd_id)
{
	switch (cmd_id) {
	case NAU8360_DSP_CMD_GET_COUNTER:
	case NAU8360_DSP_CMD_GET_FRAME_STATUS:
	case NAU8360_DSP_CMD_GET_REVISION:
	case NAU8360_DSP_CMD_GET_KCS_RSLTS:
	case NAU8360_DSP_CMD_GET_KCS_SETUP:
	case NAU8360_DSP_CMD_SET_KCS_SETUP:
	case NAU8360_DSP_CMD_CLK_STOP:
	case NAU8360_DSP_CMD_CLK_RESTART:
		return true;
	default:
		return false;
	}
}

static const char *const dsp_cmd_table[] = {
	[NAU8360_DSP_CMD_GET_COUNTER] = "GET_COUNTER",
	[NAU8360_DSP_CMD_GET_FRAME_STATUS] = "GET_FRAME_STATUS",
	[NAU8360_DSP_CMD_GET_REVISION] = "GET_REVISION",
	[NAU8360_DSP_CMD_GET_KCS_RSLTS] = "GET_KCS_RSLTS",
	[NAU8360_DSP_CMD_GET_KCS_SETUP] = "GET_KCS_SETUP",
	[NAU8360_DSP_CMD_SET_KCS_SETUP] = "SET_KCS_SETUP",
	[NAU8360_DSP_CMD_CLK_STOP] = "CLK_STOP",
	[NAU8360_DSP_CMD_CLK_RESTART] = "CLK_RESTART",
};

/* checking for DSP IDLE pattern */
static int nau8360_dsp_idle(struct snd_soc_component *component, unsigned short dsp_addr)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	unsigned int idle_pattern, timeout = NAU8360_DSP_IDLE_RETRY * USEC_PER_MSEC;
	int ret;

	ret = regmap_read_poll_timeout_atomic(nau8360->regmap, dsp_addr, idle_pattern,
		idle_pattern == NAU8360_DSP_COMM_IDLE_WORD, USEC_PER_MSEC, timeout);
	if (ret) {
		/* The driver can't establish a connection to DSP. Maybe it is not clocked,
		 * or previous synchronization issue.
		 */
		if (ret == -ETIMEDOUT)
			dev_err(nau8360->dev, "timeout waiting for DSP idle pattern");
		else
			dev_err(nau8360->dev, "failed to read dsp status: %d", ret);

		return ret;
	}

	dsp_dbg(component->dev, "idle pattern found");
	payload_read(component->dev, idle_pattern);
	return 0;
}

/**
 * nau8360_pack_preamble - Pack DSP preamble fragment
 * @cmd_id: Command ID for the DSP message
 * @frag_len: Total length of the message fragments
 *
 * Return: 32-bit packed payload in Little Endian format.
 */
static inline u32 nau8360_pack_preamble(u8 cmd_id, u16 frag_len)
{
	return (NAU8360_DSP_COMM_PREAMBLE & 0xffff) |
		(((cmd_id << 2) | (frag_len & 0x3)) << 16) |
		((frag_len >> 2) << 24);
}

/**
 * nau8360_pack_param - Pack DSP parameter fragment
 * @param_offset: Starting offset of the parameter data
 * @param_size: Size of the parameter data in bytes
 *
 * Return: 32-bit packed payload in Little Endian format.
 */
static inline u32 nau8360_pack_param(u16 param_offset, u16 param_size)
{
	return (param_offset & 0xffff) | ((param_size & 0xffff) << 16);
}

/**
 * nau8360_pack_trailing - Pack DSP trailing fragment
 * @frag_cnt: Current fragment count
 * @padding: Number of padding bytes added to the final data fragment
 *
 * Return: 32-bit packed payload in Little Endian format.
 */
static inline u32 nau8360_pack_trailing(u16 frag_cnt, u8 padding)
{
	return (frag_cnt & 0xff) |
		((((frag_cnt >> 8) << 6) | (padding << 4)) << 8);
}
static int nau8360_message_to_dsp(struct snd_soc_component *component,
	const struct nau8360_cmd_info *cmd_info, int frag_len, int param_offset,
	int param_size, void *param_data, unsigned short dsp_addr)
{
	struct device *dev = component->dev;
	u8 *b_data;
	unsigned int payload;
	int ret, i, data_size, padding = 0, frag_cnt = 0;

	ret = nau8360_dsp_idle(component, dsp_addr);
	if (ret)
		goto err;

	/* sending preamble fragment */
	payload = nau8360_pack_preamble(cmd_info->cmd_id, frag_len);
	snd_soc_component_write(component, dsp_addr, payload);
	dsp_dbg(dev, "sending preamble fragment (CMD_ID 0x%x, LEN 0x%x)",
		cmd_info->cmd_id, frag_len);
	payload_write(dev, payload);

	if (!cmd_info->msg_param)
		goto done;

	/* sending payload + padding */
	payload = nau8360_pack_param(param_offset, param_size);
	snd_soc_component_write(component, dsp_addr, payload);
	frag_cnt++;
	dsp_dbg(dev, "send fragment (offset 0x%x, size 0x%x)", param_offset, param_size);
	payload_write(dev, payload);

	if (cmd_info->setup_data) {
		b_data = (u8 *)param_data;
		payload = 0;
		for (data_size = 0, i = 0; i < param_size; i++) {
			payload |= b_data[i] << (data_size * 8);
			data_size++;
			if (data_size == NAU8360_DSP_DATA_BYTE) {
				snd_soc_component_write(component, dsp_addr, payload);
				payload_write(dev, payload);
				data_size = 0;
				payload = 0;
				frag_cnt++;
			}
		}

		if (data_size > 0 && data_size < NAU8360_DSP_DATA_BYTE) {
			/* sending the data fragments with padding bytes */
			padding = NAU8360_DSP_DATA_BYTE - data_size;
			snd_soc_component_write(component, dsp_addr, payload);
			payload_write(dev, payload);
			payload = 0;
			frag_cnt++;

		}
		dsp_dbg(dev, "\n");
	}

	/* sending trailing fragment */
	frag_cnt++;
	payload = nau8360_pack_trailing(frag_cnt, padding);
	snd_soc_component_write(component, dsp_addr, payload);
	dsp_dbg(dev, "send trailing fragment (LEN 0x%x, PAD 0x%x)", frag_cnt, padding);
	payload_write(dev, payload);
	if (frag_cnt != frag_len) {
		dev_err(dev, "message error (CMD_ID 0x%x, LEN 0x%x) !!!",
			cmd_info->cmd_id, frag_cnt);
		ret = -EPROTO;
		goto err;
	}

done:
	return 0;
err:
	return ret;
}

static int nau8360_dsp_replied(struct nau8360 *nau8360, int *length,
	unsigned short dsp_addr)
{
	struct device *dev = nau8360->dev;
	unsigned int reply_preamble, timeout = NAU8360_DSP_IDLE_RETRY * USEC_PER_MSEC;
	int ret, reply_id;

	ret = regmap_read_poll_timeout_atomic(nau8360->regmap, dsp_addr, reply_preamble,
		(reply_preamble & 0xffff) == NAU8360_DSP_COMM_PREAMBLE,
		USEC_PER_MSEC, timeout);
	if (ret) {
		dev_err(dev, "timeout for reply preamble: %d", ret);
		return ret;
	}

	*length = (reply_preamble >> 16) & 0x3;
	*length |= ((reply_preamble >> 24) & 0xff) << 2;
	reply_id = (reply_preamble >> 18) & 0x3f;
	dsp_dbg(dev, "receive preamble fragment (REPLY_ID 0x%x, LEN 0x%x)",
		reply_id, *length);
	payload_read(dev, reply_preamble);

	if (reply_id == NAU8360_DSP_REPLY_OK)
		return 0;
	else
		return -reply_id;
}

static int nau8360_reply_from_dsp(struct snd_soc_component *component,
	const struct nau8360_cmd_info *cmd_info, int data_size,
	void *data, unsigned short dsp_addr)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	struct device *dev = component->dev;
	unsigned int payload, *data_buf;
	int i, j, ret, frag_len, frag_payload_len, len_pos, pad_len, pad_len_exp;
	int data_count = 0;

	if (!cmd_info->reply_data) {
		dsp_dbg(dev, "The cmd without reply data!!");
		ret = nau8360_dsp_replied(nau8360, &frag_len, dsp_addr);
		if (ret)
			goto err;
		else if (!frag_len)
			goto done;
	}

	if (!data) {
		ret = -EINVAL;
		goto err;
	}
	data_buf = (unsigned int *)data;

	ret = nau8360_dsp_replied(nau8360, &frag_len, dsp_addr);
	if (ret)
		goto err;
	else if (!frag_len)
		goto done;

	frag_payload_len = frag_len - 1;
	if (cmd_info->msg_param)
		data_count = data_size;

	for (i = 0; i < frag_payload_len; i++) {
		ret = regmap_read(nau8360->regmap, dsp_addr, &payload);
		if (ret) {
			dev_err(dev, "failed to read payload of dsp");
			goto err;
		}

		if (cmd_info->msg_param) {
			if (data_count >= NAU8360_DSP_DATA_BYTE) {
				*data_buf++ = payload;
				data_count -= NAU8360_DSP_DATA_BYTE;
				payload_read(dev, payload);
			} else {
				for (j = 0; j < NAU8360_DSP_DATA_BYTE; j++) {
					if (data_count <= 0)
						break;

					((u8 *)data_buf)[j] = (payload >> (j * 8)) & 0xff;
					data_count--;
				}
				payload_read(dev, payload);
				break;
			}
		} else {
			*data_buf = payload;
			payload_read(dev, payload);
		}
	}

	/* check the reply length same as request */
	if (data_count && (cmd_info->cmd_id == NAU8360_DSP_CMD_GET_KCS_RSLTS ||
			cmd_info->cmd_id == NAU8360_DSP_CMD_GET_KCS_SETUP)) {
		dev_warn(dev, "payload_len %d, expected %d",
			data_size - data_count, data_size);
	}
	dsp_dbg(dev, "reading trailing fragment");
	ret = regmap_read(nau8360->regmap, dsp_addr, &payload);
	if (ret) {
		dev_err(dev, "failed to read trailing fragment");
		goto err;
	}

	len_pos = payload & 0xff;
	len_pos |= ((payload >> 8) & 0xc0) << 2;
	if (len_pos != frag_len) {
		dev_err(dev, "LEN_POST %02X, expect %02X", len_pos, frag_len);
		ret = -EPROTO;
		goto err;
	}

	pad_len = ((payload >> 8) & 0x30) >> 4;
	if (cmd_info->msg_param)
		pad_len_exp = frag_payload_len * NAU8360_DSP_DATA_BYTE -
			(data_size - data_count);
	else
		pad_len_exp = 0;
	if (pad_len != pad_len_exp) {
		dev_err(dev, "PAD_LEN %02X, expect %02X", pad_len, pad_len_exp);
		ret = -EPROTO;
		goto err;
	}
	dsp_dbg(dev, "LEN_POST 0x%x, PAD_LEN 0x%x", len_pos, pad_len);
	payload_read(dev, payload);
done:
	return 0;
err:
	dev_err(dev, "DSP reply error %d !!!", ret);
	return ret;
}

/**
 * nau8360_send_dsp_command - Send command to DSP
 *
 * @component:  component to register
 * @cmd_id:  DSP supported command ID
 * @kcs_setup: KCS setup structure
 * @dsp_addr: DSP address
 *
 * The communication protocol is a Master-Slave type protocol
 * where the host processor is the master and DSP is the slave.
 * The Master initiates the communication and can either write or
 * read back from the slave.
 * Transactions from the Master are called "Messages",
 * and read-back data from the Slave is called a "Reply".
 *
 * The function sends command to DSP according to the command ID.
 * These commands include getting the information of DSP,
 * getting or setting KCS configuration, or making DSP control.
 */
static int nau8360_send_dsp_command(struct snd_soc_component *component, int cmd_id,
	struct nau8360_kcs_setup *kcs_setup, unsigned short dsp_addr)
{
	const struct nau8360_cmd_info *cmd_info;
	int ret, frag_len = 0;

	if (!component || !kcs_setup) {
		ret = -EINVAL;
		goto msg_fail;
	}
	if (!nau8360_dsp_commands(cmd_id)) {
		dev_err(component->dev, "command not support!");
		ret = -EINVAL;
		goto msg_fail;
	}

	cmd_info = &nau8360_dsp_cmd_table[cmd_id];
	if ((cmd_info->msg_param && !kcs_setup->set_len) ||
		(cmd_info->setup_data && !kcs_setup->set_kcs_data) ||
		(cmd_info->reply_data && !kcs_setup->get_data)) {
		ret = -EFAULT;
		goto msg_fail;
	}

	/* Read up to 1kB data because the LEN field to request data is 10-bits
	 * long; and not beyond 3kB offset.
	 */
	if (cmd_id == NAU8360_DSP_CMD_GET_KCS_SETUP &&
		(kcs_setup->set_len > NAU8360_DSP_KCS_DAT_LEN_MAX ||
			kcs_setup->set_kcs_offset > NAU8360_DSP_KCS_OFFSET_MAX)) {
		ret = -ERANGE;
		goto msg_fail;
	}

	if (cmd_info->msg_param) {
		/* one fragment for offset and size parameters */
		frag_len++;
		/* one fragment for a postamble fragment */
		frag_len++;
	}

	/* fragments for KCS setup writen */
	if (cmd_info->setup_data)
		frag_len += (kcs_setup->set_len +
				NAU8360_DSP_DATA_BYTE - 1) / NAU8360_DSP_DATA_BYTE;

	ret = nau8360_message_to_dsp(component, cmd_info, frag_len,
			kcs_setup->set_kcs_offset, kcs_setup->set_len,
			kcs_setup->set_kcs_data, dsp_addr);
	if (ret)
		goto msg_fail;

	ret = nau8360_reply_from_dsp(component, cmd_info, kcs_setup->get_len,
			kcs_setup->get_data, dsp_addr);
	if (ret)
		goto reply_fail;

	return 0;

msg_fail:
	dev_err(component->dev, "fail to send a message %d to DSP (%d)", cmd_id, ret);
	return ret;
reply_fail:
	dev_err(component->dev, "reply fail (%d) from DSP.", ret);
	return ret;
}

static inline int nau8360_dsp_exec_command(struct snd_soc_component *cp, int cmd_id,
	int offset, int set_len, void *set_data, int get_len, void *get_data,
	int dsp_addr)
{
	struct nau8360_kcs_setup kcs_setup = {
		.set_kcs_offset = offset,
		.set_len = set_len,
		.set_kcs_data = set_data,
		.get_len = get_len,
		.get_data = get_data,
	};

	return nau8360_send_dsp_command(cp, cmd_id, &kcs_setup, dsp_addr);
}

static inline int nau8360_send_dsp_broadcast(struct snd_soc_component *cp, int cmd_id)
{
	int i, ret;

	for (i = 0; i < NAU8360_DSP_FW_NUM; i++) {
		ret = nau8360_dsp_exec_command(cp, cmd_id, 0, 0, NULL, 0, NULL,
			nau8360_dsp_addr[i]);
		if (ret) {
			dev_err(cp->dev, "DSP %x fail (%d)", nau8360_dsp_addr[i], ret);
			return ret;
		}
	}

	return 0;
}

/**
 * nau8360_dsp_kcs_setup - Send KCS setup command to DSP
 *
 * @component:  component to register
 * @offset: address offset relative to KCS start
 * @size: size of data writen to KCS
 * @data: data writen to KCS setup
 * @dsp_addr : DSP address
 *
 * The function sends KCS setup command to DSP for
 * setting KCS configuration. The maximum size that you can transfer into
 * the DSP is 96 bytes. Therefore, the driver has to split the data into
 * 96 bytes chucks, if the setup configuration over the threshold.
 */
static int nau8360_dsp_kcs_setup(struct snd_soc_component *component, int offset, int size,
	const void *data, unsigned short dsp_addr)
{
	u8 *data_buf;
	unsigned int kcs_rst;
	int cmd_id = NAU8360_DSP_CMD_SET_KCS_SETUP, retries, ret, data_len, data_rem,
		addr_offset;

	/* Limit full load of KCS_SETUP data and not beyond 3kB offset. */
	if (!data || size > NAU8360_DSP_KCS_DAT_LEN_MAX ||
		offset > NAU8360_DSP_KCS_OFFSET_MAX) {
		ret = -EINVAL;
		goto msg_fail;
	}

	/* sending fragments for KCS setup */
	data_buf = (u8 *)data;
	addr_offset = offset;
	data_rem = size;
	retries = 0;
	while (data_rem) {
		if (data_rem > NAU8360_DSP_KCS_TX_MAX)
			data_len = NAU8360_DSP_KCS_TX_MAX;
		else
			data_len = data_rem;

		ret = nau8360_dsp_exec_command(component, NAU8360_DSP_CMD_SET_KCS_SETUP,
			addr_offset, data_len, (void *)data_buf, 0, &kcs_rst, dsp_addr);
		if (ret) {
			if (retries++ < NAU8360_DSP_RETRY_MAX)
				continue;
			else
				goto msg_fail;
		} else {
			data_buf += (u8)data_len;
			addr_offset += data_len;
			data_rem -= data_len;
		}
		/* checking KCS result */
		ret = nau8360_dsp_exec_command(component, NAU8360_DSP_CMD_GET_KCS_RSLTS,
			0, NAU8360_DSP_DATA_BYTE, NULL,
			NAU8360_DSP_DATA_BYTE, &kcs_rst, dsp_addr);
		if (ret)
			goto msg_fail;
	}

	return 0;

msg_fail:
	dev_err(component->dev, "send a kcs setup message %d fail (%d)", cmd_id, ret);
	return ret;
}

static int nau8360_dsp_get_cmd_put(struct snd_soc_component *component,
	int dsp_addr, int cmd, int *value)
{
	int ret;

	dev_info(component->dev, "send DSP %x command %s", dsp_addr, dsp_cmd_table[cmd]);

	ret = nau8360_dsp_exec_command(component, cmd, 0, sizeof(int), NULL,
		sizeof(int), value, dsp_addr);
	if (ret) {
		dev_err(component->dev, "do command fail (%d)", ret);
		return ret;
	}

	return 0;
}

static int nau8360_dsp_clock_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	int ret, cmd;

	if (SND_SOC_DAPM_EVENT_ON(event))
		cmd = NAU8360_DSP_CMD_CLK_RESTART;
	else if (SND_SOC_DAPM_EVENT_OFF(event))
		cmd = NAU8360_DSP_CMD_CLK_STOP;

	dsp_dbg(component->dev, "send DSP command %s", dsp_cmd_table[cmd]);
	ret = nau8360_send_dsp_broadcast(component, cmd);
	if (ret) {
		dev_err(component->dev, "send DSP command %s fail (%d)",
			dsp_cmd_table[cmd], ret);
		goto err;
	}

	return 0;
err:
	return ret;
}

static const struct snd_soc_dapm_widget nau8360_dsp_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY("DSP Clock", SND_SOC_NOPM, 0, 0, nau8360_dsp_clock_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route nau8360_dsp_dapm_routes[] = {
	{ "DSP", NULL, "HW3 Engine" },
	{ "DSP", NULL, "DSP Clock" },
};

static int nau8360_dsp_chan_kcs_setup(struct snd_soc_component *cp,
	const char *fw_name, int dsp_addr)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(cp);
	const struct firmware *fw;
	int ret, status, buf_off, buf_len;

	ret = nau8360_dsp_get_cmd_put(cp, dsp_addr,
			NAU8360_DSP_CMD_GET_FRAME_STATUS, &status);
	if (ret || !(status & NAU8360_DSP_ALGO_OK)) {
		dev_err(cp->dev, "DSP %x is not ready", dsp_addr);
		ret = -EIO;
		goto err;
	}

	dev_info(cp->dev, "DSP %x is ready to load firmware %s, status %x",
		dsp_addr, fw_name, status);

	ret = request_firmware(&fw, fw_name, cp->dev);
	if (ret) {
		dev_err(cp->dev, "failed to load firmware (%d)", ret);
		goto err;
	}

	buf_off = 0;
	buf_len = nau8360->kcs_setup_size = fw->size;
	ret = nau8360_dsp_kcs_setup(cp, buf_off, buf_len, fw->data, dsp_addr);
	if (ret) {
		dev_err(cp->dev, "send DSP command %s fail (%d)",
			dsp_cmd_table[NAU8360_DSP_CMD_SET_KCS_SETUP], ret);
		goto err_loaded;
	}
	release_firmware(fw);

	return 0;

err_loaded:
	if (fw)
		release_firmware(fw);
err:
	return ret;
}

static int nau8360_dsp_set_kcs_setup(struct snd_soc_component *cp)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(cp);
	char firmware[NAU8360_DSP_FW_NAMELEN];
	const char *fw_name;
	int i, ret;

	for (i = 0; i < NAU8360_DSP_FW_NUM; i++) {
		if (nau8360->dsp_fws_num) {
			snprintf(firmware, sizeof(firmware), NAU8360_DSP_FIRMDIR"%s",
				nau8360->dsp_firmware[i]);
			fw_name = firmware;
		} else
			fw_name = nau8360_def_firmwares[i];

		ret = nau8360_dsp_chan_kcs_setup(cp, fw_name, nau8360_dsp_addr[i]);
		if (ret)
			return ret;

		msleep(100);
	}

	return 0;
}

int nau8360_dsp_init(struct snd_soc_component *component)
{
	struct nau8360 *nau8360 = snd_soc_component_get_drvdata(component);
	struct snd_soc_dapm_context *dapm = nau8360->dapm;
	int ret;

	dev_info(component->dev, "DSP initializing...");
	ret = nau8360_dsp_set_kcs_setup(component);
	if (ret)
		goto err;

	ret = snd_soc_dapm_new_controls(dapm, nau8360_dsp_dapm_widgets,
			ARRAY_SIZE(nau8360_dsp_dapm_widgets));
	if (ret) {
		dev_err(component->dev, "add DSP widget fail (%d)", ret);
		goto err;
	}
	ret = snd_soc_dapm_add_routes(dapm, nau8360_dsp_dapm_routes,
			ARRAY_SIZE(nau8360_dsp_dapm_routes));
	if (ret) {
		dev_err(component->dev, "add DSP route fail (%d)", ret);
		goto err;
	}
	nau8360->dsp_created = true;

	return 0;

err:
	return ret;
}

int nau8360_dsp_reinit(struct snd_soc_component *component)
{
	dev_info(component->dev, "DSP initializing...");
	return nau8360_dsp_set_kcs_setup(component);
}
