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

#include <linux/skbuff.h>
#include <net/netlink.h>

#include "skw_core.h"
#include "skw_iface.h"
#include "skw_msg.h"
#include "skw_vendor.h"
#include "skw_mlme.h"
#include "skw_mbssid.h"
#include "skw_cfg80211.h"
#include "skw_timer.h"
#include "skw_rx.h"
#include "skw_tx.h"
#include "skw_work.h"
#include "skw_calib.h"
#include "trace.h"
#include "skw_dfs.h"

static int skw_event_scan_complete(struct skw_core *skw,
			struct skw_iface *iface, void *buf, int len)
{
	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	skw_scan_done(skw, iface, false);

	return 0;
}

static int skw_event_sched_scan_done(struct skw_core *skw,
			struct skw_iface *iface, void *buf, int len)
{
	struct wiphy *wiphy = priv_to_wiphy(skw);

	skw_dbg("actived: %d\n", !!skw->sched_scan_req);

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	if (!skw->sched_scan_req)
		return 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
	cfg80211_sched_scan_results(wiphy, skw->sched_scan_req->reqid);
#else
	cfg80211_sched_scan_results(wiphy);
#endif

	return 0;
}

static int skw_event_disconnect(struct skw_core *skw, struct skw_iface *iface,
				void *buf, int len)
{
	int ret = 0;
	struct wiphy *wiphy = priv_to_wiphy(skw);
	struct skw_discon_event_params *param = buf;

	skw_info("bssid: %pM, reason: %u\n", param->bssid, param->reason);

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	skw_sta_leave(wiphy, iface->ndev, param->bssid,
			param->reason, false);

	if (iface->sta.sme_external) {
		skw_tx_mlme_mgmt(iface->ndev, IEEE80211_STYPE_DEAUTH,
				param->bssid, param->bssid,
				param->reason);
	} else {
		skw_disconnected(iface->ndev, param->reason, true, GFP_KERNEL);
	}

	return ret;
}

static int skw_sta_rx_deauth(struct wiphy *wiphy, struct skw_iface *iface,
			void *buf, int len)
{
	u16 reason;
	struct ieee80211_mgmt *mgmt = buf;
	struct skw_peer_ctx *ctx;

	skw_sta_assert_lock(&iface->sta.core);

	if (!ether_addr_equal(mgmt->bssid, mgmt->sa)) {
		cfg80211_tdls_oper_request(iface->ndev, mgmt->sa,
				NL80211_TDLS_TEARDOWN,
				SKW_WLAN_REASON_TDLS_TEARDOWN_UNREACHABLE,
				GFP_KERNEL);
		return 0;
	}

	ctx = skw_peer_ctx(iface, mgmt->bssid);
	if (!ctx) {
		skw_dbg("recv deauth twice\n");
		return -ENOENT;
	}

	reason = le16_to_cpu(mgmt->u.deauth.reason_code);

	skw_sta_leave(wiphy, iface->ndev, mgmt->bssid, reason, false);

	if (iface->sta.sme_external)
		skw_compat_rx_mlme_mgmt(iface->ndev, buf, len);
	else
		skw_disconnected(iface->ndev, reason, true, GFP_KERNEL);

	return 0;
}

static int skw_sta_rx_auth(struct wiphy *wiphy, struct skw_iface *iface,
			   int freq, int signal, void *buf, int len)
{
	u16 status_code;
	struct ieee80211_mgmt *mgmt = buf;
	struct skw_bss_cfg *bss = &iface->sta.core.bss;

	skw_sta_assert_lock(&iface->sta.core);

	if (!ether_addr_equal(bss->bssid, mgmt->bssid)) {
		skw_warn("bssid unmatch, current: %pM, mgmt: %pM\n",
			 bss->bssid, mgmt->bssid);

		return 0;
	}

	skw_set_state(&iface->sta.core.sm, SKW_STATE_AUTHED);

	iface->sta.core.pending.start = jiffies;
	iface->sta.core.pending.retry = 0;

	status_code = le16_to_cpu(mgmt->u.auth.status_code);
	if (status_code != WLAN_STATUS_SUCCESS) {
		skw_info("auth failed, status code: %d\n", status_code);

		skw_sta_leave(wiphy, iface->ndev, mgmt->bssid,
				WLAN_REASON_UNSPECIFIED, false);
	}

	/* SAE confirm frame received */
	if (le16_to_cpu(mgmt->u.auth.auth_alg) == 3 &&
	    le16_to_cpu(mgmt->u.auth.auth_transaction) == 2)
		SKW_SET(iface->sta.core.sm.flags, SKW_SM_FLAG_SAE_RX_CONFIRM);

	if (iface->sta.sme_external)
		skw_compat_rx_mlme_mgmt(iface->ndev, buf, len);
	else
		skw_mlme_sta_rx_auth(iface, freq, signal, buf, len);

	return 0;
}

static int skw_sta_rx_assoc(struct skw_iface *iface, int freq,
			int signal, void *buf, int len)
{
	u16 status_code;
	struct skw_peer_ctx *ctx;
	u8 *assoc_req_ie = NULL;
	struct ieee80211_mgmt *mgmt = buf;
	struct skw_sta_core *core = &iface->sta.core;

	skw_sta_assert_lock(&iface->sta.core);

	ctx = skw_get_ctx(iface->skw, core->bss.ctx_idx);
	if (!ctx) {
		skw_err("invalid pidx: %d\n", core->bss.ctx_idx);
		return 0;
	}

	skw_peer_ctx_lock(ctx);

	if (!ctx->peer ||
	    !ether_addr_equal(ctx->peer->addr, mgmt->bssid)) {
		skw_peer_ctx_unlock(ctx);
		return 0;
	}

	skw_set_state(&core->sm, SKW_STATE_ASSOCED);

	status_code = le16_to_cpu(mgmt->u.assoc_resp.status_code);
	if (status_code == WLAN_STATUS_SUCCESS) {
		u8 *ies = mgmt->u.assoc_resp.variable;
		int ies_len = len - (ies - (u8 *)mgmt);

		skw_iface_set_wmm_capa(iface, ies, ies_len);

		atomic_set(&ctx->peer->rx_filter, SKW_RX_FILTER_SET);
		__skw_peer_ctx_transmit(ctx, true);

		netif_carrier_on(iface->ndev);

	} else {
		skw_info("assoc failed, status code: %d\n", status_code);
		skw_set_state(&core->sm, SKW_STATE_NONE);
	}

	skw_peer_ctx_unlock(ctx);

	if (core->assoc_req_ie_len)
		assoc_req_ie = core->assoc_req_ie;

	if (iface->sta.sme_external)
		skw_compat_rx_assoc_resp(iface->ndev, core->cbss, buf, len, 0,
				assoc_req_ie, core->assoc_req_ie_len);
	else
		skw_mlme_sta_rx_assoc(iface, NULL, buf, len, assoc_req_ie,
				core->assoc_req_ie_len);

	core->cbss = NULL;

	return 0;
}

static int skw_sta_rx_mgmt(struct skw_core *skw, struct skw_iface *iface,
		u16 fc, int freq, int signal, void *buf, int len)
{
	u16 seq_ctrl;
	int ret = 0;
	struct ieee80211_mgmt *mgmt = buf;
	struct wiphy *wiphy = priv_to_wiphy(skw);

	seq_ctrl = le16_to_cpu(mgmt->seq_ctrl);
	if (ieee80211_has_retry(mgmt->frame_control) &&
	    iface->sta.last_seq_ctrl == seq_ctrl) {
		skw_dbg("drop retry frame (seq: %d)\n", seq_ctrl);

		return 0;
	}

	iface->sta.last_seq_ctrl = seq_ctrl;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
	mutex_lock(&iface->wdev.mtx);
#endif
	skw_sta_lock(&iface->sta.core);

	switch (fc) {
	case IEEE80211_STYPE_DEAUTH:
	case IEEE80211_STYPE_DISASSOC:
		ret = skw_sta_rx_deauth(wiphy, iface, buf, len);
		break;

	case IEEE80211_STYPE_AUTH:
		ret = skw_sta_rx_auth(wiphy, iface, freq, signal, buf, len);
		break;

	case IEEE80211_STYPE_ASSOC_RESP:
	case IEEE80211_STYPE_REASSOC_RESP:
		ret = skw_sta_rx_assoc(iface, freq, signal, buf, len);
		break;

	default:
		skw_compat_cfg80211_rx_mgmt(&iface->wdev, freq, signal, buf,
					len, 0, GFP_ATOMIC);
		break;
	}

	skw_sta_unlock(&iface->sta.core);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
	mutex_unlock(&iface->wdev.mtx);
#endif

	return ret;
}

