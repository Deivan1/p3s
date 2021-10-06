/*
 * leds-S2MPB02.h - Flash-led driver for Samsung S2MPB02
 *
 * Copyright (C) 2014 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  This driver is based on leds-max77804.h
 */

#ifndef __LEDS_S2MPB02_H__
#define __LEDS_S2MPB02_H__

#define S2MPB02_FLASH_TORCH_CURRENT_MAX 0xF
#define S2MPB02_TIMEOUT_MAX 0xF

/* S2MPB02_LV_SEL_VOUT */
#define S2MPB02_LV_SEL_VOUT_MASK 0x07
#define S2MPB02_LV_SEL_VOLT(voltage)	\
		((voltage) <= 2700 ? 0x00 : \
		((voltage) <= 3400 ? ((voltage) - 2900) / 100 : 0x07))

#define S2MPB02_FLASH_MASK 0xF0
#define S2MPB02_TORCH_MASK 0x0F

#define S2MPB02_FLED_ENABLE 1
#define S2MPB02_FLED_DISABLE 0
#define S2MPB02_FLED_ENABLE_SHIFT 7

#define S2MPB02_FLED_FLASH_MODE 0
#define S2MPB02_FLED_TORCH_MODE 1
#define S2MPB02_FLED_MODE_SHIFT 6

#define S2MPB02_FLED_FLASH_TORCH_OFF 0x00
#define S2MPB02_FLED_FLASH_ON 0x80
#define S2MPB02_FLED_TORCH_ON 0xC0
#define S2MPB02_FLED2_TORCH_ON 0xF0
#define S2MPB02_FLED_ENABLE_MODE_MASK 0xC0
#define S2MPB02_FLED2_ENABLE_MODE_MASK 0xFF

#define S2MPB02_FLED_CTRL1_LV_EN_MASK 0x08
#define S2MPB02_FLED_CTRL1_LV_ENABLE 1
#define S2MPB02_FLED_CTRL1_LV_DISABLE 0

#ifdef CONFIG_LEDS_IRIS_IRLED_CERTIFICATE_SUPPORT
#define S2MPB02_FLED_CTRL2_TORCH_ON 0xF0
#define S2MPB02_FLED_CTRL2_FLASH_ON 0x38
#define S2MPB02_FLED_CTRL2_TORCH_MASK 0xFB

#define S2MPB02_FLED_CUR2_TORCH_CUR2_MASK 0x0F

#define S2MPB02_FLED_TIME2_IRMAX_TIMER_DISABLE 0x00
#define S2MPB02_FLED_TIME2_IRMAX_TIMER_EN_MASK 0x01
#endif

#define S2MPB02_FLED2_MAX_TIME_MASK 0x1F
#define S2MPB02_FLED2_MAX_TIME_CLEAR_MASK 0x04
#define S2MPB02_FLED2_MAX_TIME_EN_MASK 0x01
#define S2MPB02_FLED2_IRON2_MASK 0xC0

#define TORCH_STEP 10

enum s2mpb02_torch_mode {
	S2MPB02_TORCH_RECORDING,
	S2MPB02_TORCH_PREFLASH,
	S2MPB02_TORCH_NORMAL,
	S2MPB02_TORCH_MAX,
};

enum s2mpb02_led_id {
	S2MPB02_FLASH_LED_1,
	S2MPB02_TORCH_LED_1,
	S2MPB02_TORCH_LED_2,
	S2MPB02_LED_MAX,
};

enum s2mpb02_flash_current {
	S2MPB02_FLASH_OUT_I_150MA = 1,
	S2MPB02_FLASH_OUT_I_250MA,
	S2MPB02_FLASH_OUT_I_350MA,
	S2MPB02_FLASH_OUT_I_450MA,
	S2MPB02_FLASH_OUT_I_550MA,
	S2MPB02_FLASH_OUT_I_650MA,
	S2MPB02_FLASH_OUT_I_750MA,
	S2MPB02_FLASH_OUT_I_850MA,
	S2MPB02_FLASH_OUT_I_950MA,
	S2MPB02_FLASH_OUT_I_1050MA,
	S2MPB02_FLASH_OUT_I_1150MA,
	S2MPB02_FLASH_OUT_I_1250MA,
	S2MPB02_FLASH_OUT_I_1350MA,
	S2MPB02_FLASH_OUT_I_1450MA,
	S2MPB02_FLASH_OUT_I_1550MA,
	S2MPB02_FLASH_OUT_I_MAX,
};

