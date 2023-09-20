// SPDX-License-Identifier: GPL-2.0-only
/*
 *   ALSA driver for MARIAN Seraph audio interfaces
 *
 *   Copyright (c) 2012 Florian Faber <faberman@linuxproaudio.org>,
 *		   2023 Ivan Orlov <ivan.orlov0322@gmail.com>
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/info.h>

#define M2_CHANNELS_COUNT	128

#define M2_SPEEDMODE		2

#define M2_FRAME_SIZE		(M2_CHANNELS_COUNT * 4)
#define SUBSTREAM_PERIOD_SIZE	(2048 * M2_FRAME_SIZE)
#define SUBSTREAM_BUF_SIZE	(2 * SUBSTREAM_PERIOD_SIZE)
#define M2_DMA_BUFSIZE		(SUBSTREAM_BUF_SIZE * 2)

#define SERAPH_RD_IRQ_STATUS      0x00
#define SERAPH_RD_HWPOINTER       0x8C

#define SERAPH_WR_DMA_ADR         0x04
#define SERAPH_WR_DMA_BLOCKS      0x10

#define M2_DISABLE_PLAY_IRQ	BIT(1)
#define M2_DISABLE_CAPT_IRQ	BIT(2)
#define M2_ENABLE_LOOPBACK	BIT(3)
#define SERAPH_WR_DMA_ENABLE      0x84
#define SERAPH_WR_IE_ENABLE       0xAC

#define PCI_VENDOR_ID_MARIAN            0x1382
#define PCI_DEVICE_ID_MARIAN_SERAPH_M2  0x5021

#define RATE_SLOW	54000

#define FREQ_MIN	28000
#define FREQ_MAX	108000

#define SPEEDMODE_SLOW	1
#define SPEEDMODE_FAST	2

#define MARIAN_PORTS_TYPE_INPUT	 0
#define MARIAN_PORTS_TYPE_OUTPUT 1

#define MARIAN_SPI_CLOCK_DIVIDER	0x74

#define WCLOCK_NEW_VAL		BIT(31)
#define SPI_ALL_READY		BIT(31)

#define M2_CLOCK_SRC_DCO	1
#define M2_CLOCK_SRC_SYNCBUS	2
#define M2_CLOCK_SRC_MADI1	4
#define M2_CLOCK_SRC_MADI2	5

#define M2_INP1_SYNC_CTL_ID	0
#define M2_INP1_CM_CTL_ID	0
#define M2_INP1_FM_CTL_ID	0
#define M2_INP1_FREQ_CTL_ID	4
#define M2_OUT1_CM_CTL_ID	0
#define M2_OUT1_FM_CTL_ID	0
#define M2_INP2_SYNC_CTL_ID	1
#define M2_INP2_CM_CTL_ID	1
#define M2_INP2_FM_CTL_ID	1
#define M2_INP2_FREQ_CTL_ID	5
#define M2_OUT2_CM_CTL_ID	1
#define M2_OUT2_FM_CTL_ID	1

#define M2_SPI_STATE		0x70
#define M2_SPI_RESET		0x70
#define M2_SPI_CHIP_SELECT	0x60
#define M2_SPI_BITS_TO_WRITE	0x64
#define M2_SPI_BITS_TO_READ	0x68
#define M2_SPI_WRITE_DATA	0x6C
#define M2_SPI_DELAY		100

#define M2_CLOCK_SRC_SELECT	0xC8
#define M2_WORD_CLOCK_REG	0x94

#define M2_SET_DCO		0x88

#define M2_VCO_CLOCK_RANGE	0x8C
#define M2_CLOCK_MODE		0x80

#define M2_SET_CLOCK_SRC	0x90

// MADI FPGA register 0x41
// Enable both MADI transmitters (=1)
#define M2_TX_ENABLE   0
// Use int (=0) or 32bit IEEE float (=1)
#define M2_INT_FLOAT   4
// Big endian (=0), little endian (=1)
#define M2_ENDIANNESS  5
// MSB first (=0), LSB first (=1)
#define M2_BIT_ORDER   6

// MADI FPGA register 0x42
// Send 56ch (=0) or 64ch (=1) MADI frames
#define M2_PORT1_MODE  0
// Send 48kHz (=0) or 96kHz (=1) MADI frames
#define M2_PORT1_FRAME 1
// Send 56ch (=0) or 64ch (=1) MADI frames
#define M2_PORT2_MODE  2
// Send 48kHz (=0) or 96kHz (=1) MADI frames
#define M2_PORT2_FRAME 3

#define M2_CARD_NAME		"Seraph M2"

struct marian_card {
	struct snd_pcm_substream *playback_substream;
	struct snd_pcm_substream *capture_substream;

	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_dma_buffer dmabuf;

	struct snd_dma_buffer playback_buf;
	struct snd_dma_buffer capture_buf;

	struct pci_dev *pci;
	unsigned long port;
	void __iomem *iobase;
	int irq;

	unsigned int idx;

	/* hardware registers lock */
	spinlock_t reglock;

	/* spinlock for SPI communication */
	spinlock_t spi_lock;

	/* mutex for frequency measurement */
	struct mutex freq_mutex;

	/* Enables or disables hardware loopback */
	int loopback;

	/* 0..15, meaning depending on the card type */
	unsigned int clock_source;

	/* Frequency of the internal oscillator (Hertz) */
	unsigned int dco;

	/* Clock settings mask */
	u8 shadow_40;

	/* TX enable/disable mask */
	u8 shadow_41;

	/* Port mode mask */
	u8 shadow_42;

	/* Frame mode mask */
	u8 frame;
};

enum CLOCK_SOURCE {
	CLOCK_SRC_INTERNAL = 0,
	CLOCK_SRC_SYNCBUS  = 1,
	CLOCK_SRC_INP1     = 2,
	CLOCK_SRC_INP2	   = 3,
};

