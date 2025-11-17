/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _AW91XXX_H_
#define _AW91XXX_H_

#define AWINIC_DEBUG		1

#ifdef AWINIC_DEBUG
#define AW_DEBUG(fmt, args...)	pr_info(fmt, ##args)
#else
#define AW_DEBUG(fmt, ...)

#endif

#define MAX_I2C_BUFFER_SIZE 65536

#define AW91XXX_ID 0x23
#define AW91XXX_KEY_PORT_MAX (0x10) /* 16 */
#define AW91XXX_INT_MASK (0xFFFF)

enum AW91XXX_FADE_TIME {
	AW91XXX_FADE_TIME_0000MS = 0x00,
	AW91XXX_FADE_TIME_0315MS = 0X01,
	AW91XXX_FADE_TIME_0630MS = 0x02,
	AW91XXX_FADE_TIME_1260MS = 0x03,
	AW91XXX_FADE_TIME_2520MS = 0x04,
	AW91XXX_FADE_TIME_5040MS = 0x05
};

enum aw91xxx_gpio_dir {
	AW91XXX_GPIO_INPUT = 0,
	AW91XXX_GPIO_OUTPUT = 1,
};

enum aw91xxx_gpio_val {
	AW91XXX_GPIO_HIGH = 1,
	AW91XXX_GPIO_LOW = 0,
};

enum aw91xxx_gpio_output_mode {
	AW91XXX_OPEN_DRAIN_OUTPUT = 0,
	AW91XXX_PUSH_PULL_OUTPUT = 1,
};

struct aw91xxx_singel_gpio {
	unsigned int gpio_idx;
	enum aw91xxx_gpio_dir gpio_direction;
	enum aw91xxx_gpio_val state;
	struct aw91xxx *priv;
};

struct aw91xxx_gpio {
	unsigned int gpio_mask;
	unsigned int gpio_num;
	enum aw91xxx_gpio_output_mode output_mode;
	struct aw91xxx_singel_gpio *single_gpio_data;
};

typedef struct {
	char name[10];
	int key_code;
	int key_val;
} KEY_STATE;

unsigned int aw91xxx_separate_key_data[AW91XXX_KEY_PORT_MAX] = {
/*      0    1    2    3 */
	1,   2,   3,   4,
	5,   6,   7,   8,
	9,   10,  11,  12,
	13,  14,  15,  16
};

struct aw91xxx_key {
	unsigned int key_mask;
	unsigned int input_port_nums;
	unsigned int output_port_nums;
	unsigned int input_port_mask;
	unsigned int output_port_mask;
	unsigned int new_input_state;
	unsigned int old_input_state;
	unsigned int *new_output_state;
	unsigned int *old_output_state;
	unsigned int *def_output_state;
	bool wake_up_enable;
	struct input_dev *input;

	unsigned int debounce_delay;
	struct delayed_work int_work;
	struct hrtimer key_timer;
	struct work_struct key_work;
	KEY_STATE *keymap;
	int keymap_len;
	struct aw91xxx *priv;
};

struct aw91xxx {
	struct i2c_client *i2c;
	struct device *dev;
	struct led_classdev cdev;
	struct work_struct brightness_work;
	struct delayed_work int_work;

	int reset_gpio;
	int irq_gpio;
	int irq_num;

	unsigned char chipid;
	unsigned char vendor_id;
	unsigned char blink;

	int imax;
	int rise_time;
	int on_time;
	int fall_time;
	int off_time;

	bool led_feature_enable;
	bool gpio_feature_enable;
	bool matrix_key_enable;
	bool single_key_enable;
	bool screen_state;

	struct aw91xxx_gpio *gpio_data;
	struct aw91xxx_key *key_data;
};


#endif

