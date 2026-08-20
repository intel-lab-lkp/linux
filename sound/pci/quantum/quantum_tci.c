// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Nicholas Johnson */
#include "quantum.h"

static int quantum_get_tx_slot(struct quantum_chip *chip, u32 *slot)
{
	u32 hw = quantum_read32(chip, QUANTUM_REG_TX_SLOT_STATUS);
	u32 sw = quantum_read32(chip, QUANTUM_REG_TX_SW_POS);
	u32 next;

	if (hw >= chip->cmd_msg_count || sw >= chip->cmd_msg_count)
		return -EIO;

	/* Keep one slot empty so equal producer/consumer indices mean empty. */
	next = (sw + 1) % chip->cmd_msg_count;
	if (next == hw)
		return -EBUSY;

	*slot = sw;
	return 0;
}

static int quantum_get_next_rx_slot(struct quantum_chip *chip)
{
	u32 hw = quantum_read32(chip, QUANTUM_REG_RX_SLOT_STATUS);
	u32 sw = quantum_read32(chip, QUANTUM_REG_RX_SW_POS);
	u32 count = chip->cmd_msg_count;
	u32 available;

	if (hw >= count || sw >= count)
		return -EIO;

	if (hw >= sw)
		available = hw - sw;
	else
		available = hw + count - sw;

	if (!available)
		return -ENODATA;

	return sw;
}

/* Advance the TX producer index */

static void quantum_inc_tx_sw_pos(struct quantum_chip *chip, u32 slot)
{
	quantum_write32(chip, QUANTUM_REG_TX_SW_POS,
			(slot + 1) % chip->cmd_msg_count);
}

/* Advance the RX consumer index */

static void quantum_inc_rx_sw_pos(struct quantum_chip *chip, u32 slot)
{
	quantum_write32(chip, QUANTUM_REG_RX_SW_POS,
			(slot + 1) % chip->cmd_msg_count);
}

/* Mark the command interface ready */

static void quantum_tci_mark_ready(struct quantum_chip *chip)
{
	if (!chip->tci_ready) {
		chip->tci_ready = true;
		chip->tci_initialized = true;
	}
}

