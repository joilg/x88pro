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

#include "skw_iface.h"
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <linux/inetdevice.h>
#include "skw_cfg80211.h"
#include "skw_timer.h"
#include "skw_dfs.h"
#include "trace.h"

#define WIDTH_OFFSET    5
#define WIDTH_LOWER(X)  (((X) * (100 - WIDTH_OFFSET) + 50)/100)
#define WIDTH_UPPER(X)  (((X) * (100 + WIDTH_OFFSET) + 50)/100)
#define PRF2PRI(PRF)    ((1000000 + (PRF)/2)/(PRF))
#define PRI_OFFSET      (16)

#define GET_PRI_TO_USE(MIN, MAX, RUNTIME) \
	(MIN + PRI_OFFSET == MAX - PRI_OFFSET ? \
	MIN + PRI_OFFSET : RUNTIME)

#define MIN_PPB_THRESH	50
#define PPB_THRESH_RATE(PPB, RATE) ((PPB * RATE + 100 - RATE) / 100)
#define PPB_THRESH(PPB) PPB_THRESH_RATE(PPB, MIN_PPB_THRESH)

#define SKW_ETSI_PATTERN(ID, WMIN, WMAX, PMIN, PMAX, PRF, PPB, CHIRP) \
{       \
	ID, WIDTH_LOWER(WMIN), WIDTH_UPPER(WMAX), \
	(PRF2PRI(PMAX) - PRI_OFFSET), \
	(PRF2PRI(PMIN) * (PRF) + PRI_OFFSET), \
	PRF, (PPB) * (PRF), \
	PPB_THRESH(PPB), PRI_OFFSET, CHIRP \
}

#define LIST_FOR_EACH_ENTRY(pos, head, type, member)    \
	list_for_each_entry(pos, head, member)

#define LIST_FOR_EACH_ENTRY_SAFE(pos, n, head, type, member) \
	list_for_each_entry_safe(pos, n, head, member)

#define LIST_FOR_EACH_ENTRY_CONTINUE(pos, head, type, member)  \
	list_for_each_entry_continue(pos, head, member)

static int g_dfs_testmode;
static struct skw_dfs_pool g_pri_pool = {};

static const struct radar_pattern_param skw_etsi_radar_ref_params[] = {
	SKW_ETSI_PATTERN(0,  0,  1,  700,  700, 1, 18, 0),
	SKW_ETSI_PATTERN(1,  0,  5,  200, 1000, 1, 10, 0),
	SKW_ETSI_PATTERN(2,  0, 15,  200, 1600, 1, 15, 0),
	SKW_ETSI_PATTERN(3,  0, 15, 2300, 4000, 1, 25, 0),
	SKW_ETSI_PATTERN(4, 20, 30, 2000, 4000, 1, 20, 0),
	SKW_ETSI_PATTERN(5,  0,  2,  300,  400, 3, 10, 0),
	SKW_ETSI_PATTERN(6,  0,  2,  400, 1200, 3, 15, 0),
};

static void skw_dfs_pool_init(void)
{
	memset(&g_pri_pool, 0, sizeof(g_pri_pool));

	INIT_LIST_HEAD(&g_pri_pool.free_ps_pool);
	INIT_LIST_HEAD(&g_pri_pool.free_pe_pool);
}

static void skw_dfs_pool_deinit(void)
{
	struct skw_pri_sequence *ps, *ps_next;
	struct skw_pulse_element *pe, *pe_next;

	LIST_FOR_EACH_ENTRY_SAFE(ps, ps_next, &g_pri_pool.free_ps_pool,
			struct skw_pri_sequence, head) {
		list_del(&ps->head);
		kfree(ps);
	}

	LIST_FOR_EACH_ENTRY_SAFE(pe, pe_next, &g_pri_pool.free_pe_pool,
			struct skw_pulse_element, head) {
		list_del(&pe->head);
		kfree(pe);
	}
}

static struct skw_pri_sequence *skw_alloc_pri_sequence(void)
{
	struct skw_pri_sequence *ps = NULL;

