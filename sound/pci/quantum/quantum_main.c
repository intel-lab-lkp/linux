// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Nicholas Johnson */
#include <linux/module.h>

#include "quantum.h"

#define DRV_NAME "snd-quantum"

/* Module parameters */

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "ALSA card index");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ALSA card ID");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable the sound card");

/*
 * Slot bookkeeping for index[]/id[]/enable[]. This Thunderbolt device can be
 * un/re-enumerated many times per boot; a monotonically increasing counter
 * would exhaust SNDRV_CARDS, so track occupancy instead.
 */
static DEFINE_MUTEX(quantum_slot_lock);
static bool quantum_slot_used[SNDRV_CARDS];

static int quantum_acquire_card_slot(void)
{
	int slot;

	mutex_lock(&quantum_slot_lock);
	for (slot = 0; slot < SNDRV_CARDS; slot++) {
		if (!quantum_slot_used[slot]) {
			quantum_slot_used[slot] = true;
			mutex_unlock(&quantum_slot_lock);
			return slot;
		}
	}
	mutex_unlock(&quantum_slot_lock);
	return -ENODEV;
}

static void quantum_release_card_slot(int slot)
{
	if (slot < 0 || slot >= SNDRV_CARDS)
		return;

	mutex_lock(&quantum_slot_lock);
	quantum_slot_used[slot] = false;
	mutex_unlock(&quantum_slot_lock);
}

static void quantum_enable_cmd_interrupt(struct quantum_chip *chip);
static void quantum_disable_interrupts(struct quantum_chip *chip);

struct quantum_device_entry {
	const char *card_id;
	const char *name;
};

static const struct quantum_device_entry quantum_2626_model = {
	.card_id = "Quantum2626",
	.name = "PreSonus Quantum 2626",
};

static const struct pci_device_id quantum_ids[] = {
	{ PCI_DEVICE(0x1c67, 0x0104),
	  .driver_data = (kernel_ulong_t)&quantum_2626_model },
	{ }
};
MODULE_DEVICE_TABLE(pci, quantum_ids);

/* Device init */

static void quantum_device_init(struct quantum_chip *chip)
{
	u32 fpga_bl_ver, fpga_image_ver, fpga_ifc_ver;
	u32 mcu_bl_ver, mcu_main_ver;
	u32 dfu_payload_size;

	if (test_bit(0, &chip->device_gone))
		return;

	fpga_bl_ver = quantum_read32(chip, QUANTUM_REG_FPGA_BL_VER);
	fpga_image_ver = quantum_read32(chip, QUANTUM_REG_FPGA_IMAGE_VER);
	fpga_ifc_ver = quantum_read32(chip, QUANTUM_REG_FPGA_IFC_VER);
	mcu_bl_ver = quantum_read32(chip, QUANTUM_REG_MCU_BL_VER);
	mcu_main_ver = quantum_read32(chip, QUANTUM_REG_MCU_MAIN_VER);

	dev_dbg(&chip->pci->dev,
		"FPGA: BL=0x%08x IMG=0x%08x IFC=0x%08x\n",
		fpga_bl_ver, fpga_image_ver, fpga_ifc_ver);
	dev_dbg(&chip->pci->dev,
		"MCU: BL=0x%08x MAIN=0x%08x\n",
		mcu_bl_ver, mcu_main_ver);

	chip->addr_per_segment_rec = quantum_read32(chip, QUANTUM_REG_ADDR_PER_SEGMENT);
	chip->addr_per_segment_play = quantum_read32(chip, QUANTUM_REG_ADDR_PER_SEGMENT2);

	if (chip->addr_per_segment_rec == 0 || chip->addr_per_segment_rec >= 512)
		chip->addr_per_segment_rec = 15;
	if (chip->addr_per_segment_play == 0 || chip->addr_per_segment_play >= 512)
		chip->addr_per_segment_play = 15;

	dev_dbg(&chip->pci->dev,
		"ADDR_PER_SEGMENT: REC=%u PLAY=%u\n",
		chip->addr_per_segment_rec, chip->addr_per_segment_play);

	dfu_payload_size = quantum_read32(chip, QUANTUM_REG_DFU_PAYLOAD_SIZE);
	dev_dbg(&chip->pci->dev, "DFU payload size: 0x%08x\n",
		dfu_payload_size);

	chip->sample_rate = 48000;
	chip->clock_source = CLK_SOURCE_INTERNAL;
	chip->powered_on = true;
}

/* Command buffers */