enum m2_num_mode {
	M2_NUM_MODE_INT		= 0,
	M2_NUM_MODE_FLOAT	= 1,
};

enum m2_endianness_mode {
	M2_BE	= 0,
	M2_LE	= 1,
};

static const struct pci_device_id snd_marian_ids[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_MARIAN, PCI_DEVICE_ID_MARIAN_SERAPH_M2), 0, 0, 6},
	{ }
};

MODULE_DEVICE_TABLE(pci, snd_marian_ids);

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX; // Index 0-MAX
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR; // ID for this card

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for MARIAN PCI soundcard");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for MARIAN PCI soundcard");

static int spi_wait_for_ar(struct marian_card *marian)
{
	int tries = 10;

	while (tries > 0) {
		if (ioread32(marian->iobase + M2_SPI_STATE) == SPI_ALL_READY)
			break;
		udelay(M2_SPI_DELAY);
		tries--;
	}
	if (tries == 0)
		return -EIO;
	return 0;
}

static int marian_spi_transfer(struct marian_card *marian, uint16_t cs, uint16_t bits_write,
			       u8 *data_write, uint16_t bits_read, u8 *data_read)
{
	u32 buf = 0;
	unsigned int i;
	int err = 0;

	spin_lock(&marian->spi_lock);

	if (spi_wait_for_ar(marian) < 0)
		iowrite32(0x1234, marian->iobase + M2_SPI_RESET); // Resetting SPI bus

	iowrite32(cs, marian->iobase + M2_SPI_CHIP_SELECT);
	iowrite32(bits_write, marian->iobase + M2_SPI_BITS_TO_WRITE);
	iowrite32(bits_read, marian->iobase + M2_SPI_BITS_TO_READ);

	if (bits_write <= 32) {
		// left-align data
		if (bits_write <= 8)
			buf = data_write[0] << (32 - bits_write);
		else if (bits_write <= 16)
			buf = data_write[0] << 24 | data_write[1] << (32 - bits_write);

		iowrite32(buf, marian->iobase + M2_SPI_WRITE_DATA);
	}
	if (bits_read > 0 && bits_read <= 32) {
		if (spi_wait_for_ar(marian) < 0) {
			dev_dbg(marian->card->dev,
				"Bus didn't signal AR\n");
			err = -EIO;
			goto unlock_exit;
		}

		buf = ioread32(marian->iobase + MARIAN_SPI_CLOCK_DIVIDER);

		buf <<= 32 - bits_read;
		i = 0;

		while (bits_read > 0) {
			data_read[i++] = (buf >> 24) & 0xFF;
			buf <<= 8;
			bits_read -= 8;
		}
	}

unlock_exit:
	spin_unlock(&marian->spi_lock);
	return err;
}

static u8 marian_m2_spi_read(struct marian_card *marian, u8 adr)
{
	u8 buf_in;

	adr = adr & 0x7F;

	if (marian_spi_transfer(marian, 0x02, 8, &adr, 8, &buf_in) == 0)
		return buf_in;

	return 0;
}

static int marian_m2_spi_write(struct marian_card *marian, u8 adr, u8 val)
{
	u8 buf_out[2];

	buf_out[0] = 0x80 | adr;
	buf_out[1] = val;

	return marian_spi_transfer(marian, 0x02, 16, (u8 *)&buf_out, 0, NULL);
}

/*
 * Measure the frequency of a clock source.
 * The measurement is triggered and the FPGA's ready
 * signal polled (normally takes up to 2ms). The measurement
 * has only a certainty of 10-20Hz, this function rounds it up
 * to the nearest 10Hz step (in 1FS).
 */
static unsigned int marian_measure_freq(struct marian_card *marian, unsigned int source)
{
	u32 val;
	int tries = 5;

	mutex_lock(&marian->freq_mutex);
	iowrite32(source, marian->iobase + M2_CLOCK_SRC_SELECT);

	mdelay(2);

	while (tries > 0) {
		val = ioread32(marian->iobase + M2_WORD_CLOCK_REG);
		if (val & WCLOCK_NEW_VAL)
			break;

		mdelay(1);
		tries--;
	}

	mutex_unlock(&marian->freq_mutex);

	if (tries > 0)
		return (((1280000000 / ((val & 0x3FFFF) + 1)) + 5 * M2_SPEEDMODE)
		/ (10 * M2_SPEEDMODE)) * 10;

	return 0;
}

static int marian_generic_frequency_info(struct snd_kcontrol *kcontrol,
					 struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = FREQ_MIN;
	uinfo->value.integer.max = FREQ_MAX;
	uinfo->value.integer.step = 1;
	return 0;
}

static int marian_generic_frequency_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = marian_measure_freq(marian, kcontrol->private_value);
	return 0;
}

static int marian_generic_frequency_create(struct marian_card *marian, char *label, u32 idx)
{
	struct snd_kcontrol_new c = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = label,
		.private_value = idx,
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = marian_generic_frequency_info,
		.get = marian_generic_frequency_get
	};

	return snd_ctl_add(marian->card, snd_ctl_new1(&c, marian));
}

static void marian_generic_set_dco(struct marian_card *marian, unsigned int freq)
{
	u64 val;

	val = freq;
	val <<= 36;
	val /= 80000000;

	iowrite32((u32)val, marian->iobase + M2_SET_DCO);

	marian->dco = freq;
}

static int marian_generic_dco_int_info(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = FREQ_MIN;
	uinfo->value.integer.max = FREQ_MAX;
	uinfo->value.integer.step = 1;
	return 0;
}

static int marian_generic_dco_int_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = marian->dco;

	return 0;
}

