/*
 *  stk3x1x.c - Linux kernel modules for sensortek stk301x, stk321x and stk331x
 *  proximity/ambient light sensor
 *
 *  Copyright (C) 2012~2013 Lex Hsieh / sensortek <lex_hsieh@sensortek.com.tw>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include   <linux/fs.h>
#include  <asm/uaccess.h>
#include "../init-input.h"
#include "stk3x1x.h"

#include <linux/regulator/consumer.h>
#include <asm/uaccess.h>

#include <linux/pinctrl/consumer.h>

#if IS_ENABLED(CONFIG_PM)
#include <linux/pm.h>
#endif

#define DRIVER_VERSION  "3.5.2"

#define REPORT_KEY 0
#define KEY_PROX_NEAR KEY_SLEEP
#define KEY_PROX_FAR  KEY_WAKEUP

/* Driver Settings */
#define CONFIG_STK_PS_ALS_USE_CHANGE_THRESHOLD
#ifdef CONFIG_STK_PS_ALS_USE_CHANGE_THRESHOLD
#define STK_ALS_CHANGE_THD	       0	/* The threshold to trigger ALS interrupt, unit: lux */
#endif

#define STK_INT_PS_MODE			1	/* 1, 2, or 3	*/
#define STK_POLL_PS
/* ALS interrupt is valid only when STK_INT_PS_MODE = 1	or 4*/
#define STK_POLL_ALS
/* #define STK_TUNE0 */
/*#define STK_DEBUG_PRINTF
#define STK_ALS_FIR
#define STK_IRS
*/
#define STK_CHK_REG
#define STK_STATE_REG 			       0x00
#define STK_PSCTRL_REG 		       0x01
#define STK_ALSCTRL_REG 		       0x02
#define STK_LEDCTRL_REG 		       0x03
#define STK_INT_REG 			       0x04
#define STK_WAIT_REG 			       0x05
#define STK_THDH1_PS_REG 		       0x06
#define STK_THDH2_PS_REG 		       0x07
#define STK_THDL1_PS_REG 		       0x08
#define STK_THDL2_PS_REG 		       0x09
#define STK_THDH1_ALS_REG 	       0x0A
#define STK_THDH2_ALS_REG 	       0x0B
#define STK_THDL1_ALS_REG 	       0x0C
#define STK_THDL2_ALS_REG 	       0x0D
#define STK_FLAG_REG 			       0x10
#define STK_DATA1_PS_REG	 	       0x11
#define STK_DATA2_PS_REG 		       0x12
#define STK_DATA1_ALS_REG 	       0x13
#define STK_DATA2_ALS_REG 	       0x14
#define STK_DATA1_OFFSET_REG 	       0x15
#define STK_DATA2_OFFSET_REG 	       0x16
#define STK_DATA1_IR_REG 		       0x17
#define STK_DATA2_IR_REG 		       0x18
#define STK_PDT_ID_REG 		       0x3E
#define STK_RSRVD_REG 		       0x3F
#define STK_SW_RESET_REG		       0x80


/* Define state reg */
#define STK_STATE_EN_IRS_SHIFT  	7
#define STK_STATE_EN_AK_SHIFT  	        6
#define STK_STATE_EN_ASO_SHIFT  	5
#define STK_STATE_EN_IRO_SHIFT  	4
#define STK_STATE_EN_WAIT_SHIFT  	2
#define STK_STATE_EN_ALS_SHIFT  	1
#define STK_STATE_EN_PS_SHIFT  	         0

#define STK_STATE_EN_IRS_MASK	      0x80
#define STK_STATE_EN_AK_MASK	      0x40
#define STK_STATE_EN_ASO_MASK      0x20
#define STK_STATE_EN_IRO_MASK	       0x10
#define STK_STATE_EN_WAIT_MASK    0x04
#define STK_STATE_EN_ALS_MASK	       0x02
#define STK_STATE_EN_PS_MASK	       0x01

/* Define PS ctrl reg */
#define STK_PS_PRS_SHIFT  		        6
#define STK_PS_GAIN_SHIFT  		        4
#define STK_PS_IT_SHIFT  			0

#define STK_PS_PRS_MASK			0xC0
#define STK_PS_GAIN_MASK			0x30
#define STK_PS_IT_MASK			0x0F

/* Define ALS ctrl reg */
#define STK_ALS_PRS_SHIFT  		        6
#define STK_ALS_GAIN_SHIFT  		4
#define STK_ALS_IT_SHIFT  			0

#define STK_ALS_PRS_MASK		        0xC0
#define STK_ALS_GAIN_MASK		0x30
#define STK_ALS_IT_MASK			0x0F

/* Define LED ctrl reg */
#define STK_LED_IRDR_SHIFT  		6
#define STK_LED_DT_SHIFT  		         0

#define STK_LED_IRDR_MASK		        0xC0
#define STK_LED_DT_MASK			0x3F

/* Define interrupt reg */
#define STK_INT_CTRL_SHIFT  		 7
#define STK_INT_OUI_SHIFT  		         4
#define STK_INT_ALS_SHIFT  		         3
#define STK_INT_PS_SHIFT  			 0

#define STK_INT_CTRL_MASK		         0x80
#define STK_INT_OUI_MASK			0x10
#define STK_INT_ALS_MASK			0x08
#define STK_INT_PS_MASK			0x07

#define STK_INT_ALS				0x08

/* Define flag reg */
#define STK_FLG_ALSDR_SHIFT  		7
#define STK_FLG_PSDR_SHIFT  		6
#define STK_FLG_ALSINT_SHIFT  		5
#define STK_FLG_PSINT_SHIFT  		4
#define STK_FLG_OUI_SHIFT  		         2
#define STK_FLG_IR_RDY_SHIFT  		1
#define STK_FLG_NF_SHIFT  		         0

#define STK_FLG_ALSDR_MASK		0x80
#define STK_FLG_PSDR_MASK		0x40
#define STK_FLG_ALSINT_MASK		0x20
#define STK_FLG_PSINT_MASK		0x10
#define STK_FLG_OUI_MASK			0x04
#define STK_FLG_IR_RDY_MASK		0x02
#define STK_FLG_NF_MASK			0x01

/* misc define */
#define MIN_ALS_POLL_DELAY_NS	20000000

#define STK2213_PID			        0x23
#define STK3010_PID			        0x33
#define STK3311_9_BLK_PID		        0x11
#define STK3311_8_PID			        0x12
#define STK3210_STK3310_PID	        0x13
#define STK3311_9_PID			        0x15

#define STK2213C_PID				0x24
#define STK3210C_PID				0x18
#define STK3311_9C_PID			0x19
#define STK3311_8C_PID			0x1A
#define STK3310C_PID			0x1B

#define STK3310SA_PID		0x17
#define STK3311SA_PID		0x1E
#define STK3311WV_PID		0x1D
#define STK_ERR_PID			0x0


#ifdef STK_TUNE0
#define STK_MAX_MIN_DIFF	                200
#define STK_LT_N_CT	                         100
#define STK_HT_N_CT	                         150
#endif	/* #ifdef STK_TUNE0 */

#define STK_IRC_MAX_ALS_CODE		20000
#define STK_IRC_MIN_ALS_CODE		25
#define STK_IRC_MIN_IR_CODE		50
#define STK_IRC_ALS_DENOMI		2
#define STK_IRC_ALS_NUMERA		5
#define STK_IRC_ALS_CORREC		748

#define DEVICE_NAME		"stk3x1x"
#define ALS_NAME        "stk3x1x_ls"
#define PS_NAME         "stk3x1x_ps"

/* report 1-5 data to android */
/*
#define NEED_REPORT_MUTIL_LEVEL_DATA 1
*/
#ifdef NEED_REPORT_MUTIL_LEVEL_DATA
#define PS_DISTANCE_0 1700
#define PS_DISTANCE_1 1360
#define PS_DISTANCE_2 1020
#define PS_DISTANCE_3 700
#define PS_DISTANCE_4 350
#define PS_DISTANCE_5 0
#endif

static int sProbeSuccess;

static struct stk3x1x_platform_data stk3x1x_pfdata = {
		.state_reg         = 0x0,        /* disable all */
		.psctrl_reg         = 0x71,     /* ps_persistance=4, ps_gain=64X, PS_IT=0.391ms */
		.alsctrl_reg        = 0x38,     /* als_persistance=1, als_gain=64X, ALS_IT=50ms */
		.ledctrl_reg       = 0xFF,     /* 100mA IRDR, 64/64 LED duty */
		.wait_reg          = 0x07,      /* 50 ms */
		.ps_thd_h          = 1700,
		.ps_thd_l           = 1500,
		.int_pin             = 0,            /* sprd_3rdparty_gpio_pls_irq*/
		.transmittance = 500,
};

#ifdef STK_ALS_FIR
#define STK_FIR_LEN	8
#define MAX_FIR_LEN    32
struct data_filter {
		u16 raw[MAX_FIR_LEN];
		int sum;
		int number;
		int idx;
};
#endif

struct stk3x1x_data {
	struct i2c_client *client;
#if (!defined(STK_POLL_PS) || !defined(STK_POLL_ALS))
		int32_t irq;
		struct work_struct stk_work;
		struct workqueue_struct *stk_wq;
#endif
	uint16_t ir_code;
	uint16_t als_correct_factor;
	uint8_t alsctrl_reg;
	uint8_t psctrl_reg;
	uint8_t ledctrl_reg;
	uint8_t state_reg;
	int		int_pin;
	uint8_t wait_reg;
	uint8_t int_reg;
	uint16_t ps_thd_h;
	uint16_t ps_thd_l;
	struct mutex io_lock;
	struct input_dev *ps_input_dev;
	int32_t ps_distance_last;
	int32_t ps_near_state_last;
	bool ps_enabled;
	bool re_enabled_ps;
	/*struct wake_lock ps_wakelock;*/
#ifdef STK_POLL_PS
	struct hrtimer ps_timer;
	struct work_struct stk_ps_work;
	struct workqueue_struct *stk_ps_wq;
	/*struct wake_lock ps_nosuspend_wl;*/
#endif
	struct input_dev *als_input_dev;
	int32_t als_lux_last;
	uint32_t als_transmittance;
	bool als_enabled;
	bool re_enable_als;
	ktime_t ps_poll_delay;
	ktime_t als_poll_delay;
#ifdef STK_POLL_ALS
    struct work_struct stk_als_work;
	struct hrtimer als_timer;
	struct workqueue_struct *stk_als_wq;
#endif
	bool first_boot;
#ifdef STK_TUNE0
	uint16_t psa;
	uint16_t psi;
	uint16_t psi_set;
	struct hrtimer ps_tune0_timer;
	struct workqueue_struct *stk_ps_tune0_wq;
    struct work_struct stk_ps_tune0_work;
	ktime_t ps_tune0_delay;
	bool tune_zero_init_proc;
	uint32_t ps_stat_data[3];
	int data_count;
#endif
#ifdef STK_ALS_FIR
	struct data_filter      fir;
	atomic_t                firlength;
#endif
	atomic_t	recv_reg;
};

#if (!defined(CONFIG_STK_PS_ALS_USE_CHANGE_THRESHOLD))
static uint32_t lux_threshold_table[] = {
	3,
	10,
	40,
	65,
	145,
	300,
	550,
	930,
	1250,
	1700,
};


#define LUX_THD_TABLE_SIZE (sizeof(lux_threshold_table) / sizeof(uint32_t) + 1)
static uint16_t code_threshold_table[LUX_THD_TABLE_SIZE + 1];
#endif

static u32 debug_mask;
static struct sensor_config_info ls_sensor_info = {
	.input_type = LS_TYPE,
	.int_number = 0,
	.ldo = NULL,
};

enum {
	DEBUG_INIT = 1U << 0,
	DEBUG_REPORT_ALS_DATA = 1U << 1,
	DEBUG_REPORT_PS_DATA = 1U << 2,
	DEBUG_SUSPEND = 1U << 3,
	DEBUG_CONTROL_INFO = 1U << 4,
	DEBUG_INT = 1U << 5,
};

#define dprintk(level_mask, fmt, arg...)	{if (unlikely(debug_mask & level_mask)) \
	printk("*stk3x1x:*" fmt, ## arg); }

static int startup(void);
static int32_t stk3x1x_enable_ps(struct stk3x1x_data *ps_data, uint8_t enable, uint8_t validate_reg);
static int32_t stk3x1x_enable_als(struct stk3x1x_data *ps_data, uint8_t enable);
static int32_t stk3x1x_set_ps_thd_l(struct stk3x1x_data *ps_data, uint16_t thd_l);
static int32_t stk3x1x_set_ps_thd_h(struct stk3x1x_data *ps_data, uint16_t thd_h);
static int32_t stk3x1x_set_als_thd_l(struct stk3x1x_data *ps_data, uint16_t thd_l);
static int32_t stk3x1x_set_als_thd_h(struct stk3x1x_data *ps_data, uint16_t thd_h);
static int32_t stk3x1x_get_ir_reading(struct stk3x1x_data *ps_data);
#ifdef STK_TUNE0
static int stk_ps_tune_zero_func_fae(struct stk3x1x_data *ps_data);
#endif
#ifdef STK_CHK_REG
static int stk3x1x_validate_n_handle(struct i2c_client *client);
#endif

static const unsigned short normal_i2c[2] = {0x48, I2C_CLIENT_END};

static int stk_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	struct i2c_adapter *adapter = client->adapter;
    if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;
	if (ls_sensor_info.twi_id == adapter->nr) {
		printk("%s: ===========addr= %x\n", __func__, client->addr);
		strlcpy(info->type, DEVICE_NAME, I2C_NAME_SIZE);
		return 0;
	} else {
		return -ENODEV;
	}
}

static int stk3x1x_i2c_read_data(struct i2c_client *client, unsigned char command, int length, unsigned char *values)
{
	uint8_t retry;
	int err;

	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &command,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = values,
		},
	};

	for (retry = 0; retry < 5; retry++) {
		err = i2c_transfer(client->adapter, msgs, 2);
		if (err == 2)
			break;
		else
			mdelay(5);
	}

	if (retry >= 5) {
		printk(KERN_ERR "%s: i2c read fail, err=%d\n", __func__, err);
		return -EIO;
	}
	return 0;
}