void quantum_process_rx_messages(struct quantum_chip *chip)
{
	int slot;

	if (quantum_device_gone(chip))
		return;

	for (;;) {
		u8 *msg;
		u32 hw_len;
		u8 channel, code;
		u16 tid;
		void *payload;
		size_t payload_len;
		u16 header_len;

		slot = quantum_get_next_rx_slot(chip);
		if (slot == -ENODATA)
			break;
		if (slot < 0) {
			dev_err_ratelimited(&chip->pci->dev,
					    "invalid command RX ring position\n");
			break;
		}

		msg = chip->cmd_rx_slots[slot];
		hw_len = quantum_read32(chip,
					QUANTUM_REG_RX_LEN_READBACK_BASE + slot * 4);
		if (hw_len < sizeof(struct tci_header)) {
			dev_warn_ratelimited(&chip->pci->dev,
					     "RX slot %d has invalid length %u\n", slot, hw_len);
			quantum_inc_rx_sw_pos(chip, slot);
			continue;
		}

		header_len = le16_to_cpu(*(__le16 *)msg);
		if (header_len < sizeof(struct tci_header) ||
		    header_len > chip->cmd_msg_size) {
			dev_warn_ratelimited(&chip->pci->dev,
					     "RX slot %d has invalid header length %u\n",
				slot, header_len);
			quantum_inc_rx_sw_pos(chip, slot);
			continue;
		}

		if (header_len != hw_len) {
			dev_warn_ratelimited(&chip->pci->dev,
					     "RX slot %d length mismatch: hardware=%u header=%u\n",
				slot, hw_len, header_len);
			if (header_len > hw_len)
				header_len = hw_len;
		}

		channel = msg[2];
		code = msg[3];
		tid = le16_to_cpu(*(__le16 *)(msg + 4));
		payload = msg + sizeof(struct tci_header);
		payload_len = header_len - sizeof(struct tci_header);
		dev_dbg(&chip->pci->dev,
			"TCI RX: slot=%d tid=%u channel=0x%02x code=0x%02x length=%u\n",
			slot, tid, channel, code, header_len);

		if (channel == TCI_CHAN_CMD_RESP || channel == TCI_CHAN_CTRL) {
			struct quantum_tci_cmd *cmd, *tmp;
			bool found = false;

			spin_lock(&chip->cmd_lock);
			list_for_each_entry_safe(cmd, tmp, &chip->pending_cmds, list) {
				if (cmd->tid == tid && !cmd->responded) {
					cmd->responded = true;
					cmd->response_code = code;
					cmd->response_len = min(payload_len,
								sizeof(cmd->response_data));
					if (cmd->response_len)
						memcpy(cmd->response_data, payload,
						       cmd->response_len);
					complete(&cmd->done);
					found = true;
					break;
				}
			}
			spin_unlock(&chip->cmd_lock);

			if (!found)
				dev_warn_ratelimited(&chip->pci->dev,
						     "RX transaction %u has no pending command\n",
						     tid);
		}

		if (channel == TCI_CHAN_CTRL_EVENT) {
			switch (code) {
			case TCI_EVT_CLOCK_STATUS:
				if (payload_len >= 8) {
					u32 wire_cs = le32_to_cpu(*(__le32 *)payload);
					u32 sr = le32_to_cpu(*(__le32 *)(payload + 4));

					if (wire_cs < 1 || wire_cs > 5) {
						dev_warn_ratelimited(&chip->pci->dev,
								     "invalid clock source event: %u\n",
							wire_cs);
						break;
					}
					dev_dbg(&chip->pci->dev,
						"clock event: source=%u rate=%u\n",
						wire_cs - 1, sr);
					chip->clock_source = wire_cs - 1;
					chip->sample_rate = sr;
				}
				break;

			case TCI_EVT_POWER_STATE:
				if (payload_len >= 4) {
					u32 state = le32_to_cpu(*(__le32 *)payload);

					dev_dbg(&chip->pci->dev, "power event: %s\n",
						state ? "on" : "off");
					quantum_power_state_changed(chip, state != 0);
				}
				break;

			default:
				dev_dbg(&chip->pci->dev,
					"unknown event code 0x%02x\n", code);
				break;
			}
		}

		if (channel == TCI_CHAN_MIDI_RX && code == TCI_EVT_MIDI_RX)
			quantum_midi_receive(chip, payload, payload_len);
		if (channel == TCI_CHAN_MIDI_TX && code == TCI_EVT_MIDI_TX_FREE &&
		    chip->midi_output_triggered)
			queue_work(chip->cmd_wq, &chip->midi_tx_work);

		quantum_inc_rx_sw_pos(chip, slot);
	}
}