static int quantum_init_command_buffers(struct quantum_chip *chip)
{
	u32 cfg;
	int i;
	size_t total_size;

	if (test_bit(0, &chip->device_gone))
		return -ENODEV;

	cfg = quantum_read32(chip, QUANTUM_REG_CMD_MSG_CONFIG);
	chip->cmd_msg_count = cfg & 0xFF;
	chip->cmd_msg_size = 4096;

	dev_dbg(&chip->pci->dev,
		"CMD_MSG_CONFIG: %u slots x %u bytes\n",
		chip->cmd_msg_count, chip->cmd_msg_size);

	if (chip->cmd_msg_count == 0 || chip->cmd_msg_count > 8)
		chip->cmd_msg_count = 8;

	total_size = chip->cmd_msg_count * chip->cmd_msg_size;

	chip->cmd_tx_cpu = dma_alloc_coherent(&chip->pci->dev, total_size,
					      &chip->cmd_tx_dma_base,
					      GFP_KERNEL);
	if (!chip->cmd_tx_cpu)
		return -ENOMEM;
	memset(chip->cmd_tx_cpu, 0, total_size);

	chip->cmd_rx_cpu = dma_alloc_coherent(&chip->pci->dev, total_size,
					      &chip->cmd_rx_dma_base,
					      GFP_KERNEL);
	if (!chip->cmd_rx_cpu) {
		dma_free_coherent(&chip->pci->dev, total_size,
				  chip->cmd_tx_cpu, chip->cmd_tx_dma_base);
		return -ENOMEM;
	}
	memset(chip->cmd_rx_cpu, 0, total_size);

	for (i = 0; i < chip->cmd_msg_count; i++) {
		chip->cmd_tx_slots[i] = chip->cmd_tx_cpu + i * chip->cmd_msg_size;
		chip->cmd_tx_dma[i] = chip->cmd_tx_dma_base + i * chip->cmd_msg_size;
		chip->cmd_rx_slots[i] = chip->cmd_rx_cpu + i * chip->cmd_msg_size;
		chip->cmd_rx_dma[i] = chip->cmd_rx_dma_base + i * chip->cmd_msg_size;
	}

	dev_dbg(&chip->pci->dev,
		"command buffers: TX=%pad RX=%pad (%zu bytes total)\n",
		 &chip->cmd_tx_dma_base, &chip->cmd_rx_dma_base,
		 total_size * 2);

	chip->dma_prepared = true;
	return 0;
}

static void quantum_prepare_dma(struct quantum_chip *chip)
{
	int i;

	if (!chip->dma_prepared)
		return;

	/* Do not expose command interrupts until the IRQ handler is installed. */
	quantum_write32_mask(chip, QUANTUM_REG_DMA_INT_ENABLE,
			     QUANTUM_INT_DMA | QUANTUM_INT_CMD_RX, 0);
	quantum_write32(chip, QUANTUM_REG_INTERRUPT_STATUS,
			QUANTUM_INT_DMA | QUANTUM_INT_CMD_RX);

	for (i = 0; i < chip->cmd_msg_count; i++) {
		quantum_write32(chip, QUANTUM_REG_CMD_TX_ADDR_BASE + i * 8,
				lower_32_bits(chip->cmd_tx_dma[i]));
		quantum_write32(chip, QUANTUM_REG_CMD_TX_ADDR_BASE + i * 8 + 4,
				upper_32_bits(chip->cmd_tx_dma[i]));
	}

	for (i = 0; i < chip->cmd_msg_count; i++) {
		quantum_write32(chip, QUANTUM_REG_CMD_RX_ADDR_BASE + i * 8,
				lower_32_bits(chip->cmd_rx_dma[i]));
		quantum_write32(chip, QUANTUM_REG_CMD_RX_ADDR_BASE + i * 8 + 4,
				upper_32_bits(chip->cmd_rx_dma[i]));
	}

	for (i = 0; i < chip->cmd_msg_count; i++)
		quantum_write32(chip, QUANTUM_REG_CMD_TX_LEN_BASE + i * 4, 0);

	quantum_write32(chip, QUANTUM_REG_TX_SW_POS, 0);
	quantum_write32(chip, QUANTUM_REG_RX_SW_POS, 0);
}

/* Enable command DMA */

static int quantum_enable_dma_engine(struct quantum_chip *chip)
{
	u32 status;
	int retry;

	if (!chip->dma_prepared)
		return -EINVAL;

	if (!chip->iobase)
		return -ENODEV;

	quantum_write32(chip, QUANTUM_REG_TX_SW_POS, 0);
	quantum_write32(chip, QUANTUM_REG_RX_SW_POS, 0);

	status = quantum_read32(chip, QUANTUM_REG_INTERRUPT_STATUS);
	if (status)
		quantum_write32(chip, QUANTUM_REG_INTERRUPT_STATUS, status);

	quantum_write32(chip, QUANTUM_REG_DMA_CONTROL, 0x101);

	for (retry = 0; retry < 100; retry++) {
		status = quantum_read32(chip, QUANTUM_REG_DMA_STATUS);
		if ((status & 0x10001) == 0x10001)
			break;

		usleep_range(100, 200);
	}

	if (retry >= 100) {
		dev_err(&chip->pci->dev,
			"command DMA did not start (status=0x%08x)\n", status);
		return -ETIMEDOUT;
	}

	dev_dbg(&chip->pci->dev, "command DMA started\n");
	return 0;
}

/* Disable command DMA */