	if (!list_empty(&g_pri_pool.free_ps_pool)) {
		ps = list_first_entry(&g_pri_pool.free_ps_pool,
				struct skw_pri_sequence, head);
		list_del_init(&ps->head);
		g_pri_pool.free_ps_cnt--;
	}

	ps = kzalloc(sizeof(*ps), GFP_KERNEL);
	if (ps == NULL) {
		g_pri_pool.alloc_err_cnt++;
		return NULL;
	}

	g_pri_pool.alloc_ps_cnt++;

	return ps;
}

static void skw_free_pri_sequence(struct skw_pri_sequence *ps)
{
	if (ps == NULL)
		return;

	g_pri_pool.alloc_ps_cnt--;
	if (g_pri_pool.free_ps_cnt > 30) {
		kfree(ps);
		return;
	}

	list_add(&ps->head, &g_pri_pool.free_ps_pool);
	g_pri_pool.free_ps_cnt++;
}

static struct skw_pulse_element *skw_alloc_pri_element(void)
{
	struct skw_pulse_element *pe = NULL;

	if (!list_empty(&g_pri_pool.free_pe_pool)) {
		pe = list_first_entry(&g_pri_pool.free_pe_pool,
				struct skw_pulse_element, head);
		list_del_init(&pe->head);
		g_pri_pool.free_pe_cnt--;
	}

	pe = kzalloc(sizeof(*pe), GFP_KERNEL);
	if (pe == NULL) {
		g_pri_pool.alloc_err_cnt++;
		return NULL;
	}

	g_pri_pool.alloc_pe_cnt++;
	return pe;
}

static void skw_free_pri_element(struct skw_pulse_element *pe)
{
	if (pe == NULL)
		return;

	g_pri_pool.alloc_pe_cnt--;
	if (g_pri_pool.free_pe_cnt > 30) {
		kfree(pe);
		return;
	}

	list_add(&pe->head, &g_pri_pool.free_pe_pool);
	g_pri_pool.free_pe_cnt++;
}

static u32 etsi_get_multiple(u32 val, u32 fraction, u32 tolerance)
{
	u32 remainder;
	u32 factor;
	u32 delta;

	if (fraction == 0)
		return 0;

	delta = (val < fraction) ? (fraction - val) : (val - fraction);

	if (delta <= tolerance)
		return 1;

	factor = val / fraction;
	remainder = val % fraction;
	if (remainder > tolerance) {
		if ((fraction - remainder) <= tolerance)
			factor++;
		else
			factor = 0;
	}

	return factor;
}

static bool etsi_create_sequences(struct skw_pri_detector *pd,
				u64 ts, u32 min_count)
{
	struct skw_pulse_element *p;
	u32 min_factor;

	LIST_FOR_EACH_ENTRY(p, &pd->pulses, struct skw_pulse_element, head) {
		struct skw_pri_sequence ps, *new_ps;
		struct skw_pulse_element *p2;
		u32 tmp_false_count;
		u64 min_valid_ts;
		u32 delta_ts = (u32)(ts - p->ts);

		if (delta_ts < pd->rpp->pri_min)
			continue;

		if (delta_ts > pd->rpp->pri_max)
			break;

		ps.count = 2;
		ps.count_falses = 0;
		ps.first_ts = p->ts;
		ps.last_ts = ts;
		ps.pri = (u32) GET_PRI_TO_USE(pd->rpp->pri_min,
				pd->rpp->pri_max,
				ts - p->ts);
		ps.dur = ps.pri * (pd->rpp->ppb - 1)
			+ 2 * pd->rpp->max_pri_tolerance;

		p2 = p;
		tmp_false_count = 0;
		min_valid_ts = ts - ps.dur;
		min_factor = 0;
		LIST_FOR_EACH_ENTRY_CONTINUE(p2, &pd->pulses, struct skw_pulse_element, head) {
			u32 factor;

			if (p2->ts < min_valid_ts)
				break;

			factor = etsi_get_multiple((u32)(ps.last_ts - p2->ts), ps.pri,
					pd->rpp->max_pri_tolerance);
			if (factor > min_factor) {
				ps.count++;
				ps.first_ts = p2->ts;
				ps.count_falses += tmp_false_count;
				tmp_false_count = 0;
				min_factor = factor;
			} else {
				tmp_false_count++;
			}
		}

		if (ps.count <= min_count)
			continue;

		ps.deadline_ts = ps.first_ts + ps.dur;
		new_ps = skw_alloc_pri_sequence();
		if (new_ps == NULL)
			return false;

		memcpy(new_ps, &ps, sizeof(ps));
		INIT_LIST_HEAD(&new_ps->head);
		list_add(&new_ps->head, &pd->sequences);
	}

	return true;
}

