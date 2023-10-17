// SPDX-License-Identifier: GPL-2.0
/*  Himax Driver Code for Common IC to simulate HID
 *
 *  Copyright (C) 2023 Himax Corporation.
 *
 *  This software is licensed under the terms of the GNU General Public
 *  License version 2,  as published by the Free Software Foundation,  and
 *  may be copied,  distributed,  and modified under those terms.
 *
 *  This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "hx_core.h"
#include "hx_hid.h"
#include "hx_ic_core.h"
#include "hx_plat.h"
#include "hx_inspect.h"

#define BS_RAWDATA     8
#define BS_NOISE       8
#define BS_OPENSHORT   0
#define	BS_LPWUG       1
#define	BS_LP_IDLE     1
#define	BS_ACT_IDLE    1

#define	NOISEFRAME                      60
#define NORMAL_IDLE_RAWDATA_NOISEFRAME  10
#define LP_RAWDATAFRAME              1
#define LP_NOISEFRAME                1
#define LP_IDLE_RAWDATAFRAME         1
#define LP_IDLE_NOISEFRAME           1

#define OTHERSFRAME		2

#define	UNIFMAX			500

/*Himax MP Password*/
#define	PWD_OPEN_START          0x7777
#define	PWD_OPEN_END            0x8888
#define	PWD_SHORT_START         0x1111
#define	PWD_SHORT_END           0x3333
#define	PWD_RAWDATA_START       0x0000
#define	PWD_RAWDATA_END         0x9999
#define	PWD_NOISE_START         0x0000
#define	PWD_NOISE_END           0x9999
#define	PWD_SORTING_START       0xAAAA
#define	PWD_SORTING_END         0xCCCC

#define PWD_ACT_IDLE_START      0x2222
#define PWD_ACT_IDLE_END        0x4444

#define PWD_LP_START         0x5555
#define PWD_LP_END           0x6666

#define PWD_LP_IDLE_START    0x5050
#define PWD_LP_IDLE_END      0x6060

/*Himax Data Ready Password*/
#define	DATA_PWD       0x5AA5

/*Inspection register*/
#define addr_normal_noise_thx   0x1000708C
#define addr_lpwug_noise_thx    0x10007090
#define addr_noise_scale        0x10007094
#define addr_recal_thx          0x10007090
#define addr_palm_num           0x100070A8
#define addr_weight_sup         0x100072C8
#define addr_normal_weight_a    0x1000709C
#define addr_lpwug_weight_a     0x100070A0
#define addr_weight_b           0x10007094
#define addr_max_dc             0x10007FC8
#define addr_skip_frame         0x100070F4
#define addr_neg_noise_sup      0x10007FD8
#define data_neg_noise          0x7F0C0000

/*Need to map THP_INSPECTION_ENUM*/
static char *g_himax_inspection_mode[] = {
	"HIMAX_OPEN",
	"HIMAX_MICRO_OPEN",
	"HIMAX_SHORT",
	"HIMAX_SC",
	"HIMAX_WEIGHT_NOISE",
	"HIMAX_ABS_NOISE",
	"HIMAX_RAWDATA",
	"HIMAX_BPN_RAWDATA",
	"HIMAX_SORTING",
	"HIMAX_GAPTEST_RAW",
	/*"HIMAX_GAPTEST_RAW_X",*/
	/*"HIMAX_GAPTEST_RAW_Y",*/

	"HIMAX_ACT_IDLE_NOISE",
	"HIMAX_ACT_IDLE_RAWDATA",
	"HIMAX_ACT_IDLE_BPN_RAWDATA",

	"HIMAX_LPWUG_WEIGHT_NOISE",
	"HIMAX_LPWUG_ABS_NOISE",
	"HIMAX_LPWUG_RAWDATA",
	"HIMAX_LPWUG_BPN_RAWDATA",

	"HIMAX_LPWUG_IDLE_NOISE",
	"HIMAX_LPWUG_IDLE_RAWDATA",
	"HIMAX_LPWUG_IDLE_BPN_RAWDATA",

	"HIMAX_BACK_NORMAL",
	NULL
};