static void quantum_clear_command_dma_registers(struct quantum_chip *chip)
{
	int i;

	for (i = 0; i < chip->cmd_msg_count; i++) {
		quantum_write32(chip, QUANTUM_REG_CMD_TX_ADDR_BASE + i * 8, 0);
		quantum_write32(chip,
				QUANTUM_REG_CMD_TX_ADDR_BASE + i * 8 + 4, 0);
		quantum_write32(chip, QUANTUM_REG_CMD_RX_ADDR_BASE + i * 8, 0);
		quantum_write32(chip,
				QUANTUM_REG_CMD_RX_ADDR_BASE + i * 8 + 4, 0);
		quantum_write32(chip, QUANTUM_REG_CMD_TX_LEN_BASE + i * 4, 0);
	}

	quantum_write32(chip, QUANTUM_REG_TX_SW_POS, 0);
	quantum_write32(chip, QUANTUM_REG_RX_SW_POS, 0);

	/* Flush posted command-ring register clears before freeing slots. */
	quantum_read32(chip, QUANTUM_REG_RX_SW_POS);
}

static void quantum_disable_dma_engine(struct quantum_chip *chip)
{
	u32 status = 0;
	int retry;

	if (!chip->dma_prepared || quantum_device_unavailable(chip))
		return;

	quantum_disable_interrupts(chip);
	quantum_write32(chip, QUANTUM_REG_DMA_CONTROL, 0);

	for (retry = 0; retry < 100; retry++) {
		status = quantum_read32(chip, QUANTUM_REG_DMA_STATUS);
		if ((status & 0x10001) == 0)
			break;

		usleep_range(100, 200);
	}

	if (retry == 100) {
		dev_warn(&chip->pci->dev,
			 "command DMA did not become idle (status=0x%08x)\n",
			 status);
	} else {
		quantum_clear_command_dma_registers(chip);
	}

	status = quantum_read32(chip, QUANTUM_REG_INTERRUPT_STATUS);
	if (status && status != U32_MAX)
		quantum_write32(chip, QUANTUM_REG_INTERRUPT_STATUS, status);

	dev_dbg(&chip->pci->dev, "command DMA stopped\n");
}

/* Complete command-interface initialization */

static int quantum_complete_init(struct quantum_chip *chip)
{
	int retry;
	int err;

	if (test_bit(0, &chip->device_gone))
		return -ENODEV;

	if (!chip->iobase) {
		dev_err(&chip->pci->dev, "cannot complete initialization without BAR mapping\n");
		return -EIO;
	}

	quantum_prepare_dma(chip);
	err = quantum_enable_dma_engine(chip);
	if (err)
		return err;

	for (retry = 0; retry < 100; retry++) {
		u32 tx = quantum_read32(chip, QUANTUM_REG_TX_SLOT_STATUS);
		u32 rx = quantum_read32(chip, QUANTUM_REG_RX_SLOT_STATUS);

		if (tx == 0 && rx == 0) {
			chip->tci_initialized = true;
			dev_dbg(&chip->pci->dev, "command interface initialized\n");
			return 0;
		}
			usleep_range(100, 200);
	}

	dev_err(&chip->pci->dev, "command ring did not become idle\n");
	return -ETIMEDOUT;
}

/* Interrupt control */

static void quantum_enable_cmd_interrupt(struct quantum_chip *chip)
{
	u32 val;

	if (quantum_device_unavailable(chip))
		return;

	quantum_write32(chip, QUANTUM_REG_INTERRUPT_STATUS, 0x80000000);

	val = quantum_read32(chip, QUANTUM_REG_DMA_INT_ENABLE);
	val |= QUANTUM_INT_CMD_RX;
	quantum_write32(chip, QUANTUM_REG_DMA_INT_ENABLE, val);
}

static void quantum_disable_interrupts(struct quantum_chip *chip)
{
	u32 val;

	if (quantum_device_unavailable(chip))
		return;

	val = quantum_read32(chip, QUANTUM_REG_DMA_INT_ENABLE);
	val &= ~(QUANTUM_INT_DMA | QUANTUM_INT_CMD_RX);
	quantum_write32(chip, QUANTUM_REG_DMA_INT_ENABLE, val);
}

/*
 * dma_mutex guards this function's read/modify/write of last_dma_pos,
 * dma_cycle_count, dma_elapsed_frames, dma_pending_frames, and the
 * per-direction period accumulators/attach-pending flags -- the same
 * fields quantum_pcm_trigger() touches under dma_mutex on the control-plane
 * side, and which were previously unsynchronized against this IRQ thread.
 *
 * The lock is held ONLY around that bookkeeping and is always released
 * before calling snd_pcm_period_elapsed(): ALSA's own XRUN handling can
 * call back into this driver's .trigger() synchronously from inside
 * snd_pcm_period_elapsed() (on this same thread), and quantum_pcm_trigger()
 * also takes dma_mutex -- holding it across that call self-deadlocks the
 * IRQ thread. Never widen the locked region to include the
 * snd_pcm_period_elapsed() calls below.
 */