static int stk3x1x_i2c_write_data(struct i2c_client *client, unsigned char command, int length, unsigned char *values)
{
	int retry;
	int err;
	unsigned char data[11];
	struct i2c_msg msg;
	int index;

	if (!client)
		return -EINVAL;
	else if (length >= 10) {
		printk(KERN_ERR "%s:length %d exceeds 10\n", __func__, length);
		return -EINVAL;
    }

	data[0] = command;
	for (index = 1; index <= length; index++)
		data[index] = values[index - 1];

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = length+1;
	msg.buf = data;

	for (retry = 0; retry < 5; retry++) {
		err = i2c_transfer(client->adapter, &msg, 1);
		if (err == 1)
			break;
		else
			mdelay(5);
	}

	if (retry >= 5) {
		printk(KERN_ERR "%s: i2c write fail, err=%d\n", __func__, err);
		return -EIO;
	}
	return 0;
}

static int stk3x1x_i2c_smbus_read_byte_data(struct i2c_client *client, unsigned char command)
{
	unsigned char value;
	int err;
	err = stk3x1x_i2c_read_data(client, command, 1, &value);
	if (err < 0)
		return err;
	return value;
}

static int stk3x1x_i2c_smbus_write_byte_data(struct i2c_client *client, unsigned char command, unsigned char value)
{
	int err;
	err = stk3x1x_i2c_write_data(client, command, 1, &value);
	return err;
}

inline uint32_t stk_alscode2lux(struct stk3x1x_data *ps_data, uint32_t alscode)
{
	alscode += ((alscode << 7) + (alscode << 3) + (alscode >> 1));
	alscode <<= 3;
	alscode /= ps_data->als_transmittance;
	return alscode;
}

uint32_t stk_lux2alscode(struct stk3x1x_data *ps_data, uint32_t lux)
{
    lux *= ps_data->als_transmittance;
    lux /= 1100;
    if (unlikely(lux >= (1 << 16)))
		lux = (1 << 16) - 1;

	return lux;
}

#ifndef CONFIG_STK_PS_ALS_USE_CHANGE_THRESHOLD
static void stk_init_code_threshold_table(struct stk3x1x_data *ps_data)
{
	uint32_t i, j;
    uint32_t alscode;

    code_threshold_table[0] = 0;
#ifdef STK_DEBUG_PRINTF
    printk(KERN_INFO "alscode[0]=%d\n", 0);
#endif
    for (i = 1, j = 0; i < LUX_THD_TABLE_SIZE; i++, j++) {
		alscode = stk_lux2alscode(ps_data, lux_threshold_table[j]);
		printk(KERN_INFO "alscode[%d]=%d\n", i, alscode);
		code_threshold_table[i] = (uint16_t)(alscode);
    }
    code_threshold_table[i] = 0xffff;
    printk(KERN_INFO "alscode[%d]=%d\n", i, alscode);
}

static uint32_t stk_get_lux_interval_index(uint16_t alscode)
{
    uint32_t i;
    for (i = 1; i <= LUX_THD_TABLE_SIZE; i++) {
		if ((alscode >= code_threshold_table[i-1]) && (alscode < code_threshold_table[i]))
			return i;
    }
    return LUX_THD_TABLE_SIZE;
}
#else
void stk_als_set_new_thd(struct stk3x1x_data *ps_data, uint16_t alscode)
{
    int32_t high_thd, low_thd;
    high_thd = alscode + stk_lux2alscode(ps_data, STK_ALS_CHANGE_THD);
    low_thd = alscode - stk_lux2alscode(ps_data, STK_ALS_CHANGE_THD);
    if (high_thd >= (1 << 16))
		high_thd = (1 << 16) - 1;
    if (low_thd < 0)
		low_thd = 0;
    stk3x1x_set_als_thd_h(ps_data, (uint16_t)high_thd);
    stk3x1x_set_als_thd_l(ps_data, (uint16_t)low_thd);
}
#endif /*CONFIG_STK_PS_ALS_USE_CHANGE_THRESHOLD*/


static void stk3x1x_proc_plat_data(struct stk3x1x_data *ps_data, struct stk3x1x_platform_data *plat_data)
{
	uint8_t w_reg;

	ps_data->state_reg = plat_data->state_reg;
	ps_data->psctrl_reg = plat_data->psctrl_reg;
#ifdef STK_POLL_PS
	ps_data->psctrl_reg &= 0x3F;
#endif
	ps_data->alsctrl_reg = plat_data->alsctrl_reg;
	ps_data->ledctrl_reg = plat_data->ledctrl_reg;

	ps_data->wait_reg = plat_data->wait_reg;
	if (ps_data->wait_reg < 2) {
		printk(KERN_WARNING "%s: wait_reg should be larger than 2, force to write 2\n", __func__);
		ps_data->wait_reg = 2;
	} else if (ps_data->wait_reg > 0xFF) {
		printk(KERN_WARNING "%s: wait_reg should be less than 0xFF, force to write 0xFF\n", __func__);
		ps_data->wait_reg = 0xFF;
	}
#ifndef STK_TUNE0
	ps_data->ps_thd_h = plat_data->ps_thd_h;
	ps_data->ps_thd_l = plat_data->ps_thd_l;
#endif

	w_reg = 0;
#ifndef STK_POLL_PS
	w_reg |= STK_INT_PS_MODE;
#else
	w_reg |= 0x01;
#endif

#if (!defined(STK_POLL_ALS) && (STK_INT_PS_MODE != 0x02) && (STK_INT_PS_MODE != 0x03))
	w_reg |= STK_INT_ALS;
#endif
	ps_data->int_reg = w_reg;
	return;
}

static int32_t stk3x1x_init_all_reg(struct stk3x1x_data *ps_data)
{
	int32_t ret;

	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_STATE_REG, ps_data->state_reg);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
    }
    ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_PSCTRL_REG, ps_data->psctrl_reg);

	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
    }
    ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_ALSCTRL_REG, ps_data->alsctrl_reg);

	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
    }

	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_LEDCTRL_REG, ps_data->ledctrl_reg);

	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
    }

	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_WAIT_REG, ps_data->wait_reg);

	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
    }

#ifdef STK_TUNE0
	ps_data->psa = 0x0;
	ps_data->psi = 0xFFFF;
#else
	stk3x1x_set_ps_thd_h(ps_data, ps_data->ps_thd_h);
	stk3x1x_set_ps_thd_l(ps_data, ps_data->ps_thd_l);
#endif



		ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_INT_REG, ps_data->int_reg);
		if (ret < 0) {
			printk(KERN_ERR "%s: write i2c error\n", __func__);
			return ret;
			}
		return 0;
}


static int32_t stk3x1x_check_pid(struct stk3x1x_data *ps_data)
{
	unsigned char value[2];
	int err;

	err = stk3x1x_i2c_read_data(ps_data->client, STK_PDT_ID_REG, 2, &value[0]);

	if (err < 0) {
		printk(KERN_ERR "%s: fail, ret=%d\n", __func__, err);
		return err;
	}

	printk(KERN_INFO "%s: PID=0x%x, RID=0x%x\n", __func__, value[0], value[1]);

	if (value[1] == 0xC0)
		printk(KERN_INFO "%s: RID=0xC0!!!!!!!!!!!!!\n", __func__);

	switch (value[0]) {
	case STK2213_PID:
	case STK3010_PID:
	case STK3311_9_BLK_PID:
	case STK3311_8_PID:
	case STK3210_STK3310_PID:
	case STK3311_9_PID:
	case STK2213C_PID:
	case STK3210C_PID:
	case STK3311_9C_PID:
	case STK3311_8C_PID:
	case STK3310C_PID:
	case STK3310SA_PID:
	case STK3311SA_PID:
	case STK3311WV_PID:
		return 0;
	case STK_ERR_PID:
		printk(KERN_ERR "PID=0x0, please make sure the chip is stk3x1x!\n");
		return -2;
	default:
		printk(KERN_ERR "%s: invalid PID(%#x)\n", __func__, value[0]);
		return -1;
	}

	return 0;
}


static int32_t stk3x1x_software_reset(struct stk3x1x_data *ps_data)
{
    int32_t r;
    uint8_t w_reg;

    w_reg = 0x7F;

    r = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_WAIT_REG, w_reg);
    if (r < 0) {
		printk(KERN_ERR "%s: software reset: write i2c error, ret=%d\n", __func__, r);
		return r;
    }
    r = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_WAIT_REG);
    if (w_reg != r) {
		printk(KERN_ERR "%s: software reset: read-back value is not the same\n", __func__);
		return -1;
    }

    r = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_SW_RESET_REG, 0);
    if (r < 0) {
		printk(KERN_ERR "%s: software reset: read error after reset\n", __func__);
		return r;
    }
    mdelay(1);
    return 0;
}


static int32_t stk3x1x_set_als_thd_l(struct stk3x1x_data *ps_data, uint16_t thd_l)
{
	unsigned char val[2];
	int ret;
	val[0] = (thd_l & 0xFF00) >> 8;
	val[1] = thd_l & 0x00FF;
	ret = stk3x1x_i2c_write_data(ps_data->client, STK_THDL1_ALS_REG, 2, val);
	if (ret < 0)
		printk(KERN_ERR "%s: fail, ret=%d\n", __func__, ret);
	return ret;
}
static int32_t stk3x1x_set_als_thd_h(struct stk3x1x_data *ps_data, uint16_t thd_h)
{
	unsigned char val[2];
	int ret;
	val[0] = (thd_h & 0xFF00) >> 8;
	val[1] = thd_h & 0x00FF;
	ret = stk3x1x_i2c_write_data(ps_data->client, STK_THDH1_ALS_REG, 2, val);
	if (ret < 0)
		printk(KERN_ERR "%s: fail, ret=%d\n", __func__, ret);
	return ret;
}

static int32_t stk3x1x_set_ps_thd_l(struct stk3x1x_data *ps_data, uint16_t thd_l)
{
	unsigned char val[2];
	int ret;
	val[0] = (thd_l & 0xFF00) >> 8;
	val[1] = thd_l & 0x00FF;
	ret = stk3x1x_i2c_write_data(ps_data->client, STK_THDL1_PS_REG, 2, val);
	if (ret < 0)
		printk(KERN_ERR "%s: fail, ret=%d\n", __func__, ret);
	return ret;
}
static int32_t stk3x1x_set_ps_thd_h(struct stk3x1x_data *ps_data, uint16_t thd_h)
{
	unsigned char val[2];
	int ret;
	val[0] = (thd_h & 0xFF00) >> 8;
	val[1] = thd_h & 0x00FF;
	ret = stk3x1x_i2c_write_data(ps_data->client, STK_THDH1_PS_REG, 2, val);
	if (ret < 0)
		printk(KERN_ERR "%s: fail, ret=%d\n", __func__, ret);
	return ret;
}

static unsigned short stk3x1x_get_ps_reading(struct stk3x1x_data *ps_data)
{
	unsigned char value[2];
	int err;
	err = stk3x1x_i2c_read_data(ps_data->client, STK_DATA1_PS_REG, 2, &value[0]);
	if (err < 0) {
		printk(KERN_ERR "%s: fail, ret=%d\n", __func__, err);
		return err;
	}
	return (value[0] << 8) | value[1];
}


static int32_t stk3x1x_set_flag(struct stk3x1x_data *ps_data, uint8_t org_flag_reg, uint8_t clr)
{
	uint8_t w_flag;
	int ret;

	w_flag = org_flag_reg | (STK_FLG_ALSINT_MASK | STK_FLG_PSINT_MASK | STK_FLG_OUI_MASK | STK_FLG_IR_RDY_MASK);
	w_flag &= (~clr);
	/*printk(KERN_INFO "%s: org_flag_reg=0x%x, w_flag = 0x%x\n", __func__, org_flag_reg, w_flag);	*/
	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_FLAG_REG, w_flag);
	if (ret < 0)
		printk(KERN_ERR "%s: fail, ret=%d\n", __func__, ret);
	return ret;
}

static int32_t stk3x1x_get_flag(struct stk3x1x_data *ps_data)
{
	int ret;
	ret = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_FLAG_REG);
	if (ret < 0)
		printk(KERN_ERR "%s: fail, ret=%d\n", __func__, ret);
	return ret;
}