static int marian_generic_dco_int_put(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	spin_lock(&marian->reglock);
	marian_generic_set_dco(marian, ucontrol->value.integer.value[0]);
	spin_unlock(&marian->reglock);

	return 0;
}

static int marian_generic_dco_int_create(struct marian_card *marian, char *label)
{
	struct snd_kcontrol_new c = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = label,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = marian_generic_dco_int_info,
		.get = marian_generic_dco_int_get,
		.put = marian_generic_dco_int_put
	};

	return snd_ctl_add(marian->card, snd_ctl_new1(&c, marian));
}

static int marian_control_pcm_loopback_info(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;

	return 0;
}

static int marian_control_pcm_loopback_get(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = marian->loopback;

	return 0;
}

static int marian_control_pcm_loopback_put(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	marian->loopback = ucontrol->value.integer.value[0];

	return 0;
}

static int marian_control_pcm_loopback_create(struct marian_card *marian)
{
	struct snd_kcontrol_new c = {
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
		.name = "Loopback",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = marian_control_pcm_loopback_info,
		.get = marian_control_pcm_loopback_get,
		.put = marian_control_pcm_loopback_put,
	};

	return snd_ctl_add(marian->card, snd_ctl_new1(&c, marian));
}

static void marian_m2_write_port_frame(struct marian_card *marian)
{
	marian->shadow_42 = marian->shadow_42 & ~((1 << M2_PORT1_FRAME) | (1 << M2_PORT2_FRAME));

	if (marian->frame & 1)
		marian->shadow_42 |= 1 << M2_PORT1_FRAME;
	if (marian->frame & 2)
		marian->shadow_42 |= 1 << M2_PORT2_FRAME;

	marian_m2_spi_write(marian, 0x42, marian->shadow_42);
}

static void marian_generic_set_speedmode(struct marian_card *marian, unsigned int rate)
{
	iowrite32(0x02, marian->iobase + M2_CLOCK_MODE);

	if (rate <= 41000)
		iowrite32(0x02, marian->iobase + M2_VCO_CLOCK_RANGE);
	else if (rate <= 82000)
		iowrite32(0x01, marian->iobase + M2_VCO_CLOCK_RANGE);
	else
		iowrite32(0x00, marian->iobase + M2_VCO_CLOCK_RANGE);

	marian_generic_set_dco(marian, rate);
}

static void marian_m2_set_speedmode(struct marian_card *marian, unsigned int rate)
{
	marian_generic_set_speedmode(marian, rate);
	marian_m2_write_port_frame(marian);
}

static int marian_m2_sync_state_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	static const char * const texts[] = { "No Signal", "Lock", "Sync" };

	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(texts), texts);
}

static int marian_m2_sync_state_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);
	u8 v = marian_m2_spi_read(marian, 0x00);

	v = (v >> (kcontrol->private_value * 2)) & 0x3;
	if (v == 3)
		v--;

	ucontrol->value.enumerated.item[0] = v;

	return 0;
}

static int marian_m2_sync_state_create(struct marian_card *marian, char *label, u32 idx)
{
	struct snd_kcontrol_new c = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = label,
		.private_value = idx,
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = marian_m2_sync_state_info,
		.get = marian_m2_sync_state_get
	};

	return snd_ctl_add(marian->card, snd_ctl_new1(&c, marian));
}

static int marian_m2_channel_mode_info(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *uinfo)
{
	static const char * const texts[] = { "56ch", "64ch" };

	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(texts), texts);
}

static int marian_m2_input_channel_mode_get(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);
	u8 v = marian_m2_spi_read(marian, 0x01);

	v = (v >> (kcontrol->private_value * 2)) & 0x1;
	ucontrol->value.enumerated.item[0] = v;

	return 0;
}

static int marian_m2_input_channel_mode_create(struct marian_card *marian,
					       char *label, u32 idx)
{
	struct snd_kcontrol_new c = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = label,
		.private_value = idx,
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = marian_m2_channel_mode_info,
		.get = marian_m2_input_channel_mode_get
	};

	return snd_ctl_add(marian->card, snd_ctl_new1(&c, marian));
}

static int marian_m2_frame_mode_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	static const char * const texts[] = { "48kHz", "96kHz" };

	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(texts), texts);
}

static int marian_m2_input_frame_mode_get(struct snd_kcontrol *kcontrol,
					  struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);
	u8 v = marian_m2_spi_read(marian, 0x01);

	v = (v >> ((kcontrol->private_value * 2) + 1)) & 0x1;
	ucontrol->value.enumerated.item[0] = v;

	return 0;
}

static int marian_m2_input_frame_mode_create(struct marian_card *marian, char *label, u32 idx)
{
	struct snd_kcontrol_new c = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = label,
		.private_value = idx,
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = marian_m2_frame_mode_info,
		.get = marian_m2_input_frame_mode_get
	};

	return snd_ctl_add(marian->card, snd_ctl_new1(&c, marian));
}

static u8 marian_m2_get_port_mode(struct marian_card *marian, unsigned int port)
{
	if (port)
		return (marian->shadow_42 >> M2_PORT2_MODE) & 1;
	else
		return (marian->shadow_42 >> M2_PORT1_MODE) & 1;
}

static int marian_m2_output_channel_mode_get(struct snd_kcontrol *kcontrol,
					     struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = marian_m2_get_port_mode(marian,
								     kcontrol->private_value);

	return 0;
}

static void marian_m2_set_port_mode(struct marian_card *marian, unsigned int port, u8 state)
{
	if (port)
		marian->shadow_42 = (marian->shadow_42 & ~(1 << M2_PORT2_MODE))
				| state << M2_PORT2_MODE;
	else
		marian->shadow_42 = (marian->shadow_42 & ~(1 << M2_PORT1_MODE))
				| state << M2_PORT1_MODE;

	marian_m2_spi_write(marian, 0x42, marian->shadow_42);
}

