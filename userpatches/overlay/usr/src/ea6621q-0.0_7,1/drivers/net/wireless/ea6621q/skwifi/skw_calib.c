// SPDX-License-Identifier: GPL-2.0

/******************************************************************************
 *
 * Copyright (C) 2020 SeekWave Technology Co.,Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 ******************************************************************************/

#include <linux/crc32c.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/firmware.h>

#include "skw_core.h"
#include "skw_msg.h"
#include "skw_log.h"
#include "skw_cfg80211.h"
#include "skw_util.h"
#include "skw_calib.h"

static int skw_dpd_ilc_download_gear_param(struct wiphy *wiphy,
			int gear, const u8 *data, u32 t_size)
{
	int ret = 0;
	int i;
	struct skw_ilc_cali_param ilc_param;
	struct skw_core *skw = NULL;

	skw = wiphy_priv(wiphy);

	for (i = 0; i < 64; i++) {
		ilc_param.gear = gear;
		ilc_param.len = 1 << 10;
		ilc_param.seq = i;

		memcpy(ilc_param.data, data + (i << 10), 1024);

		if ((i + 1) == 64)
			ilc_param.end = 1;
		else
			ilc_param.end = 0;
		skw_dbg("dbg gear: %d, seq: %d len:%d end:%d\n", ilc_param.gear,
			ilc_param.seq, ilc_param.len, ilc_param.end);

		ret = skw_msg_xmit(wiphy, 0, SKW_CMD_DPD_ILC_GEAR_PARAM,
				   &ilc_param, sizeof(ilc_param), NULL, 0);
		if (ret) {
			skw_err("failed, ret: %d, gear: %d, seq: %d\n",
				ret, gear, i);

			break;
		}
	}

	return ret;
}

static int skw_dpd_ilc_download_matrix_param(struct wiphy *wiphy,
					const u8 *data, u32 t_size)
{
	int ret = 0;

	ret = skw_msg_xmit(wiphy, 0, SKW_CMD_DPD_ILC_MARTIX_PARAM,
			   (void *)data, t_size, NULL, 0);

	return ret;
}

static int skw_dpd_cali_download(struct wiphy *wiphy,
	const char *dpd_name, const char *matrix_name, const char *store_file)
{
	const struct firmware *ilc;
	const struct firmware *mtx;
	struct skw_core *skw = NULL;
	int ret = 0;
	struct file *fp;
	int gear, i;
	struct skw_ilc_result_param *para;

	skw = wiphy_priv(wiphy);
	ret = request_firmware(&mtx, matrix_name, &wiphy->dev);
	if (ret) {
		skw_err("matrix req fail\n");
		goto ret;
	}

	if (mtx->size < SKW_DPD_MATRIX_LEN) {
		ret = -EINVAL;
		skw_err("matrix not enough\n");
		goto relese_mtx;
	}

	ret = skw_dpd_ilc_download_matrix_param(wiphy, mtx->data,
						SKW_DPD_MATRIX_LEN);
	if (ret != 0) {
		skw_err("dpd matrix msg fail\n");
		goto relese_mtx;
	}

	ret = request_firmware(&ilc, dpd_name, &wiphy->dev);
	if (ret) {
		skw_err("dpd fw req fail\n");
		goto relese_mtx;
	}

	if (ilc->size < SKW_GEAR_NUM * SKW_DPD_PARAM_LEN) {
		ret = -EINVAL;
		skw_err("ilc not enough\n");
		goto relese_ilc;
	}

	para = skw->dpd.resource;
	for (i = 0; i < SKW_DPD_CHN_CNT; i++)
		para[i].invalid = 1;

	for (gear = 0; gear < SKW_GEAR_NUM; gear++) {
		ret = skw_dpd_ilc_download_gear_param(wiphy, gear + 4,
					ilc->data + gear * SKW_DPD_PARAM_LEN,
					SKW_DPD_PARAM_LEN);
		if (ret != 0) {
			skw_err("dpd cali msg fail\n");
			goto relese_ilc;
		}

		skw_dbg("wait dpd_cmpl\n");
		if (wait_for_completion_timeout(&skw->dpd.cmpl,
					SKW_DPD_ILC_TIMEOUT) == 0) {
			skw_err("dpd cali time out\n");
			ret = -ETIME;
			goto relese_ilc;
		}
	}

	fp = skw_file_open(store_file, O_RDWR|O_CREAT, 0666);
	if (!fp) {
		skw_err("skw_file_open Fail\n");
		ret = -ENOENT;
		goto relese_ilc;
	}

	*(u32 *)(skw->dpd.resource + SKW_DPD_RESOURCE_DATA_CNT)
		= crc32c(0, skw->dpd.resource, SKW_DPD_RESOURCE_DATA_CNT);

	ret = skw_file_write(fp, skw->dpd.resource, skw->dpd.size, 0);
	if (ret < 0)
		skw_err("dpd write res fail %d", ret);

	skw_file_close(fp);

relese_ilc:
	release_firmware(ilc);
relese_mtx:
	release_firmware(mtx);
ret:
	return ret;
}