static int32_t stk3x1x_enable_ps(struct stk3x1x_data *ps_data, uint8_t enable, uint8_t validate_reg)
{
    int32_t ret;
	uint8_t w_state_reg;
	uint8_t curr_ps_enable;
	uint32_t reading;
	int32_t near_far_state;
	dprintk(DEBUG_CONTROL_INFO, "%s  data === %d\n", __func__, enable);
#ifdef STK_CHK_REG
	if (validate_reg) {
		ret = stk3x1x_validate_n_handle(ps_data->client);
		if (ret < 0)
			printk(KERN_ERR "stk3x1x_validate_n_handle fail: %d\n", ret);
	}
#endif /* #ifdef STK_CHK_REG */
	curr_ps_enable = ps_data->ps_enabled ? 1 : 0;
	if (curr_ps_enable == enable)
		return 0;

#ifdef STK_TUNE0
	if (!(ps_data->psi_set) && !enable) {
		hrtimer_cancel(&ps_data->ps_tune0_timer);
		cancel_work_sync(&ps_data->stk_ps_tune0_work);
	}
#endif

	if (ps_data->first_boot == true) {
		ps_data->first_boot = false;
	}

	ret = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_STATE_REG);
	if (ret < 0) {
			printk(KERN_ERR "%s: write i2c error, ret=%d\n", __func__, ret);
			return ret;
			}
	w_state_reg = ret;

	w_state_reg &= ~(STK_STATE_EN_PS_MASK | STK_STATE_EN_WAIT_MASK | STK_STATE_EN_AK_MASK);
	if (enable) {
		w_state_reg |= STK_STATE_EN_PS_MASK;
		if (!(ps_data->als_enabled))
			w_state_reg |= STK_STATE_EN_WAIT_MASK;
	}
	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_STATE_REG, w_state_reg);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error, ret=%d\n", __func__, ret);
		return ret;
	}

	if (enable) {
		printk(KERN_INFO "%s: HT=%d,LT=%d\n", __func__, ps_data->ps_thd_h,  ps_data->ps_thd_l);
#ifdef STK_TUNE0
		if (!(ps_data->psi_set))
			hrtimer_start(&ps_data->ps_tune0_timer, ps_data->ps_tune0_delay, HRTIMER_MODE_REL);
#endif

#ifdef STK_POLL_PS
		hrtimer_start(&ps_data->ps_timer, ps_data->ps_poll_delay, HRTIMER_MODE_REL);
		ps_data->ps_distance_last = -1;
		ps_data->ps_near_state_last = 0;
#endif
#ifndef STK_POLL_PS
	#ifndef STK_POLL_ALS
		if (!(ps_data->als_enabled))
	#endif	/* #ifndef STK_POLL_ALS	*/
			enable_irq(ps_data->irq);
#endif	/* #ifndef STK_POLL_PS */
		ps_data->ps_enabled = true;
#ifdef STK_CHK_REG

		if (!validate_reg) {
			ps_data->ps_distance_last = 1;
			input_report_abs(ps_data->ps_input_dev, ABS_DISTANCE, 1);
			input_sync(ps_data->ps_input_dev);
			/*support wake lock for ps
			wake_lock_timeout(&ps_data->ps_wakelock, 3*HZ);
			*/
			reading = stk3x1x_get_ps_reading(ps_data);
			printk(KERN_INFO "%s: force report ps input event=1, ps code = %d\n", __func__, reading);
		} else
#endif /* #ifdef STK_CHK_REG */
		{
			msleep(4);
			ret = stk3x1x_get_flag(ps_data);
			if (ret < 0)
				return ret;
			near_far_state = ret & STK_FLG_NF_MASK;
			ps_data->ps_distance_last = near_far_state;
			input_report_abs(ps_data->ps_input_dev, ABS_DISTANCE, near_far_state);
			input_sync(ps_data->ps_input_dev);
			/*support wake lock for ps*/
			/*wake_lock_timeout(&ps_data->ps_wakelock, 3*HZ);*/
			reading = stk3x1x_get_ps_reading(ps_data);
			printk(KERN_INFO "%s: ps input event=%d, ps code = %d\n", __func__, near_far_state, reading);
		}

	} else {
#ifdef STK_POLL_PS
		hrtimer_cancel(&ps_data->ps_timer);
		cancel_work_sync(&ps_data->stk_ps_work);
#else
#ifndef STK_POLL_ALS
		if (!(ps_data->als_enabled))
#endif
			/*disable_irq(ps_data->irq);
*/
			printk(KERN_ERR "%s: //disable_irq(ps_data->irq);\n", __func__);

#endif
		ps_data->ps_enabled = false;
	}
	return ret;
}

static int32_t stk3x1x_enable_als(struct stk3x1x_data *ps_data, uint8_t enable)
{
	int32_t ret;
	uint8_t w_state_reg;
	uint8_t curr_als_enable = (ps_data->als_enabled) ? 1 : 0;
	dprintk(DEBUG_CONTROL_INFO, "%s  data === %d\n", __func__, enable);
	if (curr_als_enable == enable)
		return 0;
#ifdef STK_IRS
	if (enable && !(ps_data->ps_enabled)) {
		ret = stk3x1x_get_ir_reading(ps_data);
		if (ret > 0)
			ps_data->ir_code = ret;
	}
#endif


#ifndef STK_POLL_ALS
if (enable) {
		stk3x1x_set_als_thd_h(ps_data, 0x0000);
		stk3x1x_set_als_thd_l(ps_data, 0xFFFF);
	}
#endif
	ret = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_STATE_REG);
	if (ret < 0) {
			printk(KERN_ERR "%s: write i2c error\n", __func__);
			return ret;
			}
	w_state_reg = (uint8_t)(ret & (~(STK_STATE_EN_ALS_MASK | STK_STATE_EN_WAIT_MASK)));
	if (enable)
		w_state_reg |= STK_STATE_EN_ALS_MASK;
	else if (ps_data->ps_enabled)
		w_state_reg |= STK_STATE_EN_WAIT_MASK;


	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_STATE_REG, w_state_reg);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
		}
	if (enable) {
		ps_data->als_enabled = true;
#ifdef STK_POLL_ALS
		hrtimer_start(&ps_data->als_timer, ps_data->als_poll_delay, HRTIMER_MODE_REL);
#else
#ifndef STK_POLL_PS
		if (!(ps_data->ps_enabled))
#endif
			enable_irq(ps_data->irq);
#endif
		} else {
		ps_data->als_enabled = false;
#ifdef STK_POLL_ALS
		hrtimer_cancel(&ps_data->als_timer);
		cancel_work_sync(&ps_data->stk_als_work);
#else
#ifndef STK_POLL_PS
		if (!(ps_data->ps_enabled))
#endif
			disable_irq(ps_data->irq);
#endif
}
		return ret;
	}

static int32_t stk3x1x_get_als_reading(struct stk3x1x_data *ps_data)
{
    int32_t word_data;
#ifdef STK_ALS_FIR
	int index;
	int firlen = atomic_read(&ps_data->firlength);
#endif
	unsigned char value[2];
	int ret;

	ret = stk3x1x_i2c_read_data(ps_data->client, STK_DATA1_ALS_REG, 2, &value[0]);
	if (ret < 0) {
		printk(KERN_ERR "%s fail, ret=0x%x", __func__, ret);
		return ret;
	}
	word_data = (value[0] << 8) | value[1];

#ifdef STK_ALS_FIR
	if (ps_data->fir.number < firlen) {
		ps_data->fir.raw[ps_data->fir.number] = word_data;
		ps_data->fir.sum += word_data;
		ps_data->fir.number++;
		ps_data->fir.idx++;
	} else {
		index = ps_data->fir.idx % firlen;
		ps_data->fir.sum -= ps_data->fir.raw[index];
		ps_data->fir.raw[index] = word_data;
		ps_data->fir.sum += word_data;
		ps_data->fir.idx++;
		word_data = ps_data->fir.sum/firlen;
	}
#endif

	return word_data;
}

static int32_t stk3x1x_set_irs_it_slp(struct stk3x1x_data *ps_data, uint16_t *slp_time)
{
	uint8_t irs_alsctrl;
	int32_t ret;

	irs_alsctrl = (ps_data->alsctrl_reg & 0x0F) - 2;
		switch (irs_alsctrl) {
		case 6:
			*slp_time = 12;
			break;
		case 7:
			*slp_time = 24;
			break;
		case 8:
			*slp_time = 48;
			break;
		case 9:
			*slp_time = 96;
			break;
		default:
			printk(KERN_ERR "%s: unknown ALS IT=0x%x\n", __func__, irs_alsctrl);
			ret = -EINVAL;
			return ret;
	}
	irs_alsctrl |= (ps_data->alsctrl_reg & 0xF0);
	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_ALSCTRL_REG, irs_alsctrl);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
	}
	return 0;
}

static int32_t stk3x1x_get_ir_reading(struct stk3x1x_data *ps_data)
{
	int32_t word_data, ret;
	uint8_t w_reg, retry = 0;
	uint16_t irs_slp_time = 100;
	bool re_enable_ps = false;
	unsigned char value[2];

	if (ps_data->ps_enabled) {
#ifdef STK_TUNE0
		if (!(ps_data->psi_set)) {
			hrtimer_cancel(&ps_data->ps_tune0_timer);
			cancel_work_sync(&ps_data->stk_ps_tune0_work);
		}
#endif
		stk3x1x_enable_ps(ps_data, 0, 1);
		re_enable_ps = true;
	}

	ret = stk3x1x_set_irs_it_slp(ps_data, &irs_slp_time);
	if (ret < 0)
		goto irs_err_i2c_rw;

	ret = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_STATE_REG);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		goto irs_err_i2c_rw;
	}

	 w_reg = ret | STK_STATE_EN_IRS_MASK;
	 ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_STATE_REG, w_reg);
	 if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		goto irs_err_i2c_rw;
	}
	msleep(irs_slp_time);

	do {
		msleep(3);
		ret = stk3x1x_get_flag(ps_data);
		if (ret < 0)
			goto irs_err_i2c_rw;
		retry++;
	} while (retry < 10 && ((ret&STK_FLG_IR_RDY_MASK) == 0));

	if (retry == 10) {
		printk(KERN_ERR "%s: ir data is not ready for 300ms\n", __func__);
		ret = -EINVAL;
		goto irs_err_i2c_rw;
	}

	ret = stk3x1x_set_flag(ps_data, ret, STK_FLG_IR_RDY_MASK);
	if (ret < 0)
		goto irs_err_i2c_rw;

	ret = stk3x1x_i2c_read_data(ps_data->client, STK_DATA1_IR_REG, 2, &value[0]);
	if (ret < 0) {
		printk(KERN_ERR "%s fail, ret=0x%x", __func__, ret);
		goto irs_err_i2c_rw;
	}
	word_data = ((value[0]<<8) | value[1]);

	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_ALSCTRL_REG, ps_data->alsctrl_reg);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		goto irs_err_i2c_rw;
	}
	if (re_enable_ps)
		stk3x1x_enable_ps(ps_data, 1, 1);
	return word_data;

irs_err_i2c_rw:
	if (re_enable_ps)
		stk3x1x_enable_ps(ps_data, 1, 1);
	return ret;
}

#ifdef STK_CHK_REG
static int stk3x1x_chk_reg_valid(struct stk3x1x_data *ps_data)
{
	unsigned char value[9];
	int err;
	/*
	uint8_t cnt;

	for (cnt=0; cnt<9; cnt++) {
		value[cnt] = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, (cnt+1));
		if (value[cnt] < 0) {
			printk(KERN_ERR "%s fail, ret=%d", __func__, value[cnt]);
			return value[cnt];
		}
	}
	*/
	err = stk3x1x_i2c_read_data(ps_data->client, STK_PSCTRL_REG, 9, &value[0]);
	if (err < 0) {
		printk(KERN_ERR "%s: fail, ret=%d\n", __func__, err);
		return err;
	}

	if (value[0] != ps_data->psctrl_reg) {
		printk(KERN_ERR "%s: invalid reg 0x01=0x%2x\n", __func__, value[0]);
		return 0xFF;
	}
	if (value[1] != ps_data->alsctrl_reg) {
		printk(KERN_ERR "%s: invalid reg 0x02=0x%2x\n", __func__, value[1]);
		return 0xFF;
	}
	if (value[2] != ps_data->ledctrl_reg) {
		printk(KERN_ERR "%s: invalid reg 0x03=0x%2x\n", __func__, value[2]);
		return 0xFF;
	}
	if (value[3] != ps_data->int_reg) {
		printk(KERN_ERR "%s: invalid reg 0x04=0x%2x\n", __func__, value[3]);
		return 0xFF;
	}
	if (value[4] != ps_data->wait_reg) {
		printk(KERN_ERR "%s: invalid reg 0x05=0x%2x\n", __func__, value[4]);
		return 0xFF;
	}
	if (value[5] != ((ps_data->ps_thd_h & 0xFF00) >> 8)) {
		printk(KERN_ERR "%s: invalid reg 0x06=0x%2x\n", __func__, value[5]);
		return 0xFF;
	}
	if (value[6] != (ps_data->ps_thd_h & 0x00FF)) {
		printk(KERN_ERR "%s: invalid reg 0x07=0x%2x\n", __func__, value[6]);
		return 0xFF;
	}
	if (value[7] != ((ps_data->ps_thd_l & 0xFF00) >> 8)) {
		printk(KERN_ERR "%s: invalid reg 0x08=0x%2x\n", __func__, value[7]);
		return 0xFF;
	}
	if (value[8] != (ps_data->ps_thd_l & 0x00FF)) {
		printk(KERN_ERR "%s: invalid reg 0x09=0x%2x\n", __func__, value[8]);
		return 0xFF;
	}

	return 0;
}

static int stk3x1x_validate_n_handle(struct i2c_client *client)
{
	struct stk3x1x_data *ps_data = i2c_get_clientdata(client);
	int err;

	err = stk3x1x_chk_reg_valid(ps_data);
	if (err < 0) {
		printk(KERN_ERR "stk3x1x_chk_reg_valid fail: %d\n", err);
		return err;
	}

	if (err == 0xFF) {
		printk(KERN_ERR "%s: Re-init chip\n", __func__);
		err = stk3x1x_software_reset(ps_data);
		if (err < 0)
			return err;
		err = stk3x1x_init_all_reg(ps_data);
		if (err < 0)
			return err;

		/*ps_data->psa = 0;
		ps_data->psi = 0xFFFF;
		*/
		stk3x1x_set_ps_thd_h(ps_data, ps_data->ps_thd_h);
		stk3x1x_set_ps_thd_l(ps_data, ps_data->ps_thd_l);
#ifdef STK_ALS_FIR
		memset(&ps_data->fir, 0x00, sizeof(ps_data->fir));
#endif
		return 0xFF;
	}
	return 0;
}
#endif /* #ifdef STK_CHK_REG */
static ssize_t stk_als_code_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
    int32_t reading;

    reading = stk3x1x_get_als_reading(ps_data);
    return scnprintf(buf, PAGE_SIZE, "%d\n", reading);
}


static ssize_t stk_als_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
    int32_t enable, ret;

    mutex_lock(&ps_data->io_lock);
	enable = (ps_data->als_enabled) ? 1 : 0;
    mutex_unlock(&ps_data->io_lock);
    ret = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_STATE_REG);
    ret = (ret & STK_STATE_EN_ALS_MASK) ? 1 : 0;

	if (enable != ret)
		printk(KERN_ERR "%s: driver and sensor mismatch! driver_enable=0x%x, sensor_enable=%x\n", __func__, enable, ret);

	return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}