static int marian_m2_output_channel_mode_put(struct snd_kcontrol *kcontrol,
					     struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	spin_lock(&marian->reglock);
	marian_m2_set_port_mode(marian, kcontrol->private_value,
				ucontrol->value.enumerated.item[0]);
	spin_unlock(&marian->reglock);

	return 0;
}

static int marian_m2_output_channel_mode_create(struct marian_card *marian,
						char *label, u32 idx)
{
	struct snd_kcontrol_new c = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = label,
		.private_value = idx,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = marian_m2_channel_mode_info,
		.get = marian_m2_output_channel_mode_get,
		.put = marian_m2_output_channel_mode_put
	};

	return snd_ctl_add(marian->card, snd_ctl_new1(&c, marian));
}

static int marian_m2_output_frame_mode_info(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_info *uinfo)
{
	return snd_ctl_boolean_mono_info(kcontrol, uinfo);
}

static int marian_m2_output_frame_mode_get(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	ucontrol->value.enumerated.item[0] = (marian->frame >> kcontrol->private_value) & 1;

	return 0;
}

static void marian_m2_set_port_frame(struct marian_card *marian, unsigned int port, u8 state)
{
	marian->frame = (marian->frame & ~(1 << port)) | (state << port);

	marian_m2_write_port_frame(marian);
}

static int marian_m2_output_frame_mode_put(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	spin_lock(&marian->reglock);
	marian_m2_set_port_frame(marian, kcontrol->private_value,
				 ucontrol->value.enumerated.item[0]);
	spin_unlock(&marian->reglock);

	return 0;
}

static int marian_m2_output_frame_mode_create(struct marian_card *marian, char *label, u32 idx)
{
	struct snd_kcontrol_new c = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = label,
		.private_value = idx,
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = marian_m2_output_frame_mode_info,
		.get = marian_m2_output_frame_mode_get,
		.put = marian_m2_output_frame_mode_put,
	};

	return snd_ctl_add(marian->card, snd_ctl_new1(&c, marian));
}

static void marian_m2_set_clock_source(struct marian_card *marian, u8 source)
{
	spin_lock(&marian->reglock);
	iowrite32(source, marian->iobase + M2_SET_CLOCK_SRC);
	marian->clock_source = source;
	spin_unlock(&marian->reglock);
}

static int marian_m2_clock_source_info(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *uinfo)
{
	static const char * const texts[] = {"Internal", "Sync Bus",
					      "Input Port 1", "Input Port 2"};

	return snd_ctl_enum_info(uinfo, 1, ARRAY_SIZE(texts), texts);
}

static int marian_m2_clock_source_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	switch (marian->clock_source) {
	case M2_CLOCK_SRC_DCO:
		ucontrol->value.enumerated.item[0] = CLOCK_SRC_INTERNAL;
		break;
	case M2_CLOCK_SRC_SYNCBUS:
		ucontrol->value.enumerated.item[0] = CLOCK_SRC_SYNCBUS;
		break;
	case M2_CLOCK_SRC_MADI1:
		ucontrol->value.enumerated.item[0] = CLOCK_SRC_INP1;
		break;
	case M2_CLOCK_SRC_MADI2:
		ucontrol->value.enumerated.item[0] = CLOCK_SRC_INP2;
		break;
	default:
		dev_dbg(marian->card->dev,
			"Illegal value for clock_source! (%d)\n",
			marian->clock_source);
		return -EINVAL;
	}

	return 0;
}

static int marian_m2_clock_source_put(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct marian_card *marian = snd_kcontrol_chip(kcontrol);

	switch (ucontrol->value.enumerated.item[0]) {
	case CLOCK_SRC_INTERNAL:
		marian_m2_set_clock_source(marian, M2_CLOCK_SRC_DCO);
		break;
	case CLOCK_SRC_SYNCBUS:
		marian_m2_set_clock_source(marian, M2_CLOCK_SRC_SYNCBUS);
		break;
	case CLOCK_SRC_INP1:
		marian_m2_set_clock_source(marian, M2_CLOCK_SRC_MADI1);
		break;
	case CLOCK_SRC_INP2:
		marian_m2_set_clock_source(marian, M2_CLOCK_SRC_MADI2);
		break;
	}

	return 0;
}

static int marian_m2_clock_source_create(struct marian_card *marian)
{
	struct snd_kcontrol_new c = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Clock Source",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = marian_m2_clock_source_info,
		.get = marian_m2_clock_source_get,
		.put = marian_m2_clock_source_put,
	};

	return snd_ctl_add(marian->card, snd_ctl_new1(&c, marian));
}

static void marian_m2_set_float(struct marian_card *marian, enum m2_num_mode state)
{
	marian->shadow_41 = (marian->shadow_41 & ~(1 << M2_INT_FLOAT)) | state << M2_INT_FLOAT;
	marian_m2_spi_write(marian, 0x41, marian->shadow_41);
}

static void marian_m2_set_endianness(struct marian_card *marian, enum m2_endianness_mode state)
{
	marian->shadow_41 = (marian->shadow_41 & ~(1 << M2_ENDIANNESS)) | state << M2_ENDIANNESS;
	marian_m2_spi_write(marian, 0x41, marian->shadow_41);
}

