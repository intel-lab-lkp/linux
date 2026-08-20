// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Nicholas Johnson */
#include "quantum.h"

static int quantum_midi_input_open(struct snd_rawmidi_substream *substream)
{
	struct quantum_chip *chip = substream->rmidi->private_data;
	unsigned long flags;

	if (quantum_device_gone(chip))
		return -ENODEV;
	spin_lock_irqsave(&chip->midi_lock, flags);
	chip->midi_input = substream;
	spin_unlock_irqrestore(&chip->midi_lock, flags);
	return 0;
}

static int quantum_midi_input_close(struct snd_rawmidi_substream *substream)
{
	struct quantum_chip *chip = substream->rmidi->private_data;
	unsigned long flags;

	if (quantum_device_gone(chip))
		return 0;

	spin_lock_irqsave(&chip->midi_lock, flags);
	chip->midi_input_triggered = false;
	chip->midi_input = NULL;
	spin_unlock_irqrestore(&chip->midi_lock, flags);
	return 0;
}

static void quantum_midi_input_trigger(struct snd_rawmidi_substream *substream,
				       int up)
{
	struct quantum_chip *chip = substream->rmidi->private_data;
	unsigned long flags;

	if (quantum_device_gone(chip))
		up = 0;
	spin_lock_irqsave(&chip->midi_lock, flags);
	chip->midi_input_triggered = up != 0;
	spin_unlock_irqrestore(&chip->midi_lock, flags);
}

static int quantum_midi_output_open(struct snd_rawmidi_substream *substream)
{
	struct quantum_chip *chip = substream->rmidi->private_data;
	unsigned long flags;

	if (quantum_device_gone(chip))
		return -ENODEV;
	spin_lock_irqsave(&chip->midi_lock, flags);
	chip->midi_output = substream;
	spin_unlock_irqrestore(&chip->midi_lock, flags);
	return 0;
}

static int quantum_midi_output_close(struct snd_rawmidi_substream *substream)
{
	struct quantum_chip *chip = substream->rmidi->private_data;
	unsigned long flags;

	spin_lock_irqsave(&chip->midi_lock, flags);
	chip->midi_output_triggered = false;
	chip->midi_output = NULL;
	spin_unlock_irqrestore(&chip->midi_lock, flags);
	cancel_work_sync(&chip->midi_tx_work);
	return 0;
}

static void quantum_midi_output_trigger(struct snd_rawmidi_substream *substream,
					int up)
{
	struct quantum_chip *chip = substream->rmidi->private_data;
	unsigned long flags;

	if (quantum_device_gone(chip))
		up = 0;
	spin_lock_irqsave(&chip->midi_lock, flags);
	chip->midi_output_triggered = up != 0;
	spin_unlock_irqrestore(&chip->midi_lock, flags);
	if (up && chip->cmd_wq)
		queue_work(chip->cmd_wq, &chip->midi_tx_work);
}

static const struct snd_rawmidi_ops quantum_midi_input_ops = {
	.open = quantum_midi_input_open,
	.close = quantum_midi_input_close,
	.trigger = quantum_midi_input_trigger,
};

static const struct snd_rawmidi_ops quantum_midi_output_ops = {
	.open = quantum_midi_output_open,
	.close = quantum_midi_output_close,
	.trigger = quantum_midi_output_trigger,
};

void quantum_midi_receive(struct quantum_chip *chip, const u8 *payload,
			  size_t payload_len)
{
	struct snd_rawmidi_substream *substream;
	unsigned long flags;
	u16 count;

	if (payload_len < 4 || payload[0] != 0)
		return;
	count = le16_to_cpup((const __le16 *)(payload + 2));
	if (count > payload_len - 4)
		return;
	dev_dbg_ratelimited(&chip->pci->dev,
			    "MIDI RX: port=%u bytes=%u\n", payload[0], count);

	spin_lock_irqsave(&chip->midi_lock, flags);
	substream = chip->midi_input;
	if (substream && chip->midi_input_triggered)
		snd_rawmidi_receive(substream, payload + 4, count);
	spin_unlock_irqrestore(&chip->midi_lock, flags);
}

void quantum_midi_tx_work_handler(struct work_struct *work)
{
	struct quantum_chip *chip = container_of(work, struct quantum_chip,
						 midi_tx_work);
	u8 data[256];

	if (quantum_device_gone(chip))
		return;

	for (;;) {
		struct snd_rawmidi_substream *substream;
		unsigned long flags;
		int count, err;

		spin_lock_irqsave(&chip->midi_lock, flags);
		substream = chip->midi_output;
		if (!substream || !chip->midi_output_triggered) {
			spin_unlock_irqrestore(&chip->midi_lock, flags);
			return;
		}
		spin_unlock_irqrestore(&chip->midi_lock, flags);

		count = snd_rawmidi_transmit_peek(substream, data, sizeof(data));
		if (count <= 0)
			return;
		err = quantum_tci_send_midi(chip, data, count);
		if (err == -EBUSY)
			return;
		if (err) {
			dev_warn_ratelimited(&chip->pci->dev,
					     "MIDI TX failed: %d\n", err);
			return;
		}
		snd_rawmidi_transmit_ack(substream, count);
	}
}

int quantum_midi_new(struct quantum_chip *chip)
{
	struct snd_rawmidi *rmidi;
	char id[32];
	int err;

	strscpy(id, chip->model_id, sizeof(id));
	err = snd_rawmidi_new(chip->card, id, 0, 1, 1, &rmidi);
	if (err)
		return err;
	rmidi->private_data = chip;
	snprintf(rmidi->name, sizeof(rmidi->name), "%s MIDI",
		 chip->model_name);
	rmidi->info_flags = SNDRV_RAWMIDI_INFO_INPUT |
			    SNDRV_RAWMIDI_INFO_OUTPUT |
			    SNDRV_RAWMIDI_INFO_DUPLEX;
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT,
			    &quantum_midi_input_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT,
			    &quantum_midi_output_ops);
	chip->rmidi = rmidi;
	return 0;
}