static int skw_dpd_chn_to_index(u16 chn)
{
	int index = 0;

	switch (chn) {
	case 1 ... 14:
		index = chn - 1;
		break;

	case 36 ... 64:
		index = ((chn - 36) >> 2) + 14;
		break;

	case 100 ... 144:
		index = ((chn - 100) >> 2) + 22;
		break;

	case 149 ... 165:
		index = ((chn - 149) >> 2) + 34;
		break;

	default:
		index = -1;
		break;
	}

	return index;
}

static int skw_dpd_load_resource(struct skw_dpd *dpd, const char *path)
{
	int ret;
	struct file *fp;

	fp = skw_file_open(path, O_RDONLY, 0);
	if (!fp)
		return -ENOENT;

	ret = skw_file_read(fp, dpd->resource, dpd->size, 0);
	if (ret < 0)
		goto out;

	if (*(u32 *)(dpd->resource + SKW_DPD_RESOURCE_DATA_CNT)
		!= crc32c(0, dpd->resource, SKW_DPD_RESOURCE_DATA_CNT)) {
		ret = -SKW_EDPDVRFY;
		skw_err("resource crc:%d fail", ret);
	}
out:
	skw_file_close(fp);

	return ret;
}

int skw_dpd_coeff_result_handler(struct skw_core *skw,
	void *buf, int len)
{
	int index;
	struct skw_coeff_samples *gear;
	struct skw_ilc_result_param *param;
	struct skw_event_ilc_res_of_smpl *res = buf;

	index = skw_dpd_chn_to_index(res->chidx);
	if (index < 0) {
		skw_err("dpd chn %d not found\n", res->chidx);
		return -EINVAL;
	}

	param = skw->dpd.resource;
	param[index].center_ch = res->chidx;
	param[index].invalid = !res->succ;
	gear = &param[index].r_data[res->gear - 4];
	skw_dbg("coeff idx:%d ch:%d invalid:%d gear:%d smpl:%d cch:%d\n",
		index, res->chidx, param[index].invalid,
		res->gear, res->smpl, param[index].center_ch);

	memcpy(gear->smpl[res->smpl - 1], res->s_data, sizeof(res->s_data));

	return 0;
}

int skw_dpd_gear_cmpl_handler(struct skw_core *skw, void *buf, int len)
{
	struct skw_event_ilc_gear_cmpl *cmpl = buf;

	if (cmpl->c_flag != 1) {
		skw_err("dpd %d not cpml\n", cmpl->c_flag);
		return -EINVAL;
	}

	skw_dbg("com dpd_cmpl\n");
	complete(&skw->dpd.cmpl);

	return 0;
}

static int skw_get_5g_80mhz_center_ch(u8 ch)
{
	int i;
	const int center_ch_5g[] = {42, 58, 106, 122, 138, 155, 171};

	for (i = 0; i < ARRAY_SIZE(center_ch_5g); i++) {
		if (ch >= center_ch_5g[i] - 6 &&
		    ch <= center_ch_5g[i] + 6)
			return center_ch_5g[i];
	}

	return 0;
}

