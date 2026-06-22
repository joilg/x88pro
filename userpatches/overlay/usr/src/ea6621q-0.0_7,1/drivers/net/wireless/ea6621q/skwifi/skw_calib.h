/* SPDX-License-Identifier: GPL-2.0 */

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

#ifndef __SKW_CALIB_H__
#define __SKW_CALIB_H__

#include <linux/ieee80211.h>
#include <net/cfg80211.h>

#define SKW_DPD_SRC_FW                         "dpd_param.ini"
#define SKW_DPD_MATRIX_FW                      "dpd_matrix.ini"
#define SKW_DPD_RESOURCE_FILE                  "/data/misc/wifi/dpd_result.ini"

#define SKW_EDPDVRFY                           (3000) /* DPD VERIFY ERR */
#define SKW_EDPDILCRST                         (3001) /* ILC CALI RESULT ERR */
#define SKW_EDPDGEAR                           (3002) /* ILC GEAR ERR */
#define SKW_EDPDDONE                           (3003) /* ILC CALI DONE */

#define SKW_DPD_CHN_CNT                        (105)
#define SKW_DPD_PARAM_LEN                      (64 * 1024)
#define SKW_DPD_MATRIX_LEN                     (1088)
#define SKW_DPD_RESULT_LEN                     (32 * 4)
#define SKW_DPD_ILC_TIMEOUT                    (msecs_to_jiffies(5000))
#define SKW_DPD_RESOURCE_DATA_CNT              (SKW_DPD_CHN_CNT * sizeof(struct skw_ilc_result_param))
#define SKW_DPD_RESOURCE_CRC_CNT               (4)
#define SKW_GEAR_S                             (4)
#define SKW_GEAR_E                             (8)
#define SKW_GEAR_NUM                           (SKW_GEAR_E - SKW_GEAR_S + 1)

struct skw_dpd {
	void *resource;
	struct completion cmpl;
	int size;
};

#ifdef CONFIG_SKW_CALIB_DPD
struct skw_ilc_cali_param {
	u8 gear;
	u8 seq;
	u16 len;
	u8 end;
	u8 data[1400];
} __packed;

struct skw_coeff_samples {
	u32 smpl[2][32];
} __packed;

struct skw_ilc_result_param {
	u8 chn;
	u8 center_ch;
	u8 center_two_ch;
	u8 band;
	u16 invalid;
	u16 rsv;
	struct skw_coeff_samples r_data[5];
} __packed;

struct skw_event_ilc_res_of_smpl {
	u8 gear;
	u8 chidx;
	u8 smpl;
	u8 succ;
	u32 s_data[32];
} __packed;

struct skw_event_ilc_gear_cmpl {
	u8 gear;
	u8 c_flag;
	u16 rsv;
} __packed;

int skw_dpd_set_coeff_params(struct wiphy *wiphy, struct net_device *ndev,
			     u8 chn, u8 center_chan,
			     u8 center_chan_two, u8 bandwidth);
int skw_dpd_gear_cmpl_handler(struct skw_core *skw, void *buf, int len);
int skw_dpd_coeff_result_handler(struct skw_core *skw, void *buf, int len);
int skw_dpd_init(struct skw_dpd *dpd);
void skw_dpd_deinit(struct skw_dpd *dpd);
int skw_dpd_download(struct wiphy *wiphy, struct skw_dpd *dpd);
#else
static inline int skw_dpd_set_coeff_params(struct wiphy *wiphy,
		struct net_device *ndev, u8 chn, u8 center_chan_num,
		u8 center_two_chan_num, u8 bandwidth)
{
	return 0;
}

static inline int skw_dpd_gear_cmpl_handler(struct skw_core *skw,
				void *buf, int len)
{
	return 0;
}

static inline int skw_dpd_coeff_result_handler(struct skw_core *skw,
				void *buf, int len)
{
	return 0;
}

static inline int skw_dpd_download(struct wiphy *wiphy, struct skw_dpd *dpd)
{
	return 0;
}

static inline int skw_dpd_init(struct skw_dpd *dpd)
{
	return 0;
}

static inline void skw_dpd_deinit(struct skw_dpd *dpd)
{
}
#endif

#endif
