/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2023 OpenSynergy GmbH
 */
#ifndef _LINUX_VIRTIO_VIRTIO_SPI_H
#define _LINUX_VIRTIO_VIRTIO_SPI_H

#include <linux/types.h>
#include <linux/virtio_types.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>

// clang-format off
/* Sample data on trailing clock edge */
#define VIRTIO_SPI_CPHA (1u << 0)
/* Clock is high when IDLE */
#define VIRTIO_SPI_CPOL (1u << 1)
/* Chip Select is active high */
#define VIRTIO_SPI_CS_HIGH (1u << 2)
/* Transmit LSB first */
#define VIRTIO_SPI_MODE_LSB_FIRST (1u << 3)

/*
 * Beware: From here on the bits do not match any more the Linux definitions!
 */
/* Loopback mode */
#define VIRTIO_SPI_MODE_LOOP (1u << 4)

/* All config fields are read-only for the Virtio SPI driver */
struct virtio_spi_config {
	/* /dev/spidev<bus_num>.CS. For Linux must be >= 0 and <= S16_MAX */
	__le16 bus_num;
	/* # of /dev/spidev<bus_num>.CS with CS=0..chip_select_max_number -1 */
	__le16 chip_select_max_number;
	/*
	 * 0: physical SPI master doesn't support cs timing setting"
	 * 1:_physical SPI master supports cs timing setting
	 * TODO: Comment on this, unclear and naming not good!
	 * Meant is probably word_delay_ns, cs_setup_ns and cs_delay_hold_ns
	 * while cs_change_delay_inactive_ns may be supportable everywhere
	 * Or all are meant. And the naming mismatch is the cs_ when the most
	 * critical word_delay_ns which cannot be supported everywhere is also
	 * affected.
	 */
	u8 cs_timing_setting_enable;
	/* Alignment and future extension */
	u8 reserved[3];
};

/*
 * @slave_id: chipselect index the SPI transfer used.
 *
 * @bits_per_word: the number of bits in each SPI transfer word.
 *
 * @cs_change: whether to deselect device after finishing this transfer
 *     before starting the next transfer, 0 means cs keep asserted and
 *     1 means cs deasserted then asserted again.
 *
 * @tx_nbits: bus width for write transfer.
 *     0,1: bus width is 1, also known as SINGLE
 *     2  : bus width is 2, also known as DUAL
 *     4  : bus width is 4, also known as QUAD
 *     8  : bus width is 8, also known as OCTAL
 *     other values are invalid.
 *
 * @rx_nbits: bus width for read transfer.
 *     0,1: bus width is 1, also known as SINGLE
 *     2  : bus width is 2, also known as DUAL
 *     4  : bus width is 4, also known as QUAD
 *     8  : bus width is 8, also known as OCTAL
 *     other values are invalid.
 *
 * @reserved: for future use.
 *
 * @mode: SPI transfer mode.
 *     bit 0: CPHA, determines the timing (i.e. phase) of the data
 *         bits relative to the clock pulses.For CPHA=0, the
 *         "out" side changes the data on the trailing edge of the
 *         preceding clock cycle, while the "in" side captures the data
 *         on (or shortly after) the leading edge of the clock cycle.
 *         For CPHA=1, the "out" side changes the data on the leading
 *         edge of the current clock cycle, while the "in" side
 *         captures the data on (or shortly after) the trailing edge of
 *         the clock cycle.
 *     bit 1: CPOL, determines the polarity of the clock. CPOL=0 is a
 *         clock which idles at 0, and each cycle consists of a pulse
 *         of 1. CPOL=1 is a clock which idles at 1, and each cycle
 *         consists of a pulse of 0.
 *     bit 2: CS_HIGH, if 1, chip select active high, else active low.
 *     bit 3: LSB_FIRST, determines per-word bits-on-wire, if 0, MSB
 *         first, else LSB first.
 *     bit 4: LOOP, loopback mode.
 *
 * @freq: the transfer speed in Hz.
 *
 * @word_delay_ns: delay to be inserted between consecutive words of a
 *     transfer, in ns unit.
 *
 * @cs_setup_ns: delay to be introduced after CS is asserted, in ns
 *     unit.
 *
 * @cs_delay_hold_ns: delay to be introduced before CS is deasserted
 *     for each transfer, in ns unit.
 *
 * @cs_change_delay_inactive_ns: delay to be introduced after CS is
 *     deasserted and before next asserted, in ns unit.
 */
struct spi_transfer_head {
	__u8 slave_id;
	__u8 bits_per_word;
	__u8 cs_change;
	__u8 tx_nbits;
	__u8 rx_nbits;
	__u8 reserved[3];
	__le32 mode;
	__le32 freq;
	__le32 word_delay_ns;
	__le32 cs_setup_ns;
	__le32 cs_delay_hold_ns;
	__le32 cs_change_delay_inactive_ns;
};

struct spi_transfer_result {
#define VIRTIO_SPI_TRANS_OK 0
#define VIRTIO_SPI_TRANS_ERR 1
	__u8 result;
};
// clang-format on

#endif /* #ifndef _COQOS_VIRTIO_VIRTIO_SPI_H */
