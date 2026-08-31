/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_SPI_FLASH_H
#define LINUX_SPI_FLASH_H

#include <linux/types.h>

struct mtd_partition;
struct spi_device;

/**
 * struct flash_platform_data: board-specific flash data
 * @name: optional flash device name (eg, as used with mtdparts=)
 * @parts: optional array of mtd_partitions for static partitioning
 * @nr_parts: number of mtd_partitions for static partitioning
 * @type: optional flash device type (e.g. m25p80 vs m25p64), for use
 *	with chips that can't be queried for JEDEC or other IDs
 * @is_locked: optional callback to query write protection enforced by the
 *	platform rather than by the flash chip itself, for example a SPI
 *	controller that gates writes to a range of the flash. Returns 1 if
 *	the whole range is protected, 0 if it is not, or a negative errno.
 *	When supplied it takes precedence over the chip's own block
 *	protection bits, which do not necessarily reflect what is actually
 *	being enforced.
 *
 * Board init code (in arch/.../mach-xxx/board-yyy.c files) can
 * provide information about SPI flash parts (such as DataFlash) to
 * help set up the device and its appropriate default partitioning.
 *
 * Note that for DataFlash, sizes for pages, blocks, and sectors are
 * rarely powers of two; and partitions should be sector-aligned.
 */
struct flash_platform_data {
	char		*name;
	struct mtd_partition *parts;
	unsigned int	nr_parts;

	char		*type;

	int		(*is_locked)(struct spi_device *spi, loff_t ofs, u64 len);

	/* we'll likely add more ... use JEDEC IDs, etc */
};

#endif
