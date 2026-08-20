/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Nicholas Johnson */
#ifndef QUANTUM_H
#define QUANTUM_H

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>

#include "quantum_regs.h"

/*
 * The hardware's actual DMA service granularity is fixed by rate, not by
 * whatever period size ALSA negotiated: the FPGA runs a constant base
 * quantum at <=48kHz and doubles/quadruples it at higher rates to hold
 * the same service interval in wall-clock time. Feeding it the raw ALSA
 * period size instead (e.g. 32 frames at 192kHz, where the real block
 * size is 4*32=128) is out of spec for the hardware and has been
 * observed to raise IOMMU faults and hang the machine.
 */
#define QUANTUM_BASE_HW_QUANTUM 32

static inline unsigned int quantum_hw_quantum(unsigned int rate,
					      unsigned int base_frames)
{
	(void)base_frames;

	if (rate > 96000)
		return QUANTUM_BASE_HW_QUANTUM * 4;
	if (rate > 48000)
		return QUANTUM_BASE_HW_QUANTUM * 2;
	return QUANTUM_BASE_HW_QUANTUM;
}

/* TCI protocol definitions */

#define TCI_CHAN_CMD_RESP      0x01
#define TCI_CHAN_CMD_EVENT     0x02
#define TCI_CHAN_MIDI_TX       0x11
#define TCI_CHAN_MIDI_RX       0x12
#define TCI_CHAN_CTRL          0x31
#define TCI_CHAN_CTRL_EVENT    0x32

/* Generic status reply, shared across TCI_CHAN_CMD_RESP and TCI_CHAN_CTRL */
#define TCI_RSP_STATUS         0x01

/* TCI_CHAN_CMD_RESP */
#define TCI_RSP_PARAMS         0x04
#define TCI_CMD_GET_PARAMS     0x07
#define TCI_CMD_SET_PARAMS     0x08
#define TCI_CMD_LOOPBACK       0x11
#define TCI_RSP_LOOPBACK       0x12

/* TCI_CHAN_CMD_EVENT */
#define TCI_CMD_EVENT_PARAMS   0x09

/* TCI_CHAN_MIDI_TX / TCI_CHAN_MIDI_RX */
#define TCI_CMD_MIDI_TX        0x41
#define TCI_RSP_MIDI_TX        0x42
#define TCI_EVT_MIDI_RX        0x43
#define TCI_EVT_MIDI_TX_FREE   0x44

/* TCI_CHAN_CTRL */
#define TCI_CTRL_GET_SAMPLE_RATE    0x31
#define TCI_CTRL_SET_SAMPLE_RATE    0x32
#define TCI_CTRL_GET_CLOCK_SOURCE   0x33
#define TCI_CTRL_SET_CLOCK_SOURCE   0x34
#define TCI_CTRL_RSP_SAMPLE_RATE    0x35
#define TCI_CTRL_RSP_CLOCK_SOURCE   0x36
#define TCI_CTRL_SET_SERIAL_NUMBER  0x38
#define TCI_CTRL_GET_SERIAL_NUMBER  0x39
#define TCI_CTRL_RSP_SERIAL_NUMBER  0x3a
#define TCI_CTRL_GET_POWER_STATE    0x3b
#define TCI_CTRL_RSP_POWER_STATE    0x3c
#define TCI_CTRL_GET_LATENCY        0x3d
#define TCI_CTRL_RSP_LATENCY        0x3e

/* TCI_CHAN_CTRL_EVENT */
#define TCI_EVT_CLOCK_STATUS        0x33
#define TCI_EVT_POWER_STATE         0x34

enum quantum_clk_source {
	CLK_SOURCE_INTERNAL = 0,
	CLK_SOURCE_EXTERNAL_SPDIF = 1,
	CLK_SOURCE_EXTERNAL_WORD_CLOCK = 2,
	CLK_SOURCE_EXTERNAL_ADAT1 = 3,
	CLK_SOURCE_EXTERNAL_ADAT2 = 4,
};

struct tci_header {
	__le16 length;
	u8 channel;
	u8 code;
	__le16 tid;
	__le16 reserved;
} __packed;

/* DMA structures */

struct quantum_dma_page {
	void *cpu_addr;
	dma_addr_t dma_handle;
};

