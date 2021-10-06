/*
 * Samsung Exynos5 SoC series Sensor driver
 *
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/videodev2.h>
#include <linux/videodev2_exynos_camera.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include <exynos-is-sensor.h>
#include "is-hw.h"
#include "is-core.h"
#include "is-param.h"
#include "is-device-sensor.h"
#include "is-device-sensor-peri.h"
#include "is-resourcemgr.h"
#include "is-dt.h"
#include "is-cis-gw2.h"
#include "is-cis-gw2-setA.h"
#if 0
#include "is-cis-gw2-setB.h"
#endif
#include "is-helper-i2c.h"
#include "is-sec-define.h"
#ifdef CAMERA_REAR2_SENSOR_SHIFT_CROP
#include "is-vender.h"
#include "is-vender-specific.h"
#endif

#define SENSOR_NAME "S5KGW2"
/* #define DEBUG_GW2_PLL */

static const struct v4l2_subdev_ops subdev_ops;

static const u32 *sensor_gw2_tnp;
static u32 sensor_gw2_tnp_size;
static const u32 *sensor_gw2_global;
static u32 sensor_gw2_global_size;
static const u32 **sensor_gw2_setfiles;
static const u32 *sensor_gw2_setfile_sizes;
static const struct sensor_pll_info_compact **sensor_gw2_pllinfos;
static u32 sensor_gw2_max_setfile_num;
static const u32 *sensor_gw2_dualsync_slave;
static u32 sensor_gw2_dualsync_slave_size;
static const u32 *sensor_gw2_dualsync_single;
static u32 sensor_gw2_dualsync_single_size;
static struct is_efs_info efs_info;
static bool sensor_gw2_fake_retention_status;

/* For Recovery */
static u32 sensor_gw2_frame_duration_backup;
static struct ae_param sensor_gw2_again_backup;
static struct ae_param sensor_gw2_dgain_backup;
static struct ae_param sensor_gw2_target_exp_backup;

static void sensor_gw2_set_integration_max_margin(u32 mode, cis_shared_data *cis_data)
{
	FIMC_BUG_VOID(!cis_data);

	switch (mode) {
		case SENSOR_GW2_9248x6936_15FPS:
		case SENSOR_GW2_7680X4320_30FPS:
		case SENSOR_GW2_4864x3648_30FPS:
		case SENSOR_GW2_4864x2736_30FPS:
		case SENSOR_GW2_4624x3468_30FPS:
		case SENSOR_GW2_4624x2604_30FPS:
		case SENSOR_GW2_4624x2604_60FPS:
		case SENSOR_GW2_2432x1824_30FPS:
		case SENSOR_GW2_1920X1080_120FPS:
		case SENSOR_GW2_1920X1080_240FPS:
			cis_data->max_margin_coarse_integration_time = SENSOR_GW2_COARSE_INTEGRATION_TIME_MAX_MARGIN;
			dbg_sensor(1, "max_margin_coarse_integration_time(%d)\n",
				cis_data->max_margin_coarse_integration_time);
			break;
		default:
			err("[%s] Unsupport gw2 sensor mode\n", __func__);
			cis_data->max_margin_coarse_integration_time = SENSOR_GW2_COARSE_INTEGRATION_TIME_MAX_MARGIN;
			dbg_sensor(1, "max_margin_coarse_integration_time(%d)\n",
				cis_data->max_margin_coarse_integration_time);
			break;
	}
}

static void sensor_gw2_cis_data_calculation(const struct sensor_pll_info_compact *pll_info, cis_shared_data *cis_data)
{
	u64 vt_pix_clk_hz;
	u32 frame_rate, max_fps, frame_valid_us;

	FIMC_BUG_VOID(!pll_info);

	/* 1. get pclk value from pll info */
	vt_pix_clk_hz = pll_info->pclk;

	/* 2. the time of processing one frame calculation (us) */
	cis_data->min_frame_us_time = (((u64)pll_info->frame_length_lines) * pll_info->line_length_pck * 1000
	                / (vt_pix_clk_hz / 1000));
	cis_data->cur_frame_us_time = cis_data->min_frame_us_time;
#ifdef CAMERA_REAR2
	cis_data->min_sync_frame_us_time = cis_data->min_frame_us_time;
#endif
	/* 3. FPS calculation */
	frame_rate = vt_pix_clk_hz / (pll_info->frame_length_lines * pll_info->line_length_pck);
	dbg_sensor(1, "frame_rate (%d) = vt_pix_clk_hz(%llu) / "
		KERN_CONT "(pll_info->frame_length_lines(%d) * pll_info->line_length_pck(%d))\n",
		frame_rate, vt_pix_clk_hz, pll_info->frame_length_lines, pll_info->line_length_pck);

	/* calculate max fps */
	max_fps = (vt_pix_clk_hz * 10) / (pll_info->frame_length_lines * pll_info->line_length_pck);
	max_fps = (max_fps % 10 >= 5 ? frame_rate + 1 : frame_rate);

	cis_data->pclk = vt_pix_clk_hz;
	cis_data->max_fps = max_fps;
	cis_data->frame_length_lines = pll_info->frame_length_lines;
	cis_data->line_length_pck = pll_info->line_length_pck;
	cis_data->line_readOut_time = (u64)cis_data->line_length_pck * 1000
					* 1000 * 1000 / cis_data->pclk;
	cis_data->rolling_shutter_skew = (cis_data->cur_height - 1) * cis_data->line_readOut_time;
	cis_data->stream_on = false;

	/* Frame valid time calcuration */
	frame_valid_us = (u64)cis_data->cur_height * cis_data->line_length_pck
				* 1000 * 1000 / cis_data->pclk;
	cis_data->frame_valid_us_time = (unsigned int)frame_valid_us;

	dbg_sensor(1, "%s\n", __func__);
	dbg_sensor(1, "Sensor size(%d x %d) setting: SUCCESS!\n",
	                cis_data->cur_width, cis_data->cur_height);
	dbg_sensor(1, "Frame Valid(us): %d\n", frame_valid_us);
	dbg_sensor(1, "rolling_shutter_skew: %lld\n", cis_data->rolling_shutter_skew);

	dbg_sensor(1, "Fps: %d, max fps(%d)\n", frame_rate, cis_data->max_fps);
	dbg_sensor(1, "min_frame_time(%d us)\n", cis_data->min_frame_us_time);
	dbg_sensor(1, "Pixel rate(Kbps): %d\n", cis_data->pclk / 1000);

	/* Frame period calculation */
	cis_data->frame_time = (cis_data->line_readOut_time * cis_data->cur_height / 1000);
	cis_data->rolling_shutter_skew = (cis_data->cur_height - 1) * cis_data->line_readOut_time;

	dbg_sensor(1, "[%s] frame_time(%d), rolling_shutter_skew(%lld)\n", __func__,
		cis_data->frame_time, cis_data->rolling_shutter_skew);

	/* Constant values */
	cis_data->min_fine_integration_time = SENSOR_GW2_FINE_INTEGRATION_TIME_MIN;
	cis_data->max_fine_integration_time = SENSOR_GW2_FINE_INTEGRATION_TIME_MAX;
	cis_data->min_coarse_integration_time = SENSOR_GW2_COARSE_INTEGRATION_TIME_MIN;
	cis_data->max_margin_coarse_integration_time= SENSOR_GW2_COARSE_INTEGRATION_TIME_MAX_MARGIN;
}