static ssize_t stk_als_enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data = dev_get_drvdata(dev);
	uint8_t en;
	if (sysfs_streq(buf, "1"))
		en = 1;
	else if (sysfs_streq(buf, "0"))
		en = 0;
	else {
		printk(KERN_ERR "%s, invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}
		printk(KERN_INFO "%s: Enable ALS : %d\n", __func__, en);
		mutex_lock(&ps_data->io_lock);
		stk3x1x_enable_als(ps_data, en);
		mutex_unlock(&ps_data->io_lock);
		return size;
}

static ssize_t stk_als_lux_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data = dev_get_drvdata(dev);
	int32_t als_reading;
	uint32_t als_lux;
	als_reading = stk3x1x_get_als_reading(ps_data);
	als_lux = stk_alscode2lux(ps_data, als_reading);
	return scnprintf(buf, PAGE_SIZE, "%d lux\n", als_lux);
}

static ssize_t stk_als_lux_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	unsigned long value = 0;
	int ret;
	ret = kstrtoul(buf, 16, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	ps_data->als_lux_last = value;
	input_report_abs(ps_data->als_input_dev, ABS_MISC, value);
	input_sync(ps_data->als_input_dev);
	printk(KERN_INFO "%s: als input event %ld lux\n", __func__, value);

	return size;
}


static ssize_t stk_als_transmittance_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	int32_t transmittance;
	transmittance = ps_data->als_transmittance;
	return scnprintf(buf, PAGE_SIZE, "%d\n", transmittance);
}


static ssize_t stk_als_transmittance_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	unsigned long value = 0;
	int ret;
	ret = kstrtoul(buf, 10, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	ps_data->als_transmittance = value;
	return size;
}

static ssize_t stk_als_delay_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	int64_t delay;
	mutex_lock(&ps_data->io_lock);
	delay = ktime_to_ms(ps_data->als_poll_delay);
	mutex_unlock(&ps_data->io_lock);
	return scnprintf(buf, PAGE_SIZE, "%lld\n", delay);
}


static ssize_t stk_als_delay_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	uint64_t value = 0;
	int ret;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	ret = kstrtoull(buf, 10, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoull failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
#ifdef STK_DEBUG_PRINTF
	printk(KERN_INFO "%s: set als poll delay=%lld\n", __func__, value);
#endif
    value = value * 1000 * 1000;
	if (value < MIN_ALS_POLL_DELAY_NS) {
		printk(KERN_ERR "%s: delay is too small\n", __func__);
		value = MIN_ALS_POLL_DELAY_NS;
	}
	mutex_lock(&ps_data->io_lock);
	if (value != ktime_to_ns(ps_data->als_poll_delay))
		ps_data->als_poll_delay = ns_to_ktime(value);
#ifdef STK_ALS_FIR
	ps_data->fir.number = 0;
	ps_data->fir.idx = 0;
	ps_data->fir.sum = 0;
#endif
	mutex_unlock(&ps_data->io_lock);
	return size;
}

static ssize_t stk_als_ir_code_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	int32_t reading;
	reading = stk3x1x_get_ir_reading(ps_data);
	return scnprintf(buf, PAGE_SIZE, "%d\n", reading);
}

#ifdef STK_ALS_FIR
static ssize_t stk_als_firlen_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	int len = atomic_read(&ps_data->firlength);

	printk(KERN_INFO "%s: len = %2d, idx = %2d\n", __func__, len, ps_data->fir.idx);
	printk(KERN_INFO "%s: sum = %5d, ave = %5d\n", __func__, ps_data->fir.sum, ps_data->fir.sum / len);

	return scnprintf(buf, PAGE_SIZE, "%d\n", len);
}


static ssize_t stk_als_firlen_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
    uint64_t value = 0;
	int ret;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	ret = kstrtoull(buf, 10, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoull failed, ret=0x%x\n", __func__, ret);
		return ret;
	}

	if (value > MAX_FIR_LEN) {
		printk(KERN_ERR "%s: firlen exceed maximum filter length\n", __func__);
	} else if (value < 1) {
		atomic_set(&ps_data->firlength, 1);
		memset(&ps_data->fir, 0x00, sizeof(ps_data->fir));
	} else {
		atomic_set(&ps_data->firlength, value);
		memset(&ps_data->fir, 0x00, sizeof(ps_data->fir));
	}
	return size;
}
#endif  /* #ifdef STK_ALS_FIR */

static ssize_t stk_ps_code_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
    uint32_t reading;
    reading = stk3x1x_get_ps_reading(ps_data);
    return scnprintf(buf, PAGE_SIZE, "%d\n", reading);
}

static ssize_t stk_ps_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int32_t enable, ret;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);

    mutex_lock(&ps_data->io_lock);
	enable = (ps_data->ps_enabled) ? 1 : 0;
    mutex_unlock(&ps_data->io_lock);
    ret = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_STATE_REG);
    ret = (ret & STK_STATE_EN_PS_MASK) ? 1 : 0;

	if (enable != ret)
		printk(KERN_ERR "%s: driver and sensor mismatch! driver_enable=0x%x, sensor_enable=%x\n", __func__, enable, ret);

	return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}

static ssize_t stk_ps_enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	uint8_t en;
	if (sysfs_streq(buf, "1"))
		en = 1;
	else if (sysfs_streq(buf, "0"))
		en = 0;
	else {
		printk(KERN_ERR "%s, invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}
    printk(KERN_INFO "%s: Enable PS : %d\n", __func__, en);
    mutex_lock(&ps_data->io_lock);
    stk3x1x_enable_ps(ps_data, en, 1);
    mutex_unlock(&ps_data->io_lock);
    return size;
}

static ssize_t stk_ps_delay_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	int64_t delay;
	mutex_lock(&ps_data->io_lock);
	delay = ktime_to_ms(ps_data->ps_poll_delay);
	mutex_unlock(&ps_data->io_lock);
	return scnprintf(buf, PAGE_SIZE, "%lld\n", delay);
}


static ssize_t stk_ps_delay_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	uint64_t value = 0;
	int ret;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	ret = kstrtoull(buf, 10, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoull failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
#ifdef STK_DEBUG_PRINTF
	printk(KERN_INFO "%s: set ps poll delay=%lld\n", __func__, value);
#endif
    value = value * 1000 * 1000;
	if (value < MIN_ALS_POLL_DELAY_NS) {
		printk(KERN_ERR "%s: delay is too small\n", __func__);
		value = MIN_ALS_POLL_DELAY_NS;
	}
	mutex_lock(&ps_data->io_lock);
	if (value != ktime_to_ns(ps_data->ps_poll_delay))
		ps_data->ps_poll_delay = ns_to_ktime(value);
#ifdef STK_ALS_FIR
	ps_data->fir.number = 0;
	ps_data->fir.idx = 0;
	ps_data->fir.sum = 0;
#endif
	mutex_unlock(&ps_data->io_lock);
	return size;
}

static ssize_t stk_ps_enable_aso_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int32_t ret;
    struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);

    ret = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_STATE_REG);
    ret = (ret & STK_STATE_EN_ASO_MASK) ? 1 : 0;

    return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}

static ssize_t stk_ps_enable_aso_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	uint8_t en;
	int32_t ret;
	uint8_t w_state_reg;

	 if (sysfs_streq(buf, "1"))
		en = 1;
	 else if (sysfs_streq(buf, "0"))
		en = 0;
	 else {
		printk(KERN_ERR "%s, invalid value %d\n", __func__, *buf);
		return -EINVAL;
	 }
	 printk(KERN_INFO "%s: Enable PS ASO : %d\n", __func__, en);

	 ret = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_STATE_REG);

	 if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
		}
	w_state_reg = (uint8_t)(ret & (~STK_STATE_EN_ASO_MASK));
	if (en)
		w_state_reg |= STK_STATE_EN_ASO_MASK;

	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_STATE_REG, w_state_reg);

	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
	}

	return size;
}


static ssize_t stk_ps_offset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	int32_t word_data;
	unsigned char value[2];
	int ret;

	ret = stk3x1x_i2c_read_data(ps_data->client, STK_DATA1_OFFSET_REG, 2, &value[0]);
	if (ret < 0) {
		printk(KERN_ERR "%s fail, ret=0x%x", __func__, ret);
		return ret;
	}
	word_data = (value[0] << 8) | value[1];

	return scnprintf(buf, PAGE_SIZE, "%d\n", word_data);
}

static ssize_t stk_ps_offset_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	unsigned long offset = 0;
	int ret;
	unsigned char val[2];

	ret = kstrtoul(buf, 10, &offset);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	if (offset > 65535) {
		printk(KERN_ERR "%s: invalid value, offset=%ld\n", __func__, offset);
		return -EINVAL;
	}

	val[0] = (offset & 0xFF00) >> 8;
	val[1] = offset & 0x00FF;
	ret = stk3x1x_i2c_write_data(ps_data->client, STK_DATA1_OFFSET_REG, 2, val);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
	}

	return size;
}


static ssize_t stk_ps_distance_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	int32_t dist = 1, ret;
	ret = stk3x1x_get_flag(ps_data);
	if (ret < 0)
		return ret;
	dist = (ret & STK_FLG_NF_MASK) ? 1 : 0;

	ps_data->ps_distance_last = dist;
	input_report_abs(ps_data->ps_input_dev, ABS_DISTANCE, dist);
	input_sync(ps_data->ps_input_dev);
	/*support wake lock for ps*/
	/*wake_lock_timeout(&ps_data->ps_wakelock, 3*HZ);*/
	printk(KERN_INFO "%s: ps input event %d cm\n", __func__, dist);
	return scnprintf(buf, PAGE_SIZE, "%d\n", dist);
}



static ssize_t stk_ps_distance_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	unsigned long value = 0;
	int ret;
	ret = kstrtoul(buf, 10, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	ps_data->ps_distance_last = value;
	input_report_abs(ps_data->ps_input_dev, ABS_DISTANCE, value);
	input_sync(ps_data->ps_input_dev);
	/*support wake lock for ps*/
	/*wake_lock_timeout(&ps_data->ps_wakelock, 3*HZ);*/
	printk(KERN_INFO "%s: ps input event %ld cm\n", __func__, value);
	return size;
}


static ssize_t stk_ps_code_thd_l_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int32_t ps_thd_l1_reg, ps_thd_l2_reg;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	ps_thd_l1_reg = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_THDL1_PS_REG);
	if (ps_thd_l1_reg < 0) {
		printk(KERN_ERR "%s fail, err=0x%x", __func__, ps_thd_l1_reg);
		return -EINVAL;
	}
	ps_thd_l2_reg = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_THDL2_PS_REG);
	if (ps_thd_l2_reg < 0) {
		printk(KERN_ERR "%s fail, err=0x%x", __func__, ps_thd_l2_reg);
		return -EINVAL;
	}
	ps_thd_l1_reg = ps_thd_l1_reg << 8 | ps_thd_l2_reg;
	return scnprintf(buf, PAGE_SIZE, "%d\n", ps_thd_l1_reg);
}


static ssize_t stk_ps_code_thd_l_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	unsigned long value = 0;
	int ret;
	ret = kstrtoul(buf, 10, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	stk3x1x_set_ps_thd_l(ps_data, value);
	return size;
}

static ssize_t stk_ps_code_thd_h_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int32_t ps_thd_h1_reg, ps_thd_h2_reg;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	ps_thd_h1_reg = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_THDH1_PS_REG);
	if (ps_thd_h1_reg < 0) {
		printk(KERN_ERR "%s fail, err=0x%x", __func__, ps_thd_h1_reg);
		return -EINVAL;
	}
	ps_thd_h2_reg = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_THDH2_PS_REG);
	if (ps_thd_h2_reg < 0) {
		printk(KERN_ERR "%s fail, err=0x%x", __func__, ps_thd_h2_reg);
		return -EINVAL;
	}
	ps_thd_h1_reg = ps_thd_h1_reg << 8 | ps_thd_h2_reg;
	return scnprintf(buf, PAGE_SIZE, "%d\n", ps_thd_h1_reg);
}


static ssize_t stk_ps_code_thd_h_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data = dev_get_drvdata(dev);
	unsigned long value = 0;
	int ret;
	ret = kstrtoul(buf, 10, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	stk3x1x_set_ps_thd_h(ps_data, value);
	return size;
}

#if 0
static ssize_t stk_als_lux_thd_l_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int32_t als_thd_l0_reg, als_thd_l1_reg;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	uint32_t als_lux;
	als_thd_l0_reg = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_THDL1_ALS_REG);
	als_thd_l1_reg = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_THDL2_ALS_REG);
    if (als_thd_l0_reg < 0) {
		printk(KERN_ERR "%s fail, err=0x%x", __func__, als_thd_l0_reg);
		return -EINVAL;
	}
	if (als_thd_l1_reg < 0) {
		printk(KERN_ERR "%s fail, err=0x%x", __func__, als_thd_l1_reg);
		return -EINVAL;
	}
    als_thd_l0_reg |= (als_thd_l1_reg << 8);
	als_lux = stk_alscode2lux(ps_data, als_thd_l0_reg);
    return scnprintf(buf, PAGE_SIZE, "%d\n", als_lux);
}


static ssize_t stk_als_lux_thd_l_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	unsigned long value = 0;
	int ret;
	ret = kstrtoul(buf, 10, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	value = stk_lux2alscode(ps_data, value);
    stk3x1x_set_als_thd_l(ps_data, value);
    return size;
}

static ssize_t stk_als_lux_thd_h_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int32_t als_thd_h0_reg, als_thd_h1_reg;
	struct stk3x1x_data *ps_data = dev_get_drvdata(dev);
	uint32_t als_lux;
	als_thd_h0_reg = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_THDH1_ALS_REG);
    als_thd_h1_reg = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_THDH2_ALS_REG);
    if (als_thd_h0_reg < 0) {
		printk(KERN_ERR "%s fail, err=0x%x", __func__, als_thd_h0_reg);
		return -EINVAL;
	}
	if (als_thd_h1_reg < 0) {
		printk(KERN_ERR "%s fail, err=0x%x", __func__, als_thd_h1_reg);
		return -EINVAL;
	}
    als_thd_h0_reg |= (als_thd_h1_reg << 8);
	als_lux = stk_alscode2lux(ps_data, als_thd_h0_reg);
    return scnprintf(buf, PAGE_SIZE, "%d\n", als_lux);
}