struct quantum_page_table {
	dma_addr_t dma_handle;
	void *cpu_addr;
	size_t size;
	u32 num_entries;
	u32 num_segments;
	u32 addr_per_segment;
	struct quantum_dma_page *pages;
};

/* Stream state */

struct quantum_stream {
	bool valid;
	bool owns_pages;
	u32 channels;
	size_t buffer_bytes;
	struct quantum_page_table *pt;
	struct quantum_page_table pt_storage;
	struct quantum_dma_page *pages;
	u32 num_pages;
};

/* TCI command tracking */

struct quantum_tci_cmd {
	u16 tid;
	int status;
	bool responded;
	u8 response_code;
	u8 response_data[256];
	size_t response_len;
	struct completion done;
	struct list_head list;
};

/* Main device state */

#define QUANTUM_MAX_CMD_SLOTS 8

struct quantum_chip {
	struct snd_card *card;
	struct pci_dev *pci;
	void __iomem *iobase;
	const char *model_id;
	const char *model_name;

	/* Index into index[]/id[]/enable[]; see quantum_release_card_slot(). */
	int card_slot;

	atomic_t pending_interrupts;

	int irq;
	bool irq_requested;
	bool msi_allocated;

	/* Deferred command processing */
	struct workqueue_struct *cmd_wq;

	struct work_struct cmd_work;
	struct work_struct midi_tx_work;

	/* PCM */
	struct snd_pcm *pcm;
	struct snd_pcm_substream *playback_substream;
	struct snd_pcm_substream *capture_substream;
	struct snd_pcm_substream *playback_configured_substream;
	struct snd_pcm_substream *capture_configured_substream;
	u8 pcm_configured;
	struct snd_rawmidi *rmidi;
	struct snd_rawmidi_substream *midi_input;
	struct snd_rawmidi_substream *midi_output;
	bool midi_input_triggered;
	bool midi_output_triggered;
	/* Protects MIDI runtime substream state. */
	spinlock_t midi_lock;
	/* Serializes command and MIDI writes into the TCI TX ring. */
	struct mutex tci_tx_mutex;

	/* Audio DMA */
	struct quantum_stream playback;
	struct quantum_stream capture;

	/* Command buffers */
	void *cmd_tx_cpu;
	void *cmd_rx_cpu;
	dma_addr_t cmd_tx_dma_base;
	dma_addr_t cmd_rx_dma_base;

	void *cmd_tx_slots[QUANTUM_MAX_CMD_SLOTS];
	dma_addr_t cmd_tx_dma[QUANTUM_MAX_CMD_SLOTS];
	void *cmd_rx_slots[QUANTUM_MAX_CMD_SLOTS];
	dma_addr_t cmd_rx_dma[QUANTUM_MAX_CMD_SLOTS];

	u32 cmd_msg_count;
	u32 cmd_msg_size;

	/* Hardware configuration */
	u32 addr_per_segment_rec, addr_per_segment_play;

	/* State */
	u32 sample_rate;
	u32 clock_source;
	bool powered_on;
	bool stream_active;
	bool dma_resources_allocated;
	struct pm_qos_request cpu_latency_qos;
	int cpu_latency_us;
	bool cpu_latency_auto_low_latency;

	/* DMA position tracking */
	u32 last_dma_pos;
	bool dma_position_valid;
	u32 dma_cycle_count;
	u32 period_frames;
	u32 buffer_frames;
	u32 current_sample_rate;
	u32 dma_ring_frames;
	u32 dma_quantum_frames;
	u32 dma_elapsed_frames;
	u32 dma_pending_frames;
	/*
	 * The hardware exposes one shared free-running DMA position for both
	 * directions.  ALSA may restart one substream while the other keeps the
	 * engine running, so defer wakeups for the restarted substream until the
	 * shared ring naturally reaches frame zero again.
	 */
	bool playback_attach_pending;
	bool capture_attach_pending;
	u32 playback_period_accum;
	u32 capture_period_accum;
	u64 playback_xruns;
	u64 capture_xruns;

	/* TCI command tracking */
	u16 next_tid;
	struct list_head pending_cmds;
	/* Protects pending_cmds and command completion state. */
	spinlock_t cmd_lock;