static u32 etsi_add_to_existing_seqs(struct skw_pri_detector *pd, u64 ts)
{
	u32 max_count = 0;
	struct skw_pri_sequence *ps, *ps_next;

	LIST_FOR_EACH_ENTRY_SAFE(ps, ps_next, &pd->sequences,
			struct skw_pri_sequence, head) {
		u32 delta_ts;
		u32 factor;

		if (ts > ps->deadline_ts) {
			list_del_init(&ps->head);
			skw_free_pri_sequence(ps);
			continue;
		}

		delta_ts = (u32)(ts - ps->last_ts);
		factor = etsi_get_multiple(delta_ts, ps->pri,
				pd->rpp->max_pri_tolerance);
		if (factor > 0) {
			ps->last_ts = ts;
			ps->count++;

			if (max_count < ps->count)
				max_count = ps->count;
		} else {
			ps->count_falses++;
		}
	}

	return max_count;
}

static void etsi_detector_reset(struct skw_pri_detector *pd, u64 ts)
{
	struct skw_pri_sequence *ps, *ps_next;
	struct skw_pulse_element *pe, *pe_next;

	LIST_FOR_EACH_ENTRY_SAFE(ps, ps_next, &pd->sequences,
			struct skw_pri_sequence, head) {
		list_del_init(&ps->head);
		skw_free_pri_sequence(ps);
	}

	LIST_FOR_EACH_ENTRY_SAFE(pe, pe_next, &pd->pulses,
			struct skw_pulse_element, head) {
		list_del_init(&pe->head);
		skw_free_pri_element(pe);
	}

	pd->count = 0;
	pd->last_ts = ts;
}

static struct skw_pri_sequence *etsi_check_detection(struct skw_pri_detector *pd)
{
	struct skw_pri_sequence *ps;

	if (list_empty(&pd->sequences))
		return NULL;

	LIST_FOR_EACH_ENTRY(ps, &pd->sequences, struct skw_pri_sequence, head) {
		if ((ps->count >= pd->rpp->thresh) &&
				(ps->count * pd->rpp->pri_num >= ps->count_falses))
			return ps;
	}

	return NULL;
}

static struct skw_pulse_element *pulse_queue_get_tail(struct skw_pri_detector *pd)
{
	struct list_head *l = &pd->pulses;

	if (list_empty(l))
		return NULL;

	return list_entry(l->prev, struct skw_pulse_element, head);
}

static void pulse_queue_dequeue(struct skw_pri_detector *pd)
{
	struct skw_pulse_element *pe = pulse_queue_get_tail(pd);

	if (pd == NULL)
		return;

	list_del_init(&pe->head);
	pd->count--;
	skw_free_pri_element(pe);
}

static void pulse_queue_check_window(struct skw_pri_detector *pd)
{
	u64 min_valid_ts;
	struct skw_pulse_element *pe;

	if (pd->count < 2)
		return;

	if (pd->last_ts <= pd->window_size)
		return;

	min_valid_ts = pd->last_ts - pd->window_size;
	while ((pe = pulse_queue_get_tail(pd)) != NULL) {
		if (pe->ts >= min_valid_ts)
			return;

		pulse_queue_dequeue(pd);
	}
}

static bool pulse_queue_enqueue(struct skw_pri_detector *pd, u64 ts)
{
	struct skw_pulse_element *pe;

	pe = skw_alloc_pri_element();
	if (pe == NULL)
		return false;

	INIT_LIST_HEAD(&pe->head);
	pe->ts = ts;
	list_add(&pe->head, &pd->pulses);
	pd->count++;
	pd->last_ts = ts;
	pulse_queue_check_window(pd);

	if (pd->count >= pd->max_count)
		pulse_queue_dequeue(pd);

	return true;
}