static void skw_ibss_add_sta(struct skw_iface *iface, void *frame,
					int frame_len)
{
	int ret;
	struct station_parameters params;
	struct ieee80211_mgmt *mgmt = frame;

	if (!ether_addr_equal(mgmt->bssid, iface->ibss.bssid))
		return;

	if (skw_peer_ctx(iface, mgmt->sa))
		return;

	memset(&params, 0x0, sizeof(params));
	ret = skw_add_station(iface->wdev.wiphy, iface->ndev,
			mgmt->sa, &params);
	if (ret < 0)
		return;

	params.sta_flags_set |= BIT(NL80211_STA_FLAG_ASSOCIATED);
	skw_change_station(iface->wdev.wiphy, iface->ndev,
			mgmt->sa, &params);
}

static void skw_ibss_del_sta(struct skw_iface *iface, void *frame,
					int frame_len)
{
	struct skw_peer_ctx *ctx;
	struct ieee80211_mgmt *mgmt = frame;
	u16 reason = le16_to_cpu(mgmt->u.deauth.reason_code);

	skw_dbg("iface: %d, bssid: %pM, sa: %pM, da: %pM, reason: %d\n",
		iface->id, mgmt->bssid, mgmt->sa, mgmt->bssid, reason);

	ctx = skw_peer_ctx(iface, mgmt->sa);
	if (!ctx)
		return;

	skw_peer_ctx_transmit(ctx, false);
	skw_peer_ctx_bind(iface, ctx, NULL);
}

static void skw_ibss_rx_mgmt(struct skw_iface *iface, void *frame,
					int frame_len)
{
	u16 fc;
	struct ieee80211_mgmt *mgmt = frame;

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return;
	}

	fc = SKW_MGMT_SFC(mgmt->frame_control);
	switch (fc) {
	case IEEE80211_STYPE_BEACON:
	case IEEE80211_STYPE_PROBE_RESP:
		skw_ibss_add_sta(iface, frame, frame_len);
		break;

	case IEEE80211_STYPE_DEAUTH:
		skw_ibss_del_sta(iface, frame, frame_len);
		break;

	default:
		break;
	}
}

static bool skw_sta_access_allowed(struct skw_iface *iface, u8 *mac)
{
	int nr_allowed = iface->sap.max_sta_allowed;
	int bitmap = atomic_read(&iface->peer_map);

	if (skw_get_ctx_entry(iface->skw, mac))
		return true;

	while (bitmap && nr_allowed) {
		nr_allowed--;
		bitmap &= (bitmap - 1);
	}

	return (nr_allowed && skw_acl_allowed(iface, mac));
}

static int skw_sap_rx_mgmt(struct skw_core *skw, struct skw_iface *iface,
		u16 fc, int freq, int signal, void *buf, int len)
{
	int ret;
	struct ieee80211_mgmt *mgmt = buf;

	if (fc == IEEE80211_STYPE_AUTH &&
	    !skw_sta_access_allowed(iface, mgmt->sa)) {
		skw_info("deny: sta: %pM\n", mgmt->sa);

		skw_cmd_del_sta(priv_to_wiphy(skw), iface->ndev,
				mgmt->sa,
				12, /* Deauthentication */
				5,  /* WLAN_REASON_DISASSOC_AP_BUSY */
				true);
		return 0;
	} else if (fc == IEEE80211_STYPE_DISASSOC) {
		mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						  IEEE80211_STYPE_DEAUTH);
	}

	if (iface->sap.sme_external)
		ret = !skw_compat_cfg80211_rx_mgmt(&iface->wdev, freq, signal,
						buf, len, 0, GFP_ATOMIC);
	else
		ret = skw_mlme_ap_rx_mgmt(iface, fc, freq, signal, buf, len);

	if (ret)
		skw_warn("frame %s rx failed\n", skw_mgmt_name(fc));

	return ret;
}

static int skw_event_rx_mgmt(struct skw_core *skw, struct skw_iface *iface,
			     void *buf, int len)
{
	u16 fc;
	int freq, signal;
	struct skw_peer_ctx *ctx;
	struct skw_mgmt_hdr *hdr = buf;

	if (!iface || !hdr) {
		skw_err("iface: 0x%p, buf: 0x%p\n", iface, hdr);
		return -EINVAL;
	}

	freq = skw_to_freq(hdr->chan);
	signal = DBM_TO_MBM(hdr->signal);
	fc = SKW_MGMT_SFC(hdr->mgmt->frame_control);

	skw_dbg("%s(inst: %d), sa: %pM, chn: %d, signal: %d\n",
		skw_mgmt_name(fc), iface->id, hdr->mgmt->sa, hdr->chan, signal);

	skw_hex_dump("mgmt rx", buf, len, false);

	if (fc == IEEE80211_STYPE_DEAUTH || fc == IEEE80211_STYPE_DISASSOC) {
		skw_info("iface: %d, sa: %pM, da: %pM, %s(reason: %d)\n",
			 iface->id, hdr->mgmt->sa, hdr->mgmt->da,
			 skw_mgmt_name(fc), hdr->mgmt->u.deauth.reason_code);

		ctx = skw_peer_ctx(iface, hdr->mgmt->sa);
		if (ctx) {
			skw_peer_ctx_lock(ctx);

			if (ctx->peer)
				SKW_SET(ctx->peer->flags,
						SKW_PEER_FLAG_DEAUTHED);

			skw_peer_ctx_unlock(ctx);
		}
	}

	switch (iface->wdev.iftype) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_P2P_CLIENT:
		skw_sta_rx_mgmt(skw, iface, fc, freq, signal, hdr->mgmt,
				hdr->mgmt_len);
		break;

	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_GO:
		skw_sap_rx_mgmt(skw, iface, fc, freq, signal, hdr->mgmt,
				hdr->mgmt_len);
		break;

	case NL80211_IFTYPE_ADHOC:
		skw_ibss_rx_mgmt(iface, hdr->mgmt, hdr->mgmt_len);
		break;

	default:
		skw_compat_cfg80211_rx_mgmt(&iface->wdev, freq, signal,
				(void *)hdr->mgmt, hdr->mgmt_len,
				0, GFP_ATOMIC);
		break;
	}

	return 0;
}

static int skw_event_acs_report(struct skw_core *skw, struct skw_iface *iface,
				void *buf, int len)
{
	struct skw_survey_info *sinfo = NULL;

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	sinfo = SKW_ALLOC(sizeof(*sinfo), GFP_KERNEL);
	if (!sinfo)
		return -ENOMEM;

	INIT_LIST_HEAD(&sinfo->list);
	memcpy(&sinfo->data, buf, sizeof(struct skw_survey_data));

	list_add(&sinfo->list, &iface->survey_list);

	return 0;
}

void skw_del_sta_event(struct skw_iface *iface, const u8 *addr, u16 reason)
{
	struct ieee80211_mgmt mgmt;

	if (iface->wdev.iftype == NL80211_IFTYPE_STATION) {
		cfg80211_tdls_oper_request(iface->ndev, addr,
					   NL80211_TDLS_TEARDOWN,
					   reason, GFP_KERNEL);

		return;
	}

	if (iface->sap.sme_external) {
		mgmt.duration = 0;
		mgmt.seq_ctrl = 0;
		memcpy(mgmt.da, iface->addr, ETH_ALEN);
		memcpy(mgmt.sa, addr, ETH_ALEN);
		memcpy(mgmt.bssid, iface->sap.cfg.bssid, ETH_ALEN);
		mgmt.u.deauth.reason_code = cpu_to_le16(reason);
		mgmt.frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						 IEEE80211_STYPE_DISASSOC);

		skw_compat_cfg80211_rx_mgmt(&iface->wdev,
				 iface->sap.cfg.channel->center_freq,
				 -5400, (void *)&mgmt,
				 SKW_DEAUTH_FRAME_LEN, 0, GFP_ATOMIC);
	} else {
		cfg80211_del_sta(iface->ndev, addr, GFP_KERNEL);
	}
}