static void quantum_process_audio(struct quantum_chip *chip)
{
	struct snd_pcm_substream *playback;
	struct snd_pcm_substream *capture;
	u32 elapsed;
	u32 period_size;
	u32 playback_periods_to_fire = 0;
	u32 capture_periods_to_fire = 0;

	if (quantum_device_gone(chip))
		return;

	/* Pair with stream_active publication before reading stream state. */
	if (!smp_load_acquire(&chip->stream_active) ||
	    !READ_ONCE(chip->dma_resources_allocated))
		return;

	mutex_lock(&chip->dma_mutex);

	quantum_update_dma_position(chip);
	if (chip->dma_elapsed_frames >= 2 * chip->dma_quantum_frames)
		dev_dbg_ratelimited(&chip->pci->dev,
				    "late audio service: elapsed=%u quantum=%u\n",
			chip->dma_elapsed_frames, chip->dma_quantum_frames);

	quantum_transfer_audio(chip);
	elapsed = chip->dma_elapsed_frames;
	if (!elapsed) {
		mutex_unlock(&chip->dma_mutex);
		return;
	}

	playback = READ_ONCE(chip->playback_substream);
	if (playback && playback->runtime) {
		period_size = playback->runtime->period_size;
		if (period_size) {
			if (chip->playback_attach_pending) {
				/*
				 * Playback restarted while capture kept the shared DMA
				 * engine running.  Wait until the hardware ring returns
				 * to frame zero so ALSA and the device agree on the
				 * restarted stream's initial position.
				 */
				chip->playback_period_accum = 0;
				if (!chip->last_dma_pos)
					chip->playback_attach_pending = false;
			} else {
				chip->playback_period_accum += elapsed;
				playback_periods_to_fire =
					chip->playback_period_accum / period_size;
				chip->playback_period_accum %= period_size;
			}
		}
	}

	capture = READ_ONCE(chip->capture_substream);
	if (capture && capture->runtime) {
		period_size = capture->runtime->period_size;
		if (period_size) {
			if (chip->capture_attach_pending) {
				/* Same ring-zero attach logic as playback, above. */
				chip->capture_period_accum = 0;
				if (!chip->last_dma_pos)
					chip->capture_attach_pending = false;
			} else {
				chip->capture_period_accum += elapsed;
				capture_periods_to_fire =
					chip->capture_period_accum / period_size;
				chip->capture_period_accum %= period_size;
			}
		}
	}

	mutex_unlock(&chip->dma_mutex);

	while (playback_periods_to_fire--)
		snd_pcm_period_elapsed(playback);
	while (capture_periods_to_fire--)
		snd_pcm_period_elapsed(capture);
}

static void quantum_cmd_work_handler(struct work_struct *work)
{
	struct quantum_chip *chip = container_of(work, struct quantum_chip,
						 cmd_work);

	if (quantum_device_gone(chip))
		return;

	dev_dbg_ratelimited(&chip->pci->dev, "processing command responses\n");
	quantum_process_rx_messages(chip);
}