/*
 * Controls:
 *
 * RO:
 *   - Input 1 sync state (no signal, lock, sync)
 *   - Input 1 channel mode (56/64ch)
 *   - Input 1 frame mode (48/96kHz)
 *   - Input 1 frequency
 *   - Input 2 sync state (no signal, lock, sync)
 *   - Input 2 channel mode (56/64ch)
 *   - Input 2 frame mode (48/96kHz)
 *   - Input 2 frequency
 *
 * RW:
 *   - Output 1 channel mode (56/64ch)
 *   - Output 1 frame mode (48/96kHz)
 *   - Output 2 channel mode (56/64ch)
 *   - Output 2 frame mode (48/96kHz)
 *   - Word clock source (Port 1, Port 2, Internal, Sync port, WCK input)
 *   - Speed mode (1, 2, 4FS)
 *   - DCO frequency (1 Hertz)
 *   - DCO frequency (1/1000th)
 */
static void marian_m2_create_controls(struct marian_card *marian)
{
	marian_m2_sync_state_create(marian, "Input 1 Sync", M2_INP1_SYNC_CTL_ID);
	marian_m2_sync_state_create(marian, "Input 2 Sync", M2_INP2_SYNC_CTL_ID);
	marian_m2_input_channel_mode_create(marian, "Input 1 Channel Mode",
					    M2_INP1_CM_CTL_ID);
	marian_m2_input_channel_mode_create(marian, "Input 2 Channel Mode",
					    M2_INP2_CM_CTL_ID);
	marian_m2_input_frame_mode_create(marian, "Input 1 Frame Mode",
					  M2_INP1_FM_CTL_ID);
	marian_m2_input_frame_mode_create(marian, "Input 2 Frame Mode",
					  M2_INP2_FM_CTL_ID);
	marian_generic_frequency_create(marian, "Input 1 Frequency",
					M2_INP1_FREQ_CTL_ID);
	marian_generic_frequency_create(marian, "Input 2 Frequency",
					M2_INP2_FREQ_CTL_ID);
	marian_m2_output_channel_mode_create(marian, "Output 1 Channel Mode",
					     M2_OUT1_CM_CTL_ID);
	marian_m2_output_channel_mode_create(marian, "Output 2 Channel Mode",
					     M2_OUT2_CM_CTL_ID);
	marian_m2_output_frame_mode_create(marian, "Output 1 96kHz Frame",
					   M2_OUT1_FM_CTL_ID);
	marian_m2_output_frame_mode_create(marian, "Output 2 96kHz Frame",
					   M2_OUT2_FM_CTL_ID);
	marian_m2_clock_source_create(marian);
	marian_generic_dco_int_create(marian, "DCO Freq (Hz)");
	marian_control_pcm_loopback_create(marian);
}

static void snd_marian_card_free(struct snd_card *card)
{
	struct marian_card *marian = card->private_data;

	if (!marian)
		return;

	snd_dma_free_pages(&marian->dmabuf);

	if (marian->irq >= 0)
		free_irq(marian->irq, (void *)marian);

	if (marian->iobase)
		pci_iounmap(marian->pci, marian->iobase);

	if (marian->port)
		pci_release_regions(marian->pci);

	pci_disable_device(marian->pci);
}

static void marian_proc_status_generic(struct marian_card *marian, struct snd_info_buffer *buffer)
{
	snd_iprintf(buffer, "*** Card registers\n");
	snd_iprintf(buffer, "RD 0x064: %08x (SPI bits written)\n", ioread32(marian->iobase + 0x64));
	snd_iprintf(buffer, "RD 0x068: %08x (SPI bits read)\n", ioread32(marian->iobase + 0x68));
	snd_iprintf(buffer, "RD 0x070: %08x (SPI bits status)\n", ioread32(marian->iobase + 0x70));
	snd_iprintf(buffer, "RD 0x088: %08x (Super clock measurement)\n",
		    ioread32(marian->iobase + 0x88));
	snd_iprintf(buffer, "RD 0x08C: %08x (HW Pointer)\n",
		    ioread32(marian->iobase + SERAPH_RD_HWPOINTER));
	snd_iprintf(buffer, "RD 0x094: %08x (Word clock measurement)\n",
		    ioread32(marian->iobase + 0x88));
	snd_iprintf(buffer, "RD 0x0F8: %08x (Extension board)\n",
		    ioread32(marian->iobase + 0xF8));
	snd_iprintf(buffer, "RD 0x244: %08x (DMA debug)\n",
		    ioread32(marian->iobase + 0x244));

	snd_iprintf(buffer, "\n*** Card status\n");
	snd_iprintf(buffer, "Firmware build: %08x\n", ioread32(marian->iobase + 0xFC));
	snd_iprintf(buffer, "Clock master : %s\n", (marian->clock_source == 1) ? "yes" : "no");
	snd_iprintf(buffer, "DCO frequency: %d Hz\n", marian->dco);
}