static struct skw_pri_sequence *etsi_add_pulse(struct skw_pri_detector *pd,
					struct pulse_info *pulse)
{
	u32 max_updated_seq;
	struct skw_pri_sequence *ps;
	u64 ts = pulse->ts;
	const struct radar_pattern_param *rpp = pd->rpp;

	if ((rpp->width_min > pulse->width) || (rpp->width_max < pulse->width))
		return NULL;

	if (rpp->chirp != pulse->chirp)
		return NULL;

	pd->last_ts = pulse->ts;

	max_updated_seq = etsi_add_to_existing_seqs(pd, ts);

	if (!etsi_create_sequences(pd, ts, max_updated_seq)) {
		etsi_detector_reset(pd, ts);
		return NULL;
	}

	ps = etsi_check_detection(pd);

	if (ps == NULL)
		pulse_queue_enqueue(pd, ts);

	return ps;
}

static const struct radar_params skw_etsi_radar_params = {
	.region	= NL80211_DFS_ETSI,
	.params_num = ARRAY_SIZE(skw_etsi_radar_ref_params),
	.params = skw_etsi_radar_ref_params,
	.add_pulse = etsi_add_pulse,
	.reset = etsi_detector_reset,
};

static const struct radar_params *skw_dfs_domains[] = {
	&skw_etsi_radar_params,
};

static const struct radar_params *get_dfs_domain_radar_params(enum nl80211_dfs_regions region)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(skw_dfs_domains); i++) {
		if (skw_dfs_domains[i]->region == region)
			return skw_dfs_domains[i];
	}

	return NULL;
}

static struct skw_pri_detector *skw_pri_detector_init(const struct radar_pattern_param *rpp)
{
	struct skw_pri_detector *de;

	de = kzalloc(sizeof(*de), GFP_KERNEL);
	if (de == NULL)
		return NULL;

	INIT_LIST_HEAD(&de->sequences);
	INIT_LIST_HEAD(&de->pulses);
	de->window_size = rpp->pri_max * rpp->ppb * rpp->pri_num;
	de->max_count = rpp->ppb * 2;
	de->rpp = rpp;

	return de;
}

static int skw_add_pulse(struct skw_dfs_detector_ctxt *ctxt, struct pulse_info *pulse)
{
	u32 i;

	ctxt->last_pulse_ts = pulse->ts;
	for (i = 0; i < ctxt->radar->params_num; i++) {
		struct skw_pri_detector *pd = ctxt->detectors[i];
		struct skw_pri_sequence *ps = ctxt->radar->add_pulse(pd, pulse);

		if (ps) {
			skw_log(SKW_WARN, "[SKWIFI DFS] type=%u, ts:%llu-%llu-%u cnt: %u - %u\n",
					pd->rpp->type, ps->first_ts, ps->last_ts, ps->pri,
					ps->count, ps->count_falses);
			ctxt->radar->reset(pd, pulse->ts);
			return 1;
		}
	}

	return 0;
}

static void skw_dfs_reset(struct skw_dfs_detector_ctxt *ctxt)
{
	u32 i;

	if (ctxt == NULL)
		return;

	if (ctxt->detectors) {
		for (i = 0; i < ctxt->radar->params_num; i++) {
			struct skw_pri_detector *pd = ctxt->detectors[i];

			if (pd)
				ctxt->radar->reset(pd, 0);
		}
	}

	ctxt->last_pulse_ts = 0;
}

static void skw_dfs_detector_deinit(struct skw_dfs_detector_ctxt *ctxt, u8 deinit_pool)
{
	u32 i;

	if (ctxt == NULL)
		return;

	if (ctxt->detectors) {
		for (i = 0; i < ctxt->radar->params_num; i++) {
			struct skw_pri_detector *pd = ctxt->detectors[i];

			if (pd)
				ctxt->radar->reset(pd, 0);
		}

		kfree(ctxt->detectors);
	}

	kfree(ctxt);

	if (deinit_pool)
		skw_dfs_pool_deinit();
}