static int skw_event_del_sta(struct skw_core *skw, struct skw_iface *iface,
			     void *buf, int len)
{
	struct skw_del_sta *del_sta = buf;
	struct skw_peer_ctx *ctx = NULL;

	skw_info("mac: %pM, reason: %d\n", del_sta->mac, del_sta->reason_code);

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	ctx = skw_peer_ctx(iface, del_sta->mac);
	if (!ctx) {
		skw_err("sta: %pM not exist\n", del_sta->mac);
		return -EINVAL;
	}

	skw_peer_ctx_lock(ctx);

	skw_del_sta_event(iface, del_sta->mac, del_sta->reason_code);

	skw_peer_ctx_unlock(ctx);

	return 0;
}

static int skw_event_rrm_report(struct skw_core *skw, struct skw_iface *iface,
				void *buf, int len)
{
	return 0;
}

static int skw_get_bss_channel(struct ieee80211_mgmt *mgmt, int len)
{
	const u8 *tmp;
	int chn = -1;
	const u8 *ie = mgmt->u.beacon.variable;
	size_t ielen = len - offsetof(struct ieee80211_mgmt,
				u.probe_resp.variable);

	tmp = cfg80211_find_ie(WLAN_EID_DS_PARAMS, ie, ielen);
	if (tmp && tmp[1] == 1) {
		chn = tmp[2];
	} else {
		tmp = cfg80211_find_ie(WLAN_EID_HT_OPERATION, ie, ielen);
		if (tmp && tmp[1] >= sizeof(struct ieee80211_ht_operation)) {
			struct ieee80211_ht_operation *htop = (void *)(tmp + 2);

			chn = htop->primary_chan;
		}
	}

	return chn;
}

static int skw_event_scan_report(struct skw_core *skw, struct skw_iface *iface,
				 void *buf, int len)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 17, 0)
	struct timespec64 ts;
#endif
	struct cfg80211_bss *bss = NULL;
	struct ieee80211_channel *rx_channel = NULL;
	struct skw_mgmt_hdr *hdr = buf;
	int freq = skw_to_freq(hdr->chan);
	s32 signal = DBM_TO_MBM(hdr->signal);
	bool is_beacon = ieee80211_is_beacon(hdr->mgmt->frame_control);

	skw_log(SKW_SCAN, "[SKWIFI SCAN] bssid: %pM, chn: %d, signal: %d, %s\n",
		hdr->mgmt->sa, hdr->chan, hdr->signal,
		is_beacon ? "beacon" : "probe resp");

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	rx_channel = ieee80211_get_channel(iface->wdev.wiphy, freq);
	if (!rx_channel) {
		skw_err("invalid, freq: %d, channel: %d\n", freq, hdr->chan);
		return -EINVAL;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 17, 0)
	ts = ktime_to_timespec64(ktime_get_boottime());
	hdr->mgmt->u.probe_resp.timestamp = ((u64)ts.tv_sec*1000000)
						+ ts.tv_nsec / 1000;
#else
	hdr->mgmt->u.probe_resp.timestamp = ktime_get_boottime().tv64;
	do_div(hdr->mgmt->u.probe_resp.timestamp,  1000);
#endif

	bss = cfg80211_inform_bss_frame(iface->wdev.wiphy, rx_channel,
					hdr->mgmt, hdr->mgmt_len,
					signal, GFP_KERNEL);
	if (unlikely(!bss)) {
		int bss_chn = skw_get_bss_channel(hdr->mgmt, hdr->mgmt_len);

		skw_dbg("failed, bssid: %pM, chn: %d, rx chn: %d, flags: %d\n",
				hdr->mgmt->bssid, bss_chn,
				hdr->chan, rx_channel->flags);

		return 0;
	}

	skw->nr_scan_results++;

	if (test_bit(SKW_FLAG_MBSSID_PRIV, &skw->flags) && bss) {

		skw_bss_priv(bss)->bssid_index = 0;
		skw_bss_priv(bss)->max_bssid_indicator = 0;

		skw_mbssid_data_parser(iface->wdev.wiphy, is_beacon,
			rx_channel, signal, hdr->mgmt, hdr->mgmt_len);
	}

	cfg80211_put_bss(iface->wdev.wiphy, bss);

	return 0;
}

static int skw_event_mgmt_tx_status(struct skw_core *skw,
				struct skw_iface *iface, void *buf, int len)
{
	struct skw_tx_mgmt_status *status = buf;

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	// fixme:
	// check this tx status is for driver or for apps
	switch (iface->wdev.iftype) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_P2P_CLIENT:
	case NL80211_IFTYPE_P2P_DEVICE:
		cfg80211_mgmt_tx_status(&iface->wdev, status->cookie,
					&status->mgmt, status->payload_len,
					status->ack, GFP_KERNEL);

		break;

	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_GO:
		skw_mlme_ap_tx_status(iface, status->cookie, &status->mgmt,
					status->payload_len, status->ack);
		break;

	default:
		break;
	}

	return 0;
}

static int skw_event_ba_action(struct skw_core *skw, struct skw_iface *iface,
			       void *data, int len)
{
	struct skw_peer_ctx *ctx;
	int ret;
	struct skw_ba_action *ba = (struct skw_ba_action *)data;
	const u8 *action_str[] = {"ADD_TX_BA", "DEL_TX_BA",
				  "ADD_RX_BA", "DEL_RX_BA",
				  "REQ_RX_BA"};

	skw_dbg("%s, peer: %d, tid: %d, status: %d, win start: %d, win size: %d\n",
		action_str[ba->action], ba->peer_idx, ba->tid, ba->status_code,
		ba->ssn, ba->win_size);

	if (!iface || ba->tid >= SKW_NR_TID ||
	    ba->peer_idx >= SKW_MAX_PEER_SUPPORT) {
		skw_warn("iface: 0x%p, peer idx: %d, tid: %d\n",
			 iface, ba->peer_idx, ba->tid);

		SKW_BUG_ON(1);

		return 0;
	}

	ctx = &skw->peer_ctx[ba->peer_idx];

	skw_peer_ctx_lock(ctx);

	if (!ctx->peer)
		goto unlock;

	switch (ba->action) {
	case SKW_ADD_TX_BA:
		if (ba->status_code) {
			if (++ctx->peer->txba.tx_try[ba->tid] > 5)
				ctx->peer->txba.blacklist |= BIT(ba->tid);

			SKW_CLEAR(ctx->peer->txba.bitmap, BIT(ba->tid));
		}

		break;

	case SKW_DEL_TX_BA:
		if (ba->tid != SKW_INVALID_ID) {
			SKW_CLEAR(ctx->peer->txba.bitmap, BIT(ba->tid));
			ctx->peer->txba.tx_try[ba->tid] = 0;
		} else {
			memset(&ctx->peer->txba, 0x0, sizeof(ctx->peer->txba));
		}

		break;

	case SKW_REQ_RX_BA:
		skw_update_tid_rx(ctx->peer, ba->tid, ba->ssn, ba->win_size);
		break;

	case SKW_ADD_RX_BA:
		ret = skw_add_tid_rx(ctx->peer, ba->tid, ba->ssn, ba->win_size);
		if (ret < 0) {
			struct skw_ba_action del_ba;

			del_ba.action = SKW_DEL_RX_BA;
			del_ba.peer_idx = ba->peer_idx;
			del_ba.tid = ba->tid;

			skw_send_msg(priv_to_wiphy(skw), iface->ndev,
				     SKW_CMD_BA_ACTION, &del_ba,
				     sizeof(del_ba), NULL, 0);
		} else {
			SKW_SET(ctx->peer->rx_tid_map, BIT(ba->tid));
		}

		break;

	case SKW_DEL_RX_BA:
		skw_del_tid_rx(ctx->peer, ba->tid);
		SKW_CLEAR(ctx->peer->rx_tid_map, BIT(ba->tid));
		break;

	default:
		WARN_ON(1);
		break;
	}

unlock:
	skw_peer_ctx_unlock(ctx);

	return 0;
}