static void snd_marian_proc_status(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
	struct marian_card *marian = entry->private_data;
	u8 v1, v2;

	marian_proc_status_generic(marian, buffer);

	snd_iprintf(buffer, "\n*** MADI FPGA registers\n");
	snd_iprintf(buffer, "M2 MADI 00h: %02x\n", marian_m2_spi_read(marian, 0x00));
	snd_iprintf(buffer, "M2 MADI 01h: %02x\n", marian_m2_spi_read(marian, 0x01));
	snd_iprintf(buffer, "M2 MADI 02h: %02x\n", marian_m2_spi_read(marian, 0x02));
	snd_iprintf(buffer, "M2 MADI 40h: %02x\n", marian->shadow_40);
	snd_iprintf(buffer, "M2 MADI 41h: %02x\n", marian->shadow_41);
	snd_iprintf(buffer, "M2 MADI 42h: %02x\n", marian->shadow_42);

	snd_iprintf(buffer, "\n*** MADI FPGA status\n");
	snd_iprintf(buffer, "MADI FPGA firmware: 0x%02x\n", marian_m2_spi_read(marian, 0x02));

	snd_iprintf(buffer, "Clock source: ");
	switch (marian->clock_source) {
	case 1:
		snd_iprintf(buffer, "Internal DCO\n");
		break;
	case 2:
		snd_iprintf(buffer, "Sync bus\n");
		break;
	case 4:
		snd_iprintf(buffer, "MADI Input 1\n");
		break;
	case 5:
		snd_iprintf(buffer, "MADI Input 2\n");
		break;
	default:
		snd_iprintf(buffer, "UNKNOWN\n");
		break;
	}

	snd_iprintf(buffer, "Sample format: %s, %s Endian, %s first\n",
		    (marian->shadow_41 & (1 << M2_INT_FLOAT)) ? "Float" : "Integer",
		    (marian->shadow_41 & (1 << M2_ENDIANNESS)) ? "Little" : "Big",
		    (marian->shadow_41 & (1 << M2_BIT_ORDER)) ? "LSB" : "MSB");

	v1 = marian_m2_spi_read(marian, 0x00);
	v2 = marian_m2_spi_read(marian, 0x01);

	snd_iprintf(buffer, "MADI port 1 input: ");
	if (!(v1 & 0x03))
		snd_iprintf(buffer, "No signal\n");
	else
		snd_iprintf(buffer, "%s, %dch, %dkHz frame, %u Hz\n",
			    (v1 & 0x02) ? "sync" : "lock", (v2 & 0x01) ? 64 : 56,
			    (v2 & 0x02) ? 96 : 48, marian_measure_freq(marian, 4));

	snd_iprintf(buffer, "MADI port 2 input: ");
	if (!(v1 & 0x0C))
		snd_iprintf(buffer, "No signal\n");
	else
		snd_iprintf(buffer, "%s, %dch, %dkHz frame, %u Hz\n",
			    (v1 & 0x08) ? "sync" : "lock",
			    (v2 & 0x04) ? 64 : 56, (v2 & 0x08) ? 96 : 48,
			    marian_measure_freq(marian, 5));
}

static void marian_m2_proc_ports(struct marian_card *marian,
				 struct snd_info_buffer *buffer, unsigned int type)
{
	int i;

	for (i = 0; i < M2_CHANNELS_COUNT; i++)
		snd_iprintf(buffer, "%d=MADI p%dch%02d\n", i + 1, i / 64 + 1, i % 64 + 1);
}

static void snd_marian_proc_ports_in(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
	struct marian_card *marian = entry->private_data;

	snd_iprintf(buffer, "# generated by MARIAN Seraph driver\n");
	marian_m2_proc_ports(marian, buffer, MARIAN_PORTS_TYPE_INPUT);
}

static void snd_marian_proc_ports_out(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
	struct marian_card *marian = entry->private_data;

	snd_iprintf(buffer, "# generated by MARIAN Seraph driver\n");
	marian_m2_proc_ports(marian, buffer, MARIAN_PORTS_TYPE_OUTPUT);
}

static irqreturn_t snd_marian_interrupt(int irq, void *dev_id)
{
	struct marian_card *marian = (struct marian_card *)dev_id;
	unsigned int irq_status;

	irq_status = ioread32(marian->iobase + SERAPH_RD_IRQ_STATUS);

	if (irq_status & 0x00004800) {
		if (marian->playback_substream)
			snd_pcm_period_elapsed(marian->playback_substream);

		if (marian->capture_substream)
			snd_pcm_period_elapsed(marian->capture_substream);

		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static const struct snd_pcm_hardware m2_info_playback = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_NONINTERLEAVED
		| SNDRV_PCM_INFO_JOINT_DUPLEX | SNDRV_PCM_INFO_SYNC_START,
	.formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S32_BE
		| SNDRV_PCM_FMTBIT_FLOAT_LE | SNDRV_PCM_FMTBIT_FLOAT_BE,
	.rates = (SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_44100
		| SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200
		| SNDRV_PCM_RATE_96000),
	.rate_min = FREQ_MIN,
	.rate_max = FREQ_MAX,
	.channels_min = M2_CHANNELS_COUNT,
	.channels_max = M2_CHANNELS_COUNT,
	.buffer_bytes_max = SUBSTREAM_BUF_SIZE,
	.period_bytes_min = SUBSTREAM_PERIOD_SIZE,
	.period_bytes_max = SUBSTREAM_PERIOD_SIZE,
	.periods_min = 2,
	.periods_max = 2
};

static const struct snd_pcm_hardware m2_info_capture = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_NONINTERLEAVED
		| SNDRV_PCM_INFO_SYNC_START | SNDRV_PCM_INFO_JOINT_DUPLEX,
	.formats = SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S32_BE
		| SNDRV_PCM_FMTBIT_FLOAT_LE | SNDRV_PCM_FMTBIT_FLOAT_BE,
	.rates = (SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_44100
		| SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200
		| SNDRV_PCM_RATE_96000),
	.rate_min = FREQ_MIN,
	.rate_max = FREQ_MAX,
	.channels_min = M2_CHANNELS_COUNT,
	.channels_max = M2_CHANNELS_COUNT,
	.buffer_bytes_max = SUBSTREAM_BUF_SIZE,
	.period_bytes_min = SUBSTREAM_PERIOD_SIZE,
	.period_bytes_max = SUBSTREAM_PERIOD_SIZE,
	.periods_min = 2,
	.periods_max = 2
};

static int snd_marian_playback_open(struct snd_pcm_substream *substream)
{
	struct marian_card *marian = substream->private_data;

	substream->runtime->hw = m2_info_playback;

	marian->playback_substream = substream;

	snd_pcm_set_sync(substream);

	return 0;
}

static int snd_marian_capture_open(struct snd_pcm_substream *substream)
{
	struct marian_card *marian = substream->private_data;

	substream->runtime->hw = m2_info_capture;

	marian->capture_substream = substream;

	snd_pcm_set_sync(substream);

	return 0;
}

