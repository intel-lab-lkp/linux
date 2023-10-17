/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __HX_INSPECT_H__
#define __HX_INSPECT_H__

#include "hx_core.h"
#include "hx_hid.h"

enum THP_INSPECTION_ENUM {
	HX_OPEN,
	HX_MICRO_OPEN,
	HX_SHORT,
	HX_SC,
	HX_WT_NOISE,
	HX_ABS_NOISE,
	HX_RAWDATA,
	HX_BPN_RAWDATA,
	HX_SORTING,

	HX_GAPTEST_RAW,
	/*HX_GAPTEST_RAW_X,*/
	/*HX_GAPTEST_RAW_Y,*/

	HX_ACT_IDLE_NOISE,
	HX_ACT_IDLE_RAWDATA,
	HX_ACT_IDLE_BPN_RAWDATA,
/*LPWUG test must put after Normal test*/
	HX_LP_WT_NOISE,
	HX_LP_ABS_NOISE,
	HX_LP_RAWDATA,
	HX_LP_BPN_RAWDATA,

	HX_LP_IDLE_NOISE,
	HX_LP_IDLE_RAWDATA,
	HX_LP_IDLE_BPN_RAWDATA,

	HX_BACK_NORMAL,/*Must put in the end*/
};

/*Himax DataType*/
#define DATA_SORTING            0x0A
#define DATA_OPEN               0x0B
#define DATA_MICRO_OPEN         0x0C
#define DATA_SHORT              0x0A
#define DATA_RAWDATA            0x0A
#define DATA_NOISE              0x0F
#define DATA_BACK_NORMAL        0x00
#define DATA_LP_RAWDATA      0x0C
#define DATA_LP_NOISE        0x0F
#define DATA_ACT_IDLE_RAWDATA   0x0A
#define DATA_ACT_IDLE_NOISE     0x0F
#define DATA_LP_IDLE_RAWDATA 0x0A
#define DATA_LP_IDLE_NOISE   0x0F

enum HX_DATA_TYPE_ENUM {
	HX_DATA_SORTING,
	HX_DATA_OPEN,
	HX_DATA_MICRO_OPEN,
	HX_DATA_SHORT,
	HX_DATA_RAWDATA,
	HX_DATA_NOISE,
	HX_DATA_BACK_NORMAL,
	HX_DATA_LP_RAWDATA,
	HX_DATA_LP_NOISE,
	HX_DATA_ACT_IDLE_RAWDATA,
	HX_DATA_ACT_IDLE_NOISE,
	HX_DATA_LP_IDLE_RAWDATA,
	HX_DATA_LP_IDLE_NOISE,
	HX_DATA_TYPE_MAX
};

enum HX_INSP_TEST_ERR_ENUM {
	/* OK */
	HX_INSP_OK = 0,

	/* FAIL */
	HX_INSP_FAIL = 1,

	/* Memory allocate errors */
	HX_INSP_MEMALLCTFAIL = 1 << 1,

	/* Abnormal screen state */
	HX_INSP_ESCREEN = 1 << 2,

	/* Out of specification */
	HX_INSP_ESPEC = 1 << 3,

	/* Criteria file error*/
	HX_INSP_EFILE = 1 << 4,

	/* Switch mode error*/
	HX_INSP_ESWITCHMODE = 1 << 5,

	/* Get raw data errors */
	HX_INSP_EGETRAW = 1 << 6,
};

void hx_self_test(struct work_struct *work);

int hx_get_data(struct himax_ts_data *ts, u8 *data, s32 len);

void hx_switch_data_type(struct himax_ts_data *ts, u32 type);

#endif
