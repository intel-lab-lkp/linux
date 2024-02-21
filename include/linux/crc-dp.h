/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CRC_DP_H
#define _LINUX_CRC_DP_H

#include <linux/types.h>

u8 crc_dp_msg_header(const uint8_t *data, size_t num_nibbles);
u8 crc_dp_msg_data(const uint8_t *data, u8 number_of_bytes);

#endif /* _LINUX_CRC_DP_H */