static int snd_marian_capture_release(struct snd_pcm_substream *substream)
{
	struct marian_card *marian = snd_pcm_substream_chip(substream);

	marian->capture_substream = NULL;

	return 0;
}

static int snd_marian_playback_release(struct snd_pcm_substream *substream)
{
	struct marian_card *marian = snd_pcm_substream_chip(substream);

	marian->playback_substream = NULL;

	return 0;
}

static void marian_m2_constraints(struct marian_card *marian, struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	spin_lock(&marian->reglock);
	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_FLOAT_BE:
		marian_m2_set_float(marian, M2_NUM_MODE_FLOAT);
		marian_m2_set_endianness(marian, M2_BE);
		break;
	case SNDRV_PCM_FORMAT_FLOAT_LE:
		marian_m2_set_float(marian, M2_NUM_MODE_FLOAT);
		marian_m2_set_endianness(marian, M2_LE);
		break;
	case SNDRV_PCM_FORMAT_S32_BE:
		marian_m2_set_float(marian, M2_NUM_MODE_INT);
		marian_m2_set_endianness(marian, M2_BE);
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		marian_m2_set_float(marian, M2_NUM_MODE_INT);
		marian_m2_set_endianness(marian, M2_LE);
		break;
	}
	spin_unlock(&marian->reglock);
}

static int snd_marian_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct marian_card *marian = snd_pcm_substream_chip(substream);

	spin_lock(&marian->reglock);

	marian_m2_set_speedmode(marian, params_rate(params));

	marian_generic_set_dco(marian, params_rate(params));
	spin_unlock(&marian->reglock);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		snd_pcm_set_runtime_buffer(substream, &marian->playback_buf);
	else
		snd_pcm_set_runtime_buffer(substream, &marian->capture_buf);

	marian_m2_constraints(marian, substream, params);

	return 0;
}

static int marian_m2_prepare(struct snd_pcm_substream *substream)
{
	struct marian_card *marian = snd_pcm_substream_chip(substream);
	u32 mask = 0xFFFFFFFF;

	spin_lock(&marian->reglock);
	iowrite32(mask, marian->iobase + 0x20);
	iowrite32(mask, marian->iobase + 0x24);
	iowrite32(mask, marian->iobase + 0x28);
	iowrite32(mask, marian->iobase + 0x2C);
	iowrite32(mask, marian->iobase + 0x30);
	iowrite32(mask, marian->iobase + 0x34);
	iowrite32(mask, marian->iobase + 0x38);
	iowrite32(mask, marian->iobase + 0x3C);
	spin_unlock(&marian->reglock);

	return 0;
}

// atomic by default, no need for locking here
static int snd_marian_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct marian_card *marian = snd_pcm_substream_chip(substream);
	int irq_flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		irq_flags = M2_DISABLE_PLAY_IRQ;
		if (marian->loopback)
			irq_flags |= M2_ENABLE_LOOPBACK;

		iowrite32(0x3, marian->iobase + SERAPH_WR_DMA_ENABLE);
		iowrite32(irq_flags, marian->iobase + SERAPH_WR_IE_ENABLE);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		irq_flags = M2_DISABLE_PLAY_IRQ | M2_DISABLE_CAPT_IRQ;
		iowrite32(irq_flags, marian->iobase + SERAPH_WR_IE_ENABLE);
		iowrite32(0x0, marian->iobase + SERAPH_WR_DMA_ENABLE);

		// unarm channels to inhibit playback from the FPGA's internal buffer
		iowrite32(0, marian->iobase + 0x08);
		iowrite32(0, marian->iobase + 0x0C);
		iowrite32(0, marian->iobase + 0x20);
		iowrite32(0, marian->iobase + 0x24);
		iowrite32(0, marian->iobase + 0x28);
		iowrite32(0, marian->iobase + 0x2C);
		iowrite32(0, marian->iobase + 0x30);
		iowrite32(0, marian->iobase + 0x34);
		iowrite32(0, marian->iobase + 0x38);
		iowrite32(0, marian->iobase + 0x3C);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t snd_marian_hw_pointer(struct snd_pcm_substream *substream)
{
	struct marian_card *marian = snd_pcm_substream_chip(substream);

	return ioread32(marian->iobase + SERAPH_RD_HWPOINTER);
}

static const struct snd_pcm_ops snd_marian_playback_ops = {
	.open = snd_marian_playback_open,
	.close = snd_marian_playback_release,
	.hw_params = snd_marian_hw_params,
	.prepare = marian_m2_prepare,
	.trigger = snd_marian_trigger,
	.pointer = snd_marian_hw_pointer,
};

static const struct snd_pcm_ops snd_marian_capture_ops = {
	.open = snd_marian_capture_open,
	.close = snd_marian_capture_release,
	.hw_params = snd_marian_hw_params,
	.prepare = marian_m2_prepare,
	.trigger = snd_marian_trigger,
	.pointer = snd_marian_hw_pointer,
};

static void construct_playback_buffer(struct marian_card *marian)
{
	marian->playback_buf = marian->dmabuf;
	marian->playback_buf.area += SUBSTREAM_BUF_SIZE;
	marian->playback_buf.addr += SUBSTREAM_BUF_SIZE;
	marian->playback_buf.bytes = SUBSTREAM_BUF_SIZE;
}

static void construct_capture_buffer(struct marian_card *marian)
{
	marian->capture_buf = marian->dmabuf;
	marian->capture_buf.bytes = SUBSTREAM_BUF_SIZE;
}