enum HX_INSPT_SETTING_IDX {
	RAW_BS_FRAME = 0,
	NOISE_BS_FRAME,
	ACT_IDLE_BS_FRAME,
	LP_BS_FRAME,
	LP_IDLE_BS_FRAME,

	NFRAME,
	IDLE_NFRAME,
	LP_RAW_NFRAME,
	LP_NOISE_NFRAME,
	LP_IDLE_RAW_NFRAME,
	LP_IDLE_NOISE_NFRAME,
	NFRAME_MAX,
};

static s32 g_hx_inspt_setting_val[NFRAME_MAX] = {0};

static const u16 g_hx_data_type[HX_DATA_TYPE_MAX] = {
	DATA_SORTING,
	DATA_OPEN,
	DATA_MICRO_OPEN,
	DATA_SHORT,
	DATA_RAWDATA,
	DATA_NOISE,
	DATA_BACK_NORMAL,
	DATA_LP_RAWDATA,
	DATA_LP_NOISE,
	DATA_ACT_IDLE_RAWDATA,
	DATA_ACT_IDLE_NOISE,
	DATA_LP_IDLE_RAWDATA,
	DATA_LP_IDLE_NOISE,
};

static int hx_switch_mode_inspection(struct himax_ts_data *ts, int mode)
{
	union hx_dword_data_t tmp_addr = {0};
	union hx_dword_data_t tmp_data = {0};

	I("Entering");

	/*Stop Handshaking*/
	tmp_addr.dword = cpu_to_le32(sram_adr_rawdata_addr);
	g_core_fp.fp_register_write(ts, tmp_addr.byte, tmp_data.byte, 4);

	/*Switch Mode*/
	switch (mode) {
	case HX_SORTING:
		tmp_data.dword = cpu_to_le32(PWD_SORTING_START);
		break;
	case HX_OPEN:
		tmp_data.dword = cpu_to_le32(PWD_OPEN_START);
		break;
	case HX_MICRO_OPEN:
		tmp_data.dword = cpu_to_le32(PWD_OPEN_START);
		break;
	case HX_SHORT:
		tmp_data.dword = cpu_to_le32(PWD_SHORT_START);
		break;

	case HX_GAPTEST_RAW:
	case HX_RAWDATA:
	case HX_BPN_RAWDATA:
	case HX_SC:
		tmp_data.dword = cpu_to_le32(PWD_RAWDATA_START);
		break;

	case HX_WT_NOISE:
	case HX_ABS_NOISE:
		tmp_data.dword = cpu_to_le32(PWD_NOISE_START);
		break;

	case HX_ACT_IDLE_RAWDATA:
	case HX_ACT_IDLE_BPN_RAWDATA:
	case HX_ACT_IDLE_NOISE:
		tmp_data.dword = cpu_to_le32(PWD_ACT_IDLE_START);
		break;

	case HX_LP_RAWDATA:
	case HX_LP_BPN_RAWDATA:
	case HX_LP_ABS_NOISE:
	case HX_LP_WT_NOISE:
		tmp_data.dword = cpu_to_le32(PWD_LP_START);
		break;
	case HX_LP_IDLE_RAWDATA:
	case HX_LP_IDLE_BPN_RAWDATA:
	case HX_LP_IDLE_NOISE:
		tmp_data.dword = cpu_to_le32(PWD_LP_IDLE_START);
		break;

	default:
		I("Nothing to be done!");
		break;
	}

	if (g_core_fp.fp_assign_sorting_mode)
		g_core_fp.fp_assign_sorting_mode(ts, tmp_data.byte);
	I("End of setting!");

	return 0;
}