static int skw_event_enter_roc(struct skw_core *skw, struct skw_iface *iface,
				void *buf, int len)
{
	struct skw_enter_roc *roc = buf;
	struct ieee80211_channel *chan = NULL;

	skw_dbg("cookie: %llu chn: %u duration:%u\n",
		roc->cookie, roc->chn, roc->duration);

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	chan = ieee80211_get_channel(iface->wdev.wiphy, skw_to_freq(roc->chn));
	if (unlikely(!chan)) {
		skw_err("can't get channel:%d\n", roc->chn);
		return -EINVAL;
	}

	cfg80211_ready_on_channel(&iface->wdev, roc->cookie, chan,
					  roc->duration, GFP_ATOMIC);

	return 0;
}


static int skw_event_cancel_roc(struct skw_core *skw, struct skw_iface *iface,
				void *buf, int len)
{
	struct skw_cancel_roc *roc = buf;
	struct ieee80211_channel *chan = NULL;

	skw_dbg("cookie: %llu chn: %u\n", roc->cookie, roc->chn);

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	chan = ieee80211_get_channel(iface->wdev.wiphy, skw_to_freq(roc->chn));
	if (unlikely(!chan)) {
		skw_err("can't get channel:%d\n", roc->chn);
		return -EINVAL;
	}

	cfg80211_remain_on_channel_expired(&iface->wdev, roc->cookie,
				chan, GFP_KERNEL);

	return 0;
}

static int skw_event_tdls(struct skw_core *skw, struct skw_iface *iface,
			  void *buf, int len)
{
	unsigned int length = 0;
	struct sk_buff *skb = NULL;
	struct net_device *ndev = NULL;
	int ret = 0;

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	length = (unsigned int) len;
	skb = dev_alloc_skb(length);
	if (!skb)
		return -ENOMEM;

	skb_push(skb, length);
	memcpy(skb->data, buf, length);
	ndev = iface->ndev;

	skb->dev = ndev;
	skb->protocol = eth_type_trans(skb, ndev);

	if (!(ndev->flags & IFF_UP)) {
		dev_kfree_skb(skb);
		return -ENETDOWN;
	}

	ret = netif_receive_skb(skb);
	if (ret == NET_RX_SUCCESS)
		ndev->stats.rx_packets++;
	else
		ndev->stats.rx_dropped++;

	return 0;
}

#if 0
static int skw_event_credit_update(struct skw_core *skw,
			struct skw_iface *iface, void *cred, int len)
{
	if (!cred && len != sizeof(u16))
		return -EINVAL;

	skw_add_credit(skw, 0, *(u16 *)cred);

	return 0;
}
#endif

static int skw_event_mic_failure(struct skw_core *skw, struct skw_iface *iface,
				 void *buf, int len)
{
	struct skw_mic_failure *mic_failure = buf;

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	cfg80211_michael_mic_failure(iface->ndev, mic_failure->mac,
			 (mic_failure->is_mcbc ? NL80211_KEYTYPE_GROUP :
			  NL80211_KEYTYPE_PAIRWISE), mic_failure->key_id,
			NULL, GFP_KERNEL);

	return 0;
}

static int skw_event_thermal_warn(struct skw_core *skw, struct skw_iface *iface,
				  void *buf, int len)
{
#define SKW_FW_THERMAL_TRIP      0

	u8 event = *(u8 *)buf;
	struct skw_iface *tmp_iface = NULL;
	int i;

	/*
	 * 0: stop transmit
	 * 1: resume transmit
	 */

	skw_warn("active: %u\n", !event);

	if (event == SKW_FW_THERMAL_TRIP)
		set_bit(SKW_FLAG_FW_THERMAL, &skw->flags);
	else
		clear_bit(SKW_FLAG_FW_THERMAL, &skw->flags);

	for (i = 0; i < SKW_NR_IFACE; i++) {
		tmp_iface = skw->vif.iface[i];
		if (!tmp_iface)
			continue;

		if (tmp_iface->wdev.iftype == NL80211_IFTYPE_P2P_DEVICE)
			continue;

		if (event == SKW_FW_THERMAL_TRIP)
			netif_tx_stop_all_queues(tmp_iface->ndev);
		else
			netif_tx_start_all_queues(tmp_iface->ndev);
	}

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
static int skw_event_rssi_monitor(struct skw_core *skw, struct skw_iface *iface,
				  void *buf, int len)
{
	struct skw_rssi_mointor *rssi_mointor = buf;
	struct sk_buff *skb = NULL;

	if (!iface || !buf || len != sizeof(struct skw_rssi_mointor))
		return -EINVAL;

	skb = skw_compat_vendor_event_alloc(priv_to_wiphy(skw),
			NULL, EXT_VENDOR_EVENT_BUF_SIZE + NLMSG_HDRLEN,
			SKW_NL80211_VENDOR_SUBCMD_MONITOR_RSSI, GFP_KERNEL);

	if (!skb) {
		skw_err("Alloc skb for rssi monitor event failed\n");
		return -ENOMEM;
	}

	if (nla_put_u32(skb, SKW_WLAN_VENDOR_ATTR_RSSI_MONITORING_REQUEST_ID,
		rssi_mointor->req_id) ||
		nla_put(skb, SKW_WLAN_VENDOR_ATTR_RSSI_MONITORING_CUR_BSSID,
		ETH_ALEN, rssi_mointor->curr_bssid) ||
		nla_put_s8(skb, SKW_WLAN_VENDOR_ATTR_RSSI_MONITORING_CUR_RSSI,
		rssi_mointor->curr_rssi)) {
		skw_err("nla put for rssi monitor event failed\n");
		goto fail;
	}

	cfg80211_vendor_event(skb, GFP_KERNEL);

fail:
	kfree_skb(skb);
	return 0;
}
#endif


void skw_cqm_scan_timeout(void *data)
{
	struct skw_iface *iface = data;

	skw_dbg(" enter\n");
	if (unlikely(!iface)) {
		skw_warn("iface is NULL\n");
		return;
	}

	spin_lock_bh(&iface->sta.roam_data.lock);
	iface->sta.roam_data.flags &= ~SKW_IFACE_STA_ROAM_FLAG_CQM_LOW;
	spin_unlock_bh(&iface->sta.roam_data.lock);
}

static int skw_event_cqm(struct skw_core *skw, struct skw_iface *iface,
			 void *buf, int len)
{
	struct skw_cqm_info *cqm_info = buf;

	skw_dbg("cqm_status:%d cqm_rssi:%d chan:%d %pM\n",
		cqm_info->cqm_status, cqm_info->cqm_rssi,
		cqm_info->chan, cqm_info->bssid);

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	switch (cqm_info->cqm_status) {
	case CQM_STATUS_RSSI_LOW:
		if (iface->sta.sme_external) {
			if (is_valid_ether_addr(cqm_info->bssid)) {
				spin_lock_bh(&iface->sta.roam_data.lock);
				if (!(iface->sta.roam_data.flags & SKW_IFACE_STA_ROAM_FLAG_CQM_LOW)) {
					skw_dbg("recv cqm low event bssid:%pM\n", cqm_info->bssid);
					memcpy(iface->sta.roam_data.target_bssid, cqm_info->bssid, ETH_ALEN);
					iface->sta.roam_data.target_chn = cqm_info->chan;
					skw_add_timer_work(skw, "cqm_scan_timeout", skw_cqm_scan_timeout,
							iface, SKW_CQM_SCAN_TIMEOUT,
							skw_cqm_scan_timeout, GFP_KERNEL);
					iface->sta.roam_data.flags |= SKW_IFACE_STA_ROAM_FLAG_CQM_LOW;
				}
				spin_unlock_bh(&iface->sta.roam_data.lock);
			}

			skw_compat_cqm_rssi_notify(iface->ndev,
					NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW,
					cqm_info->cqm_rssi, GFP_KERNEL);

		} else {
			skw_roam_connect(iface, cqm_info->bssid,
						  cqm_info->chan);
		}

		break;

	case CQM_STATUS_RSSI_HIGH:
		if (iface->sta.sme_external) {
			skw_compat_cqm_rssi_notify(iface->ndev,
					NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH,
					cqm_info->cqm_rssi, GFP_KERNEL);
		}

		break;

	case CQM_STATUS_BEACON_LOSS:
		if (is_valid_ether_addr(cqm_info->bssid)) {
			// FW use beacon loss event to trigger roaming
			if (iface->sta.sme_external) {
				skw_dbg("beacon loss trigger roaming");
				skw_compat_cqm_rssi_notify(iface->ndev,
					NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW,
					cqm_info->cqm_rssi, GFP_KERNEL);
			} else
				skw_roam_connect(iface, cqm_info->bssid, cqm_info->chan);
		} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
			cfg80211_cqm_beacon_loss_notify(iface->ndev, GFP_KERNEL);
#endif
		}

		break;

	case CQM_STATUS_TDLS_LOSS:
		cfg80211_cqm_pktloss_notify(iface->ndev, cqm_info->bssid,
					    0, GFP_KERNEL);
		break;

	default:
		break;
	}

	return 0;
}

