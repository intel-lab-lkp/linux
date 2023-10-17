/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __HX_CORE_H__
#define __HX_CORE_H__
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/hid.h>
#include <linux/sizes.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/types.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/proc_fs.h>
#include <linux/version.h>
#include <linux/firmware.h>
#include <linux/acpi.h>
#include <linux/stddef.h>
#include "hx_hid.h"

#define HIMAX_DRIVER_VER "1.0.0"

#define HIMAX_BUS_RETRY_TIMES 3
#define BUS_RW_MAX_LEN 0x20006
#define BUS_R_HLEN 3
#define BUS_R_DLEN ((BUS_RW_MAX_LEN - BUS_R_HLEN) - ((BUS_RW_MAX_LEN - BUS_R_HLEN) % 4))
#define BUS_W_HLEN 2
#define BUS_W_DLEN ((BUS_RW_MAX_LEN - BUS_W_HLEN) - ((BUS_RW_MAX_LEN - BUS_W_HLEN) % 4))
#define FIX_HX_INT_IS_EDGE	(false)

#define HX_DELAY_BOOT_UPDATE			(2000)
#define HID_REG_SZ_MAX					(1 + 4 + 1 + 4 + 256)

enum fix_touch_info {
	FIX_HX_RX_NUM = 48,
	FIX_HX_TX_NUM = 32,
	FIX_HX_BT_NUM = 0,
	FIX_HX_MAX_PT = 10,
	FIX_HX_STYLUS_FUNC = 1,
	FIX_HX_STYLUS_ID_V2 = 0,
	FIX_HX_STYLUS_RATIO = 1,
	HX_STACK_ORG_LEN = 128
};

#define himax_dev_name "hx_spi_hid_tp"

#if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#endif

/* log macro */
#define I(fmt, arg...) pr_info("[HXTP][%s]: " fmt "\n", __func__, ##arg)
#define W(fmt, arg...) pr_warn("[HXTP][WARNING][%s]: " fmt "\n", __func__, ##arg)
#define E(fmt, arg...) pr_err("[HXTP][ERROR][%s]: " fmt "\n", __func__, ##arg)
#define D(fmt, arg...) pr_debug("[HXTP][DEBUG][%s]: " fmt "\n", __func__, ##arg)

struct hx_hid_desc_t {
	u16 desc_length;
	u16 bcd_version;
	u16 report_desc_length;
	u16 max_input_length;
	u16 max_output_length;
	u16 max_fragment_length;
	u16 vendor_id;
	u16 product_id;
	u16 version_id;
	u16 flags;
	u32 reserved;
} __packed;

struct hx_hid_rd_data_t {
	u8 *rd_data;
	u32 rd_length;
};

#define proc_op(name) proc_##name
#define proc_opl(name) proc_##name
#define proc_ops_name proc_ops
#define time_var timespec64
#define time_var_fine tv_nsec
#define time_var_fine_unit (1000 * 1000)
#define time_func ktime_get_real_ts64
#define owner_line

#define HX_TP_BIN_CHECKSUM_SW		1
#define HX_TP_BIN_CHECKSUM_HW		2
#define HX_TP_BIN_CHECKSUM_CRC		3

#define SHIFTBITS 5
#define RAW_DATA_HEADER_LENGTH 6

#define FW_SIZE_32k		32768
#define FW_SIZE_60k		61440
#define FW_SIZE_64k		65536
#define FW_SIZE_124k	126976
#define FW_SIZE_128k	131072
#define FW_SIZE_255k    261120

#define HX83102D_ID		"HX83102D"
#define HX83102J_ID		"HX83102J"
#define HX83121A_ID		"HX83121A"

/* origin is 20/50 */
#define RST_LOW_PERIOD_S 5000
#define RST_LOW_PERIOD_E 5100
#define RST_HIGH_PERIOD_ZF_S 5000
#define RST_HIGH_PERIOD_ZF_E 5100
#define RST_HIGH_PERIOD_S 50000
#define RST_HIGH_PERIOD_E 50100

enum cell_type {
	CHIP_IS_ON_CELL,
	CHIP_IS_IN_CELL
};