void hx_switch_data_type(struct himax_ts_data *ts, u32 type)
{
	u32 datatype = 0x00;

	I("Expected type[%d]=%s",
	  type, g_himax_inspection_mode[type]);

	switch (type) {
	case HX_SORTING:
		datatype = g_hx_data_type[HX_DATA_SORTING];
		break;
	case HX_OPEN:
		datatype = g_hx_data_type[HX_DATA_OPEN];
		break;
	case HX_MICRO_OPEN:
		datatype = g_hx_data_type[HX_DATA_MICRO_OPEN];
		break;
	case HX_SHORT:
		datatype = g_hx_data_type[HX_DATA_SHORT];
		break;
	case HX_RAWDATA:
	case HX_BPN_RAWDATA:
	case HX_SC:
	case HX_GAPTEST_RAW:
		datatype = g_hx_data_type[HX_DATA_RAWDATA];
		break;

	case HX_WT_NOISE:
	case HX_ABS_NOISE:
		datatype = g_hx_data_type[HX_DATA_NOISE];
		break;
	case HX_BACK_NORMAL:
		datatype = g_hx_data_type[HX_DATA_BACK_NORMAL];
		break;
	case HX_ACT_IDLE_RAWDATA:
	case HX_ACT_IDLE_BPN_RAWDATA:
		datatype = g_hx_data_type[HX_DATA_ACT_IDLE_RAWDATA];
		break;
	case HX_ACT_IDLE_NOISE:
		datatype = DATA_ACT_IDLE_NOISE;
		break;

	case HX_LP_RAWDATA:
	case HX_LP_BPN_RAWDATA:
		datatype = g_hx_data_type[HX_DATA_LP_RAWDATA];
		break;
	case HX_LP_WT_NOISE:
	case HX_LP_ABS_NOISE:
		datatype = g_hx_data_type[HX_DATA_LP_NOISE];
		break;
	case HX_LP_IDLE_RAWDATA:
	case HX_LP_IDLE_BPN_RAWDATA:
		datatype = g_hx_data_type[HX_DATA_LP_IDLE_RAWDATA];
		break;
	case HX_LP_IDLE_NOISE:
		datatype = g_hx_data_type[HX_DATA_LP_IDLE_NOISE];
		break;

	default:
		E("Wrong type=%d", type);
		break;
	}
	g_core_fp.fp_diag_register_set(ts, datatype);
}

static void hx_bank_search_set(struct himax_ts_data *ts, u32 n_frame,
			       u32 checktype)
{
	union hx_dword_data_t tmp_data = {0};
	union hx_dword_data_t tmp_addr = {0};

	/*skip frame 0x100070F4*/
	tmp_addr.dword = cpu_to_le32(addr_skip_frame);
	g_core_fp.fp_register_read(ts, tmp_addr.byte, tmp_data.byte, 4);

	switch (checktype) {
	case HX_ACT_IDLE_RAWDATA:
	case HX_ACT_IDLE_BPN_RAWDATA:
	case HX_ACT_IDLE_NOISE:
		if (g_hx_inspt_setting_val[ACT_IDLE_BS_FRAME] > 0)
			tmp_data.byte[0] = g_hx_inspt_setting_val[ACT_IDLE_BS_FRAME];
		else
			tmp_data.byte[0] = BS_ACT_IDLE;
		break;
	case HX_LP_RAWDATA:
	case HX_LP_BPN_RAWDATA:
	case HX_LP_ABS_NOISE:
	case HX_LP_WT_NOISE:
		if (g_hx_inspt_setting_val[LP_BS_FRAME] > 0)
			tmp_data.byte[0] = g_hx_inspt_setting_val[LP_BS_FRAME];
		else
			tmp_data.byte[0] = BS_LPWUG;
		break;
	case HX_LP_IDLE_RAWDATA:
	case HX_LP_IDLE_BPN_RAWDATA:
	case HX_LP_IDLE_NOISE:
		if (g_hx_inspt_setting_val[LP_IDLE_BS_FRAME] > 0)
			tmp_data.byte[0] = g_hx_inspt_setting_val[LP_IDLE_BS_FRAME];
		else
			tmp_data.byte[0] = BS_LP_IDLE;
		break;
	case HX_RAWDATA:
	case HX_BPN_RAWDATA:
	case HX_SC:
		if (g_hx_inspt_setting_val[RAW_BS_FRAME] > 0)
			tmp_data.byte[0] = g_hx_inspt_setting_val[RAW_BS_FRAME];
		else
			tmp_data.byte[0] = BS_RAWDATA;
		break;
	case HX_WT_NOISE:
	case HX_ABS_NOISE:
		if (g_hx_inspt_setting_val[NOISE_BS_FRAME] > 0)
			tmp_data.byte[0] = g_hx_inspt_setting_val[NOISE_BS_FRAME];
		else
			tmp_data.byte[0] = BS_NOISE;
		break;
	default:
		tmp_data.byte[0] = BS_OPENSHORT;
		break;
	}
	D("Now BankSearch Value=%d", tmp_data.byte[0]);

	g_core_fp.fp_register_write(ts, tmp_addr.byte, tmp_data.byte, 4);
}