int skw_dpd_set_coeff_params(struct wiphy *wiphy,
	struct net_device *ndev, u8 chn, u8 center_chan,
	u8 center_chan_two, u8 bandwidth)
{
	int ret = 0;
	int index, i;
	struct skw_core *skw = NULL;
	struct skw_ilc_result_param *para;
	u8 cmd_center_ch;
	u8 cmd_band;

	skw_dbg("param: %d %d %d %d", chn, center_chan,
		center_chan_two, bandwidth);

	skw = wiphy_priv(wiphy);
	if (!skw) {
		skw_err("skw->dpd skw null");
		return -EINVAL;
	}
	para = skw->dpd.resource;
	if (!para) {
		skw_err("skw->dpd.resource null");
		return -EINVAL;
	}

	cmd_center_ch = center_chan;

	if (cmd_center_ch > 14) {
		cmd_band = SKW_CHAN_WIDTH_80;
		cmd_center_ch = skw_get_5g_80mhz_center_ch(chn);
	} else {
		cmd_band = SKW_CHAN_WIDTH_20;
	}

	if ((chn > 14) && (cmd_band == SKW_CHAN_WIDTH_80)) {
		index = skw_dpd_chn_to_index(cmd_center_ch);
		if (index < 0)
			return -EINVAL;
	} else {
		index = skw_dpd_chn_to_index(cmd_center_ch);
		if (index < 0)
			return -EINVAL;

		if (index >= SKW_DPD_CHN_CNT)
			return -EINVAL;

		if (para[index].invalid == 1) {
			for (i = 0; i < max(index, (SKW_DPD_CHN_CNT-index));
				i++) {
				if ((index - i >= 0))
					if (para[index - i].invalid == 0) {
						index = index - i;
						break;
					}
				if ((index + i < SKW_DPD_CHN_CNT))
					if (para[index + i].invalid == 0) {
						index = index + i;
						break;
					}
			}
		}
	}

	if (!para[index].center_ch)
		para[index].center_ch = cmd_center_ch;
	para[index].chn = chn;
	para[index].center_two_ch = center_chan_two;
	para[index].band = cmd_band;
	skw_dbg("send chn:%d center:%d center2:%d band:%d",
		para[index].chn, para[index].center_ch,
		para[index].center_two_ch, para[index].band);

	ret = skw_send_msg(wiphy, ndev, SKW_CMD_DPD_ILC_COEFF_PARAM,
		&(para[index]), sizeof(struct skw_ilc_result_param), NULL, 0);
	if (ret)
		skw_err("Send ilc coeff failed, ret: %d", ret);

	return ret;
}

int skw_dpd_download(struct wiphy *wiphy, struct skw_dpd *dpd)
{
	// TODO:
	// Maybe we should build a new dpd_resource name
	// with chip_id from chip_info struct

	if (skw_dpd_load_resource(dpd, SKW_DPD_RESOURCE_FILE) < 0) {
		if (skw_dpd_cali_download(wiphy, SKW_DPD_SRC_FW,
					SKW_DPD_MATRIX_FW,
					SKW_DPD_RESOURCE_FILE) < 0) {
			skw_err("dpd ilc dowload failed\n");
			return -EFAULT;
		}
	}

	return 0;
}

int skw_dpd_init(struct skw_dpd *dpd)
{
	init_completion(&dpd->cmpl);

	dpd->size = SKW_DPD_RESOURCE_DATA_CNT + SKW_DPD_RESOURCE_CRC_CNT;
	dpd->resource = SKW_ALLOC(dpd->size, GFP_KERNEL);
	if (!dpd->resource) {
		skw_err("malloc dpd resource failed, size: %d\n",
			dpd->size);
		return -ENOMEM;
	}

	return 0;
}

void skw_dpd_deinit(struct skw_dpd *dpd)
{
	SKW_KFREE(dpd->resource);
}