static struct skw_dfs_detector_ctxt *skw_dfs_detector_init(enum nl80211_dfs_regions region, u8 init_pool)
{
	struct skw_dfs_detector_ctxt *ctxt;
	const struct radar_params *rp;
	u32 i;
	u32 sz, flag = 0;
	u32 max_win = 0;

	if (init_pool)
		skw_dfs_pool_init();

	do {
		ctxt = kzalloc(sizeof(*ctxt), GFP_KERNEL);
		if (ctxt == NULL)
			break;

		rp = get_dfs_domain_radar_params(region);
		if (rp == NULL)
			break;
		ctxt->radar = rp;

		sz = sizeof(ctxt->detectors) * ctxt->radar->params_num;

		ctxt->detectors = kzalloc(sz, GFP_KERNEL);
		if (ctxt->detectors == NULL)
			break;

		for (i = 0; i < ctxt->radar->params_num; i++) {
			const struct radar_pattern_param *rpp = &ctxt->radar->params[i];
			struct skw_pri_detector *pd = skw_pri_detector_init(rpp);

			if (pd == NULL) {
				flag = 1;
				break;
			}
			max_win = max(pd->window_size, max_win);
			ctxt->detectors[i] = pd;
		}

		if (flag)
			break;
		ctxt->max_dur_ms = max_win / 1000 + 1;
		return ctxt;
	} while (0);

	skw_dfs_detector_deinit(ctxt, init_pool);

	return NULL;
}

static int skw_cmd_dfs_start_cac(struct wiphy *wiphy, struct net_device *ndev,
		struct cfg80211_chan_def *chandef,
		u32 cac_time_ms, u8 domain)
{
	int ret;
	u8 buf[sizeof(struct skw_dfs_cmd_hdr) +
		sizeof(struct skw_dfs_start_cac_param)];
	struct skw_dfs_cmd_hdr *hdr = (struct skw_dfs_cmd_hdr *)buf;
	struct skw_dfs_start_cac_param *param =
		(struct skw_dfs_start_cac_param *)(hdr+1);

	memset(&buf, 0x0, sizeof(buf));
	hdr->type = SKW_DFS_CAC_START;
	hdr->len = sizeof(struct skw_dfs_start_cac_param);
	param->bandwidth = to_skw_bw(chandef->width);
	param->center_chn1 = skw_freq_to_chn(chandef->center_freq1);
	param->center_chn2 = skw_freq_to_chn(chandef->center_freq2);
	param->chan = chandef->chan->hw_value;
	param->domain = domain;
	param->cac_time_ms = cac_time_ms;

	skw_log(SKW_WARN, "[SKWIFI DFS] start cac %d-%d-%d-%d %d\n",
			param->chan, param->center_chn1, param->center_chn2,
			param->bandwidth, param->domain);

	ret = skw_send_msg(wiphy, ndev, SKW_CMD_DFS, buf,
			sizeof(buf), NULL, 0);
	if (ret)
		skw_err("failed, ret: %d\n", ret);

	return ret;
}

static int skw_cmd_dfs_stop_cac(struct wiphy *wiphy, struct net_device *ndev)
{
	int ret;
	struct skw_dfs_cmd_hdr hdr;

	memset(&hdr, 0x0, sizeof(hdr));
	hdr.type = SKW_DFS_CAC_STOP;

	skw_log(SKW_WARN, "[SKWIFI DFS] stop cac\n");

	ret = skw_send_msg(wiphy, ndev, SKW_CMD_DFS, &hdr,
			sizeof(hdr), NULL, 0);
	if (ret)
		skw_err("failed, ret: %d\n", ret);

	return ret;
}

static int skw_cmd_dfs_start_monitor(struct wiphy *wiphy, struct net_device *ndev)
{
	int ret;
	struct skw_dfs_cmd_hdr hdr;

	memset(&hdr, 0x0, sizeof(hdr));
	hdr.type = SKW_DFS_MONITOR_START;

	skw_log(SKW_WARN, "[SKWIFI DFS] start monitor\n");

	ret = skw_send_msg(wiphy, ndev, SKW_CMD_DFS, &hdr,
			sizeof(hdr), NULL, 0);
	if (ret)
		skw_err("failed, ret: %d\n", ret);

	return ret;
}