#define HX_FULL_STACK_RAWDATA_SIZE \
	(HX_STACK_ORG_LEN +\
	(2 + FIX_HX_RX_NUM * FIX_HX_TX_NUM + FIX_HX_TX_NUM + FIX_HX_RX_NUM)\
	* 2)

struct himax_ic_data {
	int vendor_fw_ver;
	int vendor_config_ver;
	int vendor_touch_cfg_ver;
	int vendor_display_cfg_ver;
	int vendor_cid_maj_ver;
	int vendor_cid_min_ver;
	int vendor_panel_ver;
	int vendor_sensor_id;
	int ic_adc_num;
	u8 vendor_cus_info[12];
	u8 vendor_proj_info[12];
	u32 flash_size;
	u32 HX_RX_NUM;
	u32 HX_TX_NUM;
	u32 HX_BT_NUM;
	u32 HX_X_RES;
	u32 HX_Y_RES;
	u32 HX_MAX_PT;
	u8 HX_INT_IS_EDGE;
	u8 HX_STYLUS_FUNC;
	u8 HX_STYLUS_ID_V2;
	u8 HX_STYLUS_RATIO;
	u32 icid;
	bool enc16bits;
	bool has_flash;
};

enum HX_TS_PATH {
	HX_REPORT_COORD = 1,
	HX_REPORT_COORD_RAWDATA,
};

enum HX_TS_STATUS {
	HX_TS_GET_DATA_FAIL = -4,
	HX_EXCP_EVENT,
	HX_CHKSUM_FAIL,
	HX_PATH_FAIL,
	HX_TS_NORMAL_END = 0,
	HX_EXCP_REC_OK,
	HX_READY_SERVE,
	HX_REPORT_DATA,
	HX_EXCP_WARNING,
	HX_IC_RUNNING,
	HX_ZERO_EVENT_COUNT,
	HX_RST_OK,
};

enum HX_ERROR_CODE {
	NO_ERR = 0,
	READY_TO_SERVE = 1,
	WORK_OUT = 2,
	HX_EMBEDDED_FW = 3,
	BUS_FAIL = -1,
	HX_INIT_FAIL = -1,
	MEM_ALLOC_FAIL = -2,
	CHECKSUM_FAIL = -3,
	GESTURE_DETECT_FAIL = -4,
	INPUT_REGISTER_FAIL = -5,
	FW_NOT_READY = -6,
	LENGTH_FAIL = -7,
	OPEN_FILE_FAIL = -8,
	PROBE_FAIL = -9,
	ERR_WORK_OUT = -10,
	ERR_STS_WRONG = -11,
	ERR_TEST_FAIL = -12,
	HW_CRC_FAIL = 1
};

struct himax_platform_data {
	struct himax_ts_data *ts;
	u16 pid;
	bool power_off_3v3;
	u8 cable_config[2];
	int gpio_irq;
	int of_irq;
	int gpio_reset;
	int ic_det_delay;
	int ic_resume_delay;
	int panel_id;
	bool is_zf;
	struct regulator *vccd_supply;
	struct regulator *vcca_supply;
};

struct hx_hid_fw_unit_t {
	u8 cmd;
	u16 bin_start_offset;
	u16 unit_sz;
} __packed;

struct hx_bin_desc_t {
	u16 passwd;
	u16 cid;
	u8 panel_ver;
	u16 fw_ver;
	u8 ic_sign;
	char customer[12];
	char project[12];
	char fw_major[12];
	char fw_minor[12];
	char date[12];
	char ic_sign_2[12];
} __packed;

struct hx_hid_info_t {
	struct hx_hid_fw_unit_t main_mapping[9];
	struct hx_hid_fw_unit_t bl_mapping;
	struct hx_bin_desc_t fw_bin_desc;
	u16 vid;
	u16 pid;
	u8 cfg_info[32];
	u8 cfg_version;
	u8 disp_version;
	u8 rx;
	u8 tx;
	u16 yres;
	u16 xres;
	u8 pt_num;
	u8 mkey_num;
	u8 debug_info[78];
} __packed;