static irqreturn_t quantum_interrupt_handler(int irq, void *data)
{
	struct quantum_chip *chip = data;
	u32 status;

	if (quantum_device_unavailable(chip))
		return IRQ_NONE;

	status = quantum_read32(chip, QUANTUM_REG_INTERRUPT_STATUS);
	if (!status || status == U32_MAX)
		return IRQ_NONE;

	/* Clear the W1C status before waking the thread. */
	quantum_write32(chip, QUANTUM_REG_INTERRUPT_STATUS, status);
	atomic_or(status, &chip->pending_interrupts);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t quantum_interrupt_thread(int irq, void *data)
{
	struct quantum_chip *chip = data;
	u32 status;
	irqreturn_t ret = IRQ_NONE;

	if (quantum_device_gone(chip))
		return IRQ_NONE;

	while ((status = atomic_xchg(&chip->pending_interrupts, 0))) {
		if (quantum_device_gone(chip))
			break;

		if (status & QUANTUM_INT_DMA)
			quantum_process_audio(chip);
		if (status & QUANTUM_INT_CMD_RX)
			queue_work(chip->cmd_wq, &chip->cmd_work);
		ret = IRQ_HANDLED;
	}

	return ret;
}

/* Handle device power-state events */

void quantum_power_state_changed(struct quantum_chip *chip, bool powered_on)
{
	if (quantum_device_gone(chip) ||
	    READ_ONCE(chip->resources_released))
		return;

	if (chip->powered_on == powered_on)
		return;

	chip->powered_on = powered_on;

	if (powered_on) {
		int err;

		dev_dbg(&chip->pci->dev, "power state changed: on\n");
		quantum_prepare_dma(chip);
		err = quantum_enable_dma_engine(chip);
		if (err)
			dev_err(&chip->pci->dev,
				"failed to restart command DMA: %d\n", err);
		else
			quantum_enable_cmd_interrupt(chip);
	} else {
		dev_dbg(&chip->pci->dev, "power state changed: off\n");
		quantum_disable_dma_engine(chip);
		if (READ_ONCE(chip->stream_active))
			quantum_stop_dma(chip);
	}
}

/* Device creation */

static int snd_quantum_create(struct snd_card *card, struct pci_dev *pci,
			      const struct quantum_device_entry *model)
{
	struct quantum_chip *chip = card->private_data;
	unsigned long irq_flags;
	int err, nvec, ret;

	chip->card = card;
	chip->pci = pci;
	chip->model_id = model->card_id;
	chip->model_name = model->name;
	chip->irq = -1;
	atomic_set(&chip->pending_interrupts, 0);
	chip->device_gone = 0;
	WRITE_ONCE(chip->removing, false);
	WRITE_ONCE(chip->dma_resources_allocated, false);
	chip->playback.pt = &chip->playback.pt_storage;
	chip->capture.pt = &chip->capture.pt_storage;

	spin_lock_init(&chip->cmd_lock);
	spin_lock_init(&chip->midi_lock);
	mutex_init(&chip->dma_mutex);
	mutex_init(&chip->tci_tx_mutex);
	chip->async_quiesced = false;
	INIT_LIST_HEAD(&chip->pending_cmds);

	chip->tci_initialized = false;
	chip->tci_ready = false;

	chip->cmd_wq = alloc_workqueue("quantum-cmd",
				       WQ_UNBOUND | WQ_MEM_RECLAIM,
				       1);

	if (!chip->cmd_wq) {
		dev_err(&pci->dev, "failed to create command workqueue\n");
		err = -ENOMEM;
		goto err_wq;
	}

	INIT_WORK(&chip->cmd_work, quantum_cmd_work_handler);
	INIT_WORK(&chip->midi_tx_work, quantum_midi_tx_work_handler);

	err = pci_enable_device(pci);
	if (err)
		goto err_wq;

	err = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
	if (err) {
		dev_err(&pci->dev, "failed to set 32-bit DMA mask: %d\n", err);
		goto err_disable;
	}
	pci_set_master(pci);

	err = pci_request_regions(pci, DRV_NAME);
	if (err)
		goto err_disable;

	chip->iobase = pci_iomap(pci, 0, pci_resource_len(pci, 0));
	if (!chip->iobase) {
		err = -ENOMEM;
		goto err_regions;
	}
	quantum_device_init(chip);
	err = quantum_init_command_buffers(chip);
	if (err) {
		dev_err(&pci->dev, "command buffer initialization failed: %d\n", err);
		goto err_iounmap;
	}
	err = quantum_complete_init(chip);
	if (err) {
		dev_err(&pci->dev, "hardware initialization failed: %d\n", err);
		goto err_free_cmd;
	}
	nvec = pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSI);
	if (nvec > 0) {
		chip->irq = pci_irq_vector(pci, 0);
		chip->msi_allocated = true;
		pci_intx(pci, 0);
	} else {
		chip->irq = pci->irq;
		chip->msi_allocated = false;
		pci_intx(pci, 1);
		dev_warn(&pci->dev, "MSI unavailable, using legacy INTx\n");
	}

	if (chip->irq > 0) {
		irq_flags = chip->msi_allocated ? 0 : IRQF_SHARED;
		err = request_threaded_irq(chip->irq,
					   quantum_interrupt_handler,
					   quantum_interrupt_thread,
					   irq_flags,
					   DRV_NAME, chip);
		if (!err) {
			chip->irq_requested = true;
			card->sync_irq = chip->irq;
		} else {
			dev_err(&pci->dev, "failed to request IRQ %d: %d\n",
				chip->irq, err);
			goto err_free_irq;
		}
	} else {
		err = -ENODEV;
		goto err_free_irq;
	}

	quantum_enable_cmd_interrupt(chip);
	ret = quantum_get_serial_number(chip);
	if (!ret) {
		chip->tci_ready = true;
		chip->tci_initialized = true;
		err = snd_quantum_pcm_new(chip);
		if (err) {
			dev_err(&pci->dev, "PCM creation failed: %d\n", err);
			goto err_free_irq;
		}
		err = quantum_midi_new(chip);
		if (err) {
			dev_err(&pci->dev, "MIDI creation failed: %d\n", err);
			goto err_free_irq;
		}

		err = quantum_mixer_new(chip);
		if (err) {
			dev_err(&pci->dev, "Mixer creation failed: %d\n", err);
			goto err_free_irq;
		}
	} else {
		chip->tci_ready = false;
		chip->tci_initialized = false;
		chip->tci_fatal_error = true;
		err = -EIO;
		goto err_free_irq;
	}

	return 0;

err_free_irq:
	quantum_disable_dma_engine(chip);
	if (chip->irq_requested) {
		free_irq(chip->irq, chip);
		chip->irq_requested = false;
		card->sync_irq = -1;
	}
	cancel_work_sync(&chip->cmd_work);
	cancel_work_sync(&chip->midi_tx_work);
	if (chip->msi_allocated)
		pci_free_irq_vectors(pci);
	goto err_free_cmd_buffers;
err_free_cmd:
	quantum_disable_dma_engine(chip);
err_free_cmd_buffers:
	if (chip->cmd_tx_cpu) {
		dma_free_coherent(&pci->dev,
				  chip->cmd_msg_count * chip->cmd_msg_size,
			chip->cmd_tx_cpu, chip->cmd_tx_dma_base);
	}
	if (chip->cmd_rx_cpu) {
		dma_free_coherent(&pci->dev,
				  chip->cmd_msg_count * chip->cmd_msg_size,
			chip->cmd_rx_cpu, chip->cmd_rx_dma_base);
	}
err_iounmap:
	pci_iounmap(pci, chip->iobase);
err_regions:
	pci_release_regions(pci);
err_disable:
	pci_disable_device(pci);
err_wq:
	if (chip->cmd_wq)
		destroy_workqueue(chip->cmd_wq);
	return err;
}