static int quantum_tci_send_wait(struct quantum_chip *chip, u8 channel,
				 u8 code, u8 expected_rsp, void *tx_data,
				 size_t tx_len, void *rx_data, size_t *rx_len,
				 int timeout_ms)
{
	struct tci_header *hdr;
	struct quantum_tci_cmd cmd;
	u32 slot;
	int retry;
	int ret;
	unsigned long flags;
	u32 hw_len;
	size_t total_tx_len;

	if (chip->tci_fatal_error)
		return -EIO;

	if (quantum_device_gone(chip))
		return -ENODEV;

	if (!chip->tci_initialized)
		return -EIO;

	total_tx_len = sizeof(struct tci_header) + tx_len;

	if (total_tx_len > chip->cmd_msg_size) {
		dev_err(&chip->pci->dev, "TCI TX too large: %zu > %u\n",
			total_tx_len, chip->cmd_msg_size);
		return -EINVAL;
	}

	mutex_lock(&chip->tci_tx_mutex);
	if (quantum_device_gone(chip) || !chip->tci_initialized) {
		mutex_unlock(&chip->tci_tx_mutex);
		return -ENODEV;
	}
	memset(&cmd, 0, sizeof(cmd));
	cmd.tid = chip->next_tid++;
	init_completion(&cmd.done);

	spin_lock_irqsave(&chip->cmd_lock, flags);
	list_add_tail(&cmd.list, &chip->pending_cmds);
	spin_unlock_irqrestore(&chip->cmd_lock, flags);

	/* Get slot - short retries */
	for (retry = 0; retry < 50; retry++) {
		ret = quantum_get_tx_slot(chip, &slot);
		if (ret != -EBUSY)
			break;
		usleep_range(100, 200);
	}
	if (ret) {
		mutex_unlock(&chip->tci_tx_mutex);
		spin_lock_irqsave(&chip->cmd_lock, flags);
		list_del(&cmd.list);
		spin_unlock_irqrestore(&chip->cmd_lock, flags);
		return ret;
	}

	/* Build the message in the TX slot */
	hdr = chip->cmd_tx_slots[slot];
	memset(hdr, 0, chip->cmd_msg_size);

	hdr->length = cpu_to_le16(total_tx_len);
	hdr->reserved = 0;
	hdr->channel = channel;
	hdr->code = code;
	hdr->tid = cpu_to_le16(cmd.tid);

	/* Payload (if any) goes right after the header */
	if (tx_data && tx_len)
		memcpy((u8 *)hdr + sizeof(struct tci_header), tx_data, tx_len);

	/* Make coherent TX contents visible before ringing the doorbell. */
	dma_wmb();

	dev_dbg(&chip->pci->dev,
		"TCI TX: slot=%u tid=%u channel=0x%02x code=0x%02x length=%zu\n",
		slot, cmd.tid, channel, code, total_tx_len);

	/* Write TX_LEN = total message length (header + payload) */
	quantum_write32(chip, QUANTUM_REG_CMD_TX_LEN_BASE + slot * 4, total_tx_len);

	/* Read back RX_LEN for sanity check */
	hw_len = quantum_read32(chip, QUANTUM_REG_RX_LEN_READBACK_BASE + slot * 4);
	dev_dbg(&chip->pci->dev,
		"TCI slot %u length readback: TX=%zu RX=%u\n",
		slot, total_tx_len, hw_len);

	/* Advance TX pointer - signals hardware to process */
	quantum_write32(chip, QUANTUM_REG_TX_SW_POS, (slot + 1) % chip->cmd_msg_count);
	mutex_unlock(&chip->tci_tx_mutex);

	/* Wait for completion */
	ret = wait_for_completion_timeout(&cmd.done, msecs_to_jiffies(timeout_ms));

	spin_lock_irqsave(&chip->cmd_lock, flags);
	list_del(&cmd.list);
	spin_unlock_irqrestore(&chip->cmd_lock, flags);

	if (!ret) {
		dev_err(&chip->pci->dev,
			"command response timed out (channel=0x%02x code=0x%02x)\n",
			channel, code);
		return -ETIMEDOUT;
	}
	if (cmd.status)
		return cmd.status;

	if (cmd.response_code != expected_rsp &&
	    cmd.response_code != TCI_RSP_STATUS) {
		dev_err(&chip->pci->dev,
			"unexpected response code 0x%02x (expected 0x%02x) for channel=0x%02x code=0x%02x\n",
			cmd.response_code, expected_rsp, channel, code);
		return -EIO;
	}

	if (rx_data && rx_len && cmd.response_len) {
		size_t copy = min(cmd.response_len, *rx_len);

		memcpy(rx_data, cmd.response_data, copy);
		*rx_len = copy;
	}

	return 0;
}

int quantum_tci_send_midi(struct quantum_chip *chip, const u8 *data, size_t len)
{
	struct tci_header *hdr;
	u8 *payload;
	u32 slot;
	size_t total_len = sizeof(*hdr) + 4 + len;
	int err = 0;

	if (!len || len > 2048 || total_len > chip->cmd_msg_size)
		return -EINVAL;
	if (quantum_device_gone(chip) || !chip->tci_ready)
		return -ENODEV;

	mutex_lock(&chip->tci_tx_mutex);
	if (quantum_device_gone(chip) || !chip->tci_ready) {
		err = -ENODEV;
		goto out_unlock;
	}
	err = quantum_get_tx_slot(chip, &slot);
	if (err)
		goto out_unlock;

	hdr = chip->cmd_tx_slots[slot];
	memset(hdr, 0, chip->cmd_msg_size);
	hdr->length = cpu_to_le16(total_len);
	hdr->channel = TCI_CHAN_MIDI_TX;
	hdr->code = TCI_CMD_MIDI_TX;
	hdr->tid = cpu_to_le16(chip->next_tid++);
	payload = (u8 *)hdr + sizeof(*hdr);
	payload[0] = 0;
	payload[1] = 0;
	*(__le16 *)(payload + 2) = cpu_to_le16(len);
	memcpy(payload + 4, data, len);

	/* Make coherent TX contents visible before ringing the doorbell. */
	dma_wmb();
	quantum_write32(chip, QUANTUM_REG_CMD_TX_LEN_BASE + slot * 4,
			total_len);
	quantum_inc_tx_sw_pos(chip, slot);

out_unlock:
	mutex_unlock(&chip->tci_tx_mutex);
	return err;
}