static int skw_cmd_dfs_stop_monitor(struct wiphy *wiphy, struct net_device *ndev)
{
	int ret;
	struct skw_dfs_cmd_hdr hdr;

	memset(&hdr, 0x0, sizeof(hdr));
	hdr.type = SKW_DFS_MONITOR_STOP;

	skw_log(SKW_WARN, "[SKWIFI DFS] stop monitor\n");

	ret = skw_send_msg(wiphy, ndev, SKW_CMD_DFS, &hdr,
			sizeof(hdr), NULL, 0);
	if (ret)
		skw_err("failed, ret: %d\n", ret);

	return ret;
}

static int skw_cmd_dfs_chan_switch(struct wiphy *wiphy, struct net_device *ndev,
		struct skw_element *csa_ie, struct skw_element *ecsa_ie,
		struct skw_element *csw_ie)
{
	int ret;
	u8 buf[256 * 3 + 6];
	u16 idx;

	idx = 0;
	if (csa_ie) {
		memcpy(&buf[idx], csa_ie, csa_ie->datalen + 2);
		idx += (csa_ie->datalen + 2);
	}

	if (ecsa_ie) {
		memcpy(&buf[idx], ecsa_ie, ecsa_ie->datalen + 2);
		idx += (ecsa_ie->datalen + 2);
	}

	if (csw_ie) {
		memcpy(&buf[idx], csw_ie, csw_ie->datalen + 2);
		idx += (csw_ie->datalen + 2);
	}

	ret = skw_send_msg(wiphy, ndev, SKW_CMD_REQ_CHAN_SWITCH,
			buf, idx, NULL, 0);

	if (ret)
		skw_err("failed, ret: %d\n", ret);

	return ret;
}

static int skw_dfs_set_domain(struct skw_dfs_ctxt *ctxt,
		enum nl80211_dfs_regions region)
{
	ctxt->detector = skw_dfs_detector_init(region, 1);
	if (ctxt->detector == NULL) {
		skw_log(SKW_ERROR, "[SKWIFI DFS]: %s alloc dfs_detector_init fail\n",
				__func__);
		return -ENOMEM;
	}

	ctxt->state = SKW_DFS_INITED;
	ctxt->region = region;

	return 0;
}

static void skw_dfs_release_domain(struct skw_dfs_ctxt *ctxt)
{
	struct skw_dfs_detector_ctxt *detector;

	if (ctxt->state == SKW_DFS_UNINITED)
		return;

	ctxt->state = SKW_DFS_INITED;
	detector = (struct skw_dfs_detector_ctxt *)ctxt->detector;
	ctxt->detector = NULL;
	skw_dfs_detector_deinit(detector, 1);
	ctxt->state = SKW_DFS_UNINITED;
}

static u64 skw_dfs_get_ts(u64 cur_ts, u64 pre_ts)
{
	if (pre_ts < cur_ts)
		return cur_ts;

	cur_ts = (cur_ts & 0xFFFFFF) | (pre_ts & (~0xFFFFFF));
	if (cur_ts < pre_ts)
		cur_ts += 0x1000000;

	return cur_ts;
}

static void skw_dfs_cac_timeout(void *data)
{
	struct skw_iface *iface = data;

	if (unlikely(!iface)) {
		skw_warn("iface is NULL\n");
		return;
	}

	skw_queue_work(priv_to_wiphy(iface->skw), iface,
			SKW_WORK_RADAR_CAC_END, NULL, 0);
}