/* Device teardown */

static void quantum_begin_remove(struct quantum_chip *chip)
{
	if (READ_ONCE(chip->removing))
		return;

	WRITE_ONCE(chip->removing, true);
	/* Publish removal before any callback can enter the hardware paths. */
	smp_mb();
	WRITE_ONCE(chip->tci_ready, false);
	WRITE_ONCE(chip->tci_initialized, false);
}

static void quantum_mark_gone(struct quantum_chip *chip)
{
	set_bit(0, &chip->device_gone);
	/* Publish device_gone before clearing stream-visible pointers. */
	smp_mb__after_atomic();

	WRITE_ONCE(chip->stream_active, false);
	WRITE_ONCE(chip->playback_substream, NULL);
	WRITE_ONCE(chip->capture_substream, NULL);
}

/*
 * Marks the device gone under dma_mutex, the lock hw_params()/hw_free() hold
 * around their own DMA-API calls, so those calls always either finish first
 * or see device_gone before starting.
 */
static void quantum_mark_gone_locked(struct quantum_chip *chip)
{
	mutex_lock(&chip->dma_mutex);
	quantum_mark_gone(chip);
	mutex_unlock(&chip->dma_mutex);
}

static void quantum_abort_pending_commands(struct quantum_chip *chip)
{
	struct quantum_tci_cmd *cmd;
	unsigned long flags;

	spin_lock_irqsave(&chip->cmd_lock, flags);
	list_for_each_entry(cmd, &chip->pending_cmds, list) {
		if (cmd->responded)
			continue;

		cmd->status = -ENODEV;
		cmd->responded = true;
		complete(&cmd->done);
	}
	spin_unlock_irqrestore(&chip->cmd_lock, flags);
}

static void snd_quantum_drain_tci(struct quantum_chip *chip)
{
	/* Drain a sender that passed its removal check before removing was set. */
	mutex_lock(&chip->tci_tx_mutex);
	mutex_unlock(&chip->tci_tx_mutex);
	quantum_abort_pending_commands(chip);
}

static void snd_quantum_stop_async(struct snd_card *card)
{
	struct quantum_chip *chip = card->private_data;
	unsigned long flags;

	if (!chip || READ_ONCE(chip->async_quiesced))
		return;

	if (chip->irq_requested) {
		free_irq(chip->irq, chip);
		chip->irq_requested = false;
		card->sync_irq = -1;
	}
	atomic_set(&chip->pending_interrupts, 0);

	cancel_work_sync(&chip->cmd_work);
	cancel_work_sync(&chip->midi_tx_work);

	spin_lock_irqsave(&chip->midi_lock, flags);
	chip->midi_input_triggered = false;
	chip->midi_output_triggered = false;
	chip->midi_input = NULL;
	chip->midi_output = NULL;
	spin_unlock_irqrestore(&chip->midi_lock, flags);

	WRITE_ONCE(chip->async_quiesced, true);
}

static void snd_quantum_shutdown_hardware(struct quantum_chip *chip)
{
	if (quantum_device_unavailable(chip))
		return;

	dev_dbg(&chip->pci->dev, "stopping hardware DMA engines\n");

	mutex_lock(&chip->dma_mutex);
	quantum_shutdown_audio_dma(chip);
	mutex_unlock(&chip->dma_mutex);

	mutex_lock(&chip->tci_tx_mutex);
	quantum_disable_dma_engine(chip);
	mutex_unlock(&chip->tci_tx_mutex);

	/* No coherent mapping may be released while the endpoint can bus-master. */
	if (!pci_dev_is_disconnected(chip->pci))
		pci_clear_master(chip->pci);
	dev_dbg(&chip->pci->dev, "hardware DMA engines stopped\n");
}

static void snd_quantum_quiesce_surprise(struct snd_card *card)
{
	struct quantum_chip *chip = card->private_data;

	if (!chip)
		return;

	quantum_begin_remove(chip);
	quantum_mark_gone_locked(chip);
	snd_quantum_drain_tci(chip);
	snd_quantum_stop_async(card);
}