static ssize_t stk_als_lux_thd_h_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct stk3x1x_data *ps_data = dev_get_drvdata(dev);
	unsigned long value = 0;
	int ret;
	ret = kstrtoul(buf, 10, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
    value = stk_lux2alscode(ps_data, value);
    stk3x1x_set_als_thd_h(ps_data, value);
    return size;
}
#endif

static ssize_t stk_all_reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    int32_t ps_reg[0x22];
	uint8_t cnt;
	int len = 0;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);

	for (cnt = 0; cnt < 0x20; cnt++) {
		ps_reg[cnt] = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, (cnt));
		if (ps_reg[cnt] < 0) {
			printk(KERN_ERR "%s fail, ret=%d", __func__, ps_reg[cnt]);
			return -EINVAL;
		} else {
			printk(KERN_INFO "reg[0x%2X]=0x%2X\n", cnt, ps_reg[cnt]);
			len += scnprintf(buf+len, PAGE_SIZE-len, "[%2X]%2X,", cnt, ps_reg[cnt]);
		}
	}
	ps_reg[cnt] = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_PDT_ID_REG);
	if (ps_reg[cnt] < 0) {
		printk(KERN_ERR "%s fail, ret=%d", __func__, ps_reg[cnt]);
		return -EINVAL;
	}
	printk(KERN_INFO "reg[0x%x]=0x%2X\n", STK_PDT_ID_REG, ps_reg[cnt]);
	cnt++;
	ps_reg[cnt] = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_RSRVD_REG);
	if (ps_reg[cnt] < 0) {
		printk(KERN_ERR "%s fail, ret=%d", __func__, ps_reg[cnt]);
		return -EINVAL;
	}
	printk(KERN_INFO "reg[0x%x]=0x%2X\n", STK_RSRVD_REG, ps_reg[cnt]);
	len += scnprintf(buf+len, PAGE_SIZE-len, "[%2X]%2X,[%2X]%2X\n", cnt-1, ps_reg[cnt-1], cnt, ps_reg[cnt]);
	return len;
	/*return scnprintf(buf, PAGE_SIZE, "[0]%2X [1]%2X [2]%2X [3]%2X [4]%2X [5]%2X [6/7 HTHD]%2X,%2X [8/9 LTHD]%2X, %2X [A]%2X [B]%2X [C]%2X [D]%2X [E/F Aoff]%2X,%2X,[10]%2X [11/12 PS]%2X,%2X [13]%2X [14]%2X [15/16 Foff]%2X,%2X [17]%2X [18]%2X [3E]%2X [3F]%2X\n",
		ps_reg[0], ps_reg[1], ps_reg[2], ps_reg[3], ps_reg[4], ps_reg[5], ps_reg[6], ps_reg[7], ps_reg[8],
		ps_reg[9], ps_reg[10], ps_reg[11], ps_reg[12], ps_reg[13], ps_reg[14], ps_reg[15], ps_reg[16], ps_reg[17],
		ps_reg[18], ps_reg[19], ps_reg[20], ps_reg[21], ps_reg[22], ps_reg[23], ps_reg[24], ps_reg[25], ps_reg[26]);
		*/
}

static ssize_t stk_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int32_t ps_reg[27];
	uint8_t cnt;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	for (cnt = 0; cnt < 25; cnt++) {
		ps_reg[cnt] = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, (cnt));
		if (ps_reg[cnt] < 0) {
			printk(KERN_ERR "%s fail, ret=%d", __func__, ps_reg[cnt]);
			return -EINVAL;
		} else {
			printk(KERN_INFO "reg[0x%2X]=0x%2X\n", cnt, ps_reg[cnt]);
		}
	}
	ps_reg[cnt] = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_PDT_ID_REG);
	if (ps_reg[cnt] < 0) {
		printk(KERN_ERR "%s fail, ret=%d", __func__, ps_reg[cnt]);
		return -EINVAL;
	}
	printk(KERN_INFO "reg[0x%x]=0x%2X\n", STK_PDT_ID_REG, ps_reg[cnt]);
	cnt++;
	ps_reg[cnt] = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, STK_RSRVD_REG);
	if (ps_reg[cnt] < 0) {
		printk(KERN_ERR "%s fail, ret=%d", __func__, ps_reg[cnt]);
		return -EINVAL;
	}
	printk(KERN_INFO "reg[0x%x]=0x%2X\n", STK_RSRVD_REG, ps_reg[cnt]);
	return scnprintf(buf, PAGE_SIZE, "[PS=%2X] [ALS=%2X] [WAIT=0x%4Xms] [EN_ASO=%2X] [EN_AK=%2X] [NEAR/FAR=%2X] [FLAG_OUI=%2X] [FLAG_PSINT=%2X] [FLAG_ALSINT=%2X]\n",
		ps_reg[0] & 0x01, (ps_reg[0] & 0x02) >> 1, ((ps_reg[0] & 0x04) >> 2) * ps_reg[5] * 6, (ps_reg[0]&0x20) >> 5,
		(ps_reg[0] & 0x40) >> 6, ps_reg[16] & 0x01, (ps_reg[16] & 0x04) >> 2, (ps_reg[16] & 0x10) >> 4, (ps_reg[16] & 0x20) >> 5);
}

static ssize_t stk_recv_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	return scnprintf(buf, PAGE_SIZE, "0x%04X\n", atomic_read(&ps_data->recv_reg));
}


static ssize_t stk_recv_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned long value = 0;
	int ret;
	int32_t recv_data;
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	ret = kstrtoul(buf, 16, &value);
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	recv_data = stk3x1x_i2c_smbus_read_byte_data(ps_data->client, value);
	printk("%s: reg 0x%x=0x%x\n", __func__, (int)value, recv_data);
	atomic_set(&ps_data->recv_reg, recv_data);
	return size;
}

static ssize_t stk_send_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return 0;
}


static ssize_t stk_send_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int addr, cmd;
	int32_t ret, i;
	char *token[10];
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	for (i = 0; i < 2; i++)
		token[i] = strsep((char **)&buf, " ");
	ret = kstrtoul(token[0], 16, (unsigned long *)&(addr));
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	ret = kstrtoul(token[1], 16, (unsigned long *)&(cmd));
	if (ret < 0) {
		printk(KERN_ERR "%s:kstrtoul failed, ret=0x%x\n", __func__, ret);
		return ret;
	}
	printk(KERN_INFO "%s: write reg 0x%x=0x%x\n", __func__, addr, cmd);

	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, (unsigned char)addr, (unsigned char)cmd);
	if (0 != ret) {
		printk(KERN_ERR "%s: stk3x1x_i2c_smbus_write_byte_data fail\n", __func__);
		return ret;
	}
	return size;
}

#ifdef STK_TUNE0

static ssize_t stk_ps_cali_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct stk3x1x_data *ps_data =  dev_get_drvdata(dev);
	int32_t word_data;
	unsigned char value[2];
	int ret;

	ret = stk3x1x_i2c_read_data(ps_data->client, 0x20, 2, &value[0]);
	if (ret < 0) {
		printk(KERN_ERR "%s fail, ret=0x%x", __func__, ret);
		return ret;
	}
	word_data = (value[0] << 8) | value[1];

	ret = stk3x1x_i2c_read_data(ps_data->client, 0x22, 2, &value[0]);
	if (ret < 0) {
		printk(KERN_ERR "%s fail, ret=0x%x", __func__, ret);
		return ret;
	}
	word_data += ((value[0] << 8) | value[1]);

	printk("%s: psi_set=%d, psa=%d,psi=%d, word_data=%d\n", __func__,
		ps_data->psi_set, ps_data->psa, ps_data->psi, word_data);
	return 0;
}

#endif	/* #ifdef STK_TUNE0 */

static struct device_attribute als_enable_attribute = __ATTR(enable, 0660, stk_als_enable_show, stk_als_enable_store);
static struct device_attribute als_lux_attribute = __ATTR(lux, 0664, stk_als_lux_show, stk_als_lux_store);
static struct device_attribute als_code_attribute = __ATTR(code, 0444, stk_als_code_show, NULL);
static struct device_attribute als_transmittance_attribute = __ATTR(transmittance, 0664, stk_als_transmittance_show, stk_als_transmittance_store);
#if 0
static struct device_attribute als_lux_thd_l_attribute = __ATTR(luxthdl, 0664, stk_als_lux_thd_l_show, stk_als_lux_thd_l_store);
static struct device_attribute als_lux_thd_h_attribute = __ATTR(luxthdh, 0664, stk_als_lux_thd_h_show, stk_als_lux_thd_h_store);
#endif
static struct device_attribute als_poll_delay_attribute = __ATTR(ls_poll_delay, 0660, stk_als_delay_show, stk_als_delay_store);
static struct device_attribute als_ir_code_attribute = __ATTR(ircode, 0444, stk_als_ir_code_show, NULL);
#ifdef STK_ALS_FIR
static struct device_attribute als_firlen_attribute = __ATTR(firlen, 0664, stk_als_firlen_show, stk_als_firlen_store);
#endif


static struct attribute *stk_als_attrs[] = {
	&als_enable_attribute.attr,
    &als_lux_attribute.attr,
    &als_code_attribute.attr,
    &als_transmittance_attribute.attr,
#if 0
	&als_lux_thd_l_attribute.attr,
	&als_lux_thd_h_attribute.attr,
#endif
	&als_poll_delay_attribute.attr,
	&als_ir_code_attribute.attr,
#ifdef STK_ALS_FIR
	&als_firlen_attribute.attr,
#endif
    NULL
};

/*
static struct attribute_group stk_als_attribute_group = {
	.name = "driver",
	.attrs = stk_als_attrs,
};
*/

static struct device_attribute ps_enable_attribute = __ATTR(enable, 0660, stk_ps_enable_show, stk_ps_enable_store);
static struct device_attribute ps_delay_attribute = __ATTR(ps_poll_delay, 0660, stk_ps_delay_show, stk_ps_delay_store);
static struct device_attribute ps_enable_aso_attribute = __ATTR(enableaso, 0664, stk_ps_enable_aso_show, stk_ps_enable_aso_store);
static struct device_attribute ps_distance_attribute = __ATTR(distance, 0664, stk_ps_distance_show, stk_ps_distance_store);
static struct device_attribute ps_offset_attribute = __ATTR(offset, 0664, stk_ps_offset_show, stk_ps_offset_store);
static struct device_attribute ps_code_attribute = __ATTR(code, 0444, stk_ps_code_show, NULL);
static struct device_attribute ps_code_thd_l_attribute = __ATTR(codethdl, 0664, stk_ps_code_thd_l_show, stk_ps_code_thd_l_store);
static struct device_attribute ps_code_thd_h_attribute = __ATTR(codethdh, 0664, stk_ps_code_thd_h_show, stk_ps_code_thd_h_store);
static struct device_attribute recv_attribute = __ATTR(recv, 0664, stk_recv_show, stk_recv_store);
static struct device_attribute send_attribute = __ATTR(send, 0664, stk_send_show, stk_send_store);
static struct device_attribute all_reg_attribute = __ATTR(allreg, 0444, stk_all_reg_show, NULL);
static struct device_attribute status_attribute = __ATTR(status, 0444, stk_status_show, NULL);
#ifdef STK_TUNE0
static struct device_attribute ps_cali_attribute = __ATTR(cali, 0444, stk_ps_cali_show, NULL);
#endif


static struct attribute *stk_ps_attrs[] = {
    &ps_enable_attribute.attr,
    &ps_delay_attribute.attr,
    &ps_enable_aso_attribute.attr,
    &ps_distance_attribute.attr,
	&ps_offset_attribute.attr,
    &ps_code_attribute.attr,
	&ps_code_thd_l_attribute.attr,
	&ps_code_thd_h_attribute.attr,
	&recv_attribute.attr,
	&send_attribute.attr,
	&all_reg_attribute.attr,
	&status_attribute.attr,
#ifdef STK_TUNE0
	&ps_cali_attribute.attr,
#endif
    NULL
};

/*
static struct attribute_group stk_ps_attribute_group = {
	.name = "driver",
	.attrs = stk_ps_attrs,
};*/

#ifdef STK_TUNE0
static int stk_ps_tune_zero_val(struct stk3x1x_data *ps_data)
{
	int mode;
	int32_t word_data, lii;
	unsigned char value[2];
	int ret;

	ret = stk3x1x_i2c_read_data(ps_data->client, 0x20, 2, &value[0]);
	if (ret < 0) {
		printk(KERN_ERR "%s fail, ret=0x%x", __func__, ret);
		return ret;
	}
	word_data = (value[0] << 8) | value[1];

	ret = stk3x1x_i2c_read_data(ps_data->client, 0x22, 2, &value[0]);
	if (ret < 0) {
		printk(KERN_ERR "%s fail, ret=0x%x", __func__, ret);
		return ret;
	}
	word_data += ((value[0] << 8) | value[1]);

	mode = (ps_data->psctrl_reg) & 0x3F;
	if (mode == 0x30)
		lii = 100;
	else if (mode == 0x31)
		lii = 200;
	else if (mode == 0x32)
		lii = 400;
	else if (mode == 0x33)
		lii = 800;
	else {
		printk(KERN_ERR "%s: unsupported PS_IT(0x%x)\n", __func__, mode);
		return -1;
	}

	if (word_data > lii) {
		printk(KERN_INFO "%s: word_data=%d, lii=%d\n", __func__, word_data, lii);
		return 0xFFFF;
	}
	return 0;
}

static int stk_ps_tune_zero_final(struct stk3x1x_data *ps_data)
{
	int ret;

	ps_data->tune_zero_init_proc = false;
	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_INT_REG, ps_data->int_reg);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
	}

	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_STATE_REG, 0);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
	}

	if (ps_data->data_count == -1) {
		printk(KERN_INFO "%s: exceed limit\n", __func__);
		hrtimer_cancel(&ps_data->ps_tune0_timer);
		return 0;
	}

	ps_data->psa = ps_data->ps_stat_data[0];
	ps_data->psi = ps_data->ps_stat_data[2];
	ps_data->ps_thd_h = ps_data->ps_stat_data[1] + STK_HT_N_CT;
	ps_data->ps_thd_l = ps_data->ps_stat_data[1] + STK_LT_N_CT;
	stk3x1x_set_ps_thd_h(ps_data, ps_data->ps_thd_h);
	stk3x1x_set_ps_thd_l(ps_data, ps_data->ps_thd_l);
	printk(KERN_INFO "%s: set HT=%d,LT=%d\n", __func__, ps_data->ps_thd_h, ps_data->ps_thd_l);
	hrtimer_cancel(&ps_data->ps_tune0_timer);
	return 0;
}