static void hx_neg_noise_sup(struct himax_ts_data *ts, u8 *data)
{
	union hx_dword_data_t tmp_data = {0};
	union hx_dword_data_t tmp_addr = {0};

	/*0x10007FD8 Check support negative value or not */
	tmp_addr.dword = cpu_to_le32(addr_neg_noise_sup);
	g_core_fp.fp_register_read(ts, tmp_addr.byte, tmp_data.byte, 4);

	if ((tmp_data.byte[3] & 0x04) == 0x04) {
		tmp_data.dword = cpu_to_le32(data_neg_noise);
		data[2] = tmp_data.byte[2]; data[3] = tmp_data.byte[3];
	} else {
		I("Not support negative noise");
	}
}

static void hx_set_N_frame(struct himax_ts_data *ts, u32 n_frame,
			   u32 checktype)
{
	union hx_dword_data_t tmp_data = {0};
	union hx_dword_data_t tmp_addr = {0};

	hx_bank_search_set(ts, n_frame, checktype);

	/*IIR MAX - 0x10007294*/
	tmp_addr.dword = cpu_to_le32(fw_addr_set_frame_addr);
	tmp_data.dword = cpu_to_le32(n_frame);
	g_core_fp.fp_register_write(ts, tmp_addr.byte, tmp_data.byte, 4);

	if (checktype == HX_WT_NOISE ||
	    checktype == HX_ABS_NOISE ||
		checktype == HX_LP_WT_NOISE ||
		checktype == HX_LP_ABS_NOISE)
		hx_neg_noise_sup(ts, tmp_data.byte);
	I("Now N frame Value=0x%X",
	  le32_to_cpu(tmp_data.dword));

	g_core_fp.fp_register_write(ts, tmp_addr.byte, tmp_data.byte, 4);
}

static u32 hx_check_mode(struct himax_ts_data *ts, u8 checktype)
{
	int ret = 0;
	union hx_dword_data_t tmp_data = {0};
	u16 wait_pwd = {0};

	switch (checktype) {
	case HX_SORTING:
		wait_pwd = PWD_SORTING_END;
		break;
	case HX_OPEN:
		wait_pwd = PWD_OPEN_END;
		break;
	case HX_MICRO_OPEN:
		wait_pwd = PWD_OPEN_END;
		break;
	case HX_SHORT:
		wait_pwd = PWD_SHORT_END;
		break;
	case HX_RAWDATA:
	case HX_BPN_RAWDATA:
	case HX_SC:
	case HX_GAPTEST_RAW:
		wait_pwd = PWD_RAWDATA_END;
		break;

	case HX_WT_NOISE:
	case HX_ABS_NOISE:
		wait_pwd = PWD_NOISE_END;
		break;

	case HX_ACT_IDLE_RAWDATA:
	case HX_ACT_IDLE_BPN_RAWDATA:
	case HX_ACT_IDLE_NOISE:
		wait_pwd = PWD_ACT_IDLE_END;
		break;

	case HX_LP_RAWDATA:
	case HX_LP_BPN_RAWDATA:
	case HX_LP_ABS_NOISE:
	case HX_LP_WT_NOISE:
		wait_pwd = PWD_LP_END;
		break;
	case HX_LP_IDLE_RAWDATA:
	case HX_LP_IDLE_BPN_RAWDATA:
	case HX_LP_IDLE_NOISE:
		wait_pwd = PWD_LP_IDLE_END;
		break;

	default:
		E("Wrong type=%d", checktype);
		break;
	}

	if (g_core_fp.fp_check_sorting_mode) {
		ret = g_core_fp.fp_check_sorting_mode(ts, tmp_data.byte);
		if (ret != NO_ERR)
			return ret;
	}

	if ((le32_to_cpu(tmp_data.dword) & 0xFFFF) == wait_pwd) {
		I("It had been changed to [%d]=%s",
		  checktype, g_himax_inspection_mode[checktype]);
		return NO_ERR;
	} else {
		return 1;
	}
}

