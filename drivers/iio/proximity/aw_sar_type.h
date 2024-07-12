/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SAR_TYPE_H_
#define _SAR_TYPE_H_

#include "aw_sar_comm_interface.h"

typedef int (*aw_sar_chip_other_operation_t)(void *data);
typedef void (*aw_sar_chip_other_opera_free_t)(void *data);

enum aw_i2c_flags {
	AW_SAR_I2C_WR,
	AW_SAR_I2C_RD,
	AW_SAR_PACKAGE_RD,
};

enum sar_health_check {
	AW_SAR_HEALTHY = 0,
	AW_SAR_UNHEALTHY = 1,
};
typedef int (*aw_sar_bin_opera_t)(struct aw_bin *aw_bin, void *load_bin_para);
typedef int (*aw_sar_bin_load_fail_opera_t)(struct aw_bin *aw_bin, void *load_bin_para);

struct aw_sar_get_chip_info_t {
	void (*p_get_chip_info_node_fn)(void *data, char *buf, ssize_t *p_len);
};

struct aw_sar_load_bin_t {
	const unsigned char *bin_name;
	aw_sar_bin_opera_t bin_opera_func;
	aw_sar_bin_load_fail_opera_t bin_load_fail_opera;

	void (*p_get_prot_update_fw_node_fn)(void *data, char *buf, ssize_t *p_len);

	/* Perform different operations to update parameters */
	int (*p_update_fn)(void *data);
};

struct aw_sar_reg_data {
	unsigned char rw;
	unsigned short reg;
};

struct aw_sar_awrw_t {
	ssize_t (*p_set_awrw_node_fn)(void *data, const char *buf, size_t count);
	ssize_t (*p_get_awrw_node_fn)(void *data, char *buf);
};

struct aw_sar_reg_list_t {
	unsigned char reg_none_access;
	unsigned char reg_rd_access;
	unsigned char reg_wd_access;
	const struct aw_sar_reg_data *reg_perm;
	unsigned int reg_num;
};

typedef void (*aw_sar_update_work_t)(struct work_struct *work);
struct aw_sar_update_static_t {
	aw_sar_update_work_t update_work_func;
	unsigned int delay_ms;
};

typedef irqreturn_t (*aw_sar_irq_t)(int irq, void *data);
typedef unsigned int (*sar_rc_irqscr_t)(void *i2c);
/*
 * If the return value is 1, there is an initialization completion interrupt;
 * if the return value is 0, there is no
 */
typedef unsigned int (*aw_sar_is_init_over_irq)(unsigned int irq_status);
typedef void (*aw_sar_irq_spec_handler_t)(unsigned int irq_status, void *data);

struct aw_sar_check_chipid_t {
	/* Read chipid and check chipid, Must be implemented externally */
	int (*p_check_chipid_fn)(void *data);
};

struct aw_sar_irq_init_t {
	unsigned long flags;
	unsigned long irq_flags;
	irq_handler_t handler;
	irq_handler_t thread_fn;
	/* Interrupt processing parameters */
	sar_rc_irqscr_t rc_irq_fn;
	/* aw_sar_is_init_over_irq is_init_over_irq_fn; */
	aw_sar_irq_spec_handler_t irq_spec_handler_fn;

	/* Use a different initialization interrupt to initialize the operation */
	int (*p_irq_init_fn)(void *data);
	/* Release interrupt resource */
	int (*p_irq_deinit_fn)(void *data);
};

struct aw_sar_pm_t {
	unsigned int suspend_set_mode;
	unsigned int resume_set_mode;
	unsigned int shutdown_set_mode;
	/* system api */
	int (*p_suspend_fn)(void *data);
	int (*p_resume_fn)(void *data);
	int (*p_shutdown_fn)(void *data);
};

struct aw_sar_chip_mode_t {
	unsigned int init_mode;
	unsigned int active;
	unsigned int pre_init_mode;
};

struct aw_sar_regulator_config_t {
	/* Note that "_sar_num" after VCC name is defined by SAR C auto add */
	const unsigned char *vcc_name;
	int min_uV;
	int max_uV;
};

struct aw_channels_info {
	unsigned short used;
	unsigned int last_channel_info;
};

struct aw_sar_dts_info {
	unsigned int sar_num;
	unsigned int channel_use_flag;
	bool use_regulator_flag;
	bool use_inter_pull_up;
	bool use_pm;
	bool use_plug_cail_flag;
	bool monitor_esd_flag;
};

