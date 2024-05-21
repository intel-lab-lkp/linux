/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QCOM_FW_HELPER_H__
#define __QCOM_FW_HELPER_H__

struct device;

const char *qcom_get_board_fw(const char *firmware);
const char *devm_qcom_get_board_fw(struct device *dev, const char *firmware);

#endif
