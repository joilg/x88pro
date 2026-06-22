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

#ifndef __SKW_DFS_H__
#define __SKW_DFS_H__
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <linux/inetdevice.h>

#define PULSE_INFO_HDR_LEN              2
#define PULSE_INFO_LEN                  8

#define SKW_GET_PULSE_TYPE(l, h)        ((l) & 0x1)
#define SKW_GET_PULSE_POWER(l, h)       (((l) >> 1) & 0x1f)
#define SKW_GET_PULSE_WIDTH(l, h)       (((l) >> 6) & 0xff)
#define SKW_GET_PULSE_TS(l, h)          ((((l) >> 14) | ((h) << 18)) & 0xFFFFFF)
/**
 * PRI：pulse repetition interval
 * PPB: Pulses Per burst for each PRI
 */

enum skw_dfs_state {
	SKW_DFS_UNINITED    = 0,
	SKW_DFS_INITED	    = 1,
	SKW_DFS_CAC         = 2,
	SKW_DFS_MONITOR     = 3,
};

struct skw_dfs_ctxt {
	struct cfg80211_chan_def chan_def;
	enum skw_dfs_state state;
	u32 cac_time_ms;
	u32 last_local_ts;
	enum nl80211_dfs_regions region;
	void *detector;
};

#ifdef CONFIG_SKW_DFS_MASTER
struct radar_pattern_param {
	u8 type;
	u8 width_min;
	u8 width_max;
	u16 pri_min;
	u16 pri_max;
	u8 pri_num;
	u8 ppb;
	u8 thresh;
	u8 max_pri_tolerance;
	u8 chirp;
};

struct pulse_info {
	u64 ts;
	u8 width;
	s8 power;
	u8 chirp;
	u16 freq;
};

struct skw_dfs_detector_ctxt {
	u64 last_pulse_ts;
	const struct radar_params *radar;
	struct skw_pri_detector **detectors;
	u32 max_dur_ms;
};

struct skw_pulse_element {
	struct list_head head;
	u64 ts;
};

struct skw_pri_sequence {
	struct list_head head;
	u32 pri;
	u32 dur;
	u32 count;
	u32 count_falses;
	u64 first_ts;
	u64 last_ts;
	u64 deadline_ts;
};

struct skw_pri_detector {
	u64 last_ts;
	struct list_head sequences;
	struct list_head pulses;
	const struct radar_pattern_param *rpp;
	u32 count;
	u32 max_count;
	u32 window_size;
};

struct radar_params {
	struct skw_pri_sequence *(*add_pulse)(struct skw_pri_detector *pd, struct pulse_info *pi);
	void (*reset)(struct skw_pri_detector *de, u64 ts);
	enum nl80211_dfs_regions region;
	u32 params_num;
	const struct radar_pattern_param *params;
};

struct channel_detector {
	struct list_head head;
	u16 freq;
	struct skw_pri_detector **detectors;
};

struct skw_dfs_pool {
	struct list_head free_ps_pool;
	struct list_head free_pe_pool;
	u32 free_ps_cnt;
	u32 alloc_ps_cnt;
	u32 free_pe_cnt;
	u32 alloc_pe_cnt;
	u32 alloc_err_cnt;
};

struct skw_dfs_start_detector_param {
	struct cfg80211_chan_def def;
	u32 cac_time_ms;
};

struct skw_dfs_cmd_hdr {
	u16 type;
	u16 len;
} __packed;

struct skw_dfs_start_cac_param {
	u8 chan;
	u8 center_chn1;
	u8 center_chn2;
	u8 bandwidth;
	u32 cac_time_ms;
	u8 domain;
} __packed;

enum SKW_CMD_DFS_TYPE_E {
	SKW_DFS_CAC_START = 1,
	SKW_DFS_CAC_STOP = 2,
	SKW_DFS_MONITOR_START = 3,
	SKW_DFS_MONITOR_STOP = 4,
};

void skw_dfs_radar_pulse_event(struct wiphy *wiphy, struct skw_iface *iface,
		u8 *data, u8 data_len);
int skw_dfs_start_cac_event(struct wiphy *wiphy, struct skw_iface *iface,
		u8 *data, u8 data_len);
void skw_dfs_stop_cac_event(struct wiphy *wiphy, struct skw_iface *iface);

void skw_dfs_start_monitor_event(struct wiphy *wiphy, struct skw_iface *iface,
		struct cfg80211_chan_def *chandef);
void skw_dfs_stop_monitor_event(struct wiphy *wiphy, struct skw_iface *iface);

int skw_dfs_trig_chan_switch(struct wiphy *wiphy, struct net_device *ndev,
		const u8 *ies, size_t ies_len);

void skw_dfs_init(struct skw_iface *iface);
void skw_dfs_deinit(struct skw_iface *iface);
#else
static inline void skw_dfs_radar_pulse_event(struct wiphy *wiphy,
		struct skw_iface *iface, u8 *data, u8 data_len)
{
}
static inline int skw_dfs_start_cac_event(struct wiphy *wiphy,
		struct skw_iface *iface, u8 *data, u8 data_len)
{
	return 0;
}
static inline void skw_dfs_stop_cac_event(struct wiphy *wiphy,
		struct skw_iface *iface)
{
}

static inline void skw_dfs_start_monitor_event(struct wiphy *wiphy,
		struct skw_iface *iface, struct cfg80211_chan_def *chandef)
{
}
static inline void skw_dfs_stop_monitor_event(struct wiphy *wiphy,
		struct skw_iface *iface)
{
}

static inline int skw_dfs_trig_chan_switch(struct wiphy *wiphy,
		struct net_device *ndev, const u8 *ies, size_t ies_len)
{
	return 0;
}

static inline void skw_dfs_init(struct skw_iface *iface) {}
static inline void skw_dfs_deinit(struct skw_iface *iface) {}
#endif

#endif