struct aw_sar_irq_init_comm_t {
	unsigned char host_irq_stat;
	void *data;
	unsigned char dev_id[30];
};

struct aw_sar_load_bin_comm_t {
	unsigned char bin_name[30];
	unsigned int bin_data_ver;
	aw_sar_bin_opera_t bin_opera_func;
	aw_sar_bin_load_fail_opera_t bin_load_fail_opera_func;
};

struct aw_awrw_info {
	unsigned char rw_flag;
	unsigned char addr_len;
	unsigned char data_len;
	unsigned char reg_num;
	unsigned int i2c_tranfar_data_len;
	unsigned char *p_i2c_tranfar_data;
};

typedef void (*sar_enable_clock_t)(void *i2c);
typedef void (*sar_operation_irq_t)(int to_irq);
typedef void (*sar_mode_update_t)(void *i2c);

struct aw_sar_mode_switch_ops {
	sar_enable_clock_t enable_clock;
	sar_rc_irqscr_t rc_irqscr;
	sar_mode_update_t mode_update;
};

struct aw_sar_chip_mode {
	unsigned char curr_mode;
	unsigned char last_mode;
};

struct aw_sar_mode_set_t {
	unsigned char chip_id;
	struct aw_sar_chip_mode chip_mode;
	struct aw_sar_mode_switch_ops mode_switch_ops;
};

struct aw_sar_mode_t {
	const struct aw_sar_mode_set_t *mode_set_arr;
	unsigned char mode_set_arr_len;
	ssize_t (*p_set_mode_node_fn)(void *data, unsigned char curr_mode);
	ssize_t (*p_get_mode_node_fn)(void *data, char *buf);
};

struct aw_sar_init_over_irq_t {
	short wait_times;
	unsigned char daley_step;
	unsigned int reg_irqsrc;
	unsigned int irq_offset_bit;
	unsigned int irq_mask;
	unsigned int irq_flag;
	/*
	 * Perform different verification initialization
	 * to complete the interrupt operation
	 */
	int (*p_check_init_over_irq_fn)(void *data);
	/*
	 * When initialization fails, get the corresponding error type and
	 * apply it to the chip with flash
	 */
	int (*p_get_err_type_fn)(void *data);
};

struct aw_sar_soft_rst_t {
	unsigned short reg_rst;
	unsigned int reg_rst_val;
	unsigned int delay_ms;
	/* Perform different soft reset operations */
	int (*p_soft_reset_fn)(void *data);
};

struct aw_sar_aot_t {
	unsigned int aot_reg;
	unsigned int aot_mask;
	unsigned int aot_flag;
	ssize_t (*p_set_aot_node_fn)(void *data);
};

struct aw_sar_diff_t {
	unsigned short diff0_reg;
	unsigned short diff_step;
	/* Data format:S21.10, Floating point types generally need to be removed */
	unsigned int rm_float;
	ssize_t (*p_get_diff_node_fn)(void *data, char *buf);
};

struct aw_sar_offset_t {
	ssize_t (*p_get_offset_node_fn)(void *data, char *buf);
};

struct aw_sar_pinctrl {
	struct pinctrl *pinctrl;
	struct pinctrl_state *default_sta;
	struct pinctrl_state *int_out_high;
	struct pinctrl_state *int_out_low;
};

/* update reg node */
struct aw_sar_para_load_t {
	const unsigned int *reg_arr;
	unsigned int reg_arr_len;
};

struct aw_sar_platform_config {
	/* The chip needs to parse more DTS contents for addition */
	int (*p_add_parse_dts_fn)(void *data);

	const struct aw_sar_regulator_config_t *p_regulator_config;

	/* The chip needs to add more nodes */
	int (*p_add_node_create_fn)(void *data);
	/* Release the added node */
	int (*p_add_node_free_fn)(void *data);

	/* Use a different initialization interrupt to initialize the operation */
	int (*p_input_init_fn)(void *data);
	/* Release input resource */
	int (*p_input_deinit_fn)(void *data);

	/* The parameters passed in are required for interrupt initialization */
	const struct aw_sar_irq_init_t *p_irq_init;

	/* The chip is set to different modes in different power management interfaces */
	const struct aw_sar_pm_t *p_pm_chip_mode;
};

struct aw_sar_power_on_prox_detection_t {
	/* en_flag is true enable */
	void (*p_power_on_prox_detection_en_fn)(void *data, unsigned char en_flag);
	unsigned int irq_en_cali_bit;
	unsigned char power_on_prox_en_flag;
};

