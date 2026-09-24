/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Thunderbolt Stream API
 *
 * Copyright (C) 2026, Intel Corporation
 * Author: Alan Borzeszkowski <alan.borzeszkowski@linux.intel.com>
 */

#ifndef THUNDERBOLT_STREAM_H_
#define THUNDERBOLT_STREAM_H_

struct tbstream_dev *tbstream_dev_from_file(struct file *file);

#endif /* THUNDERBOLT_STREAM_H_ */