static int skw_trans_80211_to_8023(struct sk_buff *skb, int len)
{
	int ret = 0;
	struct ieee80211_hdr *wh = (struct ieee80211_hdr *) skb->data;
	uint32_t hdrsize;
	struct llc *llchdr;
	struct ethhdr *eth_hdr;
	uint16_t ether_type = 0;
	uint8_t a1[ETH_ALEN];
	uint8_t a2[ETH_ALEN];
	uint8_t a3[ETH_ALEN];
	uint16_t fc;

	wh = (struct ieee80211_hdr *)skb->data;
	memcpy(a1, wh->addr1, ETH_ALEN);
	memcpy(a2, wh->addr2, ETH_ALEN);
	memcpy(a3, wh->addr3, ETH_ALEN);
	fc = wh->frame_control;

	if (ieee80211_is_data_qos(fc))
		hdrsize = sizeof(struct ieee80211_qos_hdr);
	else
		hdrsize = sizeof(struct ieee80211_hdr_3addr);

	llchdr = (struct llc *)(((uint8_t *) skb->data) + hdrsize);
	ether_type = llchdr->llc_un.type_snap.ether_type;

	/*
	 * Now move the data pointer to the beginning of the mac header :
	 * new-header = old-hdr + (wifhdrsize + llchdrsize - ethhdrsize)
	 */
	skb_pull(skb,
		(hdrsize + sizeof(struct llc) - sizeof(struct ethhdr)));
	eth_hdr = (struct ethhdr *)skb->data;

	if (ieee80211_has_a4(fc))
		ret = -EINVAL;
	else if (ieee80211_has_tods(fc)) {
		memcpy(eth_hdr->h_dest, a3, ETH_ALEN);
		memcpy(eth_hdr->h_source, a2, ETH_ALEN);
	} else if (ieee80211_has_fromds(fc)) {
		memcpy(eth_hdr->h_dest, a1, ETH_ALEN);
		memcpy(eth_hdr->h_source, a3, ETH_ALEN);
	} else { //TDLS IEEE80211_FC1_DIR_NODS
		memcpy(eth_hdr->h_dest, a1, ETH_ALEN);
		memcpy(eth_hdr->h_source, a2, ETH_ALEN);
	}

	eth_hdr->h_proto = ether_type;

	return ret;
}

static int skw_event_rx_unprotect_frame(struct skw_core *skw,
				struct skw_iface *iface, void *buf, int len)
{
	struct ieee80211_hdr *hdr = buf;
	struct ieee80211_mgmt *mgmt = buf;
	int ret = 0;
	unsigned long sz;

	skw_dbg("frame control: %02x, len: %d\n",
		SKW_MGMT_SFC(mgmt->frame_control), len);

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	if (ieee80211_is_data(hdr->frame_control)) {
		//Convert it to 802.3 then check the ethernet type,
		//drop it while it is not EAPOL or WAPI data.
		struct sk_buff *skb = container_of(buf, struct sk_buff, data);
		struct ethhdr *eth_hdr = NULL;

		ret = skw_trans_80211_to_8023(skb, len);
		if (ret) {
			skw_err("convert to 802.3 failed ret:%d\n", ret);
			return ret;
		}

		eth_hdr = (struct ethhdr *)skb->data;

		if (htons(ETH_P_PAE) == eth_hdr->h_proto) {
			skb->dev = iface->ndev;
			skb->protocol = eth_type_trans(skb, iface->ndev);
			skb->csum = 0;
			skb->ip_summed = CHECKSUM_NONE;

			sz = skb->len;
			if (netif_receive_skb(skb) == NET_RX_SUCCESS) {
				iface->ndev->stats.rx_packets++;
				iface->ndev->stats.rx_bytes += sz;
			} else {
				iface->ndev->stats.rx_dropped++;
			}

		} else {
			skw_warn("received unprotect frame from:%pM\n",
						eth_hdr->h_source);
		}
	} else if (ieee80211_is_mgmt(hdr->frame_control)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0)
		cfg80211_rx_unprot_mlme_mgmt(iface->ndev, buf, len);
#else
		if (ieee80211_is_deauth(hdr->frame_control))
			cfg80211_send_unprot_deauth(iface->ndev, buf, len);
		else
			cfg80211_send_unprot_disassoc(iface->ndev, buf, len);
#endif
	} else {
		skw_err("Unsupported frames\n");
		return -EINVAL;
	}

	return 0;
}

static int skw_chbw_to_cfg80211_chan_def(struct wiphy *wiphy,
					 struct cfg80211_chan_def *chdef,
					 struct skw_event_csa_param *csa)
{
	int freq;
	struct ieee80211_channel *chan = NULL;

	skw_dbg("chn: %d, bw: %d, center1: %d, center2: %d, bss type: 0x%x\n",
		csa->chan, csa->band_width, csa->center_chan1,
		csa->center_chan2, csa->bss_type);

	memset(chdef, 0, sizeof(struct cfg80211_chan_def));

	freq = skw_to_freq(csa->chan);
	if (!freq) {
		skw_err("invalid channel: %d\n", csa->chan);
		return -EINVAL;
	}

	chan = ieee80211_get_channel(wiphy, freq);
	if (!chan || chan->flags & IEEE80211_CHAN_DISABLED) {
		skw_err("invalid freq: %d\n", freq);
		return -EINVAL;
	}

	chdef->chan = chan;
	chdef->center_freq1 = skw_to_freq(csa->center_chan1);
	chdef->center_freq2 = 0;

	switch (csa->band_width) {
	case SKW_CHAN_WIDTH_20:
		if (csa->bss_type & SKW_CAPA_HT)
			chdef->width = NL80211_CHAN_WIDTH_20;
		else
			chdef->width = NL80211_CHAN_WIDTH_20_NOHT;
		break;

	case SKW_CHAN_WIDTH_40:
		chdef->width = NL80211_CHAN_WIDTH_40;
		break;

	case SKW_CHAN_WIDTH_80:
		chdef->width = NL80211_CHAN_WIDTH_80;
		break;

	case SKW_CHAN_WIDTH_80P80:
		chdef->width = NL80211_CHAN_WIDTH_80P80;
		chdef->center_freq2 = skw_to_freq(csa->center_chan2);
		break;

	case SKW_CHAN_WIDTH_160:
		chdef->width = NL80211_CHAN_WIDTH_160;
		break;

	default:
		skw_err("invalid band width: %d\n", csa->band_width);
		return -EINVAL;
	}

	if (!cfg80211_chandef_valid(chdef)) {
		skw_err("chandef invalid\n");
		return -EINVAL;
	}

	return 0;
}

