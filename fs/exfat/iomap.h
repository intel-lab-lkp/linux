/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2026 Namjae Jeon <linkinjeon@kernel.org>
 */

#ifndef _LINUX_EXFAT_IOMAP_H
#define _LINUX_EXFAT_IOMAP_H

extern const struct iomap_write_ops exfat_iomap_folio_ops;
extern const struct iomap_ops exfat_read_iomap_ops;
extern const struct iomap_ops exfat_write_iomap_ops;
extern const struct iomap_dio_ops exfat_write_dio_ops;
extern const struct iomap_writeback_ops exfat_writeback_ops;
extern const struct iomap_ops exfat_mkwrite_iomap_ops;

#endif /* _LINUX_EXFAT_IOMAP_H */