static u32 hx_wait_sorting_mode(struct himax_ts_data *ts,
				u8 checktype)
{
	int count = 0;
	union hx_dword_data_t tmp_addr = {0};
	union hx_dword_data_t tmp_data = {0};
	u16 wait_pwd = {0};

	D("start!");

	switch (checktype) {
	case HX_SORTING:
		wait_pwd = PWD_SORTING_END;
		break;
	case HX_OPEN:
		wait_pwd = PWD_OPEN_END;
		break;
	case HX_MICRO_OPEN:
		wait_pwd = PWD_OPEN_END;
		break;
	case HX_SHORT:
		wait_pwd = PWD_SHORT_END;
		break;
	case HX_RAWDATA:
	case HX_BPN_RAWDATA:
	case HX_SC:
	case HX_GAPTEST_RAW:
		wait_pwd = PWD_RAWDATA_END;
		break;
	case HX_WT_NOISE:
	case HX_ABS_NOISE:
		wait_pwd = PWD_NOISE_END;
		break;
	case HX_ACT_IDLE_RAWDATA:
	case HX_ACT_IDLE_BPN_RAWDATA:
	case HX_ACT_IDLE_NOISE:
		wait_pwd = PWD_ACT_IDLE_END;
		break;

	case HX_LP_RAWDATA:
	case HX_LP_BPN_RAWDATA:
	case HX_LP_ABS_NOISE:
	case HX_LP_WT_NOISE:
		wait_pwd = PWD_LP_END;
		break;
	case HX_LP_IDLE_RAWDATA:
	case HX_LP_IDLE_BPN_RAWDATA:
	case HX_LP_IDLE_NOISE:
		wait_pwd = PWD_LP_IDLE_END;
		break;

	default:
		I("No Change Mode and now type=%d", checktype);
		break;
	}
	I("NowType[%d] = %s, Expected=0x%04X",
	  checktype, g_himax_inspection_mode[checktype],
		 wait_pwd);
	do {
		D("start check_sorting_mode!");
		if (g_core_fp.fp_check_sorting_mode)
			g_core_fp.fp_check_sorting_mode(ts, tmp_data.byte);
		D("end check_sorting_mode!");
		if ((le32_to_cpu(tmp_data.dword) & 0xFFFF) == wait_pwd)
			return HX_INSP_OK;

		tmp_addr.dword = cpu_to_le32(fw_addr_chk_fw_status);
		g_core_fp.fp_register_read(ts, tmp_addr.byte, tmp_data.byte, 4);
		D("0x%08X : %08X", fw_addr_chk_fw_status,
		  le32_to_cpu(tmp_data.dword));

		tmp_addr.dword = cpu_to_le32(fw_addr_flag_reset_event);
		g_core_fp.fp_register_read(ts, tmp_addr.byte, tmp_data.byte, 4);
		D("0x%08X : %08X", fw_addr_flag_reset_event,
		  le32_to_cpu(tmp_data.dword));

		tmp_addr.dword = cpu_to_le32(fw_addr_fw_dbg_msg_addr);
		g_core_fp.fp_register_read(ts, tmp_addr.byte, tmp_data.byte, 4);
		D("0x%08X : %08X", fw_addr_fw_dbg_msg_addr,
		  le32_to_cpu(tmp_data.dword));

		D("Now retry %d times!", count);

		count++;
		usleep_range(50000, 50001);
	} while (count < 10);

	D("end");
	return HX_INSP_ESWITCHMODE;
}

