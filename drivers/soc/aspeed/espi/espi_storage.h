/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ASPEED_ESPI_STORAGE_H
#define ASPEED_ESPI_STORAGE_H

#include <linux/fs.h>
#include <linux/types.h>

struct aspeed_espi_lun {
	struct file	*filp;
	loff_t		 file_length;
	loff_t		 num_sectors;
	unsigned int	 blksize;
	unsigned int	 blkbits;
	bool		 ro;
	bool		 cdrom;
};

int aspeed_espi_lun_open(struct aspeed_espi_lun *lun, const char *path,
			 bool initially_ro, bool cdrom);
void aspeed_espi_lun_close(struct aspeed_espi_lun *lun);
int aspeed_espi_lun_rw(struct aspeed_espi_lun *lun, bool write,
		       sector_t sector, unsigned int nsect, void *buf);
int aspeed_espi_lun_read(struct aspeed_espi_lun *lun, sector_t sector,
			 unsigned int nsect, void *buf);
int aspeed_espi_lun_write(struct aspeed_espi_lun *lun, sector_t sector,
			  unsigned int nsect, void *buf);
int aspeed_espi_lun_rw_bytes(struct aspeed_espi_lun *lun, bool write,
			     u32 addr, u32 len, u8 *buf);
int aspeed_espi_lun_erase_bytes(struct aspeed_espi_lun *lun,
				u32 addr, u32 len);

#endif /* _ASPEED_ESPI_STORAGE_H_ */