void skw_dfs_radar_pulse_event(struct wiphy *wiphy, struct skw_iface *iface,
		u8 *data, u8 data_len)
{
	int i;
	struct pulse_info info;
	u8 pulse_num;
	u32 idx, val_l, val_h;
	u8 power;
	u32 delt_time_ms;
	u32 cur_time_ms = jiffies_to_msecs(jiffies);
	struct skw_dfs_ctxt *ctxt = &iface->dfs;
	struct skw_dfs_detector_ctxt *detector =
		(struct skw_dfs_detector_ctxt *)ctxt->detector;
	u64 pre_last_ts;
	u64 src_ts;

	if (ctxt->state == SKW_DFS_INITED ||
			ctxt->state == SKW_DFS_UNINITED) {
		skw_log(SKW_INFO, "[SKWIFI DFS]: not start radar detector.\n");
		return;
	}

	delt_time_ms = cur_time_ms - ctxt->last_local_ts;
	if (delt_time_ms > (detector->max_dur_ms * 2))
		skw_dfs_reset(detector);

	ctxt->last_local_ts = cur_time_ms;
	pre_last_ts = detector->last_pulse_ts;
	pulse_num = data[0];
	idx = PULSE_INFO_HDR_LEN;

	for (i = 0; i < pulse_num; i++) {
		val_l = *((u32 *)(data + idx));
		val_h = *((u32 *)(data + idx + 4));
		idx += PULSE_INFO_LEN;
		info.chirp = SKW_GET_PULSE_TYPE(val_l, val_h);

		info.width = SKW_GET_PULSE_WIDTH(val_l, val_h);
		if (info.width == 0)
			continue;

		info.width = info.width/2;
		/* Get Ts of Pulse */
		info.ts = SKW_GET_PULSE_TS(val_l, val_h);
		src_ts = info.ts;
		info.ts = skw_dfs_get_ts(info.ts, pre_last_ts);
		pre_last_ts = info.ts;

		/* Get power of Pulse */
		power = SKW_GET_PULSE_POWER(val_l, val_h);
		info.freq = 5000;
		if (power < 0x10)
			info.power = -60 + power;
		else
			info.power = -60 + power - 32;

		skw_log(SKW_INFO, "[SKWIFI DFS]: pulse: %llu %u %u %d %llu.\n",
				info.ts, info.width, info.chirp, info.power, src_ts);

		if (skw_add_pulse(detector, &info)) {
			skw_log(SKW_WARN, "[SKWIFI DFS]: detector radar.\n");
			if (g_dfs_testmode == 0) {
				skw_del_timer_work(wiphy_priv(wiphy), (void *)skw_dfs_cac_timeout);
				cfg80211_radar_event(wiphy, &ctxt->chan_def, GFP_KERNEL);
				if (ctxt->state == SKW_DFS_CAC)
					skw_cmd_dfs_stop_cac(wiphy, iface->ndev);
				else if (ctxt->state == SKW_DFS_MONITOR)
					skw_cmd_dfs_stop_monitor(wiphy, iface->ndev);
				skw_dfs_release_domain(ctxt);
			}

			break;
		}
	}
}

int skw_dfs_start_cac_event(struct wiphy *wiphy, struct skw_iface *iface,
		u8 *data, u8 data_len)
{
	struct skw_dfs_start_detector_param *param =
		(struct skw_dfs_start_detector_param *)data;
	struct skw_dfs_ctxt *ctxt = &iface->dfs;
	int ret;

	if (ctxt->state != SKW_DFS_UNINITED) {
		skw_log(SKW_WARN, "[SKWIFI DFS]: DFS is busy.\n");
		return -EBUSY;
	}

	ret = skw_dfs_set_domain(ctxt, NL80211_DFS_ETSI);
	if (ret)
		return ret;

	ctxt->chan_def = param->def;
	ctxt->state = SKW_DFS_CAC;
	ctxt->cac_time_ms = param->cac_time_ms;
	skw_cmd_dfs_start_cac(wiphy, iface->ndev, &param->def,
			param->cac_time_ms, NL80211_DFS_ETSI);

	skw_add_timer_work(wiphy_priv(wiphy), "dfs_cac_timeout", skw_dfs_cac_timeout, iface,
			param->cac_time_ms + 10000, skw_dfs_cac_timeout, GFP_KERNEL);
	skw_log(SKW_DEBUG, "[SKWIFI DFS]: start CAC %u %u.\n",
			ctxt->cac_time_ms, ctxt->region);

	return 0;
}