void hx_self_test(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
			work_self_test.work);
	u32 checktype = ts->hid_req_cfg.self_test_type;
	s32 n_frame = 0;
	u32 ret_val = NO_ERR;
	int check_sort_sts = NO_ERR;
	int switch_mode_cnt = 0;

	check_sort_sts = hx_check_mode(ts, checktype);
	if (check_sort_sts < NO_ERR) {
		ret_val = HX_INSP_ESWITCHMODE;
		ts->hid_req_cfg.handshake_get = HID_SELF_TEST_ERROR;
		goto END;
	}

	if (check_sort_sts) {
		I("Need Change Mode, target=%s",
		  g_himax_inspection_mode[checktype]);
SWITCH_MODE:
		D("start sense off!");
		g_core_fp.fp_sense_off(ts, true);
		D("end sense off!");

		if (ts->ic_data->has_flash) {
			g_core_fp.fp_turn_on_mp_func(ts, 1);
			if (g_core_fp.fp_reload_disable)
				g_core_fp.fp_reload_disable(ts, 1);
		}
		hx_switch_mode_inspection(ts, checktype);

		switch (checktype) {
		case HX_WT_NOISE:
		case HX_ABS_NOISE:
			if (g_hx_inspt_setting_val[NFRAME] > 0)
				n_frame = g_hx_inspt_setting_val[NFRAME];
			else
				n_frame = NOISEFRAME;
			break;
		case HX_ACT_IDLE_RAWDATA:
		case HX_ACT_IDLE_NOISE:
		case HX_ACT_IDLE_BPN_RAWDATA:
			if (g_hx_inspt_setting_val[IDLE_NFRAME] > 0)
				n_frame = g_hx_inspt_setting_val[IDLE_NFRAME];
			else
				n_frame = NORMAL_IDLE_RAWDATA_NOISEFRAME;
			break;
		case HX_LP_RAWDATA:
		case HX_LP_BPN_RAWDATA:
			if (g_hx_inspt_setting_val[LP_RAW_NFRAME] > 0)
				n_frame = g_hx_inspt_setting_val[LP_RAW_NFRAME];
			else
				n_frame = LP_RAWDATAFRAME;
			break;
		case HX_LP_WT_NOISE:
		case HX_LP_ABS_NOISE:
			if (g_hx_inspt_setting_val[LP_NOISE_NFRAME] > 0)
				n_frame =
					g_hx_inspt_setting_val[LP_NOISE_NFRAME];
			else
				n_frame = LP_NOISEFRAME;
			break;
		case HX_LP_IDLE_RAWDATA:
		case HX_LP_IDLE_BPN_RAWDATA:
			if (g_hx_inspt_setting_val[LP_IDLE_RAW_NFRAME] > 0)
				n_frame =
				g_hx_inspt_setting_val[LP_IDLE_RAW_NFRAME];
			else
				n_frame = LP_IDLE_RAWDATAFRAME;
			break;
		case HX_LP_IDLE_NOISE:
			if (g_hx_inspt_setting_val[LP_IDLE_NOISE_NFRAME] > 0)
				n_frame =
				g_hx_inspt_setting_val[LP_IDLE_NOISE_NFRAME];
			else
				n_frame = LP_IDLE_NOISEFRAME;
			break;
		default:
			n_frame = OTHERSFRAME;
		}
		hx_set_N_frame(ts, n_frame, checktype);
		g_core_fp.fp_sense_on(ts, 1);
	}

	ret_val = hx_wait_sorting_mode(ts, checktype);
	if (ret_val) {
		if (ret_val == HX_INSP_ESWITCHMODE && switch_mode_cnt < 3) {
			switch_mode_cnt++;
			g_core_fp.fp_ic_reset(ts, false, false);
			goto SWITCH_MODE;
		}
		E("himax_wait_sorting_mode FAIL");
		ts->hid_req_cfg.handshake_get = HID_SELF_TEST_ERROR;
		goto END;
	}
	hx_switch_data_type(ts, checktype);

	ts->hid_req_cfg.handshake_get = HID_SELF_TEST_FINISH;
END:
	mutex_unlock(&ts->hid_ioctl_lock);
}

int hx_get_data(struct himax_ts_data *ts, u8 *data, s32 len)
{
	bool get_raw_rlst = false;

	get_raw_rlst = g_core_fp.fp_get_DSRAM_data(ts, data, false);

	if (get_raw_rlst)
		return HX_INSP_OK;
	else
		return HX_INSP_EGETRAW;
}