union hx_dword_data_t {
	u32 dword;
	u8 byte[4];
};

enum hid_reg_action {
	REG_READ = 0,
	REG_WRITE = 1
};

enum hid_reg_types {
	REG_TYPE_EXT_AHB,
	REG_TYPE_EXT_SRAM,
	REG_TYPE_EXT_TYPE = 0xFFFFFFFF
};

struct hx_hid_req_cfg_t {
	u32 processing_id;
	u32 data_type;
	u32 self_test_type;
	u32 handshake_set;
	u32 handshake_get;
	struct firmware *fw;
	u32 current_size;
	// HID REG READ/WRITE format:
	// STANDARD TYPE
	// [ID:1][READ/WRITE:1][REG_ADDR:4][REG_DATA:4] : 10 bytes
	//  0     1             2~5         6~9
	// EXT TYPE
	// [ID:1][READ/WRITE:1][0xFFFFFFFF][REG_TYPE:1][REG_ADDR:1|4][REG_DATA:1~256]
	//  0	  1             2~5         6           7|7~10        8~263|11~266
	union hx_dword_data_t reg_addr;
	u32 reg_addr_sz;
	u8 reg_data[HID_REG_SZ_MAX - 1 - 4];
	u32 reg_data_sz;
	u32 input_RD_de;
};

struct himax_ts_data {
	bool initialized;
	bool probe_finish;
	bool suspended;
	s32 notouch_frame;
	s32 ic_notouch_frame;
	atomic_t suspend_mode;
	u8 x_channel;
	u8 y_channel;
	char chip_name[30];
	u8 chip_cell_type;
	u32 chip_max_dsram_size;
	u32 ic_checksum_type;
	bool ic_boot_done;
	u32 probe_fail_flag;
	u8 *xfer_data;
	struct himax_ic_data *ic_data;

	int touch_all_size;
	int touch_info_size;

	u8 *hx_rawdata_buf;
#if defined(HX_HEATMAP_EN)
	u8 *hx_heatmap_buf;
	u32 heatmap_data_size;
#endif
	bool boot_upgrade_flag;
	const struct firmware *hxfw;
	bool has_alg_overlay;
	u8 *ovl_idx;
	bool zf_update_flag;
	u8 *zf_update_cfg_buffer;
#if !defined(__HIMAX_MOD__)
	struct time_var deferred_start;
	unsigned int ic_det_delay;
#endif

	u8 n_finger_support;
	u8 irq_enabled;

	u32 debug_log_level;

	s32 rst_gpio;
	s32 use_irq;
	s32 (*power)(s32 on);

	struct device *dev;
	struct workqueue_struct *himax_wq;
	struct work_struct work;

	struct hrtimer timer;
	struct i2c_client *client;
	struct himax_platform_data *pdata;
	/* mutex lock for reg access */
	struct mutex reg_lock;
	/* mutex lock for read/write action */
	struct mutex rw_lock;
	/* mutex lock for hid ioctl action */
	struct mutex hid_ioctl_lock;
	atomic_t irq_state;
	/* spin lock for irq */
	spinlock_t irq_lock;

/******* SPI-start *******/
	struct spi_device	*spi;
	s32 hx_irq;
	u8 *xfer_buff;
/******* SPI-end *******/

	struct hid_device *hid;
	struct hx_hid_desc_t hid_desc;
	struct hx_hid_rd_data_t hid_rd_data;
	struct hx_hid_info_t hid_info;
	struct hx_hid_req_cfg_t hid_req_cfg;
	struct hx_bin_desc_t fw_bin_desc;
	bool hid_probe;
	bool resume_success;

	s32 in_self_test;
	s32 suspend_resume_done;
	s32 bus_speed;

	s32 excp_reset_active;
	s32 excp_eb_event_flag;
	s32 excp_ec_event_flag;
	s32 excp_ed_event_flag;
	s32 excp_zero_event_count;

#if defined(CONFIG_FB)
	struct notifier_block fb_notif;
	struct workqueue_struct *himax_att_wq;
	struct delayed_work work_att;
#endif

	struct notifier_block power_notif;
	struct workqueue_struct *himax_pwr_wq;
	struct delayed_work work_pwr;