void skw_dfs_stop_cac_event(struct wiphy *wiphy, struct skw_iface *iface)
{
	struct skw_dfs_ctxt *ctxt = &iface->dfs;

	if (ctxt->state != SKW_DFS_CAC) {
		skw_log(SKW_INFO, "[SKWIFI DFS]: %s not start radar detector.\n", __func__);
		return;
	}

	skw_log(SKW_INFO, "[SKWIFI DFS] CAC timer finished; No radar detected\n");

	skw_cmd_dfs_stop_cac(wiphy, iface->ndev);
	skw_dfs_release_domain(ctxt);

	cfg80211_cac_event(iface->ndev,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
			&ctxt->chan_def,
#endif
			NL80211_RADAR_CAC_FINISHED, GFP_KERNEL);

}

void skw_dfs_start_monitor_event(struct wiphy *wiphy, struct skw_iface *iface,
		struct cfg80211_chan_def *chandef)
{
	int ret;
	struct skw_dfs_ctxt *ctxt = &iface->dfs;

	if (iface->dfs.state != SKW_DFS_UNINITED) {
		skw_log(SKW_ERROR, "[SKWIFI DFS] detector is running. state = %u\n",
				iface->dfs.state);
		return;
	}

	// ret = skw_dfs_set_domain(ctxt, chandef->chan->dfs_state);
	ret = skw_dfs_set_domain(ctxt, NL80211_DFS_ETSI);
	if (ret)
		return;

	skw_log(SKW_ERROR, "[SKWIFI DFS] start monitor\n");
	ctxt->state = SKW_DFS_MONITOR;
	ctxt->chan_def = *chandef;
	skw_cmd_dfs_start_monitor(wiphy, iface->ndev);
}

void skw_dfs_stop_monitor_event(struct wiphy *wiphy, struct skw_iface *iface)
{
	struct skw_dfs_ctxt *ctxt = &iface->dfs;

	if (ctxt->state != SKW_DFS_MONITOR)
		return;

	skw_log(SKW_ERROR, "[SKWIFI DFS] stop monitor\n");
	skw_cmd_dfs_stop_monitor(wiphy, iface->ndev);
	skw_dfs_release_domain(ctxt);
}

int skw_dfs_trig_chan_switch(struct wiphy *wiphy, struct net_device *ndev,
		const u8 *ies, size_t ies_len)
{
	int ret;
	struct skw_element *csa_ie, *ecsa_ie, *csw_ie;
	struct skw_iface *iface = netdev_priv(ndev);

	skw_log(SKW_ERROR, "[SKWIFI DFS] %s: %s\n", __func__, netdev_name(ndev));

	csa_ie = (struct skw_element *)cfg80211_find_ie(WLAN_EID_CHANNEL_SWITCH, ies, ies_len);
	ecsa_ie = (struct skw_element *)cfg80211_find_ie(WLAN_EID_EXT_CHANSWITCH_ANN, ies, ies_len);
	csw_ie = (struct skw_element *)cfg80211_find_ie(WLAN_EID_CHANNEL_SWITCH_WRAPPER, ies, ies_len);

	if (!csa_ie && !ecsa_ie) {
		skw_log(SKW_ERROR, "[SKWIFI DFS]: channel_switch not csa and ecsa ie.\n");
		return -EINVAL;
	}

	ret = skw_cmd_dfs_chan_switch(wiphy, iface->ndev, csa_ie, ecsa_ie, csw_ie);
	if (ret)
		return -EBUSY;

	return 0;
}

void skw_dfs_deinit(struct skw_iface *iface)
{
	struct skw_dfs_ctxt *ctxt = &iface->dfs;

	if (ctxt->state != SKW_DFS_UNINITED)
		skw_dfs_release_domain(&iface->dfs);

	ctxt->state = SKW_DFS_UNINITED;
}

void skw_dfs_init(struct skw_iface *iface)
{
	struct skw_dfs_ctxt *ctxt = &iface->dfs;

	memset(&iface->dfs, 0, sizeof(iface->dfs));
	ctxt->state = SKW_DFS_UNINITED;
}