static int skw_event_chan_switch(struct skw_core *skw, struct skw_iface *iface,
				 void *buf, int len)
{
	struct skw_event_csa_param *csa_param = buf;
	struct cfg80211_chan_def chan_def;

	skw_dbg("mode: %d\n", csa_param->mode);

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	if (csa_param->mode == SKW_CSA_START) {
		netif_carrier_off(iface->ndev);
	} else {
		netif_carrier_on(iface->ndev);

		if (!skw_chbw_to_cfg80211_chan_def(iface->wdev.wiphy,
					&chan_def, csa_param)) {

			skw_info("chan: %d\n", csa_param->chan);

			skw_ch_switch_notify(iface->ndev, &chan_def, 10, true);

			iface->sap.cfg.channel = chan_def.chan;
		}
	}

	return 0;
}

static int skw_event_tx_frame(struct skw_core *skw, struct skw_iface *iface,
			      void *buf, int len)
{
	u16 fc;
	u8 *ie;
	int ie_len;
	struct skw_frame_tx_status *tx = buf;
	struct skw_sta_core *core;

	if (unlikely(!iface)) {
		skw_warn("iface invalid\n");
		return -EINVAL;
	}

	if (iface->wdev.iftype != NL80211_IFTYPE_STATION &&
	    iface->wdev.iftype != NL80211_IFTYPE_P2P_CLIENT)
		return 0;

	skw_hex_dump("tx frame", buf, len, false);

	fc = SKW_MGMT_SFC(tx->mgmt->frame_control);

	// skw_dbg("iface: %d, fc: 0x%x\n", iface->id, fc);

	if (fc == IEEE80211_STYPE_ASSOC_REQ ||
	    fc == IEEE80211_STYPE_REASSOC_REQ) {
		if (fc == IEEE80211_STYPE_ASSOC_REQ) {
			ie = tx->mgmt->u.assoc_req.variable;
			ie_len = tx->mgmt_len - offsetof(struct ieee80211_mgmt,
				u.assoc_req.variable);
		} else {
			ie = tx->mgmt->u.reassoc_req.variable;
			ie_len = tx->mgmt_len - offsetof(struct ieee80211_mgmt,
				u.reassoc_req.variable);
		}

		core = &iface->sta.core;
		skw_sta_lock(core);

		if (ie_len <= SKW_2K_SIZE) {
			memcpy(core->assoc_req_ie, ie, ie_len);
			core->assoc_req_ie_len = ie_len;
		}

		skw_sta_unlock(core);
	}

	return 0;
}

static int skw_event_dpd_coeff_result(struct skw_core *skw,
			struct skw_iface *iface, void *buf, int len)
{
	skw_dpd_coeff_result_handler(skw, buf, len);

	return 0;

}

static int skw_event_dpd_gear_cmpl(struct skw_core *skw,
			struct skw_iface *iface, void *buf, int len)
{
	skw_dpd_gear_cmpl_handler(skw, buf, len);

	return 0;
}

static int skw_event_fw_recovery(struct skw_core *skw,
			struct skw_iface *iface, void *buf, int len)
{
	u8 done = *(u8 *)buf;

	skw_dbg("done: %d\n", done);

	/* Frimware start recovery */
	if (done)
		clear_bit(SKW_FLAG_FW_MAC_RECOVERY, &skw->flags);
	else
		set_bit(SKW_FLAG_FW_MAC_RECOVERY, &skw->flags);

	return 0;
}

static int skw_event_mp_mode_handler(struct skw_core *skw,
			struct skw_iface *iface, void *buf, int len)
{
	u8 state = *(u8 *)buf;

	skw_dbg("state: %d\n", state);

	if (state) {
		set_bit(SKW_FLAG_MP_MODE, &skw->flags);
		skw_abort_cmd(skw);

	} else {
		clear_bit(SKW_FLAG_MP_MODE, &skw->flags);
	}

	return 0;
}

static int skw_event_radar_pulse_handler(struct skw_core *skw,
		struct skw_iface *iface, void *buf, int len)
{
	int ret;
	u8 *p = (u8 *)buf;
	u8 pulse_num = p[0];

	skw_dbg("event_radar_pulse pulse_num = %d\n", pulse_num);

	if (pulse_num) {
		ret = skw_queue_work(priv_to_wiphy(skw), iface, SKW_WORK_RADAR_PULSE,
				&(p[0]), PULSE_INFO_HDR_LEN + pulse_num * PULSE_INFO_LEN);
	}

	return 0;
}


static int skw_local_ap_auth_timeout(struct skw_core *skw,
			struct skw_iface *iface, void *buf, int len)
{
	struct skw_client *client = buf;

	skw_warn("client: %pM\n", client->addr);

	skw_mlme_ap_del_sta(priv_to_wiphy(skw), iface->ndev,
					client->addr, false);

	return 0;
}

static int skw_local_ibss_connect(struct skw_core *skw,
			struct skw_iface *iface, void *buf, int len)
{
	u16 chn;
	int ret;
	struct skw_ibss_params params;

	memcpy(params.ssid, iface->ibss.ssid, iface->ibss.ssid_len);
	params.ssid_len = iface->ibss.ssid_len;

	memcpy(params.bssid, iface->ibss.bssid, ETH_ALEN);

	params.type = 0;
	params.chan = iface->ibss.channel;
	params.bw = iface->ibss.bw;
	params.beacon_int = iface->ibss.beacon_int;

	chn = skw_freq_to_chn(iface->ibss.center_freq1);
	params.center_chan1 = chn;

	chn = skw_freq_to_chn(iface->ibss.center_freq2);
	params.center_chan2 = chn;

	ret = skw_send_msg(iface->wdev.wiphy, iface->ndev, SKW_CMD_IBSS_JOIN,
			&params, sizeof(params), NULL, 0);
	if (!ret) {
		netif_carrier_on(iface->ndev);

		cfg80211_ibss_joined(iface->ndev, iface->ibss.bssid,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0)
				iface->ibss.chandef.chan,
#endif
				GFP_KERNEL);
	} else {
		skw_err("failed, ret: %d, ssid: %s, bssid: %pM\n",
			ret, iface->ibss.ssid, iface->ibss.bssid);
	}

	return ret;
}

static int skw_local_sta_connect(struct skw_core *skw,
			struct skw_iface *iface, void *buf, int len)
{
	int ret = 0;
	struct cfg80211_bss *bss;
	struct wiphy *wiphy = iface->wdev.wiphy;
	struct skw_connect_param *conn = iface->sta.conn;

	if (!iface->sta.conn)
		return 0;

	bss = cfg80211_get_bss(wiphy, conn->channel, conn->bssid,
			conn->ssid, conn->ssid_len,
			SKW_BSS_TYPE_ESS, SKW_PRIVACY_ESS_ANY);

	if (conn->auth_type == NL80211_AUTHTYPE_SAE)
		ret = skw_connect_sae_auth(wiphy, iface->ndev, bss);
	else
		ret = skw_connect_auth(wiphy, iface->ndev, conn, bss);

	cfg80211_put_bss(wiphy, bss);

	return ret;
}

#define FUNC_INIT(e, f)         \
	[e] = {                 \
		.id = e,        \
		.name = #e,     \
		.func = f       \
	}