/**
 * struct aw_sar_chip_config -
 * @ch_num_max:	Number of channels of the chip
 * @p_platform_config:	Chip related platform content configuration
 * @p_check_chipid:	Parameters required for verification of chipid
 * @p_soft_rst:	Parameters required for soft reset
 * @p_init_over_irq:	Verify the parameters required to initialize a complete interrupt
 * @p_reg_bin:	Parameters required for load register bin file
 * @p_chip_mode:	The mode set before and after the initialization of the chip
 * @p_reg_list:	Register permission table
 * @p_reg_arr:	Default register table
 * @p_aot:	Parameters required for set Auto-Offset-Tuning(aot)
 * @p_diff:	Parameters required for get chip diff val
 * @p_offset:	Parameters required for get chip offset val
 * @p_mode:	Set the parameters of different working modes of the chip
 * @p_get_chip_info:	Obtain the necessary information of the chip
 * @p_aw_sar_awrw:	Continuous read/write register interface
 * @p_other_operation:	Other operations during initialization, Add according to different usage
 * @p_other_opera_free:	If requested by resources, please release
 */
struct aw_sar_chip_config {
	unsigned char ch_num_max;
	const struct aw_sar_platform_config *p_platform_config;
	const struct aw_sar_check_chipid_t *p_check_chipid;
	const struct aw_sar_soft_rst_t *p_soft_rst;
	const struct aw_sar_init_over_irq_t *p_init_over_irq;
	const struct aw_sar_load_bin_t *p_reg_bin;
	const struct aw_sar_chip_mode_t *p_chip_mode;
	const struct aw_sar_reg_list_t *p_reg_list;
	const struct aw_sar_para_load_t *p_reg_arr;
	const struct aw_sar_aot_t *p_aot;
	const struct aw_sar_diff_t *p_diff;
	const struct aw_sar_offset_t *p_offset;
	const struct aw_sar_mode_t *p_mode;
	const struct aw_sar_get_chip_info_t *p_get_chip_info;
	const struct aw_sar_awrw_t *p_aw_sar_awrw;
	aw_sar_chip_other_operation_t p_other_operation;
	aw_sar_chip_other_opera_free_t p_other_opera_free;
	const struct aw_sar_power_on_prox_detection_t *power_on_prox_detection;
};

struct aw_sar {
	struct i2c_client *i2c;
	struct device *dev;
	struct regulator *vcc;
	struct delayed_work update_work;
	/* Set pin pull-up mode */
	struct aw_sar_pinctrl pinctrl;
	/* eds work */
	struct delayed_work monitor_work;
	struct workqueue_struct *monitor_wq;
	struct iio_dev *aw_iio_dev;

	unsigned char chip_type;
	unsigned char chip_name[20];

	bool power_enable;
	bool fw_fail_flag;
	unsigned char last_mode;

	/* handler_anomalies */
	unsigned char fault_flag;
	unsigned char driver_code_initover_flag;
	/* handler_anomalies */

	unsigned char ret_val;
	unsigned char curr_use_driver_type;
	int prot_update_state;

	unsigned char aot_irq_num;
	unsigned char enter_irq_handle_num;
	unsigned char exit_power_on_prox_detection;

	struct work_struct ps_notify_work;
	struct notifier_block ps_notif;
	bool ps_is_present;

	/* Parameters related to platform logic */
	struct aw_sar_dts_info dts_info;
	struct aw_sar_load_bin_comm_t load_bin;
	struct aw_sar_irq_init_comm_t irq_init;
	struct aw_awrw_info awrw_info;
	struct aw_channels_info *channels_arr;

	/* Private arguments required for public functions */
	const struct aw_sar_chip_config *p_sar_para;
	/* Private arguments required for private functions */
	void *priv_data;
};

/* Determine whether the chip exists by verifying chipid */
typedef int (*aw_sar_who_am_i_t)(void *data);
typedef int (*aw_sar_chip_init_t)(struct aw_sar *p_sar);
typedef void (*aw_sar_chip_deinit_t)(struct aw_sar *p_sar);

struct aw_sar_driver_type {
	unsigned char driver_type;
	aw_sar_who_am_i_t p_who_am_i;
	aw_sar_chip_init_t p_chip_init;
	aw_sar_chip_deinit_t p_chip_deinit;
};

#endif