void sensor_gw2_cis_data_calc(struct v4l2_subdev *subdev, u32 mode)
{
	//int ret = 0;
	struct is_cis *cis = NULL;

	FIMC_BUG_VOID(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG_VOID(!cis);
	FIMC_BUG_VOID(!cis->cis_data);

	if (mode >= sensor_gw2_max_setfile_num) {
		err("invalid mode(%d)!!", mode);
		return;
	}

	sensor_gw2_cis_data_calculation(sensor_gw2_pllinfos[mode], cis->cis_data);
}

static int sensor_gw2_wait_stream_off_status(cis_shared_data *cis_data)
{
	int ret = 0;
	u32 timeout = 0;

	FIMC_BUG(!cis_data);

#define STREAM_OFF_WAIT_TIME 250
	while (timeout < STREAM_OFF_WAIT_TIME) {
		if (cis_data->is_active_area == false &&
				cis_data->stream_on == false) {
			pr_debug("actual stream off\n");
			break;
		}
		timeout++;
	}

	if (timeout == STREAM_OFF_WAIT_TIME) {
		pr_err("actual stream off wait timeout\n");
		ret = -1;
	}

	return ret;
}

/* CIS OPS */
int sensor_gw2_cis_init(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	u32 setfile_index = 0;
	cis_setting_info setinfo;
	setinfo.param = NULL;
	setinfo.return_value = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (!cis) {
		err("cis is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	FIMC_BUG(!cis->cis_data);
#if !defined(CONFIG_CAMERA_VENDER_MCD)
	memset(cis->cis_data, 0, sizeof(cis_shared_data));

	ret = sensor_cis_check_rev(cis);
	if (ret < 0) {
		warn("sensor_gw2_check_rev is fail when cis init");
		ret = -EINVAL;
		goto p_err;
	}
#endif

	cis->cis_data->cur_width = SENSOR_GW2_MAX_WIDTH;
	cis->cis_data->cur_height = SENSOR_GW2_MAX_HEIGHT;
	cis->cis_data->low_expo_start = 33000;
	cis->need_mode_change = false;
	cis->cis_data->dual_slave = false;
	cis->mipi_clock_index_cur = CAM_MIPI_NOT_INITIALIZED;
	cis->mipi_clock_index_new = CAM_MIPI_NOT_INITIALIZED;

	sensor_gw2_cis_data_calculation(sensor_gw2_pllinfos[setfile_index], cis->cis_data);
	sensor_gw2_set_integration_max_margin(setfile_index, cis->cis_data);

	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_exposure_time, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min exposure time : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_exposure_time, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max exposure time : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_analog_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min again : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_analog_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max again : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_min_digital_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] min dgain : %d\n", __func__, setinfo.return_value);
	setinfo.return_value = 0;
	CALL_CISOPS(cis, cis_get_max_digital_gain, subdev, &setinfo.return_value);
	dbg_sensor(1, "[%s] max dgain : %d\n", __func__, setinfo.return_value);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gw2_cis_log_status(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client = NULL;
	u8 data8 = 0;
	u16 data16 = 0;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (!cis) {
		err("cis is NULL");
		ret = -ENODEV;
		goto p_err;
	}

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -ENODEV;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);

	pr_info("[%s] *******************************\n", __func__);
	ret = is_sensor_read16(client, 0x0000, &data16);
	if (unlikely(!ret)) pr_info("model_id(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0002, &data8);
	if (unlikely(!ret)) pr_info("revision_number(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0005, &data8);
	if (unlikely(!ret)) pr_info("frame_count(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0100, &data8);
	if (unlikely(!ret)) pr_info("0x0100(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0104, &data8);
	if (unlikely(!ret)) pr_info("0x0104(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0136, &data16);
	if (unlikely(!ret)) pr_info("0x0136(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0202, &data16);
	if (unlikely(!ret)) pr_info("0x0202(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0204, &data16);
	if (unlikely(!ret)) pr_info("0x0204(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x021E, &data16);
	if (unlikely(!ret)) pr_info("0x021E(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0226, &data16);
	if (unlikely(!ret)) pr_info("0x0226(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0340, &data16);
	if (unlikely(!ret)) pr_info("0x0340(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0342, &data16);
	if (unlikely(!ret)) pr_info("0x0342(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x3000, &data8);
	if (unlikely(!ret)) pr_info("0x3000(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0702, &data8);
	if (unlikely(!ret)) pr_info("0x0702(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0704, &data8);
	if (unlikely(!ret)) pr_info("0x0704(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read8(client, 0x0705, &data8);
	if (unlikely(!ret)) pr_info("0x0705(0x%x)\n", data8);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0A70, &data16);
	if (unlikely(!ret)) pr_info("0x0A70(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0A72, &data16);
	if (unlikely(!ret)) pr_info("0x0A72(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0A74, &data16);
	if (unlikely(!ret)) pr_info("0x0A74(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0A76, &data16);
	if (unlikely(!ret)) pr_info("0x0A76(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0A78, &data16);
	if (unlikely(!ret)) pr_info("0x0A78(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0A7A, &data16);
	if (unlikely(!ret)) pr_info("0x0A7A(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0A7C, &data16);
	if (unlikely(!ret)) pr_info("0x0A7C(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_read16(client, 0x0A80, &data16);
	if (unlikely(!ret)) pr_info("0x0A80(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602C, 0x2000);
	if (unlikely(!ret)) pr_info("0x2000 page\n");
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x13B0);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x13B0(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x13B2);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x13B2(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x13B8);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x13B8(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x13CA);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x13CA(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x1438);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x1438(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x143A);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x143A(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602C, 0x2001);
	if (unlikely(!ret)) pr_info("0x2001 page\n");
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4140);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4140(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4142);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4142(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4144);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4144(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4146);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4146(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4148);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4148(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x414A);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x414A(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x414C);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x414C(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4168);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4168(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x416A);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x416A(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x416C);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x416C(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x416E);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x416E(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4170);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4170(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4172);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4172(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4174);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4174(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x4176);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x4176(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x92AC);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x92AC(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x92AE);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x92AE(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x92B0);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x92B0(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x3E78);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x3E78(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x429C);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x429C(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602E, 0x3E94);
	ret |= is_sensor_read16(client, 0x6F12, &data16);
	if (unlikely(!ret)) pr_info("0x3E94(0x%x)\n", data16);
	else goto i2c_err;
	ret = is_sensor_write16(client, 0x602C, 0x4000);
	if (unlikely(!ret)) pr_info("0x4000 page(indirect)\n");
	else goto i2c_err;
	ret = is_sensor_write16(client, 0xFCFC, 0x4000);
	if (unlikely(!ret)) pr_info("0x4000 page\n");
	else goto i2c_err;

	pr_info("[%s] *******************************\n", __func__);

i2c_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);
p_err:
	return ret;
}

#if USE_GROUP_PARAM_HOLD
static int sensor_gw2_cis_group_param_hold_func(struct v4l2_subdev *subdev, unsigned int hold)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	struct i2c_client *client = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	if (hold == cis->cis_data->group_param_hold) {
		pr_debug("already group_param_hold (%d)\n", cis->cis_data->group_param_hold);
		goto p_err;
	}

	ret = is_sensor_write8(client, 0x0104, hold);
	if (ret < 0)
		goto p_err;

	cis->cis_data->group_param_hold = hold;
	ret = 1;
p_err:
	return ret;
}
#else
static inline int sensor_gw2_cis_group_param_hold_func(struct v4l2_subdev *subdev, unsigned int hold)
{ return 0; }
#endif

/* Input
 *	hold : true - hold, flase - no hold
 * Output
 *      return: 0 - no effect(already hold or no hold)
 *		positive - setted by request
 *		negative - ERROR value
 */
int sensor_gw2_cis_group_param_hold(struct v4l2_subdev *subdev, bool hold)
{
	int ret = 0;
	struct is_cis *cis = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	ret = sensor_gw2_cis_group_param_hold_func(subdev, hold);
	if (ret < 0)
		goto p_err;

p_err:
	return ret;
}

int sensor_gw2_cis_set_pdxtc_calibration(struct is_cis *cis)
{
	int ret = 0, i;
	struct is_rom_info *finfo = NULL;
	char *cal_buf;
	u8* pdxtc_cal;
	int coef_size, val_size;

	is_sec_get_cal_buf(&cal_buf, ROM_ID_REAR);
	is_sec_get_sysfs_finfo(&finfo, ROM_ID_REAR);

#ifdef CAMERA_GW2_PDXTC_MODULE_VERSION
	if(finfo->header_ver[10] < CAMERA_GW2_PDXTC_MODULE_VERSION) {
		info("%s - skip pdxtc_cal, cal value not valid(cur_header : %s)", __func__, finfo->header_ver);
		return 0;
	}
#endif

	coef_size	= finfo->rom_pdxtc_cal_data_0_size;
	val_size	= finfo->rom_pdxtc_cal_data_1_size;

	pdxtc_cal = &cal_buf[finfo->rom_pdxtc_cal_data_start_addr];

	/* PDXTC Setting */
	/* Pre callibration */
	ret = is_sensor_write16(cis->client, 0x6028, 0x4000);
	ret = is_sensor_write16(cis->client, 0x602A, 0x0100);
	ret = is_sensor_write8(cis->client, 0x6F12, 0x00);
	usleep_range(33000,33000);
	ret = is_sensor_write16(cis->client, 0x6028, 0x2000);
	ret = is_sensor_write16(cis->client, 0x602A, 0x55B0);
	ret = is_sensor_write16(cis->client, 0x6F12, 0x0100);
	ret = is_sensor_write16(cis->client, 0x602A, 0x55B6);
	ret = is_sensor_write16(cis->client, 0x6F12, 0x0002);
	if (ret < 0) {
		err("sensor_gw2_set_registers fail!!");
		goto p_err;
	}

	/* EEPROM Write */
	is_sensor_write16(cis->client, 0x602A, 0x5604);
	for(i = 0; i < coef_size; i+=2) {
		u16 val = (pdxtc_cal[i] << 8)|(pdxtc_cal[i + 1]);	/* Big Endian => Little Endian */
		ret = is_sensor_write16(cis->client, 0x6F12, val);
		if (ret < 0) {
			err("sensor_gw2_set_registers fail!!");
			goto p_err;
		}
	}

	is_sensor_write16(cis->client, 0x6028, 0x2001);
	is_sensor_write16(cis->client, 0x602A, 0x2DF0);

	for(i = 0; i < val_size; i+=2) {
		u16 val = (pdxtc_cal[i + coef_size] << 8)|(pdxtc_cal[i + coef_size + 1]);	/* Big Endian => Little Endian */
		ret = is_sensor_write16(cis->client, 0x6F12, val);
		if (ret < 0) {
			err("sensor_gw2_set_registers fail!!");
			goto p_err;
		}
	}

	/* after cal */
	ret = is_sensor_write16(cis->client, 0x6028, 0x2000);
	ret = is_sensor_write16(cis->client, 0x602A, 0x39F6);
	ret = is_sensor_write8(cis->client, 0x6F12, 0x80);
	usleep_range(10000, 10000);
	if (ret < 0) {
		err("sensor_gw2_set_registers fail!!");
		goto p_err;
	}
	info("[%s] pdxtc Applied\n", __func__);

p_err:
	return ret;
}

int sensor_gw2_cis_set_global_setting(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	if (sensor_gw2_fake_retention_status) {
		info("[%s] skip global setting\n", __func__);
		sensor_gw2_fake_retention_status = false;
	} else {
		/* setfile global setting is at camera entrance */
		info("[%s] global setting enter\n", __func__);
		ret = sensor_cis_set_registers(subdev, sensor_gw2_tnp, sensor_gw2_tnp_size);
		ret = sensor_cis_set_registers(subdev, sensor_gw2_global, sensor_gw2_global_size);
		if (ret < 0) {
			err("sensor_gw2_set_registers fail!!");
			goto p_err;
		}
		info("[%s] global setting done\n", __func__);

		if (sensor_gw2_cis_set_pdxtc_calibration(cis) < 0) {
			err("sensor_gw2_pdxtc_calibration fail!");
			goto p_err;
		}
	}
p_err:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);

	return ret;
}

#ifdef CAMERA_REAR2_SENSOR_SHIFT_CROP
int sensor_gw2_cis_update_crop_region(struct v4l2_subdev *subdev)
{
	int ret = 0;
	u16 x_start = 0;
	u16 y_start = 0;
	u16 x_end = 0;
	u16 y_end = 0;
	char *cal_buf;
	struct is_cis *cis = NULL;
	struct i2c_client *client;
	struct is_device_sensor *device;
	s16 delta_x, delta_y;
	struct is_rom_info *finfo = NULL;
	long efs_size = 0;
	int rom_id = 0;
	struct is_core *core = NULL;
	struct is_vender_specific *specific;

	device = (struct is_device_sensor *)v4l2_get_subdev_hostdata(subdev);
	if (device == NULL) {
		err("device is NULL");
		return -1;
	}

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	WARN_ON(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		return -1;
	}

	if (device->cfg->mode == SENSOR_GW2_9248x6936_15FPS
		|| device->cfg->mode == SENSOR_GW2_7680X4320_30FPS
		|| device->cfg->mode == SENSOR_GW2_4624x3468_30FPS
		|| device->cfg->mode == SENSOR_GW2_4624x2604_30FPS
		|| device->cfg->mode == SENSOR_GW2_4624x2604_60FPS
		|| device->cfg->mode == SENSOR_GW2_3200x2400_60FPS
		|| device->cfg->mode == SENSOR_GW2_3200x1800_60FPS
		|| device->cfg->mode == SENSOR_GW2_2432x1824_30FPS
		|| device->cfg->mode == SENSOR_GW2_1920X1080_120FPS
		|| device->cfg->mode == SENSOR_GW2_1920X1080_240FPS) {
		info("[%s] skip crop shift in full & fast ae sensor mode", __func__);
		return 0;
	}

	rom_id = is_vendor_get_rom_id_from_position(device->position);

	is_sec_get_cal_buf(&cal_buf, rom_id);
	is_sec_get_sysfs_finfo(&finfo, rom_id);

	core = (struct is_core *)dev_get_drvdata(is_dev);
	specific = core->vender.private_data;

	efs_size = specific->tilt_cal_tele_efs_size;
	if (efs_size) {
		efs_info.crop_shift_delta_x = *((s16 *)&specific->tilt_cal_tele_efs_data[CROP_SHIFT_ADDR_X]);
		efs_info.crop_shift_delta_y = *((s16 *)&specific->tilt_cal_tele_efs_data[CROP_SHIFT_ADDR_Y]);
		set_bit(IS_EFS_STATE_READ, &efs_info.efs_state);
	} else {
		clear_bit(IS_EFS_STATE_READ, &efs_info.efs_state);
	}

	if (!test_bit(IS_EFS_STATE_READ, &efs_info.efs_state)) {
		delta_x = *((s16 *)&cal_buf[finfo->rom_dualcal_slave1_cropshift_x_addr]);
		delta_y = *((s16 *)&cal_buf[finfo->rom_dualcal_slave1_cropshift_y_addr]);
		info("[%s] Read from eeprom. delta_x[%d], delta_y[%d]", __func__, delta_x, delta_y);
	} else {
		delta_x = efs_info.crop_shift_delta_x;
		delta_y = efs_info.crop_shift_delta_y;
		info("[%s] Read from efs. delta_x[%d], delta_y[%d]", __func__, delta_x, delta_y);
	}

	info("[%s] Applied delta_x[%d], delta_y[%d]", __func__, delta_x, delta_y);

	ret = is_sensor_read16(client, 0x0344, &x_start);
	ret = is_sensor_read16(client, 0x0346, &y_start);
	ret = is_sensor_read16(client, 0x0348, &x_end);
	ret = is_sensor_read16(client, 0x034A, &y_end);

	x_start += delta_x;
	y_start += delta_y;
	x_end += delta_x;
	y_end += delta_y;

	ret = is_sensor_write16(client, 0x0344, x_start);
	ret = is_sensor_write16(client, 0x0346, y_start);
	ret = is_sensor_write16(client, 0x0348, x_end);
	ret = is_sensor_write16(client, 0x034A, y_end);

	info("[%s] x_start(%d), y_start(%d), x_end(%d), y_end(%d)\n",
		__func__, x_start, y_start, x_end, y_end);

	return ret;
}

static int sensor_gw2_cis_update_pdaf_tail_size(struct v4l2_subdev *subdev, struct is_sensor_cfg *select)
{
	struct is_cis *cis = NULL;
	struct i2c_client *client;
	char *cal_buf;
	u32 width = 0, height = 0;
	struct is_rom_info *finfo = NULL;

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	if (cis == NULL) {
		err("cis is NULL");
		return -1;
	}

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		return -1;
	}

	if (select->mode == SENSOR_GW2_1920X1080_120FPS
		|| select->mode == SENSOR_GW2_1920X1080_240FPS) {
		info("skip tele shift in fast ae sensor mode\n");
		return 0;
	}

	is_sec_get_cal_buf(&cal_buf, ROM_ID_REAR);
	is_sec_get_sysfs_finfo(&finfo, ROM_ID_REAR);

	info("[%s] dummy_flag[%d]", __func__, cal_buf[finfo->rom_dualcal_slave1_dummy_flag_addr]);

	if (cal_buf[finfo->rom_dualcal_slave1_dummy_flag_addr] != 7) { /* INVALID cal data */
		return 0;
	}

	switch (select->mode) {
	case SENSOR_GW2_9248x6936_15FPS:
		width = 1152;
		height = 1720;
		break;
	case SENSOR_GW2_7680X4320_30FPS:
		width = 960;
		height = 1064;
		break;
	case SENSOR_GW2_4864x3648_30FPS:
	case SENSOR_GW2_2432x1824_30FPS:
		width = 608;
		height = 904;
		break;
	case SENSOR_GW2_4864x2736_30FPS:
		width = 608;
		height = 680;
		break;
	case SENSOR_GW2_4624x3468_30FPS:
		width = 1152;
		height = 1712;
		break;
	case SENSOR_GW2_4624x2604_30FPS:
	case SENSOR_GW2_4624x2604_60FPS:
		width = 1152;
		height = 1280;
		break;
	case SENSOR_GW2_3200x2400_60FPS:
		width = 400;
		height = 584;
		break;
	case SENSOR_GW2_3200x1800_60FPS:
		width = 400;
		height = 440;
		break;
	default:
		warn("[%s] Don't change pdaf tail size\n", __func__);
		break;
	}

	select->input[CSI_VIRTUAL_CH_1].width = width;
	select->input[CSI_VIRTUAL_CH_1].height = height;
	select->output[CSI_VIRTUAL_CH_1].width = width;
	select->output[CSI_VIRTUAL_CH_1].height = height;

	info("[%s] PDAF tail size (%d x %d)\n",
		__func__, width, height);

	return 0;
}
#endif

static void sensor_gw2_cis_set_paf_stat_enable(u32 mode, cis_shared_data *cis_data)
{
	WARN_ON(!cis_data);

	switch (mode) {
	case SENSOR_GW2_1920X1080_120FPS:
	case SENSOR_GW2_1920X1080_240FPS:
	case SENSOR_GW2_9248x6936_15FPS:
		cis_data->is_data.paf_stat_enable = false;
		break;
	default:
		cis_data->is_data.paf_stat_enable = true;
		break;
	}
}

int sensor_gw2_cis_mode_change(struct v4l2_subdev *subdev, u32 mode)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	struct is_device_sensor *device;
	char *buf;
	struct is_rom_info *finfo = NULL;
	struct is_core *core = NULL;

	core = (struct is_core *)dev_get_drvdata(is_dev);
	if (!core) {
		err("core device is null");
		return -EINVAL;
	}

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	device = (struct is_device_sensor *)v4l2_get_subdev_hostdata(subdev);
	if (unlikely(!device)) {
		err("device sensor is null");
		return -EINVAL;
	}

	if (mode >= sensor_gw2_max_setfile_num) {
		err("invalid mode(%d)!!", mode);
		ret = -EINVAL;
		goto p_err;
	}

#if 0
	sensor_gw2_cis_data_calculation(sensor_gw2_pllinfos[mode], cis->cis_data);
#endif

	cis->mipi_clock_index_cur = CAM_MIPI_NOT_INITIALIZED;

	sensor_gw2_cis_set_paf_stat_enable(mode, cis->cis_data);

	is_sec_get_cal_buf(&buf, ROM_ID_REAR);
	is_sec_get_sysfs_finfo(&finfo, ROM_ID_REAR);

	I2C_MUTEX_LOCK(cis->i2c_lock);


	if (test_bit(IS_SENSOR_OPEN, &(core->sensor[0].state))) {
		info("[%s] dual sync slave mode\n", __func__);
		ret = sensor_cis_set_registers(subdev, sensor_gw2_dualsync_slave, sensor_gw2_dualsync_slave_size);
		cis->cis_data->dual_slave = true;
	} else {
		info("[%s] dual sync single mode\n", __func__);
		ret = sensor_cis_set_registers(subdev, sensor_gw2_dualsync_single, sensor_gw2_dualsync_single_size);
		cis->cis_data->dual_slave = false;
	}

	if (ret < 0) {
		err("gw2 dual sync fail");
		goto p_err_i2c_unlock;
	}

	ret = sensor_cis_set_registers(subdev, sensor_gw2_setfiles[mode], sensor_gw2_setfile_sizes[mode]);
	if (ret < 0) {
		err("sensor_gw2_set_registers fail!!");
		goto p_err_i2c_unlock;
	}

#ifdef CAMERA_REAR2_SENSOR_SHIFT_CROP
	if (device->position == SENSOR_POSITION_REAR2) {
		ret = sensor_gw2_cis_update_crop_region(subdev);
		if (ret < 0) {
			err("sensor_gw2_cis_update_crop_region fail!!");
			goto p_err_i2c_unlock;
		}
	}
#endif

#ifdef CAMERA_GW2_PDXTC_MODULE_VERSION
	if (finfo->header_ver[10] < CAMERA_GW2_PDXTC_MODULE_VERSION) {
		ret = is_sensor_write16(cis->client, 0x6028, 0x2000);
		ret = is_sensor_write16(cis->client, 0x602A, 0x55B0);
		ret = is_sensor_write16(cis->client, 0x6F12, 0x0000);
		if (ret < 0){
			err("sensor_gw2 disable pdxtc fail!");
			goto p_err_i2c_unlock;
		}
		info("%s - disable pdxtc", __func__);
	}
#endif

	info("[%s] mode changed(%d)\n", __func__, mode);

p_err_i2c_unlock:
	I2C_MUTEX_UNLOCK(cis->i2c_lock);
p_err:
	return ret;
}

/* TODO: Sensor set size sequence(sensor done, sensor stop, 3AA done in FW case */
int sensor_gw2_cis_set_size(struct v4l2_subdev *subdev, cis_shared_data *cis_data)
{
	int ret = 0;
	bool binning = false;
	u32 ratio_w = 0, ratio_h = 0, start_x = 0, start_y = 0, end_x = 0, end_y = 0;
	u32 even_x= 0, odd_x = 0, even_y = 0, odd_y = 0;
	struct i2c_client *client = NULL;
	struct is_cis *cis = NULL;
#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif
	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	if (unlikely(!cis_data)) {
		err("cis data is NULL");
		if (unlikely(!cis->cis_data)) {
			ret = -EINVAL;
			goto p_err;
		} else {
			cis_data = cis->cis_data;
		}
	}

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	/* Wait actual stream off */
	ret = sensor_gw2_wait_stream_off_status(cis_data);
	if (ret) {
		err("Must stream off\n");
		ret = -EINVAL;
		goto p_err;
	}

	binning = cis_data->binning;
	if (binning) {
		ratio_w = (SENSOR_GW2_MAX_WIDTH / cis_data->cur_width);
		ratio_h = (SENSOR_GW2_MAX_HEIGHT / cis_data->cur_height);
	} else {
		ratio_w = 1;
		ratio_h = 1;
	}

	if (((cis_data->cur_width * ratio_w) > SENSOR_GW2_MAX_WIDTH) ||
		((cis_data->cur_height * ratio_h) > SENSOR_GW2_MAX_HEIGHT)) {
		err("Config max sensor size over~!!\n");
		ret = -EINVAL;
		goto p_err;
	}

	/* 1. page_select */
	ret = is_sensor_write16(client, 0x6028, 0x2000);
	if (ret < 0)
		 goto p_err;

	/* 2. pixel address region setting */
	start_x = ((SENSOR_GW2_MAX_WIDTH - cis_data->cur_width * ratio_w) / 2) & (~0x1);
	start_y = ((SENSOR_GW2_MAX_HEIGHT - cis_data->cur_height * ratio_h) / 2) & (~0x1);
	end_x = start_x + (cis_data->cur_width * ratio_w - 1);
	end_y = start_y + (cis_data->cur_height * ratio_h - 1);

	if (!(end_x & (0x1)) || !(end_y & (0x1))) {
		err("Sensor pixel end address must odd\n");
		ret = -EINVAL;
		goto p_err;
	}

	ret = is_sensor_write16(client, 0x0344, start_x);
	if (ret < 0)
		 goto p_err;
	ret = is_sensor_write16(client, 0x0346, start_y);
	if (ret < 0)
		 goto p_err;
	ret = is_sensor_write16(client, 0x0348, end_x);
	if (ret < 0)
		 goto p_err;
	ret = is_sensor_write16(client, 0x034A, end_y);
	if (ret < 0)
		 goto p_err;

	/* 3. output address setting */
	ret = is_sensor_write16(client, 0x034C, cis_data->cur_width);
	if (ret < 0)
		 goto p_err;
	ret = is_sensor_write16(client, 0x034E, cis_data->cur_height);
	if (ret < 0)
		 goto p_err;

	/* If not use to binning, sensor image should set only crop */
	if (!binning) {
		dbg_sensor(1, "Sensor size set is not binning\n");
		goto p_err;
	}

	/* 4. sub sampling setting */
	even_x = 1;	/* 1: not use to even sampling */
	even_y = 1;
	odd_x = (ratio_w * 2) - even_x;
	odd_y = (ratio_h * 2) - even_y;

	ret = is_sensor_write16(client, 0x0380, even_x);
	if (ret < 0)
		 goto p_err;
	ret = is_sensor_write16(client, 0x0382, odd_x);
	if (ret < 0)
		 goto p_err;
	ret = is_sensor_write16(client, 0x0384, even_y);
	if (ret < 0)
		 goto p_err;
	ret = is_sensor_write16(client, 0x0386, odd_y);
	if (ret < 0)
		 goto p_err;

	/* 5. binnig setting */
	ret = is_sensor_write8(client, 0x0900, binning);	/* 1:  binning enable, 0: disable */
	if (ret < 0)
		goto p_err;
	ret = is_sensor_write8(client, 0x0901, (ratio_w << 4) | ratio_h);
	if (ret < 0)
		goto p_err;

	/* 6. scaling setting: but not use */
	/* scaling_digital_scaling */
	ret = is_sensor_write16(client, 0x0402, 0x1010);
	if (ret < 0)
		goto p_err;
	/* scaling_hbin_digital_binning_factor */
	ret = is_sensor_write16(client, 0x0404, 0x0010);
	if (ret < 0)
		goto p_err;
	/* scaling_tetracell_digital_binning_factor */
	ret = is_sensor_write16(client, 0x0400, 0x1010);
	if (ret < 0)
		goto p_err;

	cis_data->frame_time = (cis_data->line_readOut_time * cis_data->cur_height / 1000);
	cis->cis_data->rolling_shutter_skew = (cis->cis_data->cur_height - 1) * cis->cis_data->line_readOut_time;
	dbg_sensor(1, "[%s] frame_time(%d), rolling_shutter_skew(%lld)\n", __func__,
		cis->cis_data->frame_time, cis->cis_data->rolling_shutter_skew);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec) * 1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gw2_cis_stream_on(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;
	struct is_device_sensor *device;
#ifdef CAMERA_REAR2
	u32 mode;
#endif

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);

	device = (struct is_device_sensor *)v4l2_get_subdev_hostdata(subdev);
	FIMC_BUG(!device);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	is_vendor_set_mipi_clock(device);

	I2C_MUTEX_LOCK(cis->i2c_lock);
	ret = sensor_gw2_cis_group_param_hold_func(subdev, 0x00);
	if (ret < 0)
		err("group_param_hold_func failed at stream on");

#ifdef DEBUG_gw2_PLL
	{
	u16 pll;
	is_sensor_read16(client, 0x0300, &pll);
	dbg_sensor(1, "______ vt_pix_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x0302, &pll);
	dbg_sensor(1, "______ vt_sys_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x0304, &pll);
	dbg_sensor(1, "______ pre_pll_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x0306, &pll);
	dbg_sensor(1, "______ pll_multiplier(%x)\n", pll);
	is_sensor_read16(client, 0x0308, &pll);
	dbg_sensor(1, "______ op_pix_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x030a, &pll);
	dbg_sensor(1, "______ op_sys_clk_div(%x)\n", pll);

	is_sensor_read16(client, 0x030c, &pll);
	dbg_sensor(1, "______ secnd_pre_pll_clk_div(%x)\n", pll);
	is_sensor_read16(client, 0x030e, &pll);
	dbg_sensor(1, "______ secnd_pll_multiplier(%x)\n", pll);
	is_sensor_read16(client, 0x0340, &pll);
	dbg_sensor(1, "______ frame_length_lines(%x)\n", pll);
	is_sensor_read16(client, 0x0342, &pll);
	dbg_sensor(1, "______ line_length_pck(%x)\n", pll);
	}
#endif

	/* Sensor stream on */
	is_sensor_write16(client, 0x0100, 0x0100);

	info("%s\n", __func__);

#if 0 /* FIXME */
	/* WDR */
	if (is_vender_wdr_mode_on(cis_data))
		is_sensor_write8(client, 0x021E, 0x01);
	else
		is_sensor_write8(client, 0x021E, 0x00);
#endif
	cis_data->stream_on = true;

#if 0 /* For debug - fcount check */
	{
		u16 value, timeout, addr;
		timeout = 0;
		addr = 0x3000; /* smiaRegs_vendor_error_type */
		is_sensor_read16(client, addr, &value);
		info("%s - 0x%x(0x%x)\n", __func__, addr, value);
		while (timeout < 10) {
			addr = 0x0005; /* smiaRegs_rd_general_frame_count */
			is_sensor_read16(client, addr, &value);
			info("%s - 0x%x(0x%x)\n", __func__, addr, value);
			msleep(5);
			timeout++;
		}
	}
#endif
#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

#ifdef CAMERA_REAR2
	mode = cis_data->sens_config_index_cur;
	dbg_sensor(1, "[%s] sens_config_index_cur=%d\n", __func__, mode);

	switch (mode) {
	case SENSOR_GW2_4864x3648_30FPS:
	case SENSOR_GW2_4864x2736_30FPS:
	case SENSOR_GW2_4624x3468_30FPS:
	case SENSOR_GW2_4624x2604_30FPS:
	case SENSOR_GW2_2432x1824_30FPS:
		cis->cis_data->min_sync_frame_us_time = cis->cis_data->min_frame_us_time = 33333;
		break;
	default:
		break;
	}
#endif

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gw2_cis_stream_off(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;
	u8 cur_frame_count = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	dbg_sensor(1, "[MOD:D:%d] %s\n", cis->id, __func__);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	ret = sensor_gw2_cis_group_param_hold_func(subdev, 0x00);
	if (ret < 0)
		err("group_param_hold_func failed at stream off");

	is_sensor_read8(client, 0x0005, &cur_frame_count);
	info("%s: frame_count(0x%x) mode(%d) ex_mode(%d)\n", __func__, cur_frame_count,
		cis->cis_data->sens_config_index_cur, cis->cis_data->sens_config_ex_mode_cur);
	info("%s: backup param - frame_duration(%d), again(%d:%d), dgain(%d:%d), target_exp(%d:%d)",__func__,
		sensor_gw2_frame_duration_backup,
		sensor_gw2_again_backup.short_val, sensor_gw2_again_backup.long_val,
		sensor_gw2_dgain_backup.short_val, sensor_gw2_dgain_backup.long_val,
		sensor_gw2_target_exp_backup.short_val, sensor_gw2_target_exp_backup.long_val);

	/* Sensor stream off */
	is_sensor_write16(client, 0x0100, 0x00);

	info("%s\n", __func__);

	cis_data->stream_on = false;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gw2_cis_set_exposure_time(struct v4l2_subdev *subdev, struct ae_param *target_exposure)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u64 vt_pic_clk_freq_khz = 0;
	u16 long_coarse_int = 0;
	u16 short_coarse_int = 0;
	u32 line_length_pck = 0;
	u32 min_fine_int = 0;
	u16 coarse_integration_time_shifter = 0;

	u16 cit_shifter_array[17] = {0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5};
	u16 cit_shifter_val = 0;
	int cit_shifter_idx = 0;
	u8 cit_denom_array[6] = {1, 2, 4, 8, 16, 32};

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!target_exposure);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	sensor_gw2_target_exp_backup.short_val = target_exposure->short_val;
	sensor_gw2_target_exp_backup.long_val = target_exposure->long_val;

	if ((target_exposure->long_val <= 0) || (target_exposure->short_val <= 0)) {
		err("[%s] invalid target exposure(%d, %d)\n", __func__,
				target_exposure->long_val, target_exposure->short_val);
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	if (cis->long_term_mode.sen_strm_off_on_enable == false) {
		switch(cis_data->sens_config_index_cur) {
		case SENSOR_GW2_4624x2604_60FPS:
		case SENSOR_GW2_1920X1080_120FPS:
		case SENSOR_GW2_1920X1080_240FPS:
		case SENSOR_GW2_3200x2400_60FPS:
		case SENSOR_GW2_3200x1800_60FPS:
			if (MAX(target_exposure->long_val, target_exposure->short_val) > 160000) {
				cit_shifter_idx = MIN(MAX(MAX(target_exposure->long_val, target_exposure->short_val) / 160000, 0), 16);
				cit_shifter_val = MAX(cit_shifter_array[cit_shifter_idx], cis_data->frame_length_lines_shifter);
			} else {
				cit_shifter_val = (u16)(cis_data->frame_length_lines_shifter);
			}
			target_exposure->long_val = target_exposure->long_val / cit_denom_array[cit_shifter_val];
			target_exposure->short_val = target_exposure->short_val / cit_denom_array[cit_shifter_val];
			coarse_integration_time_shifter = ((cit_shifter_val<<8) & 0xFF00) + (cit_shifter_val & 0x00FF);
			break;
		default:
			if (MAX(target_exposure->long_val, target_exposure->short_val) > 400000) {
				cit_shifter_idx = MIN(MAX(MAX(target_exposure->long_val, target_exposure->short_val) / 400000, 0), 16);
				cit_shifter_val = MAX(cit_shifter_array[cit_shifter_idx], cis_data->frame_length_lines_shifter);
			} else {
				cit_shifter_val = (u16)(cis_data->frame_length_lines_shifter);
			}
			target_exposure->long_val = target_exposure->long_val / cit_denom_array[cit_shifter_val];
			target_exposure->short_val = target_exposure->short_val / cit_denom_array[cit_shifter_val];
			coarse_integration_time_shifter = ((cit_shifter_val<<8) & 0xFF00) + (cit_shifter_val & 0x00FF);
			break;
		}
	}

	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), target long(%d), short(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, target_exposure->long_val, target_exposure->short_val);

	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	line_length_pck = cis_data->line_length_pck;
	min_fine_int = cis_data->min_fine_integration_time;

	dbg_sensor(1, "[MOD:D:%d] %s, vt_pic_clk_freq_khz (%d), line_length_pck(%d), min_fine_int (%d)\n",
		cis->id, __func__, vt_pic_clk_freq_khz, line_length_pck, min_fine_int);

	long_coarse_int = ((target_exposure->long_val * vt_pic_clk_freq_khz) / 1000 - min_fine_int) / line_length_pck;
	short_coarse_int = ((target_exposure->short_val * vt_pic_clk_freq_khz) / 1000 - min_fine_int) / line_length_pck;

	if (long_coarse_int > cis_data->max_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), long coarse(%d) max(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, long_coarse_int, cis_data->max_coarse_integration_time);
		long_coarse_int = cis_data->max_coarse_integration_time;
	}

	if (short_coarse_int > cis_data->max_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), short coarse(%d) max(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, short_coarse_int, cis_data->max_coarse_integration_time);
		short_coarse_int = cis_data->max_coarse_integration_time;
	}

	if (long_coarse_int < cis_data->min_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), long coarse(%d) min(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, long_coarse_int, cis_data->min_coarse_integration_time);
		long_coarse_int = cis_data->min_coarse_integration_time;
	}

	if (short_coarse_int < cis_data->min_coarse_integration_time) {
		dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), short coarse(%d) min(%d)\n", cis->id, __func__,
			cis_data->sen_vsync_count, short_coarse_int, cis_data->min_coarse_integration_time);
		short_coarse_int = cis_data->min_coarse_integration_time;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);

	hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	/* WDR mode */
	if (is_vender_wdr_mode_on(cis_data)) {
		is_sensor_write16(client, 0x021E, 0x0100);
	} else {
		is_sensor_write16(client, 0x021E, 0x0000);
	}

	/* Short exposure */
	ret = is_sensor_write16(client, 0x0202, short_coarse_int);
	if (ret < 0)
		goto p_err_i2c_unlock;

	/* Long exposure */
	if (is_vender_wdr_mode_on(cis_data)) {
		ret = is_sensor_write16(client, 0x0226, long_coarse_int);
		if (ret < 0)
			goto p_err_i2c_unlock;
	}

	/* CIT shifter */
	if (cis->long_term_mode.sen_strm_off_on_enable == false) {
		ret = is_sensor_write16(client, 0x0704, coarse_integration_time_shifter);
		if (ret < 0)
			goto p_err_i2c_unlock;
	}

	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), vt_pic_clk_freq_khz (%d),"
		KERN_CONT "line_length_pck(%d), min_fine_int (%d)\n", cis->id, __func__,
		cis_data->sen_vsync_count, vt_pic_clk_freq_khz, line_length_pck, min_fine_int);
	dbg_sensor(1, "[MOD:D:%d] %s, vsync_cnt(%d), frame_length_lines(%#x),"
		KERN_CONT "long_coarse_int %#x, short_coarse_int %#x\n", cis->id, __func__,
		cis_data->sen_vsync_count, cis_data->frame_length_lines, long_coarse_int, short_coarse_int);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gw2_cis_get_min_exposure_time(struct v4l2_subdev *subdev, u32 *min_expo)
{
	int ret = 0;
	struct is_cis *cis = NULL;
	cis_shared_data *cis_data = NULL;
	u32 min_integration_time = 0;
	u32 min_coarse = 0;
	u32 min_fine = 0;
	u64 vt_pic_clk_freq_khz = 0;
	u32 line_length_pck = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!min_expo);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	if (vt_pic_clk_freq_khz == 0) {
		pr_err("[MOD:D:%d] %s, Invalid vt_pic_clk_freq_khz(%d)\n", cis->id, __func__, vt_pic_clk_freq_khz);
		goto p_err;
	}
	line_length_pck = cis_data->line_length_pck;
	min_coarse = cis_data->min_coarse_integration_time;
	min_fine = cis_data->min_fine_integration_time;

	min_integration_time = (u32)((u64)((line_length_pck * min_coarse) + min_fine) * 1000 / vt_pic_clk_freq_khz);
	*min_expo = min_integration_time;

	dbg_sensor(1, "[%s] min integration time %d\n", __func__, min_integration_time);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:
	return ret;
}

int sensor_gw2_cis_get_max_exposure_time(struct v4l2_subdev *subdev, u32 *max_expo)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;
	u32 max_integration_time = 0;
	u32 max_coarse_margin = 0;
	u32 max_fine_margin = 0;
	u32 max_coarse = 0;
	u32 max_fine = 0;
	u64 vt_pic_clk_freq_khz = 0;
	u32 line_length_pck = 0;
	u32 frame_length_lines = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!max_expo);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	if (vt_pic_clk_freq_khz == 0) {
		pr_err("[MOD:D:%d] %s, Invalid vt_pic_clk_freq_khz(%d)\n", cis->id, __func__, vt_pic_clk_freq_khz);
		goto p_err;
	}
	line_length_pck = cis_data->line_length_pck;
	frame_length_lines = cis_data->frame_length_lines;

	max_coarse_margin = cis_data->max_margin_coarse_integration_time;
	max_fine_margin = line_length_pck - cis_data->min_fine_integration_time;
	max_coarse = frame_length_lines - max_coarse_margin;
	max_fine = cis_data->max_fine_integration_time;

	max_integration_time = (u32)((u64)((line_length_pck * max_coarse) + max_fine) * 1000 / vt_pic_clk_freq_khz);

	*max_expo = max_integration_time;

	/* TODO: Is this values update hear? */
	cis_data->max_margin_fine_integration_time = max_fine_margin;
	cis_data->max_coarse_integration_time = max_coarse;

	dbg_sensor(1, "[%s] max integration time %d, max margin fine integration %d, max coarse integration %d\n",
			__func__, max_integration_time, cis_data->max_margin_fine_integration_time, cis_data->max_coarse_integration_time);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:

	return ret;
}

int sensor_gw2_cis_adjust_frame_duration(struct v4l2_subdev *subdev,
						u32 input_exposure_time,
						u32 *target_duration)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u64 vt_pic_clk_freq_khz = 0;
	u32 line_length_pck = 0;
	u32 frame_length_lines = 0;
	u32 frame_duration = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!target_duration);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	if (input_exposure_time == 0) {
		input_exposure_time  = cis_data->min_frame_us_time;
		info("[%s] Not proper exposure time(0), so apply min frame duration to exposure time forcely!!!(%d)\n",
			__func__, cis_data->min_frame_us_time);
	}

	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	line_length_pck = cis_data->line_length_pck;
	frame_length_lines = (u32)(((vt_pic_clk_freq_khz * input_exposure_time) / 1000
						- cis_data->min_fine_integration_time) / line_length_pck);
	frame_length_lines += cis_data->max_margin_coarse_integration_time;

	frame_duration = (u32)(((u64)frame_length_lines * line_length_pck) * 1000 / vt_pic_clk_freq_khz);

	dbg_sensor(1, "[%s](vsync cnt = %d) input exp(%d), adj duration, frame duraion(%d), min_frame_us(%d)\n",
			__func__, cis_data->sen_vsync_count, input_exposure_time, frame_duration, cis_data->min_frame_us_time);
	dbg_sensor(1, "[%s](vsync cnt = %d) adj duration, frame duraion(%d), min_frame_us(%d)\n",
			__func__, cis_data->sen_vsync_count, frame_duration, cis_data->min_frame_us_time);

	*target_duration = MAX(frame_duration, cis_data->min_frame_us_time);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gw2_cis_set_frame_duration(struct v4l2_subdev *subdev, u32 frame_duration)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u64 vt_pic_clk_freq_khz = 0;
	u32 line_length_pck = 0;
	u16 frame_length_lines = 0;
	u8 frame_length_lines_shifter = 0;

	u8 fll_shifter_array[17] = {0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5};
	int fll_shifter_idx = 0;
	u8 fll_denom_array[6] = {1, 2, 4, 8, 16, 32};

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	cis_data = cis->cis_data;

	if (frame_duration < cis_data->min_frame_us_time) {
		dbg_sensor(1, "frame duration is less than min(%d)\n", frame_duration);
		frame_duration = cis_data->min_frame_us_time;
	}

	sensor_gw2_frame_duration_backup = frame_duration;
	cis_data->cur_frame_us_time = frame_duration;

	if (cis->long_term_mode.sen_strm_off_on_enable == false) {
		switch(cis_data->sens_config_index_cur) {
		case SENSOR_GW2_4624x2604_60FPS:
		case SENSOR_GW2_1920X1080_120FPS:
		case SENSOR_GW2_1920X1080_240FPS:
		case SENSOR_GW2_3200x2400_60FPS:
		case SENSOR_GW2_3200x1800_60FPS:
			if (frame_duration > 160000) {
				fll_shifter_idx = MIN(MAX(frame_duration / 160000, 0), 16);
				frame_length_lines_shifter = fll_shifter_array[fll_shifter_idx];
				frame_duration = frame_duration / fll_denom_array[frame_length_lines_shifter];
			} else {
				frame_length_lines_shifter = 0x0;
			}
			break;
		default:
			if (frame_duration > 400000) {
				fll_shifter_idx = MIN(MAX(frame_duration / 400000, 0), 16);
				frame_length_lines_shifter = fll_shifter_array[fll_shifter_idx];
				frame_duration = frame_duration / fll_denom_array[frame_length_lines_shifter];
			} else {
				frame_length_lines_shifter = 0x0;
			}
			break;
		}
	}

	vt_pic_clk_freq_khz = cis_data->pclk / 1000;
	line_length_pck = cis_data->line_length_pck;

	frame_length_lines = (u16)((vt_pic_clk_freq_khz * frame_duration) / (line_length_pck * 1000));

	dbg_sensor(1, "[MOD:D:%d] %s, vt_pic_clk_freq_khz(%#x) frame_duration = %d us,"
		KERN_CONT "(line_length_pck%#x), frame_length_lines(%#x)\n",
		cis->id, __func__, vt_pic_clk_freq_khz, frame_duration, line_length_pck, frame_length_lines);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	ret = is_sensor_write16(client, 0x0340, frame_length_lines);
	if (ret < 0)
		goto p_err_i2c_unlock;

	/* frame duration shifter */
	if (cis->long_term_mode.sen_strm_off_on_enable == false) {
		ret = is_sensor_write8(client, 0x0702, frame_length_lines_shifter);
		if (ret < 0)
			goto p_err_i2c_unlock;
	}

	cis_data->frame_length_lines = frame_length_lines;
	cis_data->max_coarse_integration_time = cis_data->frame_length_lines - cis_data->max_margin_coarse_integration_time;
	cis_data->frame_length_lines_shifter = frame_length_lines_shifter;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gw2_cis_set_frame_rate(struct v4l2_subdev *subdev, u32 min_fps)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 frame_duration = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	if (min_fps > cis_data->max_fps) {
		err("[MOD:D:%d] %s, request FPS is too high(%d), set to max(%d)\n",
			cis->id, __func__, min_fps, cis_data->max_fps);
		min_fps = cis_data->max_fps;
	}

	if (min_fps == 0) {
		err("[MOD:D:%d] %s, request FPS is 0, set to min FPS(1)\n",
			cis->id, __func__);
		min_fps = 1;
	}

	frame_duration = (1 * 1000 * 1000) / min_fps;

	dbg_sensor(1, "[MOD:D:%d] %s, set FPS(%d), frame duration(%d)\n",
			cis->id, __func__, min_fps, frame_duration);

	ret = sensor_gw2_cis_set_frame_duration(subdev, frame_duration);
	if (ret < 0) {
		err("[MOD:D:%d] %s, set frame duration is fail(%d)\n",
			cis->id, __func__, ret);
		goto p_err;
	}

#ifdef CAMERA_REAR2
	cis_data->min_frame_us_time = MAX(frame_duration, cis_data->min_sync_frame_us_time);
#else
	cis_data->min_frame_us_time = frame_duration;
#endif

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err:

	return ret;
}

int sensor_gw2_cis_adjust_analog_gain(struct v4l2_subdev *subdev, u32 input_again, u32 *target_permile)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

	u32 again_code = 0;
	u32 again_permile = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!target_permile);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	again_code = sensor_cis_calc_again_code(input_again);

	if (again_code > cis_data->max_analog_gain[0]) {
		again_code = cis_data->max_analog_gain[0];
	} else if (again_code < cis_data->min_analog_gain[0]) {
		again_code = cis_data->min_analog_gain[0];
	}

	again_permile = sensor_cis_calc_again_permile(again_code);

	dbg_sensor(1, "[%s] min again(%d), max(%d), input_again(%d), code(%d), permile(%d)\n", __func__,
			cis_data->max_analog_gain[0],
			cis_data->min_analog_gain[0],
			input_again,
			again_code,
			again_permile);

	*target_permile = again_permile;

	return ret;
}

int sensor_gw2_cis_set_analog_gain(struct v4l2_subdev *subdev, struct ae_param *again)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 analog_gain = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	sensor_gw2_again_backup.short_val = again->short_val;
	sensor_gw2_again_backup.long_val = again->long_val;

	analog_gain = (u16)sensor_cis_calc_again_code(again->val);

	if (analog_gain < cis->cis_data->min_analog_gain[0]) {
		info("[%s] not proper analog_gain value, reset to min_analog_gain\n", __func__);
		analog_gain = cis->cis_data->min_analog_gain[0];
	}

	if (analog_gain > cis->cis_data->max_analog_gain[0]) {
		info("[%s] not proper analog_gain value, reset to max_analog_gain\n", __func__);
		analog_gain = cis->cis_data->max_analog_gain[0];
	}

	dbg_sensor(1, "[MOD:D:%d] %s(vsync cnt = %d), input_again = %d us, analog_gain(%#x)\n",
		cis->id, __func__, cis->cis_data->sen_vsync_count, again->val, analog_gain);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	ret = is_sensor_write16(client, 0x0204, analog_gain);
	if (ret < 0)
		goto p_err_i2c_unlock;

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gw2_cis_get_analog_gain(struct v4l2_subdev *subdev, u32 *again)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 analog_gain = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);

	hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	ret = is_sensor_read16(client, 0x0204, &analog_gain);
	if (ret < 0)
		goto p_err_i2c_unlock;

	*again = sensor_cis_calc_again_permile(analog_gain);

	dbg_sensor(1, "[MOD:D:%d] %s, cur_again = %d us, analog_gain(%#x)\n",
			cis->id, __func__, *again, analog_gain);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gw2_cis_get_min_analog_gain(struct v4l2_subdev *subdev, u32 *min_again)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!min_again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	cis_data->min_analog_gain[0] = 0x20; /* x1, gain=x/0x20 */
	cis_data->min_analog_gain[1] = sensor_cis_calc_again_permile(cis_data->min_analog_gain[0]);

	*min_again = cis_data->min_analog_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->min_analog_gain[0], cis_data->min_analog_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gw2_cis_get_max_analog_gain(struct v4l2_subdev *subdev, u32 *max_again)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!max_again);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	cis_data->max_analog_gain[0] = 0x200; /* x16, gain=x/0x20 */
	cis_data->max_analog_gain[1] = sensor_cis_calc_again_permile(cis_data->max_analog_gain[0]);

	*max_again = cis_data->max_analog_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->max_analog_gain[0], cis_data->max_analog_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gw2_cis_set_digital_gain(struct v4l2_subdev *subdev, struct ae_param *dgain)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;
	cis_shared_data *cis_data;

	u16 long_gain = 0;
	u16 short_gain = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	sensor_gw2_dgain_backup.short_val = dgain->short_val;
	sensor_gw2_dgain_backup.long_val = dgain->long_val;

	cis_data = cis->cis_data;

	long_gain = (u16)sensor_cis_calc_dgain_code(dgain->long_val);
	short_gain = (u16)sensor_cis_calc_dgain_code(dgain->short_val);

	if (long_gain < cis->cis_data->min_digital_gain[0]) {
		info("[%s] not proper long_gain value, reset to min_digital_gain\n", __func__);
		long_gain = cis->cis_data->min_digital_gain[0];
	}
	if (long_gain > cis->cis_data->max_digital_gain[0]) {
		info("[%s] not proper long_gain value, reset to max_digital_gain\n", __func__);
		long_gain = cis->cis_data->max_digital_gain[0];
	}

	if (short_gain < cis->cis_data->min_digital_gain[0]) {
		info("[%s] not proper short_gain value, reset to min_digital_gain\n", __func__);
		short_gain = cis->cis_data->min_digital_gain[0];
	}
	if (short_gain > cis->cis_data->max_digital_gain[0]) {
		info("[%s] not proper short_gain value, reset to max_digital_gain\n", __func__);
		short_gain = cis->cis_data->max_digital_gain[0];
	}

	dbg_sensor(1, "[MOD:D:%d] %s(vsync cnt = %d), input_dgain = %d/%d us, long_gain(%#x), short_gain(%#x)\n",
			cis->id, __func__, cis->cis_data->sen_vsync_count, dgain->long_val, dgain->short_val, long_gain, short_gain);

	I2C_MUTEX_LOCK(cis->i2c_lock);

	hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	/* DGain WDR Mode */
	if (is_vender_wdr_mode_on(cis_data)) {
		is_sensor_write16(client, 0x020D, 0x0000);
	} else {
		is_sensor_write16(client, 0x020D, 0x0002);
	}

	/* Short digital gain */
	ret = is_sensor_write16(client, 0x020E, short_gain);
	if (ret < 0)
		goto p_err_i2c_unlock;

	/* Long digital gain */
	if (is_vender_wdr_mode_on(cis_data)) {
		ret = is_sensor_write16(client, 0x0230, long_gain);
		if (ret < 0)
			goto p_err_i2c_unlock;
	}

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gw2_cis_get_digital_gain(struct v4l2_subdev *subdev, u32 *dgain)
{
	int ret = 0;
	int hold = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	u16 digital_gain = 0;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);

	client = cis->client;
	if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	I2C_MUTEX_LOCK(cis->i2c_lock);

	hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x01);
	if (hold < 0) {
		ret = hold;
		goto p_err_i2c_unlock;
	}

	ret = is_sensor_read16(client, 0x020E, &digital_gain);
	if (ret < 0)
		goto p_err_i2c_unlock;

	*dgain = sensor_cis_calc_dgain_permile(digital_gain);

	dbg_sensor(1, "[MOD:D:%d] %s, cur_dgain = %d us, digital_gain(%#x)\n",
			cis->id, __func__, *dgain, digital_gain);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

p_err_i2c_unlock:
	if (hold > 0) {
		hold = sensor_gw2_cis_group_param_hold_func(subdev, 0x00);
		if (hold < 0)
			ret = hold;
	}

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gw2_cis_get_min_digital_gain(struct v4l2_subdev *subdev, u32 *min_dgain)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!min_dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	/* gw2 cannot read min/max digital gain */
	cis_data->min_digital_gain[0] = 0x0100;
	cis_data->min_digital_gain[1] = sensor_cis_calc_dgain_permile(cis_data->min_digital_gain[0]);

	*min_dgain = cis_data->min_digital_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->min_digital_gain[0], cis_data->min_digital_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gw2_cis_set_adjust_sync(struct v4l2_subdev *subdev, u32 sync)
{
	int ret = 0;
	struct is_cis *cis;
	struct i2c_client *client;

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);

	client = cis->client;
    if (unlikely(!client)) {
		err("client is NULL");
		ret = -EINVAL;
		goto p_err;
	}

	/* Adjust Sync */
	I2C_MUTEX_LOCK(cis->i2c_lock);

	ret = is_sensor_write8(client, 0x0B32, 0x01); // same with dual sync enable setting

	I2C_MUTEX_UNLOCK(cis->i2c_lock);

p_err:
	return ret;
}

int sensor_gw2_cis_get_max_digital_gain(struct v4l2_subdev *subdev, u32 *max_dgain)
{
	int ret = 0;
	struct is_cis *cis;
	cis_shared_data *cis_data;

#ifdef DEBUG_SENSOR_TIME
	struct timeval st, end;
	do_gettimeofday(&st);
#endif

	FIMC_BUG(!subdev);
	FIMC_BUG(!max_dgain);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);

	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	cis_data = cis->cis_data;

	/* gw2 cannot read min/max digital gain */
	cis_data->max_digital_gain[0] = 0x8000;
	cis_data->max_digital_gain[1] = sensor_cis_calc_dgain_permile(cis_data->max_digital_gain[0]);

	*max_dgain = cis_data->max_digital_gain[1];

	dbg_sensor(1, "[%s] code %d, permile %d\n", __func__,
		cis_data->max_digital_gain[0], cis_data->max_digital_gain[1]);

#ifdef DEBUG_SENSOR_TIME
	do_gettimeofday(&end);
	dbg_sensor(1, "[%s] time %lu us\n", __func__, (end.tv_sec - st.tv_sec)*1000000 + (end.tv_usec - st.tv_usec));
#endif

	return ret;
}

int sensor_gw2_cis_recover_stream_on(struct v4l2_subdev *subdev)
{
	int ret = 0;
	struct is_cis *cis = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	info("%s start\n", __func__);

	ret = sensor_gw2_cis_set_global_setting(subdev);
	if (ret < 0) goto p_err;
	ret = sensor_gw2_cis_mode_change(subdev, cis->cis_data->sens_config_index_cur);
	if (ret < 0) goto p_err;
	ret = sensor_gw2_cis_set_frame_duration(subdev, sensor_gw2_frame_duration_backup);
	if (ret < 0) goto p_err;
	ret = sensor_gw2_cis_set_analog_gain(subdev, &sensor_gw2_again_backup);
	if (ret < 0) goto p_err;
	ret = sensor_gw2_cis_set_digital_gain(subdev, &sensor_gw2_dgain_backup);
	if (ret < 0) goto p_err;
	ret = sensor_gw2_cis_set_exposure_time(subdev, &sensor_gw2_target_exp_backup);
	if (ret < 0) goto p_err;
	ret = sensor_gw2_cis_stream_on(subdev);
	if (ret < 0) goto p_err;
	ret = sensor_cis_wait_streamon(subdev);
	if (ret < 0) goto p_err;

	info("%s end\n", __func__);
p_err:
	return ret;
}

int sensor_gw2_cis_set_fake_retention(struct v4l2_subdev *subdev, bool enable)
{
	struct is_cis *cis = NULL;

	FIMC_BUG(!subdev);

	cis = (struct is_cis *)v4l2_get_subdevdata(subdev);
	FIMC_BUG(!cis);
	FIMC_BUG(!cis->cis_data);

	info("%s(%d)\n", __func__, enable);
	sensor_gw2_fake_retention_status = enable;

	return 0;
}

static struct is_cis_ops cis_ops = {
	.cis_init = sensor_gw2_cis_init,
	.cis_log_status = sensor_gw2_cis_log_status,
	.cis_group_param_hold = sensor_gw2_cis_group_param_hold,
	.cis_set_global_setting = sensor_gw2_cis_set_global_setting,
	.cis_mode_change = sensor_gw2_cis_mode_change,
	.cis_set_size = sensor_gw2_cis_set_size,
	.cis_stream_on = sensor_gw2_cis_stream_on,
	.cis_stream_off = sensor_gw2_cis_stream_off,
	.cis_set_exposure_time = sensor_gw2_cis_set_exposure_time,
	.cis_get_min_exposure_time = sensor_gw2_cis_get_min_exposure_time,
	.cis_get_max_exposure_time = sensor_gw2_cis_get_max_exposure_time,
	.cis_adjust_frame_duration = sensor_gw2_cis_adjust_frame_duration,
	.cis_set_frame_duration = sensor_gw2_cis_set_frame_duration,
	.cis_set_frame_rate = sensor_gw2_cis_set_frame_rate,
	.cis_adjust_analog_gain = sensor_gw2_cis_adjust_analog_gain,
	.cis_set_analog_gain = sensor_gw2_cis_set_analog_gain,
	.cis_get_analog_gain = sensor_gw2_cis_get_analog_gain,
	.cis_get_min_analog_gain = sensor_gw2_cis_get_min_analog_gain,
	.cis_get_max_analog_gain = sensor_gw2_cis_get_max_analog_gain,
	.cis_set_digital_gain = sensor_gw2_cis_set_digital_gain,
	.cis_get_digital_gain = sensor_gw2_cis_get_digital_gain,
	.cis_get_min_digital_gain = sensor_gw2_cis_get_min_digital_gain,
	.cis_get_max_digital_gain = sensor_gw2_cis_get_max_digital_gain,
	.cis_compensate_gain_for_extremely_br = sensor_cis_compensate_gain_for_extremely_br,
	.cis_wait_streamoff = sensor_cis_wait_streamoff,
	.cis_wait_streamon = sensor_cis_wait_streamon,
	.cis_data_calculation = sensor_gw2_cis_data_calc,
	.cis_set_adjust_sync = sensor_gw2_cis_set_adjust_sync,
#ifdef CAMERA_REAR2_SENSOR_SHIFT_CROP
	.cis_update_pdaf_tail_size = sensor_gw2_cis_update_pdaf_tail_size,
#endif
	.cis_check_rev_on_init = sensor_cis_check_rev_on_init,
	.cis_set_initial_exposure = sensor_cis_set_initial_exposure,
	.cis_recover_stream_on = sensor_gw2_cis_recover_stream_on,
	.cis_set_fake_retention = sensor_gw2_cis_set_fake_retention,
};

static int cis_gw2_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int ret = 0;
	struct is_core *core = NULL;
	struct v4l2_subdev *subdev_cis = NULL;
	struct is_cis *cis = NULL;
	struct is_device_sensor *device = NULL;
	struct is_device_sensor_peri *sensor_peri = NULL;
	u32 sensor_id = 0;
	char const *setfile;
	struct device *dev;
	struct device_node *dnode;
	int i, index;
	const int *verify_sensor_mode = NULL;
	int verify_sensor_mode_size = 0;

	FIMC_BUG(!client);
	FIMC_BUG(!is_dev);

	core = (struct is_core *)dev_get_drvdata(is_dev);
	if (!core) {
		probe_info("core device is not yet probed");
		return -EPROBE_DEFER;
	}

	dev = &client->dev;
	dnode = dev->of_node;

	ret = of_property_read_u32(dnode, "id", &sensor_id);
	if (ret) {
		err("sensor id read is fail(%d)", ret);
		goto p_err;
	}

	probe_info("%s sensor id %d\n", __func__, sensor_id);

	device = &core->sensor[sensor_id];

	sensor_peri = find_peri_by_cis_id(device, SENSOR_NAME_S5KGW2);
	if (!sensor_peri) {
		probe_info("sensor peri is net yet probed");
		return -EPROBE_DEFER;
	}

	cis = &sensor_peri->cis;
	if (!cis) {
		err("cis is NULL");
		ret = -ENOMEM;
		goto p_err;
	}

	subdev_cis = kzalloc(sizeof(struct v4l2_subdev), GFP_KERNEL);
	if (!subdev_cis) {
		probe_err("subdev_cis is NULL");
		ret = -ENOMEM;
		goto p_err;
	}
	sensor_peri->subdev_cis = subdev_cis;

	sensor_gw2_fake_retention_status = false;
	cis->id = SENSOR_NAME_S5KGW2;
	cis->subdev = subdev_cis;
	cis->device = 0;
	cis->client = client;
	sensor_peri->module->client = cis->client;
	cis->ctrl_delay = N_PLUS_TWO_FRAME;
	cis->mipi_clock_index_cur = CAM_MIPI_NOT_INITIALIZED;
	cis->mipi_clock_index_new = CAM_MIPI_NOT_INITIALIZED;
	cis->cis_data = kzalloc(sizeof(cis_shared_data), GFP_KERNEL);
	if (!cis->cis_data) {
		err("cis_data is NULL");
		ret = -ENOMEM;
		goto p_err;
	}
	cis->cis_ops = &cis_ops;

	if (of_property_read_string(dnode, "setfile", &setfile)) {
		err("setfile index read fail(%d), take default setfile!!", ret);
		setfile = "default";
	}

	if (strcmp(setfile, "default") == 0 || strcmp(setfile, "setA") == 0) {
		probe_info("%s setfile_A\n", __func__);
		sensor_gw2_tnp = sensor_GW2_setfile_A_Tnp;
		sensor_gw2_tnp_size = ARRAY_SIZE(sensor_GW2_setfile_A_Tnp);
		sensor_gw2_global = sensor_GW2_setfile_A_Global;
		sensor_gw2_global_size = ARRAY_SIZE(sensor_GW2_setfile_A_Global);
		sensor_gw2_setfiles = sensor_gw2_setfiles_A;
		sensor_gw2_setfile_sizes = sensor_gw2_setfile_A_sizes;
		sensor_gw2_pllinfos = sensor_gw2_pllinfos_A;
		sensor_gw2_max_setfile_num = ARRAY_SIZE(sensor_gw2_setfiles_A);
		sensor_gw2_dualsync_slave = sensor_gw2_dual_slave_A_settings;
		sensor_gw2_dualsync_slave_size = ARRAY_SIZE(sensor_gw2_dual_slave_A_settings);
		sensor_gw2_dualsync_single = sensor_gw2_dual_single_A_settings;
		sensor_gw2_dualsync_single_size = ARRAY_SIZE(sensor_gw2_dual_single_A_settings);
		cis->mipi_sensor_mode = sensor_gw2_setfile_A_mipi_sensor_mode;
		cis->mipi_sensor_mode_size = ARRAY_SIZE(sensor_gw2_setfile_A_mipi_sensor_mode);
		verify_sensor_mode = sensor_gw2_setfile_A_verify_sensor_mode;
		verify_sensor_mode_size = ARRAY_SIZE(sensor_gw2_setfile_A_verify_sensor_mode);
	}
#if 0
	else if (strcmp(setfile, "setB") == 0) {
		probe_info("%s setfile_B\n", __func__);
		sensor_gw2_global = sensor_gw2_setfile_B_Global;
		sensor_gw2_global_size = ARRAY_SIZE(sensor_gw2_setfile_B_Global);
		sensor_gw2_setfiles = sensor_gw2_setfiles_B;
		sensor_gw2_setfile_sizes = sensor_gw2_setfile_B_sizes;
		sensor_gw2_pllinfos = sensor_gw2_pllinfos_B;
		sensor_gw2_max_setfile_num = ARRAY_SIZE(sensor_gw2_setfiles_B);
		sensor_gw2_dualsync_slave = sensor_gw2_dual_slave_B_settings;
		sensor_gw2_dualsync_slave_size = ARRAY_SIZE(sensor_gw2_dual_slave_B_settings);
	}
#endif
	else {
		err("%s setfile index out of bound, take default (setfile_A)", __func__);
		sensor_gw2_tnp = sensor_GW2_setfile_A_Tnp;
		sensor_gw2_tnp_size = ARRAY_SIZE(sensor_GW2_setfile_A_Tnp);
		sensor_gw2_global = sensor_GW2_setfile_A_Global;
		sensor_gw2_global_size = ARRAY_SIZE(sensor_GW2_setfile_A_Global);
		sensor_gw2_setfiles = sensor_gw2_setfiles_A;
		sensor_gw2_setfile_sizes = sensor_gw2_setfile_A_sizes;
		sensor_gw2_pllinfos = sensor_gw2_pllinfos_A;
		sensor_gw2_max_setfile_num = ARRAY_SIZE(sensor_gw2_setfiles_A);
		sensor_gw2_dualsync_slave = sensor_gw2_dual_slave_A_settings;
		sensor_gw2_dualsync_slave_size = ARRAY_SIZE(sensor_gw2_dual_slave_A_settings);
		sensor_gw2_dualsync_single = sensor_gw2_dual_single_A_settings;
		sensor_gw2_dualsync_single_size = ARRAY_SIZE(sensor_gw2_dual_single_A_settings);
	}

	/* belows are depend on sensor cis. MUST check sensor spec */
	cis->bayer_order = OTF_INPUT_ORDER_BAYER_GR_BG;

	if (of_property_read_bool(dnode, "sensor_f_number")) {
		ret = of_property_read_u32(dnode, "sensor_f_number", &cis->aperture_num);
		if (ret) {
			warn("f-number read is fail(%d)",ret);
		}
	} else {
		cis->aperture_num = F2_0;
	}

	probe_info("%s f-number %d\n", __func__, cis->aperture_num);

	cis->use_dgain = true;
	cis->hdr_ctrl_by_again = false;

	ret = of_property_read_string(dnode, "setfile", &setfile);
	if (ret) {
		err("setfile index read fail(%d), take default setfile!!", ret);
		setfile = "default";
	}

	cis->use_initial_ae = of_property_read_bool(dnode, "use_initial_ae");
	probe_info("%s use initial_ae(%d)\n", __func__, cis->use_initial_ae);

	cis->vendor_use_adaptive_mipi = of_property_read_bool(dnode, "vendor_use_adaptive_mipi");
	probe_info("%s vendor_use_adaptive_mipi(%d)\n", __func__, cis->vendor_use_adaptive_mipi);

	v4l2_i2c_subdev_init(subdev_cis, client, &subdev_ops);
	v4l2_set_subdevdata(subdev_cis, cis);
	v4l2_set_subdev_hostdata(subdev_cis, device);
	snprintf(subdev_cis->name, V4L2_SUBDEV_NAME_SIZE, "cis-subdev.%d", cis->id);

	sensor_cis_parse_dt(dev, cis->subdev);
	clear_bit(IS_EFS_STATE_READ, &efs_info.efs_state);

	if (cis->vendor_use_adaptive_mipi) {
		for (i = 0; i < verify_sensor_mode_size; i++) {
			index = verify_sensor_mode[i];

			if (index >= cis->mipi_sensor_mode_size || index < 0) {
				panic("wrong mipi_sensor_mode index");
				break;
			}

			if (is_vendor_verify_mipi_channel(cis->mipi_sensor_mode[index].mipi_channel,
						cis->mipi_sensor_mode[index].mipi_channel_size)) {
				panic("wrong mipi channel");
				break;
			}
		}
	}

	probe_info("%s done\n", __func__);

p_err:
	return ret;
}

static const struct of_device_id sensor_cis_gw2_match[] = {
	{
		.compatible = "samsung,exynos-is-cis-gw2",
	},
	{},
};
MODULE_DEVICE_TABLE(of, sensor_cis_gw2_match);

static const struct i2c_device_id sensor_cis_gw2_idt[] = {
	{ SENSOR_NAME, 0 },
	{},
};

static struct i2c_driver sensor_cis_gw2_driver = {
	.probe	= cis_gw2_probe,
	.driver = {
		.name	= SENSOR_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = sensor_cis_gw2_match,
		.suppress_bind_attrs = true,
	},
	.id_table = sensor_cis_gw2_idt
};

#ifdef MODULE
builtin_i2c_driver(sensor_cis_gw2_driver);
#else
static int __init sensor_cis_gw2_init(void)
{
	int ret;

	ret = i2c_add_driver(&sensor_cis_gw2_driver);
	if (ret)
		err("failed to add %s driver: %d\n",
			sensor_cis_gw2_driver.driver.name, ret);

	return ret;
}
late_initcall_sync(sensor_cis_gw2_init);
#endif

MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: fimc-is");