static const struct skw_event_func g_event_fn[] = {
	FUNC_INIT(SKW_EVENT_NORMAL_SCAN_CMPL, skw_event_scan_complete),
	FUNC_INIT(SKW_EVENT_SCHED_SCAN_CMPL, skw_event_sched_scan_done),
	FUNC_INIT(SKW_EVENT_DISCONNECT, skw_event_disconnect),
	FUNC_INIT(SKW_EVNET_RX_MGMT, skw_event_rx_mgmt),
	FUNC_INIT(SKW_EVENT_ACS_REPORT, skw_event_acs_report),
	FUNC_INIT(SKW_EVENT_DEL_STA, skw_event_del_sta),
	FUNC_INIT(SKW_EVENT_RRM_REPORT, skw_event_rrm_report),
	FUNC_INIT(SKW_EVENT_SCAN_REPORT, skw_event_scan_report),
	FUNC_INIT(SKW_EVENT_MGMT_TX_STATUS, skw_event_mgmt_tx_status),
	FUNC_INIT(SKW_EVENT_BA_ACTION, skw_event_ba_action),
	FUNC_INIT(SKW_EVENT_ENTER_ROC, skw_event_enter_roc),
	FUNC_INIT(SKW_EVENT_CANCEL_ROC, skw_event_cancel_roc),
	FUNC_INIT(SKW_EVENT_TDLS, skw_event_tdls),
	// FUNC_INIT(SKW_EVENT_CREDIT_UPDATE, skw_event_credit_update),
	FUNC_INIT(SKW_EVENT_MIC_FAILURE, skw_event_mic_failure),
	FUNC_INIT(SKW_EVENT_THERMAL_WARN, skw_event_thermal_warn),
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
	FUNC_INIT(SKW_EVENT_RSSI_MONITOR, skw_event_rssi_monitor),
#endif
	FUNC_INIT(SKW_EVENT_CQM, skw_event_cqm),
	FUNC_INIT(SKW_EVENT_RX_UNPROTECT_FRAME, skw_event_rx_unprotect_frame),
	FUNC_INIT(SKW_EVENT_CHAN_SWITCH, skw_event_chan_switch),
	FUNC_INIT(SKW_EVENT_TX_FRAME, skw_event_tx_frame),
	FUNC_INIT(SKW_EVENT_DPD_ILC_COEFF_REPORT, skw_event_dpd_coeff_result),
	FUNC_INIT(SKW_EVENT_DPD_ILC_GEAR_CMPL, skw_event_dpd_gear_cmpl),
	FUNC_INIT(SKW_EVENT_FW_RECOVERY, skw_event_fw_recovery),
	FUNC_INIT(SKW_EVENT_NPI_MP_MODE, skw_event_mp_mode_handler),
	FUNC_INIT(SKW_EVENT_RADAR_PULSE, skw_event_radar_pulse_handler),
	FUNC_INIT(SKW_EVENT_MAX, NULL),
};

static const struct skw_event_func g_local_event_fn[] = {
	// FUNC_INIT(SKW_EVENT_LOCAL_STA_AUTH_ASSOC_TIMEOUT, skw_local_sta_auth_assoc_timeout),
	FUNC_INIT(SKW_EVENT_LOCAL_AP_AUTH_TIMEOUT, skw_local_ap_auth_timeout),
	FUNC_INIT(SKW_EVENT_LOCAL_STA_CONNECT, skw_local_sta_connect),
	FUNC_INIT(SKW_EVENT_LOCAL_IBSS_CONNECT, skw_local_ibss_connect),
	FUNC_INIT(SKW_EVENT_LOCAL_MAX, NULL),
};

#undef FUNC_INIT

static inline void skw_cmd_lock(struct skw_core *skw, unsigned long flags)
{
	mutex_lock(&skw->cmd.lock);

	if (flags & BIT(SKW_CMD_FLAG_NO_WAKELOCK))
		return;

	__pm_stay_awake(skw->cmd.ws);
}

static inline void skw_cmd_unlock(struct skw_core *skw, unsigned long flags)
{
	if (!(flags & BIT(SKW_CMD_FLAG_NO_WAKELOCK)))
		__pm_relax(skw->cmd.ws);

	mutex_unlock(&skw->cmd.lock);
}

static bool skw_cmd_tx_allowed(struct skw_core *skw, int inst, int cmd, unsigned long mask)
{
	struct skw_iface *iface;

	if (!skw_tx_allowed(skw, mask)) {
		skw_warn("skw->flags: 0x%lx, mask: 0x%lx\n",
			 skw->flags, mask);

		return false;
	}

	iface = to_skw_iface(skw, inst);
	if (iface && iface->ndev &&
	    iface->ndev->ieee80211_ptr->iftype == NL80211_IFTYPE_MONITOR) {
		if (cmd == SKW_CMD_SET_MONITOR_PARAM ||
		    cmd == SKW_CMD_CLOSE_DEV ||
		    cmd == SKW_CMD_OPEN_DEV)
			return true;

		return false;
	}

	if (likely(skw->vif.opened_dev))
		return true;

	return (cmd == SKW_CMD_GET_INFO ||
		cmd == SKW_CMD_SYN_VERSION ||
		cmd == SKW_CMD_OPEN_DEV ||
		cmd == SKW_CMD_PHY_BB_CFG ||
		cmd == SKW_CMD_SET_REGD ||
		cmd == SKW_CMD_DPD_ILC_GEAR_PARAM ||
		cmd == SKW_CMD_DPD_ILC_MARTIX_PARAM);
}

static int skw_set_cmd(struct skw_core *skw, int dev_id, int cmd,
		       void *data, int data_len, void *arg,
		       int arg_size, char *name, unsigned long extra_flags)
{
	struct skw_msg *msg_hdr;
	int total_len, msg_len;
	void *pos;

	lockdep_assert_held(&skw->cmd.lock);

	pos = skw->cmd.data;
	total_len = msg_len = data_len + sizeof(*msg_hdr);

	if (skw_need_extra_hdr(skw)) {
		total_len = round_up(total_len + skw->hw.extra.hdr_len,
				     skw->hw.align);

		skw_set_extra_hdr(skw, pos, skw->hw.cmd_port, total_len, 0, 0);

		pos += skw->hw.extra.hdr_len;
	}

	if (total_len > SKW_CMD_MAX_LEN) {
		skw_warn("total_len: %d\n", total_len);
		SKW_BUG_ON(1);

		return -E2BIG;
	}

	skw->cmd.id = cmd;
	skw->cmd.name = name;
	skw->cmd.seq++;
	skw->cmd.start_time = jiffies;
	skw->cmd.arg = arg;
	skw->cmd.arg_size = arg_size;
	skw->cmd.status = 0;
	skw->cmd.data_len = total_len;
	WRITE_ONCE(skw->cmd.flags, extra_flags);


	msg_hdr = pos;
	msg_hdr->inst_id = dev_id;
	msg_hdr->type = SKW_MSG_CMD;
	msg_hdr->id = cmd;
	msg_hdr->total_len = msg_len;
	msg_hdr->seq = skw->cmd.seq;

	pos += sizeof(*msg_hdr);
	if (data_len)
		memcpy(pos, data, data_len);

	skw_log(SKW_CMD, "[SKWIFI CMD] TX %s[%d], iface: %d, seq: %d, len = %d\n",
		name, cmd, dev_id, skw->cmd.seq, data_len);

	return 0;
}

static void skw_cmd_timeout_fn(void *data)
{
	struct skw_core *skw = data;

	skw_err("bus: %s, %s[%d], seq: %d ret: %d, flags: 0x%lx, xmit_data: %d, timeout: %d(md)\n",
		skw_bus_name(skw->hw.bus), skw->cmd.name, skw->cmd.id,
		skw->cmd.seq, -ETIMEDOUT, skw->flags,
		atomic_read(&skw->xmitting_data),
		jiffies_to_msecs(SKW_CMD_TIMEOUT));

	set_bit(SKW_FLAG_BLOCK_TX, &skw->flags);
	skw_cmd_unlock(skw, 0);

	skw_assert_schedule(priv_to_wiphy(skw));
}

static void skw_msg_try_send_cb(struct skw_core *skw)
{
	skw_del_timer_work(skw, skw->cmd.data);
	skw_cmd_unlock(skw, 0);
}

int skw_msg_try_send(struct skw_core *skw, int inst, int cmd, void *data,
		     int data_len, void *arg, int arg_size, char *name)
{
	int ret;

	if (!mutex_trylock(&skw->cmd.lock))
		return -EBUSY;

	__pm_stay_awake(skw->cmd.ws);

	if (!skw_cmd_tx_allowed(skw, inst, cmd, 0)) {
		skw_cmd_unlock(skw, 0);
		return -EIO;
	}

	ret = skw_set_cmd(skw, inst, cmd, data, data_len,
			  arg, arg_size, name, 0);
	if (ret) {
		skw_cmd_unlock(skw, 0);
		return ret;
	}

	skw_add_timer_work(skw, name, skw_cmd_timeout_fn, skw, SKW_CMD_TIMEOUT,
			   skw->cmd.data, GFP_ATOMIC);

	skw->cmd.callback = skw_msg_try_send_cb;

	set_bit(SKW_CMD_FLAG_XMIT, &skw->cmd.flags);
	skw_wakeup_tx(skw);

	return 0;
}