static int32_t stk_tune_zero_get_ps_data(struct stk3x1x_data *ps_data)
{
	uint32_t ps_adc;
	int ret;

	ret = stk_ps_tune_zero_val(ps_data);
	if (ret == 0xFFFF) {
		ps_data->data_count = -1;
		stk_ps_tune_zero_final(ps_data);
		return 0;
	}

	ps_adc = stk3x1x_get_ps_reading(ps_data);
	printk(KERN_INFO "%s: ps_adc #%d=%d\n", __func__, ps_data->data_count, ps_adc);
	if (ps_adc < 0)
		return ps_adc;

	ps_data->ps_stat_data[1]  +=  ps_adc;
	if (ps_adc > ps_data->ps_stat_data[0])
		ps_data->ps_stat_data[0] = ps_adc;
	if (ps_adc < ps_data->ps_stat_data[2])
		ps_data->ps_stat_data[2] = ps_adc;
	ps_data->data_count++;

	if (ps_data->data_count == 5) {
		ps_data->ps_stat_data[1] /= ps_data->data_count;
		stk_ps_tune_zero_final(ps_data);
	}

	return 0;
}

static int stk_ps_tune_zero_init(struct stk3x1x_data *ps_data)
{
	int32_t ret = 0;
	uint8_t w_state_reg;

	ps_data->psi_set = 0;
	ps_data->tune_zero_init_proc = true;
	ps_data->ps_stat_data[0] = 0;
	ps_data->ps_stat_data[2] = 9999;
	ps_data->ps_stat_data[1] = 0;
	ps_data->data_count = 0;

	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_INT_REG, 0);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
	}

	w_state_reg = (STK_STATE_EN_PS_MASK | STK_STATE_EN_WAIT_MASK);
	ret = stk3x1x_i2c_smbus_write_byte_data(ps_data->client, STK_STATE_REG, w_state_reg);
	if (ret < 0) {
		printk(KERN_ERR "%s: write i2c error\n", __func__);
		return ret;
	}
	hrtimer_start(&ps_data->ps_tune0_timer, ps_data->ps_tune0_delay, HRTIMER_MODE_REL);
	return 0;
}

static int stk_ps_tune_zero_func_fae(struct stk3x1x_data *ps_data)
{
	int32_t word_data;
	int ret, diff;
	unsigned char value[2];

	if (ps_data->psi_set || !(ps_data->ps_enabled))
		return 0;

	ret = stk3x1x_get_flag(ps_data);
	if (ret < 0)
		return ret;
	if (!(ret&STK_FLG_PSDR_MASK)) {
		/*printk(KERN_INFO "%s: ps data is not ready yet\n", __func__);*/
		return 0;
	}

	ret = stk_ps_tune_zero_val(ps_data);
	if (ret == 0) {
		ret = stk3x1x_i2c_read_data(ps_data->client, 0x11, 2, &value[0]);
		if (ret < 0) {
			printk(KERN_ERR "%s fail, ret=0x%x", __func__, ret);
			return ret;
		}
		word_data = (value[0]<<8) | value[1];
		/*printk(KERN_INFO "%s: word_data=%d\n", __func__, word_data);*/

		if (word_data == 0) {
			/*printk(KERN_ERR "%s: incorrect word data (0)\n", __func__);*/
			return 0xFFFF;
		}

		if (word_data > ps_data->psa) {
			ps_data->psa = word_data;
			printk(KERN_INFO "%s: update psa: psa=%d,psi=%d\n", __func__, ps_data->psa, ps_data->psi);
		}
		if (word_data < ps_data->psi) {
			ps_data->psi = word_data;
			printk(KERN_INFO "%s: update psi: psa=%d,psi=%d\n", __func__, ps_data->psa, ps_data->psi);
		}
	}
	diff = ps_data->psa - ps_data->psi;
	if (diff > STK_MAX_MIN_DIFF) {
		ps_data->psi_set = ps_data->psi;
		ps_data->ps_thd_h = ps_data->psi + STK_HT_N_CT;
		ps_data->ps_thd_l = ps_data->psi + STK_LT_N_CT;
		stk3x1x_set_ps_thd_h(ps_data, ps_data->ps_thd_h);
		stk3x1x_set_ps_thd_l(ps_data, ps_data->ps_thd_l);
#ifdef STK_DEBUG_PRINTF
	printk(KERN_INFO "%s: FAE tune0 psa-psi(%d) > STK_DIFF found\n", __func__, diff);
#endif
	hrtimer_cancel(&ps_data->ps_tune0_timer);
	}

	return 0;
}

static void stk_ps_tune0_work_func(struct work_struct *work)
{
	struct stk3x1x_data *ps_data = container_of(work, struct stk3x1x_data, stk_ps_tune0_work);
	if (ps_data->tune_zero_init_proc)
		stk_tune_zero_get_ps_data(ps_data);
	else
		stk_ps_tune_zero_func_fae(ps_data);
	return;
}


static enum hrtimer_restart stk_ps_tune0_timer_func(struct hrtimer *timer)
{
	struct stk3x1x_data *ps_data = container_of(timer, struct stk3x1x_data, ps_tune0_timer);
	queue_work(ps_data->stk_ps_tune0_wq, &ps_data->stk_ps_tune0_work);
	hrtimer_forward_now(&ps_data->ps_tune0_timer, ps_data->ps_tune0_delay);
	return HRTIMER_RESTART;
}
#endif

#ifdef STK_POLL_ALS
static enum hrtimer_restart stk_als_timer_func(struct hrtimer *timer)
{
	struct stk3x1x_data *ps_data = container_of(timer, struct stk3x1x_data, als_timer);
	queue_work(ps_data->stk_als_wq, &ps_data->stk_als_work);
	hrtimer_forward_now(&ps_data->als_timer, ps_data->als_poll_delay);
	return HRTIMER_RESTART;
}

static void stk_als_poll_work_func(struct work_struct *work)
{
	struct stk3x1x_data *ps_data = container_of(work, struct stk3x1x_data, stk_als_work);
	int32_t reading, reading_lux, als_comperator, flag_reg;
	flag_reg = stk3x1x_get_flag(ps_data);
	if (flag_reg < 0)
		return;

	if (!(flag_reg&STK_FLG_ALSDR_MASK))
		return;

	reading = stk3x1x_get_als_reading(ps_data);
	if (reading < 0)
		return;

	if (ps_data->ir_code) {
		ps_data->als_correct_factor = 1000;
		if (reading < STK_IRC_MAX_ALS_CODE && reading > STK_IRC_MIN_ALS_CODE &&
			ps_data->ir_code > STK_IRC_MIN_IR_CODE) {
			als_comperator = reading * STK_IRC_ALS_NUMERA / STK_IRC_ALS_DENOMI;
			if (ps_data->ir_code > als_comperator)
				ps_data->als_correct_factor = STK_IRC_ALS_CORREC;
		}
#ifdef STK_DEBUG_PRINTF
		printk(KERN_INFO "%s: als=%d, ir=%d, als_correct_factor=%d", __func__, reading, ps_data->ir_code, ps_data->als_correct_factor);
#endif
		ps_data->ir_code = 0;
	}
	reading = reading * ps_data->als_correct_factor / 1000;

	reading_lux = stk_alscode2lux(ps_data, reading);
	if (abs(ps_data->als_lux_last - reading_lux) >= STK_ALS_CHANGE_THD) {
		ps_data->als_lux_last = reading_lux;
		dprintk(DEBUG_REPORT_ALS_DATA, "lightsensor data = %d\n", reading_lux);
		input_report_abs(ps_data->als_input_dev, ABS_MISC, reading_lux);
		input_sync(ps_data->als_input_dev);
#ifdef STK_DEBUG_PRINTF
		printk(KERN_INFO "%s: als input event %d lux\n", __func__, reading_lux);
#endif
	}
	return;
}
#endif /* #ifdef STK_POLL_ALS */


#ifdef STK_POLL_PS
static enum hrtimer_restart stk_ps_timer_func(struct hrtimer *timer)
{
	struct stk3x1x_data *ps_data = container_of(timer, struct stk3x1x_data, ps_timer);
	queue_work(ps_data->stk_ps_wq, &ps_data->stk_ps_work);
	hrtimer_forward_now(&ps_data->ps_timer, ps_data->ps_poll_delay);
	return HRTIMER_RESTART;
}

#define CODE_H_THD  1800
#define CODE_L_THD  1400

#ifdef NEED_REPORT_MUTIL_LEVEL_DATA
static int32_t stk_get_distance_form_code(unsigned short code)
{
	int32_t distance;
	if (code >= PS_DISTANCE_0)
		distance = 0;
	else if (code >= PS_DISTANCE_1)
		distance = 1;
	else if (code >= PS_DISTANCE_2)
		distance = 2;
	else if (code >= PS_DISTANCE_3)
		distance = 3;
	else if (code >= PS_DISTANCE_4)
		distance = 4;
	else
		distance = 5;
	return distance;
}
#endif

static void stk_ps_poll_work_func(struct work_struct *work)
{
	struct stk3x1x_data *ps_data = container_of(work, struct stk3x1x_data, stk_ps_work);
	unsigned short reading;
	int32_t near_far_state;
	uint8_t org_flag_reg;
	int32_t ret;
	uint8_t disable_flag = 0;
#ifdef STK_TUNE0
	if (!(ps_data->psi_set) || !(ps_data->ps_enabled))
		return;
#endif

	org_flag_reg = stk3x1x_get_flag(ps_data);
	if (org_flag_reg < 0)
		goto err_i2c_rw;
	if (!(org_flag_reg&STK_FLG_PSDR_MASK))
		return;

#ifdef NEED_REPORT_MUTIL_LEVEL_DATA
	reading = stk3x1x_get_ps_reading(ps_data);
	near_far_state = stk_get_distance_form_code(reading);
#else
	near_far_state = (org_flag_reg & STK_FLG_NF_MASK) ? 1 : 0;
	reading = stk3x1x_get_ps_reading(ps_data);
#endif
	ps_data->ps_distance_last = near_far_state;
	dprintk(DEBUG_REPORT_ALS_DATA, "proximity state = %d\n", near_far_state);
	input_report_abs(ps_data->ps_input_dev, ABS_DISTANCE, near_far_state);
	input_sync(ps_data->ps_input_dev);
	/*support wake lock for ps*/
	/*wake_lock_timeout(&ps_data->ps_wakelock, 3*HZ);*/

#ifdef STK_DEBUG_PRINTF
	printk(KERN_INFO "%s: ps input event %d cm, ps code = %d\n", __func__, near_far_state, reading);
#endif
	ret = stk3x1x_set_flag(ps_data, org_flag_reg, disable_flag);
	if (ret < 0)
		goto err_i2c_rw;

#if REPORT_KEY
	if (ps_data->ps_near_state_last == 0 && reading > CODE_H_THD) {
		printk(KERN_INFO "%s: >>>>>>>>>>>>\n", __func__);
		input_report_key(ps_data->ps_input_dev, KEY_PROX_NEAR, 1);
		input_sync(ps_data->ps_input_dev);
		msleep(100);
		input_report_key(ps_data->ps_input_dev, KEY_PROX_NEAR, 0);
		input_sync(ps_data->ps_input_dev);
		ps_data->ps_near_state_last = 1;
	}
	if (ps_data->ps_near_state_last == 1 && reading < CODE_L_THD) {
		printk(KERN_INFO "%s: <<<<<<<<<<<<\n", __func__);
		input_report_key(ps_data->ps_input_dev, KEY_PROX_FAR, 1);
		input_sync(ps_data->ps_input_dev);
		msleep(100);
		input_report_key(ps_data->ps_input_dev, KEY_PROX_FAR, 0);
		input_sync(ps_data->ps_input_dev);
		ps_data->ps_near_state_last = 0;
	}
#endif
	return;

err_i2c_rw:
	msleep(30);
	return;
}
#endif