static int marian_m2_init(struct marian_card *marian)
{
	// reset DMA engine
	iowrite32(0x00000000, marian->iobase);

	// disable play interrupt
	iowrite32(M2_DISABLE_PLAY_IRQ, marian->iobase + SERAPH_WR_IE_ENABLE);

	marian_generic_set_speedmode(marian, RATE_SLOW);

	// init internal clock and set it as clock source
	marian_m2_set_clock_source(marian, 1);

	// init SPI clock divider
	iowrite32(0x1F, marian->iobase + MARIAN_SPI_CLOCK_DIVIDER);

	marian->shadow_40 = 0x00;
	marian->shadow_41 = (1 << M2_TX_ENABLE);
	marian->shadow_42 = (1 << M2_PORT1_MODE) | (1 << M2_PORT2_MODE);

	marian_m2_spi_write(marian, 0x40, marian->shadow_40);
	marian_m2_spi_write(marian, 0x41, marian->shadow_41);
	marian_m2_spi_write(marian, 0x42, marian->shadow_42);

	return 0;
}

static int snd_marian_create(struct snd_card *card, struct pci_dev *pci, unsigned int idx)
{
	struct snd_info_entry *entry;
	struct marian_card *marian = card->private_data;
	int err;
	unsigned int len;

	marian->card = card;
	marian->pcm = NULL;
	marian->pci = pci;
	marian->port = 0;
	marian->iobase = NULL;
	marian->irq = -1;
	marian->idx = idx;
	spin_lock_init(&marian->reglock);
	spin_lock_init(&marian->spi_lock);
	mutex_init(&marian->freq_mutex);

	err = pci_enable_device(pci);
	if (err < 0)
		return err;

	if (dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32))) {
		dev_err(&pci->dev, "Unable to set DMA mask\n");
		return err;
	}

	pci_set_master(pci);

	err = pci_request_regions(pci, "marian");
	if (err < 0)
		return err;

	marian->port = pci_resource_start(pci, 0);
	len = pci_resource_len(pci, 0);
	marian->iobase = pci_iomap(pci, 0, 0);
	if (!marian->iobase) {
		dev_err(&pci->dev, "unable to grab region 0x%lx-0x%lx\n",
			marian->port, marian->port + len - 1);
		return -EBUSY;
	}

	if (request_irq(pci->irq, snd_marian_interrupt, IRQF_SHARED, "marian", marian)) {
		dev_err(&pci->dev, "unable to grab IRQ %d\n", pci->irq);
		return -EBUSY;
	}
	marian->irq = pci->irq;

	card->private_free = snd_marian_card_free;

	strscpy(card->driver, "MARIAN FPGA", sizeof(card->driver));
	strscpy(card->shortname, M2_CARD_NAME, sizeof(card->shortname));
	sprintf(card->longname, "%s PCIe audio at 0x%lx, irq %d",
		card->shortname, marian->port, marian->irq);

	snd_card_set_dev(card, &pci->dev);

	err = snd_pcm_new(card, M2_CARD_NAME, 0, 1, 1, &marian->pcm);
	if (err < 0)
		return err;
	marian->pcm->private_data = marian;
	snd_pcm_set_ops(marian->pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_marian_playback_ops);
	snd_pcm_set_ops(marian->pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_marian_capture_ops);

	len = PAGE_ALIGN(M2_DMA_BUFSIZE);
	err = snd_dma_alloc_pages(SNDRV_DMA_TYPE_CONTINUOUS, &pci->dev,
				  M2_DMA_BUFSIZE, &marian->dmabuf);
	if (err < 0) {
		dev_err(card->dev, "Could not allocate %d Bytes (%d)\n", len, err);

		snd_dma_free_pages(&marian->dmabuf);
		return err;
	}

	iowrite32((u32)marian->dmabuf.addr, marian->iobase + SERAPH_WR_DMA_ADR);

	// Set 'block' count to buffer_frames/16 to set channel 'buffers' count (16 samples each)
	iowrite32(SUBSTREAM_BUF_SIZE / M2_FRAME_SIZE / 16, marian->iobase + SERAPH_WR_DMA_BLOCKS);

	construct_capture_buffer(marian);
	construct_playback_buffer(marian);

	if (!snd_card_proc_new(card, "status", &entry))
		snd_info_set_text_ops(entry, marian, snd_marian_proc_status);
	if (!snd_card_proc_new(card, "ports.in", &entry))
		snd_info_set_text_ops(entry, marian, snd_marian_proc_ports_in);
	if (!snd_card_proc_new(card, "ports.out", &entry))
		snd_info_set_text_ops(entry, marian, snd_marian_proc_ports_out);

	marian_m2_init(marian);
	marian_m2_create_controls(marian);

	return snd_card_register(card);
}

static int snd_marian_m2_probe(struct pci_dev *pci, const struct pci_device_id *pci_id)
{
	static unsigned int dev;
	struct snd_card *card;
	int err;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;

	err = snd_card_new(&pci->dev, index[dev], id[dev],
			   THIS_MODULE, sizeof(struct marian_card), &card);
	if (err < 0)
		return err;

	err = snd_marian_create(card, pci, dev);
	if (err < 0) {
		snd_card_free(card);
		return err;
	}

	pci_set_drvdata(pci, card);

	dev++;

	return 0;
}

static void snd_marian_m2_remove(struct pci_dev *pci)
{
	snd_card_free(pci_get_drvdata(pci));
	pci_set_drvdata(pci, NULL);
}

static struct pci_driver marian_driver = {
	.name = "MARIAN",
	.id_table = snd_marian_ids,
	.probe = snd_marian_m2_probe,
	.remove = snd_marian_m2_remove,
};

module_pci_driver(marian_driver);

MODULE_AUTHOR("Florin Faber, Ivan Orlov");
MODULE_DESCRIPTION("MARIAN Seraph M2");
MODULE_LICENSE("GPL");