static void skw_msg_xmit_timeout_cb(struct skw_core *skw)
{
	if (!test_bit(SKW_CMD_FLAG_DONE, &skw->cmd.flags))
		return;

	wake_up(&skw->cmd.wq);
}

/* SDIO BUS
 *             +--------------------- MSG_HDR->total_len ---------------------+
 *             |                                                              |
 * +-----------+----------+------------+------------+-----------+-------------+
 * | EXTRA_HDR |  MSG_HDR | IE_OFFSET  |  PARAM ... |   IE ...  |  OTHERS ... |
 * +-----------+----------+------------+------------+-----------+-------------+
 *                        |                         |
 *                        +-------- IE_OFFSET ------+
 */
int skw_msg_xmit_timeout(struct wiphy *wiphy, int dev_id, int cmd,
			 void *buf, int buf_len, void *arg, int arg_size,
			 char *name, unsigned long timeout,
			 unsigned long extra_flags)
{
	int ret;
	unsigned long mask = 0;
	struct skw_core *skw = wiphy_priv(wiphy);

	BUG_ON(in_interrupt());

	if (dev_id < 0) {
		skw_warn("invalid instance\n");

		SKW_BUG_ON(1);
		return -EINVAL;
	}

	skw_cmd_lock(skw, extra_flags);

	if (extra_flags & BIT(SKW_CMD_FLAG_IGNORE_BLOCK_TX))
		mask |= BIT(SKW_FLAG_BLOCK_TX);

	if (!skw_cmd_tx_allowed(skw, dev_id, cmd, mask)) {
		skw_cmd_unlock(skw, extra_flags);
		return -EIO;
	}

	ret = skw_set_cmd(skw, dev_id, cmd, buf, buf_len, arg, arg_size,
			  name, extra_flags);
	if (ret) {
		skw_cmd_unlock(skw, extra_flags);
		return ret;
	}

	skw->cmd.callback = skw_msg_xmit_timeout_cb;
	set_bit(SKW_CMD_FLAG_XMIT, &skw->cmd.flags);

	skw_wakeup_tx(skw);

	ret = wait_event_interruptible_timeout(skw->cmd.wq,
			test_bit(SKW_CMD_FLAG_DONE, &skw->cmd.flags), timeout);

	if (likely(ret != 0)) {
		ret = ret > 0 ? 0 - skw->cmd.status : ret;
	} else {
		ret = -ETIMEDOUT;

		skw_err("bus: %s, %s[%d], seq: %d, ret: %d, flags: 0x%lx, xmit_data: %d, timeout:%d(ms)\n",
			skw_bus_name(skw->hw.bus), name, cmd, skw->cmd.seq,
			ret, skw->flags, atomic_read(&skw->xmitting_data),
			jiffies_to_msecs(timeout));

		skw_hw_assert(skw);
	}

	skw_cmd_unlock(skw, extra_flags);

	return ret;
}

/*
 *        +--------------+-----------------+------------------+
 *        |   msg_hdr    |  status_code    |     payload      |
 *        +--------------+-----------------+------------------+
 * octets:        8               2              variable
 *
 */

int skw_cmd_ack_handler(struct skw_core *skw, void *data, int data_len)
{
	struct skw_msg *msg_ack = data;

	if (msg_ack->id != skw->cmd.id || msg_ack->seq != skw->cmd.seq ||
	    test_and_set_bit(SKW_CMD_FLAG_ACKED, &skw->cmd.flags)) {
		skw_err("ack id: %d, ack seq: %d, cmd flag: %ld, cmd seq: %d\n",
			msg_ack->id, msg_ack->seq,
			skw->cmd.flags, skw->cmd.seq);

		return -EINVAL;
	}

	skw->cmd.status = msg_ack->data[0];

	skw_log(SKW_CMD, "[SKWIFI CMD] RX %s[%d], status = %d, used %d msec\n",
		skw->cmd.name, skw->cmd.id, skw->cmd.status,
		jiffies_to_msecs(jiffies - skw->cmd.start_time));

	if (skw->cmd.arg) {
		u16 hdr_len = sizeof(struct skw_msg) + sizeof(u16);
		u16 len = msg_ack->total_len - hdr_len;

		// WARN_ON(msg_ack->total_len - hdr_len != skw->cmd.arg_size);
		if (len != skw->cmd.arg_size)
			skw_warn("%s expect len: %d, recv len: %d\n",
				 skw->cmd.name, skw->cmd.arg_size, len);

		memcpy(skw->cmd.arg, data + hdr_len,
		       min(data_len - hdr_len, (int)skw->cmd.arg_size));
	}

	set_bit(SKW_CMD_FLAG_DONE, &skw->cmd.flags);

	/* make sure cmd.done is valid */
	smp_mb();

	skw->cmd.callback(skw);

	return 0;
}

void skw_event_handler(struct skw_core *skw, struct skw_iface *iface,
		       struct skw_msg *msg_hdr, void *data, size_t data_len)
{
	const char *tag;
	int inst, max_event_id;
	const struct skw_event_func *handler, *func;

	if (msg_hdr->type == SKW_MSG_EVENT_LOCAL) {
		tag = "[SKWIFI LOCAL]";

		inst = iface->id;
		func = g_local_event_fn;
		max_event_id = SKW_EVENT_LOCAL_MAX;
	} else {
		tag = "[SKWIFI EVENT]";

		inst = 0;
		func = g_event_fn;
		max_event_id = SKW_EVENT_MAX;
	}

	if (msg_hdr->id >= max_event_id) {
		skw_err("invalid event id, type: %d, id: %d(max: %d)\n",
			 msg_hdr->type, msg_hdr->id, max_event_id);
		return;
	}

	handler = &func[msg_hdr->id];
	if (!handler->func) {
		skw_err("function not implement, type: %d, id: %d\n",
			 msg_hdr->type, msg_hdr->id);
		return;
	}

	skw_log(SKW_EVENT, "%s iface: %d, %s[%d], seq: %d, len: %ld\n",
		tag, inst, handler->name, msg_hdr->id,
		msg_hdr->seq, (long)data_len);

	handler->func(skw, iface, data, data_len);
}

void skw_default_event_work(struct work_struct *work)
{
	struct skw_core *skw;
	struct sk_buff *skb;
	struct skw_msg *msg_hdr;

	skw = container_of(work, struct skw_core, event_work.work);

	while ((skb = skb_dequeue(&skw->event_work.qlist))) {

		msg_hdr = (struct skw_msg *)skb->data;
		skb_pull(skb, sizeof(struct skw_msg));

		skw_event_handler(skw, NULL, msg_hdr, skb->data, skb->len);

		kfree_skb(skb);
	}
}

int skw_queue_local_event(struct wiphy *wiphy, struct skw_iface *iface,
			  int event_id, void *data, size_t data_len)
{
	int ret;
	struct skw_msg msg;
	struct sk_buff *skb;
	struct skw_event_work *work;
	static u16 local_event_sn;

	skw_dbg("iface: %d, msg: %s, msg len: %ld\n", iface ? iface->id : 0,
		g_local_event_fn[event_id].name, (long)data_len);

	msg.total_len = data_len + sizeof(msg);

	skb = netdev_alloc_skb(NULL, msg.total_len);
	if (!skb) {
		skw_err("alloc skb failed, len: %d\n", msg.total_len);
		return -ENOMEM;
	}

	msg.inst_id = iface ? iface->id : 0;
	msg.type = SKW_MSG_EVENT_LOCAL;
	msg.id = event_id;
	msg.seq = ++local_event_sn;

	skw_put_skb_data(skb, &msg, sizeof(msg));
	skw_put_skb_data(skb, data, data_len);

	if (iface) {
		work = &iface->event_work;
	} else {
		struct skw_core *skw = wiphy_priv(wiphy);

		work = &skw->event_work;
	}

	ret = skw_queue_event_work(wiphy, work, skb);
	if (ret < 0)
		kfree_skb(skb);

	return ret;
}