static void snd_quantum_release_resources(struct snd_card *card)
{
	struct quantum_chip *chip = card->private_data;
	size_t cmd_bytes;

	if (!chip)
		return;

	/* private_free may be reached outside the normal PCI remove path. */
	if (!READ_ONCE(chip->removing))
		snd_quantum_quiesce_surprise(card);

	if (READ_ONCE(chip->resources_released))
		return;
	WRITE_ONCE(chip->resources_released, true);

	/*
	 * Release the slot only once truly done, not merely disconnected:
	 * releasing it from .remove() let a fresh hot-add reuse the same
	 * ALSA card index while the old, deferred-release instance was
	 * still alive.
	 */
	quantum_release_card_slot(chip->card_slot);

	dev_dbg(&chip->pci->dev, "releasing DMA and PCI resources\n");
	quantum_cpu_latency_sysfs_remove(chip);

	mutex_lock(&chip->dma_mutex);
	chip->pcm_configured = 0;
	chip->playback_configured_substream = NULL;
	chip->capture_configured_substream = NULL;
	quantum_free_dma_resources(chip);
	mutex_unlock(&chip->dma_mutex);

	/*
	 * This can run long after the PCI core tore down this device's
	 * IOMMU group, so skip the DMA-API teardown and leak instead of
	 * touching a device that's already gone.
	 */
	cmd_bytes = chip->cmd_msg_count * chip->cmd_msg_size;
	if (chip->cmd_tx_cpu) {
		if (!pci_dev_is_disconnected(chip->pci))
			dma_free_coherent(&chip->pci->dev, cmd_bytes,
					  chip->cmd_tx_cpu, chip->cmd_tx_dma_base);
		chip->cmd_tx_cpu = NULL;
	}
	if (chip->cmd_rx_cpu) {
		if (!pci_dev_is_disconnected(chip->pci))
			dma_free_coherent(&chip->pci->dev, cmd_bytes,
					  chip->cmd_rx_cpu, chip->cmd_rx_dma_base);
		chip->cmd_rx_cpu = NULL;
	}
	memset(chip->cmd_tx_slots, 0, sizeof(chip->cmd_tx_slots));
	memset(chip->cmd_rx_slots, 0, sizeof(chip->cmd_rx_slots));
	chip->dma_prepared = false;

	if (chip->msi_allocated) {
		pci_free_irq_vectors(chip->pci);
		chip->msi_allocated = false;
	}

	if (chip->iobase) {
		void __iomem *base = chip->iobase;

		/*
		 * Paired with the READ_ONCE() in quantum_iobase_or_null():
		 * concurrent PCM/mixer/MIDI callbacks read chip->iobase
		 * through that helper without any lock, so this write must
		 * be a single atomic store, not something the compiler could
		 * tear or reorder relative to pci_iounmap() below.
		 */
		WRITE_ONCE(chip->iobase, NULL);
		pci_iounmap(chip->pci, base);
	}

	/*
	 * By last-close a fresh hot-add may already have claimed the same
	 * BAR range on a new struct pci_dev. pci_release_regions() touches
	 * the global PCI resource tree, not just this pci_dev, so skip it
	 * once disconnected -- the PCI core already reclaimed these regions
	 * when it tore down the old slot.
	 */
	if (!pci_dev_is_disconnected(chip->pci)) {
		pci_release_regions(chip->pci);
		pci_disable_device(chip->pci);
	}

	dev_dbg(&chip->pci->dev, "resources released\n");

	/* Balances pci_dev_get() in snd_quantum_probe(). */
	pci_dev_put(chip->pci);
}

static void snd_quantum_free(struct snd_card *card)
{
	struct quantum_chip *chip = card->private_data;

	if (!chip)
		return;

	snd_quantum_release_resources(card);
	if (chip->cmd_wq) {
		destroy_workqueue(chip->cmd_wq);
		chip->cmd_wq = NULL;
	}
}

/* PCI glue */

static int snd_quantum_probe(struct pci_dev *pci,
			     const struct pci_device_id *pci_id)
{
	struct snd_card *card;
	struct quantum_chip *chip;
	const struct quantum_device_entry *model;
	int card_slot;
	int err;

	model = (const void *)pci_id->driver_data;
	dev_dbg(&pci->dev, "probing PCI device %04x:%04x\n",
		pci->vendor, pci->device);

	card_slot = quantum_acquire_card_slot();
	if (card_slot < 0)
		return card_slot;
	if (!enable[card_slot]) {
		quantum_release_card_slot(card_slot);
		return -ENOENT;
	}

	err = snd_card_new(&pci->dev, index[card_slot],
			   id[card_slot] ? id[card_slot] : model->card_id,
			   THIS_MODULE, sizeof(struct quantum_chip), &card);
	if (err < 0) {
		quantum_release_card_slot(card_slot);
		return err;
	}

	chip = card->private_data;
	chip->card_slot = card_slot;
	err = snd_quantum_create(card, pci, model);
	if (err < 0) {
		quantum_release_card_slot(card_slot);
		snd_card_free(card);
		return err;
	}

	/*
	 * Pin the pci_dev: on a surprise removal, teardown is deferred until
	 * the last open file closes, well after the PCI core has torn down
	 * this struct pci_dev. Balanced by pci_dev_put() in
	 * snd_quantum_release_resources().
	 */
	pci_dev_get(pci);
	card->private_free = snd_quantum_free;
	strscpy(card->driver, DRV_NAME, sizeof(card->driver));
	strscpy(card->shortname, model->name, sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s at %s",
		 model->name, pci_name(pci));

	err = snd_card_register(card);
	if (err < 0) {
		dev_err(&pci->dev, "failed to register sound card: %d\n", err);
		quantum_release_card_slot(card_slot);
		snd_card_free(card);
		return err;
	}

	pci_set_drvdata(pci, card);
	err = quantum_cpu_latency_sysfs_create(chip);
	if (err) {
		dev_err(&pci->dev,
			"CPU latency sysfs creation failed: %d\n", err);
		quantum_release_card_slot(card_slot);
		snd_card_free(card);
		pci_set_drvdata(pci, NULL);
		return err;
	}

	dev_info(&pci->dev, "%s, ALSA card %d, %s IRQ %d\n",
		 card->shortname, card->number,
		 chip->msi_allocated ? "MSI" : "legacy", chip->irq);
	return 0;
}

