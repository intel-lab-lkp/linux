// SPDX-License-Identifier: GPL-2.0
 /*
  * Universal Flash Storage Host Initiated Defragmentation
  * Authors:
  *	Huan Tang <tanghuan@vivo.com>
  *	Wenxing Cheng <wenxing.cheng@vivo.com>
  */

#include <linux/unaligned.h>
#include <linux/device.h>
#include <linux/stddef.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/container_of.h>
#include "ufshcd-priv.h"

#define HID_SCHED_COUNT_LIMIT	300
static int hid_sched_cnt;
static void ufs_hid_enable_work_fn(struct work_struct *work)
{
	struct ufs_hba *hba;
	int ret = 0;
	enum ufs_hid_defrag_operation defrag_op;
	u32 hid_ahit = 0;
	bool hid_flag = false;

	hba = container_of(work, struct ufs_hba, ufs_hid_enable_work.work);

	if (!hba->dev_info.hid_sup)
		return;

	down(&hba->host_sem);

	if (!ufshcd_is_user_access_allowed(hba)) {
		up(&hba->host_sem);
		return;
	}

	ufshcd_rpm_get_sync(hba);
	hid_ahit = hba->ahit;
	ufshcd_auto_hibern8_update(hba, 0);

	ret = ufshcd_query_attr(hba, UPIU_QUERY_OPCODE_READ_ATTR,
			QUERY_ATTR_IDN_HID_STATE, 0, 0, &hba->dev_info.hid_state);
	if (ret)
		hba->dev_info.hid_state = HID_IDLE;

	switch (hba->dev_info.hid_state) {
	case HID_IDLE:
		defrag_op = HID_ANALYSIS_ENABLE;
		hid_flag = true;
		break;
	case DEFRAG_REQUIRED:
		defrag_op = HID_ANALYSIS_AND_DEFRAG_ENABLE;
		hid_flag = true;
		break;
	case DEFRAG_COMPLETED:
	case DEFRAG_IS_NOT_REQUIRED:
		defrag_op = HID_ANALYSIS_AND_DEFRAG_DISABLE;
		hid_flag = true;
		break;
	case ANALYSIS_IN_PROGRESS:
	case DEFRAG_IN_PROGRESS:
	default:
		break;
	}

	if (hid_flag) {
		ufshcd_query_attr(hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
				QUERY_ATTR_IDN_HID_DEFRAG_OPERATION, 0, 0, &defrag_op);
		hid_flag = false;
	}
	ret = ufshcd_query_attr(hba, UPIU_QUERY_OPCODE_READ_ATTR,
				QUERY_ATTR_IDN_HID_STATE, 0, 0, &hba->dev_info.hid_state);
	if (ret)
		hba->dev_info.hid_state = HID_IDLE;

	ufshcd_auto_hibern8_update(hba, hid_ahit);
	ufshcd_rpm_put_sync(hba);
	up(&hba->host_sem);

	if (hba->dev_info.hid_state != HID_IDLE &&
			hid_sched_cnt++ < HID_SCHED_COUNT_LIMIT)
		schedule_delayed_work(&hba->ufs_hid_enable_work,
					msecs_to_jiffies(1000));
	else
		hid_sched_cnt = 0;
}

int ufs_hid_disable(struct ufs_hba *hba)
{
	enum ufs_hid_defrag_operation defrag_op = HID_ANALYSIS_AND_DEFRAG_DISABLE;
	u32 hid_ahit;
	int ret;

	down(&hba->host_sem);

	if (!ufshcd_is_user_access_allowed(hba)) {
		up(&hba->host_sem);
		return -EBUSY;
	}

	ufshcd_rpm_get_sync(hba);
	hid_ahit = hba->ahit;
	ufshcd_auto_hibern8_update(hba, 0);

	ret = ufshcd_query_attr(hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
				QUERY_ATTR_IDN_HID_DEFRAG_OPERATION, 0, 0, &defrag_op);

	ufshcd_auto_hibern8_update(hba, hid_ahit);
	ufshcd_rpm_put_sync(hba);
	up(&hba->host_sem);

	return ret;
}

void ufs_hid_init(struct ufs_hba *hba, const u8 *desc_buf)
{
	u32 ext_ufs_feature;
	struct ufs_dev_info *dev_info = &hba->dev_info;

	ext_ufs_feature = get_unaligned_be32(desc_buf +
				DEVICE_DESC_PARAM_EXT_UFS_FEATURE_SUP);

	if (ext_ufs_feature & UFS_DEV_HID_SUPPORT) {
		dev_info->hid_sup = true;
		INIT_DELAYED_WORK(&hba->ufs_hid_enable_work,
				ufs_hid_enable_work_fn);
	}
}