	/* TCI readiness */
	bool tci_initialized;
	bool tci_ready;

	/* State bits */
	unsigned long device_gone;
	bool removing;
	bool tci_fatal_error;
	bool resources_released;
	unsigned long sysfs_removed;

	/* Serial number */
	u8 serial[32];

	/* DMA preparation flag */
	bool dma_prepared;

	/* Locks */
	struct mutex dma_mutex;
	bool async_quiesced;
};

/* MMIO helpers */

static inline bool quantum_device_removed(struct quantum_chip *chip)
{
	return !chip || !chip->pci ||
	       test_bit(0, &chip->device_gone) ||
	       READ_ONCE(chip->resources_released) ||
	       pci_dev_is_disconnected(chip->pci);
}

static inline bool quantum_device_gone(struct quantum_chip *chip)
{
	return quantum_device_removed(chip) || READ_ONCE(chip->removing);
}

/*
 * Returns the current iobase, or NULL if unavailable, reading chip->iobase
 * exactly once. quantum_read32()/quantum_write32() use only this returned
 * value; checking chip->iobase and separately dereferencing it is a
 * check-then-use race against snd_quantum_release_resources() nulling it
 * out from another thread between the two.
 */
static inline void __iomem *quantum_iobase_or_null(struct quantum_chip *chip)
{
	if (quantum_device_removed(chip))
		return NULL;

	return READ_ONCE(chip->iobase);
}

static inline bool quantum_device_unavailable(struct quantum_chip *chip)
{
	return !quantum_iobase_or_null(chip);
}

static inline u32 quantum_read32(struct quantum_chip *chip, u32 off)
{
	void __iomem *base = quantum_iobase_or_null(chip);

	if (!base)
		return U32_MAX;

	return readl(base + off);
}

static inline void quantum_write32(struct quantum_chip *chip, u32 off, u32 val)
{
	void __iomem *base = quantum_iobase_or_null(chip);

	if (!base)
		return;

	writel(val, base + off);
}

static inline void quantum_write32_mask(struct quantum_chip *chip, u32 off,
					u32 mask, u32 val)
{
	u32 tmp = quantum_read32(chip, off);

	if (tmp == U32_MAX)
		return;

	tmp &= ~mask;
	tmp |= val & mask;
	quantum_write32(chip, off, tmp);
}

/* Cross-file interfaces */

/* dma */
int quantum_allocate_dma_resources(struct quantum_chip *chip,
				   unsigned int sample_rate,
				   unsigned int period_frames,
				   unsigned int buffer_frames,
				   struct snd_pcm_substream *playback_substream,
				   struct snd_pcm_substream *capture_substream);
void quantum_free_dma_resources(struct quantum_chip *chip);
int quantum_start_dma(struct quantum_chip *chip);
void quantum_stop_dma(struct quantum_chip *chip);
void quantum_shutdown_audio_dma(struct quantum_chip *chip);
int quantum_cpu_latency_sysfs_create(struct quantum_chip *chip);
void quantum_cpu_latency_sysfs_remove(struct quantum_chip *chip);
void quantum_update_dma_position(struct quantum_chip *chip);
void quantum_transfer_audio(struct quantum_chip *chip);

/* pcm */
int snd_quantum_pcm_new(struct quantum_chip *chip);

/* mixer */
int quantum_mixer_new(struct quantum_chip *chip);

/* tci */
void quantum_process_rx_messages(struct quantum_chip *chip);
int quantum_tci_send_midi(struct quantum_chip *chip, const u8 *data, size_t len);

/* MIDI */
int quantum_midi_new(struct quantum_chip *chip);
void quantum_midi_receive(struct quantum_chip *chip, const u8 *payload,
			  size_t payload_len);
void quantum_midi_tx_work_handler(struct work_struct *work);
int quantum_set_sample_rate(struct quantum_chip *chip, unsigned int rate);
int quantum_set_clock_source(struct quantum_chip *chip, unsigned int source);
int quantum_get_clock_source(struct quantum_chip *chip, unsigned int *source);
int quantum_get_serial_number(struct quantum_chip *chip);

/* power */
void quantum_power_state_changed(struct quantum_chip *chip, bool powered_on);

#endif