static void snd_quantum_remove(struct pci_dev *pci)
{
	struct snd_card *card = pci_get_drvdata(pci);
	struct quantum_chip *chip;
	bool surprise;

	if (!card)
		return;

	chip = card->private_data;
	surprise = pci_dev_is_disconnected(pci);
	pci_set_drvdata(pci, NULL);
	dev_dbg(&pci->dev, "removing sound card (%s removal)\n",
		surprise ? "surprise" : "orderly");

	quantum_cpu_latency_sysfs_remove(chip);

	quantum_begin_remove(chip);
	snd_quantum_drain_tci(chip);

	if (surprise) {
		/*
		 * BAR access is no longer safe; quiesce only software state.
		 * Do NOT use snd_card_disconnect_sync()/snd_card_free(): called
		 * from the pciehp hotplug IST, they'd block until every open
		 * file closes, stalling the PCI core's teardown indefinitely if
		 * a client still has the device open. snd_card_free_when_closed()
		 * disconnects immediately and defers release to last-close.
		 *
		 * Disconnect first so a client gets an immediate error instead
		 * of a handle that still looks live while we tear things down.
		 */
		snd_card_disconnect(card);

		quantum_mark_gone_locked(chip);

		/*
		 * ALSA core frees each PCM substream's preallocated buffer at
		 * last-close via dma_free_attrs() on &chip->pci->dev, whose
		 * DMA/IOMMU capability the PCI core tears down independently,
		 * long before last-close, causing that automatic free to oops
		 * in dma_free_contiguous(). A client's existing mmap of the
		 * buffer also stays valid and in use until then (disconnect
		 * doesn't revoke it), so we can't free the pages out from
		 * under it either. Forget the buffer instead of freeing it:
		 * do_free_pages() (pcm_memory.c) skips dmab->area == NULL,
		 * so ALSA core's later automatic free becomes a no-op and the
		 * pages are simply leaked once per surprise removal.
		 */
		if (chip->pcm) {
			struct snd_pcm_substream *substream;
			int stream;

			for_each_pcm_streams(stream) {
				substream = chip->pcm->streams[stream].substream;
				for (; substream; substream = substream->next)
					substream->dma_buffer.area = NULL;
			}
		}

		snd_quantum_stop_async(card);

		/*
		 * free_irq() above released the IRQ line, not the MSI vector
		 * allocation. pci_stop_bus_device() tears down the MSI domain
		 * right after this returns and WARNs in irq_domain_remove()
		 * if vectors are still allocated, so free them here rather
		 * than in the deferred release path.
		 */
		if (chip->msi_allocated) {
			pci_free_irq_vectors(chip->pci);
			chip->msi_allocated = false;
		}

		snd_card_free_when_closed(card);
		dev_dbg(&pci->dev, "sound card disconnected after surprise removal\n");
		return;
	}

	/*
	 * Disconnect first so no userspace operation can race an orderly hardware
	 * shutdown. This also waits for PCM hw_free() and close() release paths.
	 */
	dev_dbg(&pci->dev, "waiting for open ALSA files to close\n");
	snd_card_disconnect_sync(card);

	if (!surprise) {
		/* Drain every asynchronous MMIO user before stopping the endpoint. */
		snd_quantum_stop_async(card);
		snd_quantum_shutdown_hardware(chip);
		quantum_mark_gone_locked(chip);
	}

	snd_card_free(card);

	dev_dbg(&pci->dev, "sound card removed\n");
}

static struct pci_driver quantum_driver = {
	.name = DRV_NAME,
	.id_table = quantum_ids,
	.probe = snd_quantum_probe,
	.remove = snd_quantum_remove,
};

module_pci_driver(quantum_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nicholas Johnson <nicholas.johnson-opensource@outlook.com.au>");
MODULE_DESCRIPTION("PreSonus Quantum ALSA driver");