#if (!defined(STK_POLL_PS) || !defined(STK_POLL_ALS))
static void stk_work_func(struct work_struct *work)
{
	uint32_t reading;
#if ((STK_INT_PS_MODE != 0x03) && (STK_INT_PS_MODE != 0x02))
	int32_t ret;
	uint8_t disable_flag = 0;
	uint8_t org_flag_reg = 0;
#endif	/* #if ((STK_INT_PS_MODE != 0x03) && (STK_INT_PS_MODE != 0x02)) */

#ifndef CONFIG_STK_PS_ALS_USE_CHANGE_THRESHOLD
	uint32_t nLuxIndex;
#endif
	struct stk3x1x_data *ps_data = container_of(work, struct stk3x1x_data, stk_work);
	int32_t near_far_state;
	int32_t als_comperator;

#if (STK_INT_PS_MODE == 0x03)
	near_far_state = gpio_get_value(ps_data->int_pin);
#elif (STK_INT_PS_MODE	== 0x02)
	near_far_state = !(gpio_get_value(ps_data->int_pin));
#endif

#if ((STK_INT_PS_MODE == 0x03) || (STK_INT_PS_MODE	== 0x02))
	ps_data->ps_distance_last = near_far_state;
	input_report_abs(ps_data->ps_input_dev, ABS_DISTANCE, near_far_state);
	input_sync(ps_data->ps_input_dev);
	/*support wake lock for ps*/
	/*wake_lock_timeout(&ps_data->ps_wakelock, 3*HZ);*/
	reading = stk3x1x_get_ps_reading(ps_data);
#ifdef STK_DEBUG_PRINTF
	printk(KERN_INFO "%s: ps input event %d cm, ps code = %d\n", __func__, near_far_state, reading);
#endif
#else
	/* mode 0x01 or 0x04 */
	org_flag_reg = stk3x1x_get_flag(ps_data);
	if (org_flag_reg < 0)
		goto err_i2c_rw;
	if (org_flag_reg & STK_FLG_ALSINT_MASK) {
		disable_flag |= STK_FLG_ALSINT_MASK;
		reading = stk3x1x_get_als_reading(ps_data);
		if (reading < 0) {
			printk(KERN_ERR "%s: stk3x1x_get_als_reading fail, ret=%d", __func__, reading);
			goto err_i2c_rw;
		}
#ifndef CONFIG_STK_PS_ALS_USE_CHANGE_THRESHOLD
		nLuxIndex = stk_get_lux_interval_index(reading);
		stk3x1x_set_als_thd_h(ps_data, code_threshold_table[nLuxIndex]);
		stk3x1x_set_als_thd_l(ps_data, code_threshold_table[nLuxIndex-1]);
#else
		stk_als_set_new_thd(ps_data, reading);
#endif /*CONFIG_STK_PS_ALS_USE_CHANGE_THRESHOLD*/

		if (ps_data->ir_code) {
			if (reading < STK_IRC_MAX_ALS_CODE && reading > STK_IRC_MIN_ALS_CODE &&
			ps_data->ir_code > STK_IRC_MIN_IR_CODE) {
				als_comperator = reading * STK_IRC_ALS_NUMERA / STK_IRC_ALS_DENOMI;
				if (ps_data->ir_code > als_comperator)
					ps_data->als_correct_factor = STK_IRC_ALS_CORREC;
				else
					ps_data->als_correct_factor = 1000;
			}
			printk(KERN_INFO "%s: als=%d, ir=%d, als_correct_factor=%d", __func__, reading, ps_data->ir_code, ps_data->als_correct_factor);
			ps_data->ir_code = 0;
		}

		reading = reading * ps_data->als_correct_factor / 1000;

		ps_data->als_lux_last = stk_alscode2lux(ps_data, reading);
		input_report_abs(ps_data->als_input_dev, ABS_MISC, ps_data->als_lux_last);
		input_sync(ps_data->als_input_dev);
#ifdef STK_DEBUG_PRINTF
		printk(KERN_INFO "%s: als input event %d lux\n", __func__, ps_data->als_lux_last);
#endif
    }
    if (org_flag_reg & STK_FLG_PSINT_MASK) {
		disable_flag |= STK_FLG_PSINT_MASK;
		near_far_state = (org_flag_reg & STK_FLG_NF_MASK) ? 1 : 0;

		ps_data->ps_distance_last = near_far_state;
		input_report_abs(ps_data->ps_input_dev, ABS_DISTANCE, near_far_state);
		input_sync(ps_data->ps_input_dev);
		/*support wake lock for ps*/
		/*wake_lock_timeout(&ps_data->ps_wakelock, 3*HZ);*/
		reading = stk3x1x_get_ps_reading(ps_data);
		printk(KERN_INFO "%s: ps input event=%d, ps code = %d\n", __func__, near_far_state, reading);
#ifdef STK_DEBUG_PRINTF
		printk(KERN_INFO "%s: ps input event=%d, ps code = %d\n", __func__, near_far_state, reading);
#endif

		ret = stk3x1x_set_flag(ps_data, org_flag_reg, disable_flag);
		if (ret < 0)
			goto err_i2c_rw;

		/*added by guoying if we are in far away  status, asleep the device*/
		/*if (near_far_state) {
			printk(KERN_ERR "%s:object leave before report power key event\n", __func__);
			input_report_key(ps_data->ps_input_dev, KEY_PROX_SLEEP, 1);*/
			input_sync(ps_data->ps_input_dev);
			msleep(100);
			/*input_report_key(ps_data->ps_input_dev, KEY_PROX_SLEEP, 0);*/
			input_sync(ps_data->ps_input_dev);
			/*printk(KERN_ERR "%s: object leave after report power key event\n", __func__);
			}*/
		if (!near_far_state) {
			/*printk(KERN_ERR "%s: object come before report power key event\n", __func__);
			input_report_key(ps_data->ps_input_dev, KEY_PROX_WAKE, 1);*/
			input_sync(ps_data->ps_input_dev);
			msleep(100);
			/*input_report_key(ps_data->ps_input_dev, KEY_PROX_WAKE, 0);*/
			input_sync(ps_data->ps_input_dev);
			/*printk(KERN_ERR "%s: object come after report power key event\n", __func__);*/
		}
	}

#endif

	msleep(1);
	input_set_int_enable(&(ls_sensor_info.input_type), 1);
	return;

err_i2c_rw:
	msleep(30);
	input_set_int_enable(&(ls_sensor_info.input_type), 1);
	return;
}
/*#include <mach/platform.h>*/
#include <asm/io.h>

/*#define PLDATA_INTERRUPT_REG_VADDR SUNXI_R_PIO_VBASE + 0x238*/
static irqreturn_t stk_oss_irq_handler(int irq, void *data)
{
	struct stk3x1x_data *pData = data;
	input_set_int_enable(&(ls_sensor_info.input_type), 0);

	/*pr_err("aw==== in the stk_oss_irq_handle:0x%x\n", readl(PLDATA_INTERRUPT_REG_VADDR));*/
	queue_work(pData->stk_wq, &pData->stk_work);
	return IRQ_HANDLED;
}
#endif	/*	#if (!defined(STK_POLL_PS) || !defined(STK_POLL_ALS))	*/
static int32_t stk3x1x_init_all_setting(struct i2c_client *client, struct stk3x1x_platform_data *plat_data)
{
	int32_t ret;
	struct stk3x1x_data *ps_data = i2c_get_clientdata(client);

	stk3x1x_proc_plat_data(ps_data, plat_data);
	ret = stk3x1x_software_reset(ps_data);
	if (ret < 0)
		return ret;

	ret = stk3x1x_check_pid(ps_data);
	if (ret < 0)
		return ret;
	ret = stk3x1x_init_all_reg(ps_data);
	if (ret < 0)
		return ret;

	ps_data->als_enabled = false;
	ps_data->ps_enabled = false;
	ps_data->re_enable_als = false;
	ps_data->re_enabled_ps = false;
	ps_data->ir_code = 0;
	ps_data->als_correct_factor = 1000;
	ps_data->first_boot = true;
#ifndef CONFIG_STK_PS_ALS_USE_CHANGE_THRESHOLD
	stk_init_code_threshold_table(ps_data);
#endif
#ifdef STK_TUNE0
	stk_ps_tune_zero_init(ps_data);
#endif
#ifdef STK_ALS_FIR
	memset(&ps_data->fir, 0x00, sizeof(ps_data->fir));
	atomic_set(&ps_data->firlength, STK_FIR_LEN);
#endif
	atomic_set(&ps_data->recv_reg, 0);
    return 0;
}

#if (!defined(STK_POLL_PS) || !defined(STK_POLL_ALS))
static int stk3x1x_setup_irq(struct i2c_client *client)
{
	int err = -EIO;
	struct stk3x1x_data *ps_data = i2c_get_clientdata(client);

	pr_err("aw==== %s:light sensor irq_number= %d\n", __func__,
			ls_sensor_info.int_number);

	ls_sensor_info.dev = &(ps_data->ps_input_dev->dev);
	if (0 != ls_sensor_info.int_number) {
		err = input_request_int(&(ls_sensor_info.input_type), stk_oss_irq_handler,
				IRQF_TRIGGER_FALLING, ps_data);
		if (err) {
			printk("Failed to request gpio irq \n");
			return err;
		}
	}
	err = 0;
	return err;

}
#endif


#if IS_ENABLED(CONFIG_PM)
static int stk3x1x_suspend(struct device *dev)
{
	struct i2c_client *client =  to_i2c_client(dev);
	struct stk3x1x_data *ps_data =  i2c_get_clientdata(client);
	int err;
#ifndef STK_POLL_PS
	/*struct i2c_client *client = to_i2c_client(dev);*/
#endif
#if 0
	if (NORMAL_STANDBY == standby_type) {

		/* process for super standby */
	} else if (SUPER_STANDBY == standby_type) {

		if (check_scene_locked(SCENE_TALKING_STANDBY) == 0) {
			printk("lradc-key: talking standby, enable wakeup source lradc!!\n");
			enable_wakeup_src(CPUS_GPIO_SRC, 0);
		} else {
				}
	}

	return 0;
#endif

	printk(KERN_INFO "%s\n", __func__);
	mutex_lock(&ps_data->io_lock);
#ifdef STK_CHK_REG
	err = stk3x1x_validate_n_handle(ps_data->client);
	if (err < 0)
		printk(KERN_ERR "stk3x1x_validate_n_handle fail: %d\n", err);
	else if (err == 0xFF) {
		if (ps_data->ps_enabled)
			stk3x1x_enable_ps(ps_data, 1, 0);
	}
#endif
	if (ps_data->als_enabled) {
		stk3x1x_enable_als(ps_data, 0);
		ps_data->re_enable_als = true;
	}
	if (ps_data->ps_enabled) {
#ifdef STK_POLL_PS
	/*wake_lock(&ps_data->ps_nosuspend_wl);*/
       {
			stk3x1x_enable_ps(ps_data, 0, 1);
			ps_data->re_enabled_ps = true;
			pr_err("aw==== in the re enable ps \n");
       }
#else
		pr_err("aw==== suspend \n");


		if (SUPER_STANDBY == standby_type) {
			err = enable_wakeup_src(CPUS_GPIO_SRC, 0);
			if (err)
				printk(KERN_WARNING "%s: set_irq_wake(%d) failed, err=(%d)\n", __func__, ps_data->irq, err);
		} else {
			printk(KERN_ERR "%s: not support wakeup source", __func__);
		}
#endif
	}
	mutex_unlock(&ps_data->io_lock);

	if (ls_sensor_info.sensor_power_ldo != NULL) {
		err = regulator_disable(ls_sensor_info.sensor_power_ldo);
		if (err)
			printk("stk power down failed\n");
	}

	return 0;
}

static int stk3x1x_resume(struct device *dev)
{
	struct i2c_client *client =  to_i2c_client(dev);
	/*added by guoying*/
	uint8_t disable_flag = 0;
	uint8_t org_flag_reg;
	int32_t ret;
	int err;
	int32_t near_far_state;
	uint32_t reading;
	struct stk3x1x_data *ps_data;
	/*ended by guoying*/
	#if 0
	if (NORMAL_STANDBY == standby_type) {

	/* process for super standby */
	} else if (SUPER_STANDBY == standby_type) {
		if (check_scene_locked(SCENE_TALKING_STANDBY) != 0) {
		} else {
			disable_wakeup_src(CPUS_GPIO_SRC, 0);
			printk("aw=== ps-key: resume from talking standby!!\n");
		}
	}
	return 0;
	#endif
	if (ls_sensor_info.sensor_power_ldo != NULL) {
		err = regulator_enable(ls_sensor_info.sensor_power_ldo);
		if (err)
			printk("stk power on failed\n");
		}
	ps_data =  i2c_get_clientdata(client);
#ifndef STK_POLL_PS
	/*struct i2c_client *client = to_i2c_client(dev);*/
#endif
	printk(KERN_INFO "%s\n", __func__);

	mutex_lock(&ps_data->io_lock);
#ifdef STK_CHK_REG
	err = stk3x1x_validate_n_handle(ps_data->client);

	if (err < 0) {
		printk(KERN_ERR "stk3x1x_validate_n_handle fail: %d\n", err);
	} else if (err == 0xFF) {
		if (ps_data->ps_enabled)
		stk3x1x_enable_ps(ps_data, 1, 0);
	}
#endif
	if (ps_data->re_enable_als) {
		stk3x1x_enable_als(ps_data, 1);
		ps_data->re_enable_als = false;
	}
	if (ps_data->ps_enabled) {
#ifdef STK_POLL_PS

	/*wake_unlock(&ps_data->ps_nosuspend_wl);*/
#else
		if (SUPER_STANDBY == standby_type) {
			err = disable_wakeup_src(CPUS_GPIO_SRC, 0);
			if (err)
				printk(KERN_WARNING "%s: disable_irq_wake(%d) failed, err=(%d)\n", __func__, ps_data->irq, err);
		}
#endif
	} else if (ps_data->re_enabled_ps) {
		stk3x1x_enable_ps(ps_data, 1, 1);
		ps_data->re_enabled_ps = false;
	}
	mutex_unlock(&ps_data->io_lock);

	/* added by guoying */
	org_flag_reg = stk3x1x_get_flag(ps_data);
	if (org_flag_reg & STK_FLG_PSINT_MASK) {
		printk(KERN_ERR "%s:before stk3x1x_set_flag ,org_flag_reg = 0x%X\n", __func__, org_flag_reg);
		disable_flag |= STK_FLG_PSINT_MASK;
		near_far_state = (org_flag_reg & STK_FLG_NF_MASK) ? 1 : 0;
	    reading = stk3x1x_get_ps_reading(ps_data);
		printk(KERN_INFO "%s: ps input event=%d, ps code = %d\n", __func__, near_far_state, reading);
		ret = stk3x1x_set_flag(ps_data, org_flag_reg, disable_flag);
	    if (ret < 0)
			return 0;
		/*if we are in near status, wake up the device*/
		/*if (!near_far_state) {
		printk(KERN_ERR "%s: object come in resume before report power key event\n", __func__);
		input_report_key(ps_data->ps_input_dev, KEY_PROX_WAKE, 1);*/
		input_sync(ps_data->ps_input_dev);
		msleep(100);
		/*input_report_key(ps_data->ps_input_dev, KEY_PROX_WAKE, 0);*/
		input_sync(ps_data->ps_input_dev);
		printk(KERN_ERR "%s: after report power key event\n", __func__);	/*
		}	*/
	}
	return 0;
}

#endif

static int stk3x1x_sysfs_create_files(struct kobject *kobj, struct attribute **attrs)
{
		int err;
		while (*attrs != NULL) {
			err = sysfs_create_file(kobj, *attrs);
			if (err)
				return err;
			attrs++;
       }
		return 0;
}