enum s2mpb02_torch_current {
	S2MPB02_TORCH_OUT_I_20MA = 1,
	S2MPB02_TORCH_OUT_I_40MA,
	S2MPB02_TORCH_OUT_I_60MA,
	S2MPB02_TORCH_OUT_I_80MA,
	S2MPB02_TORCH_OUT_I_100MA,
	S2MPB02_TORCH_OUT_I_120MA,
	S2MPB02_TORCH_OUT_I_140MA,
	S2MPB02_TORCH_OUT_I_160MA,
	S2MPB02_TORCH_OUT_I_180MA,
	S2MPB02_TORCH_OUT_I_200MA,
	S2MPB02_TORCH_OUT_I_220MA,
	S2MPB02_TORCH_OUT_I_240MA,
	S2MPB02_TORCH_OUT_I_260MA,
	S2MPB02_TORCH_OUT_I_280MA,
	S2MPB02_TORCH_OUT_I_300MA,
	S2MPB02_TORCH_OUT_I_MAX,
};

enum s2mpb02_flash_timeout {
	S2MPB02_FLASH_TIMEOUT_62P5MS,
	S2MPB02_FLASH_TIMEOUT_125MS,
	S2MPB02_FLASH_TIMEOUT_187P5MS,
	S2MPB02_FLASH_TIMEOUT_250MS,
	S2MPB02_FLASH_TIMEOUT_312P5MS,
	S2MPB02_FLASH_TIMEOUT_375MS,
	S2MPB02_FLASH_TIMEOUT_437P5MS,
	S2MPB02_FLASH_TIMEOUT_500MS,
	S2MPB02_FLASH_TIMEOUT_562P5MS,
	S2MPB02_FLASH_TIMEOUT_625MS,
	S2MPB02_FLASH_TIMEOUT_687P5MS,
	S2MPB02_FLASH_TIMEOUT_750MS,
	S2MPB02_FLASH_TIMEOUT_812P5MS,
	S2MPB02_FLASH_TIMEOUT_875MS,
	S2MPB02_FLASH_TIMEOUT_937P5MS,
	S2MPB02_FLASH_TIMEOUT_1000MS,
	S2MPB02_FLASH_TIMEOUT_MAX,
};


enum s2mpb02_torch_timeout {
	S2MPB02_TORCH_TIMEOUT_1S,
	S2MPB02_TORCH_TIMEOUT_2S,
	S2MPB02_TORCH_TIMEOUT_3S,
	S2MPB02_TORCH_TIMEOUT_4S,
	S2MPB02_TORCH_TIMEOUT_5S,
	S2MPB02_TORCH_TIMEOUT_6S,
	S2MPB02_TORCH_TIMEOUT_7S,
	S2MPB02_TORCH_TIMEOUT_8S,
	S2MPB02_TORCH_TIMEOUT_9S,
	S2MPB02_TORCH_TIMEOUT_10S,
	S2MPB02_TORCH_TIMEOUT_11S,
	S2MPB02_TORCH_TIMEOUT_12S,
	S2MPB02_TORCH_TIMEOUT_13S,
	S2MPB02_TORCH_TIMEOUT_14S,
	S2MPB02_TORCH_TIMEOUT_15S,
	S2MPB02_TORCH_TIMEOUT_16S,
	S2MPB02_TORCH_TIMEOUT_MAX,
};

enum s2mpb02_led_turn_way {
	S2MPB02_LED_TURN_WAY_I2C,
	S2MPB02_LED_TURN_WAY_GPIO,
	S2MPB02_LED_TURN_WAY_MAX,
};

struct s2mpb02_led {
	const char *name;
	int id;
	int brightness;
	int timeout;
	int irda_off;
	int torch_table_enable;
	int torch_table[TORCH_STEP];
};

struct s2mpb02_led_platform_data {
	int num_leds;
	struct s2mpb02_led leds[S2MPB02_LED_MAX];
};

extern int s2mpb02_led_en(int mode, int onoff, enum s2mpb02_led_turn_way turn_way);

#ifdef CONFIG_LEDS_IRIS_IRLED_SUPPORT
extern int s2mpb02_ir_led_current(uint32_t current_value);
extern int s2mpb02_ir_led_pulse_width(uint32_t width);
extern int s2mpb02_ir_led_pulse_delay(uint32_t delay);
extern int s2mpb02_ir_led_max_time(uint32_t max_time);
#endif

#endif