/* Read the device serial number */

int quantum_get_serial_number(struct quantum_chip *chip)
{
	u8 serial[32];
	size_t len = sizeof(serial);
	int ret;

	ret = quantum_tci_send_wait(chip, TCI_CHAN_CTRL,
				    TCI_CTRL_GET_SERIAL_NUMBER,
				    TCI_CTRL_RSP_SERIAL_NUMBER,
				    NULL, 0, serial, &len, 2000);
	if (ret < 0)
		return ret;

	if (len > 0)
		memcpy(chip->serial, serial, min(len, sizeof(chip->serial)));

	quantum_tci_mark_ready(chip);
	return 0;
}

/* Set the sample rate */

int quantum_set_sample_rate(struct quantum_chip *chip, unsigned int rate)
{
	__le32 val = cpu_to_le32(rate);
	int ret;

	if (chip->tci_fatal_error)
		return -EIO;

	if (chip->sample_rate == rate)
		return 0;

	ret = quantum_tci_send_wait(chip, TCI_CHAN_CTRL,
				    TCI_CTRL_SET_SAMPLE_RATE,
				    TCI_CTRL_RSP_SAMPLE_RATE,
				    &val, sizeof(val), NULL, NULL, 1000);
	if (ret < 0) {
		dev_err(&chip->pci->dev, "Set sample rate failed: %d\n", ret);
		return ret;
	}

	chip->sample_rate = rate;
	dev_dbg(&chip->pci->dev, "sample rate set to %u Hz\n", rate);
	return 0;
}

/* Set the clock source */

int quantum_set_clock_source(struct quantum_chip *chip, unsigned int source)
{
	__le32 val = cpu_to_le32(source + 1);
	int ret;

	if (chip->tci_fatal_error)
		return -EIO;

	if (source > CLK_SOURCE_EXTERNAL_ADAT2)
		return -EINVAL;

	ret = quantum_tci_send_wait(chip, TCI_CHAN_CTRL,
				    TCI_CTRL_SET_CLOCK_SOURCE,
				    TCI_CTRL_RSP_CLOCK_SOURCE,
				    &val, sizeof(val), NULL, NULL, 1000);
	if (ret < 0) {
		dev_err(&chip->pci->dev, "Set clock source failed: %d\n", ret);
		return ret;
	}

	chip->clock_source = source;
	dev_dbg(&chip->pci->dev, "clock source set to %u\n", source);
	return 0;
}

/* Read the clock source */

int quantum_get_clock_source(struct quantum_chip *chip, unsigned int *source)
{
	__le32 reply;
	size_t len = sizeof(reply);
	int ret;

	if (chip->tci_fatal_error)
		return -EIO;

	ret = quantum_tci_send_wait(chip, TCI_CHAN_CTRL,
				    TCI_CTRL_GET_CLOCK_SOURCE,
				    TCI_CTRL_RSP_CLOCK_SOURCE,
				    NULL, 0, &reply, &len, 1000);
	if (ret < 0) {
		dev_err(&chip->pci->dev, "Get clock source failed: %d\n", ret);
		return ret;
	}

	if (len >= 4) {
		u32 wire_source = le32_to_cpu(reply);

		if (wire_source < 1 || wire_source > 5)
			return -EIO;
		*source = wire_source - 1;
		chip->clock_source = *source;
		dev_dbg(&chip->pci->dev, "clock source read as %u\n", *source);
	}

	return 0;
}