static int stk3x1x_sysfs_remove_files(struct kobject *kobj, struct attribute **attrs)
{
			while (*attrs != NULL) {
				sysfs_remove_file(kobj, *attrs);
				attrs++;
				}
			return 0;
}

static int stk3x1x_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
		int err = -ENODEV;
		struct stk3x1x_data *ps_data;
		struct stk3x1x_platform_data *plat_data;
/*
*added by guoying
*/
		struct gpio_config *pin_cfg = &ls_sensor_info.irq_gpio;
		unsigned long config;
/*
*ended by guoying
*/
		if (ls_sensor_info.dev == NULL)
			ls_sensor_info.dev = &client->dev;
		printk(KERN_INFO "%s: driver version = %s\n", __func__, DRIVER_VERSION);

		if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
			printk(KERN_ERR "%s: No Support for I2C_FUNC_I2C\n", __func__);
			return -ENODEV;
			}

		ps_data = kzalloc(sizeof(struct stk3x1x_data), GFP_KERNEL);
		if (!ps_data) {
			printk(KERN_ERR "%s: failed to allocate stk3x1x_data\n", __func__);
			return -ENOMEM;
			}
		ps_data->client = client;
		i2c_set_clientdata(client, ps_data);
		mutex_init(&ps_data->io_lock);

	/*support wake lock for ps*/
/*	wake_lock_init(&ps_data->ps_wakelock,WAKE_LOCK_SUSPEND, "stk_input_wakelock");
#ifdef STK_POLL_PS
	wake_lock_init(&ps_data->ps_nosuspend_wl,WAKE_LOCK_SUSPEND, "stk_nosuspend_wakelock");
#endif	*/
		plat_data = &stk3x1x_pfdata;
		ps_data->als_transmittance = plat_data->transmittance;
		ps_data->int_pin = plat_data->int_pin;
		if (ps_data->als_transmittance == 0) {
			printk(KERN_ERR "%s: Please set als_transmittance in platform data\n", __func__);
			goto err_als_input_allocate;
			}

	ps_data->als_input_dev = input_allocate_device();
	if (ps_data->als_input_dev == NULL) {
		printk(KERN_ERR "%s: could not allocate als device\n", __func__);
		err = -ENOMEM;
		goto err_als_input_allocate;
		}
	ps_data->ps_input_dev = input_allocate_device();
	if (ps_data->ps_input_dev == NULL) {
		printk(KERN_ERR "%s: could not allocate ps device\n", __func__);
		err = -ENOMEM;
		goto err_ps_input_allocate;
		}
	ps_data->als_input_dev->name = ALS_NAME;
	ps_data->ps_input_dev->name = PS_NAME;
	set_bit(EV_ABS, ps_data->als_input_dev->evbit);
	set_bit(EV_ABS, ps_data->ps_input_dev->evbit);
#if REPORT_KEY
	/*added by guoying for wake up system*/
	set_bit(EV_KEY, ps_data->ps_input_dev->evbit);
	set_bit(EV_REL, ps_data->ps_input_dev->evbit);
	set_bit(KEY_PROX_NEAR, ps_data->ps_input_dev->keybit);
	set_bit(KEY_PROX_FAR, ps_data->ps_input_dev->keybit);
#endif
	input_set_abs_params(ps_data->als_input_dev, ABS_MISC, 0, stk_alscode2lux(ps_data, (1 << 16) - 1), 0, 0);
#ifdef NEED_REPORT_MUTIL_LEVEL_DATA
	input_set_abs_params(ps_data->ps_input_dev, ABS_DISTANCE, 0, 5, 0, 0);
#else
	input_set_abs_params(ps_data->ps_input_dev, ABS_DISTANCE, 0, 1, 0, 0);
#endif
	err = input_register_device(ps_data->als_input_dev);
	if (err < 0) {
		printk(KERN_ERR "%s: can not register als input device\n", __func__);
		input_free_device(ps_data->als_input_dev);
		goto err_als_input_register;
		}
	err = input_register_device(ps_data->ps_input_dev);
	if (err < 0) {
		printk(KERN_ERR "%s: can not register ps input device\n", __func__);
		input_free_device(ps_data->ps_input_dev);
		goto err_ps_input_register;
		}

	err = stk3x1x_sysfs_create_files(&ps_data->als_input_dev->dev.kobj, stk_als_attrs);
	if (err < 0) {
		printk(KERN_ERR "%s:could not create sysfs group for als\n", __func__);
		goto err_als_sysfs_create_group;
	}
	kobject_uevent(&ps_data->als_input_dev->dev.kobj, KOBJ_CHANGE);
	err = stk3x1x_sysfs_create_files(&ps_data->ps_input_dev->dev.kobj, stk_ps_attrs);
	if (err < 0) {
		printk(KERN_ERR "%s:could not create sysfs group for ps\n", __func__);
		goto err_ps_sysfs_create_group;
	}
	kobject_uevent(&ps_data->ps_input_dev->dev.kobj, KOBJ_CHANGE);
	input_set_drvdata(ps_data->als_input_dev, ps_data);
	input_set_drvdata(ps_data->ps_input_dev, ps_data);

#ifdef STK_POLL_ALS
	ps_data->stk_als_wq = create_singlethread_workqueue("stk_als_wq");
	INIT_WORK(&ps_data->stk_als_work, stk_als_poll_work_func);
	hrtimer_init(&ps_data->als_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ps_data->als_poll_delay = ns_to_ktime(20 * NSEC_PER_MSEC);
	ps_data->als_timer.function = stk_als_timer_func;
#endif

#ifdef STK_POLL_PS
	ps_data->stk_ps_wq = create_singlethread_workqueue("stk_ps_wq");
	INIT_WORK(&ps_data->stk_ps_work, stk_ps_poll_work_func);
	hrtimer_init(&ps_data->ps_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ps_data->ps_poll_delay = ns_to_ktime(60 * NSEC_PER_MSEC);
	ps_data->ps_timer.function = stk_ps_timer_func;
#endif

#ifdef STK_TUNE0
	ps_data->stk_ps_tune0_wq = create_singlethread_workqueue("stk_ps_tune0_wq");
	INIT_WORK(&ps_data->stk_ps_tune0_work, stk_ps_tune0_work_func);
	hrtimer_init(&ps_data->ps_tune0_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ps_data->ps_tune0_delay = ns_to_ktime(60 * NSEC_PER_MSEC);
	ps_data->ps_tune0_timer.function = stk_ps_tune0_timer_func;
#endif

#if (!defined(STK_POLL_ALS) || !defined(STK_POLL_PS))
	ps_data->stk_wq = create_singlethread_workqueue("stk_wq");
	INIT_WORK(&ps_data->stk_work, stk_work_func);

	err = stk3x1x_setup_irq(client);
	if (err < 0)
		goto err_stk3x1x_setup_irq;
#endif
	device_init_wakeup(&client->dev, true);
	err = stk3x1x_init_all_setting(client, plat_data);
	if (err < 0)
		goto err_init_all_setting;

	sProbeSuccess = 1;
	printk(KERN_INFO "%s: probe successfully\n", __func__);
/*
added by guoying
*/
	/*
	*printk(KERN_ERR "%s:pin_cfg->mul_sel = %d\n", __func__, pin_cfg->mul_sel);
	*/
	if (pin_cfg->pull != STK_GPIO_PULL_DEFAULT) {
		config = PIN_CONF_PACKED(STK_PINCFG_TYPE_PUD, pin_cfg->pull);
		pinctrl_gpio_set_config(pin_cfg->gpio, config);
	}
	if (pin_cfg->drv_level != STK_GPIO_DRVLVL_DEFAULT) {
		config = PIN_CONF_PACKED(STK_PINCFG_TYPE_DRV, pin_cfg->drv_level);
		pinctrl_gpio_set_config(pin_cfg->gpio, config);
	}
	if (pin_cfg->data != STK_GPIO_DATA_DEFAULT) {
		config = PIN_CONF_PACKED(STK_PINCFG_TYPE_DAT, pin_cfg->data);
		pinctrl_gpio_set_config(pin_cfg->gpio, config);
	}
/*
ended by guoying
*/
	return 0;

err_init_all_setting:
	input_sensor_free(&(ls_sensor_info.input_type));
	device_init_wakeup(&client->dev, false);
#if (!defined(STK_POLL_ALS) || !defined(STK_POLL_PS))
err_stk3x1x_setup_irq:
	if (0 != ls_sensor_info.int_number)
		input_free_int(&(ls_sensor_info.input_type), ps_data);
#endif
#ifdef STK_POLL_ALS
	hrtimer_try_to_cancel(&ps_data->als_timer);
	destroy_workqueue(ps_data->stk_als_wq);
#endif
#ifdef STK_TUNE0
	destroy_workqueue(ps_data->stk_ps_tune0_wq);
#endif
#ifdef STK_POLL_PS
	hrtimer_try_to_cancel(&ps_data->ps_timer);
	destroy_workqueue(ps_data->stk_ps_wq);
#endif
#if (!defined(STK_POLL_ALS) || !defined(STK_POLL_PS))
	destroy_workqueue(ps_data->stk_wq);
#endif
	stk3x1x_sysfs_remove_files(&ps_data->ps_input_dev->dev.kobj, stk_ps_attrs);
err_ps_sysfs_create_group:
	stk3x1x_sysfs_remove_files(&ps_data->als_input_dev->dev.kobj, stk_als_attrs);
err_als_sysfs_create_group:
	input_unregister_device(ps_data->ps_input_dev);
err_ps_input_register:
	input_unregister_device(ps_data->als_input_dev);
err_als_input_register:
err_ps_input_allocate:
err_als_input_allocate:

	/*support wake lock for ps*/
/*#ifdef STK_POLL_PS
	wake_lock_destroy(&ps_data->ps_nosuspend_wl);
#endif
	wake_lock_destroy(&ps_data->ps_wakelock);	*/
    mutex_destroy(&ps_data->io_lock);
	kfree(ps_data);
    return err;
}


static int stk3x1x_remove(struct i2c_client *client)
{
	struct stk3x1x_data *ps_data = i2c_get_clientdata(client);

	device_init_wakeup(&client->dev, false);
#if (!defined(STK_POLL_ALS) || !defined(STK_POLL_PS))
	if (0 != ls_sensor_info.int_number)
		input_free_int(&(ls_sensor_info.input_type), ps_data);
#endif
#ifdef STK_POLL_ALS
	hrtimer_try_to_cancel(&ps_data->als_timer);
	destroy_workqueue(ps_data->stk_als_wq);
#endif
#ifdef STK_TUNE0
	destroy_workqueue(ps_data->stk_ps_tune0_wq);
#endif
#ifdef STK_POLL_PS
	hrtimer_try_to_cancel(&ps_data->ps_timer);
	destroy_workqueue(ps_data->stk_ps_wq);
#endif
#if (!defined(STK_POLL_ALS) || !defined(STK_POLL_PS))
	destroy_workqueue(ps_data->stk_wq);
#endif
	stk3x1x_sysfs_remove_files(&ps_data->ps_input_dev->dev.kobj, stk_ps_attrs);
	stk3x1x_sysfs_remove_files(&ps_data->als_input_dev->dev.kobj, stk_als_attrs);
	input_unregister_device(ps_data->ps_input_dev);
	input_unregister_device(ps_data->als_input_dev);

	/*support wake lock for ps*
#ifdef STK_POLL_PS
	wake_lock_destroy(&ps_data->ps_nosuspend_wl);
#endif
	wake_lock_destroy(&ps_data->ps_wakelock);	*/
	mutex_destroy(&ps_data->io_lock);
	kfree(ps_data);

    return 0;
}

static const struct i2c_device_id stk_ps_id[] = {
		{ "stk3x1x", 0},
		{}
};

MODULE_DEVICE_TABLE(i2c, stk_ps_id);

static const struct of_device_id stk3x1x_of_match[] = {
	{.compatible = "allwinner,stk3x1x"},
	{},
};

#if IS_ENABLED(CONFIG_PM)
static UNIVERSAL_DEV_PM_OPS(stk_pm_ops, stk3x1x_suspend,
		stk3x1x_resume, NULL);
#endif

static struct i2c_driver stk_ps_driver = {
		.class   = I2C_CLASS_HWMON,
		.driver = {
			.of_match_table = stk3x1x_of_match,
			.name  = DEVICE_NAME,
			.owner = THIS_MODULE,
			#if IS_ENABLED(CONFIG_PM)
			.pm = &stk_pm_ops,
			#endif
		},
		.probe    = stk3x1x_probe,
		.remove = stk3x1x_remove,
		.id_table = stk_ps_id,
		.address_list	= normal_i2c,
};

static int startup(void)
{
	int ret = 0;
	dprintk(DEBUG_INIT, "%s:light sensor driver init\n", __func__);

	if (input_sensor_startup(&(ls_sensor_info.input_type))) {
		printk("%s: ls_fetch_sysconfig_para err.\n", __func__);
		return -1;
	} else {
		ret = input_sensor_init(&(ls_sensor_info.input_type));
		if (0 != ret) {
			printk("%s:ls_init_platform_resource err. \n", __func__);
		}
	}
	if (ls_sensor_info.sensor_used == 0) {
		printk("*** ls_used set to 0 !\n");
		printk("*** if use light_sensor,please put the sys_config.fex ls_used set to 1. \n");
		return -1;
	}
	return 0;
}

static int __init stk3x1x_init(void)
{
	if (startup() != 0)
		return -1;
	if (!ls_sensor_info.isI2CClient)
		stk_ps_driver.detect = stk_detect;

	i2c_add_driver(&stk_ps_driver);

	return sProbeSuccess ? 0 : -ENODEV;
}

static void __exit stk3x1x_exit(void)
{
		printk("%s  exit !!\n", __func__);
		i2c_del_driver(&stk_ps_driver);
		input_sensor_free(&(ls_sensor_info.input_type));
}

late_initcall(stk3x1x_init);
module_exit(stk3x1x_exit);
module_param_named(debug_mask, debug_mask, int, 0644);
MODULE_AUTHOR("allwinner");
MODULE_DESCRIPTION("Sensortek stk3x1x Proximity Sensor driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
MODULE_VERSION("1.0.2");