	struct workqueue_struct *himax_boot_upgrade_wq;
	struct delayed_work work_boot_upgrade;

	struct workqueue_struct *himax_hid_debug_wq;
	struct delayed_work work_self_test;
	struct delayed_work work_hid_update;
	u8 usb_connected;
	u8 *cable_config;
	u8 latest_power_status;

	struct workqueue_struct *himax_resume_delayed_work_wq;
	struct delayed_work work_resume_delayed_work;

	u8 slave_write_reg;
	u8 slave_read_reg;
	bool acc_slave_reg;
	bool select_slave_reg;
	struct list_head list;
};

struct rd_feature_unit_t {
	u8 id_tag;
	u8 id;
	u8 usage_tag;
	u8 usage;
	u8 report_cnt_tag;
	u16 report_cnt;
	u8 feature_tag[2];
} __packed;

union host_ext_rd_t {
	struct __packed rd_struct_t {
		u8 header[14];
		struct rd_feature_unit_t cfg;// ID_CFG
		struct rd_feature_unit_t reg_rw;// ID_REG_RW
		struct rd_feature_unit_t monitor_sel;// ID_TOUCH_MONITOR_SEL
		struct rd_feature_unit_t monitor;// ID_TOUCH_MONITOR
		// rd_feature_unit_t monitor_partial;// ID_TOUCH_MONITOR_PARTIAL
		struct rd_feature_unit_t fw_update;// ID_FW_UPDATE
		struct rd_feature_unit_t fw_update_handshaking;// ID_FW_UPDATE_HANDSHAKING
		struct rd_feature_unit_t self_test;// ID_SELF_TEST
		struct rd_feature_unit_t input_rd_en;// ID_INPUT_RD_DE
		u8 end_collection;
	} rd_struct;
	u8 host_report_descriptor[sizeof(struct rd_struct_t)];
};

union heatmap_rd_t {
	struct __packed heatmap_struct_t {
		u8 header[17];
		u8 heatmap_info_desc[29];
		u8 heatmap_data_hdr[9];
		u8 heatmap_data_cnt_tag;
		u16 heatmap_data_cnt;
		u8 heatmap_input_desc[2];
		u8 end_collection;
	} heatmap_struct;
	u8 host_report_descriptor[sizeof(struct heatmap_struct_t)];
};

/* used for 102e/p zero flash */
/*#define HW_ED_EXCP_EVENT*/

/* FW Auto upgrade case, you need to setup the fix_touch_info of module
 */
extern char *g_fw_boot_upgrade_name;
#define BOOT_UPGRADE_FWNAME "himax_i2chid"
extern char *g_fw_mp_upgrade_name;
#define MPAP_FWNAME "himax_mpfw.bin"

extern struct himax_ts_data *g_himax_ts;
extern struct himax_core_fp g_core_fp;

#define HID_REPORT_HDR_SZ (2)
#if defined(HX_HEATMAP_EN)
#define HEAT_MAP_HEADER_SZ (3)
#define HEAT_MAP_HID_HDR_SZ (12)
#define HEAT_MAP_DATA_HDR_SZ (8)
#define HEAT_MAP_INFO_SZ (HEAT_MAP_HID_HDR_SZ + HEAT_MAP_DATA_HDR_SZ)
#endif

#if defined(CONFIG_OF)
int himax_parse_dt(struct device_node *dt, struct himax_platform_data *pdata);
#endif
#if defined(CONFIG_ACPI)
int himax_parse_acpi(struct device *dev, struct himax_platform_data *pdata);
#endif
void himax_ts_work(struct himax_ts_data *ts);
enum hrtimer_restart himax_ts_timer_func(struct hrtimer *timer);
int himax_resume(struct device *dev);
int himax_suspend(struct device *dev);

int himax_spi_drv_init(struct himax_ts_data *ts);
void himax_spi_drv_exit(void);
int himax_chip_init(struct himax_ts_data *ts);
int himax_report_data_init(struct himax_ts_data *ts);
void himax_cable_detect_func(struct himax_ts_data *ts, bool force_renew);

#endif
