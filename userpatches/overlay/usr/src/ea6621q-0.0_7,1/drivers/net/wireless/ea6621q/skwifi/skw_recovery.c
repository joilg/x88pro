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

#include "skw_core.h"
#include "skw_cfg80211.h"
#include "skw_iface.h"
#include "skw_msg.h"
#include "skw_iw.h"
#include "skw_calib.h"
#include "skw_recovery.h"
#include "skw_mlme.h"
#include "skw_rx.h"
#include "skw_tx.h"

static inline void
skw_recovery_sta_disconnect(struct net_device *ndev, u8 *addr)
{
	struct skw_iface *iface = netdev_priv(ndev);

	if (iface->sta.sme_external)
		skw_tx_mlme_mgmt(ndev, IEEE80211_STYPE_DEAUTH, addr, addr, 3);
	else
		skw_disconnected(ndev, 3, true, GFP_KERNEL);
}

static int skw_recovery_sta(struct wiphy *wiphy, struct skw_recovery_data *rd,
				struct skw_iface *iface)
{
	int ret;
	u32 peer_map = rd->iface[iface->id].peer_map;
	struct skw_sta_core *core = &iface->sta.core;
	struct net_device *dev = iface->ndev;

#ifdef SKW_STATE_RECOVERY
	// TODO:
	// recovery peer state

	struct cfg80211_bss *cbss;

	cbss = cfg80211_get_bss(wiphy, core->bss.channel, core->bss.bssid,
				core->bss.ssid, core->bss.ssid_len,
				IEEE80211_BSS_TYPE_ANY, IEEE80211_PRIVACY_ANY);
	if (!cbss) {
		if (!skw_cmd_unjoin(wiphy, dev, peer->addr, 3, true))
			peer->flags |= SKW_PEER_FLAG_DEAUTHED;

		skw_recovery_sta_disconnect(dev, core->bss.bssid);
		return 0;
	}

	ret = skw_join(wiphy, dev, cbss, false);
	if (ret) {
		if (!skw_cmd_unjoin(wiphy, dev, peer->addr, 3, true))
			peer->flags |= SKW_PEER_FLAG_DEAUTHED;

		skw_recovery_sta_disconnect(dev, core->bss.bssid);
		return 0;
	}

	// set key
	// set ip

	cfg80211_put_bss(wiphy, cbss);
#else
	while (peer_map) {
		u8 idx = ffs(peer_map) - 1;
		struct skw_peer *peer = rd->peer[idx];

		if (!peer || peer->flags & SKW_PEER_FLAG_DEAUTHED)
			continue;

		if (ether_addr_equal(peer->addr, core->bss.bssid)) {
			del_timer_sync(&core->timer);
			cancel_work_sync(&iface->sta.work);

			ret = skw_cmd_unjoin(wiphy, dev, peer->addr,
					SKW_LEAVE, true);
			if (ret)
				skw_warn("failed, sta: %pM, ret: %d\n",
					 peer->addr, ret);

			skw_set_state(&core->sm, SKW_STATE_NONE);
			memset(&core->bss, 0, sizeof(struct skw_bss_cfg));
			core->bss.ctx_idx = SKW_INVALID_ID;

			skw_recovery_sta_disconnect(dev, peer->addr);
			peer->flags |= SKW_PEER_FLAG_DEAUTHED;

		} else {
			/* TDLS */
			cfg80211_tdls_oper_request(dev, peer->addr,
					NL80211_TDLS_TEARDOWN,
					SKW_WLAN_REASON_TDLS_TEARDOWN_UNREACHABLE,
					GFP_KERNEL);

			peer->flags |= SKW_PEER_FLAG_DEAUTHED;
		}

		SKW_CLEAR(peer_map, BIT(idx));
	}

	atomic_set(&iface->actived_ctx, 0);
#endif

	return 0;
}

static void
skw_recovery_sap_flush_sta(struct wiphy *wiphy, struct skw_recovery_data *rd,
			struct skw_iface *iface, u8 subtype, u16 reason)
{
	int idx, ret;
	u8 addr[ETH_ALEN];
	struct skw_peer *peer;
	struct skw_core *skw = wiphy_priv(wiphy);
	u32 peer_map = rd->iface[iface->id].peer_map;

	while (peer_map) {

		if (test_bit(SKW_FLAG_FW_ASSERT, &skw->flags))
			break;

		idx = ffs(peer_map) - 1;

		peer = rd->peer[idx];
		if (!peer || peer->flags & SKW_PEER_FLAG_DEAUTHED)
			continue;

		peer->flags |= SKW_PEER_FLAG_DEAUTHED;
		skw_mlme_ap_remove_client(iface, peer->addr);
		skw_del_sta_event(iface, peer->addr, SKW_LEAVE);

		SKW_CLEAR(peer_map, BIT(idx));
	}

	memset(addr, 0xff, ETH_ALEN);
	ret = skw_cmd_del_sta(wiphy, iface->ndev, addr, subtype, reason, true);
	if (ret)
		skw_warn("failed, sta: %pM, ret: %d\n", addr, ret);
}

static int skw_recovery_sap(struct wiphy *wiphy, struct skw_recovery_data *rd,
			struct skw_iface *iface)
{
	int ret, size;
	struct skw_startap_param *param;
	struct net_device *ndev = iface->ndev;

	ret = skw_set_mib(wiphy, iface->ndev);
	if (ret) {
		skw_err("set tlv failed, ret: %d\n", ret);
		return ret;
	}

	param = rd->iface[iface->id].param;
	if (!param) {
		skw_err("invalid param\n");
		return -EINVAL;
	}

	size = rd->iface[iface->id].size;

	ret = skw_send_msg(wiphy, ndev, SKW_CMD_START_AP, param, size, NULL, 0);
	if (ret) {
		skw_err("failed, ret: %d\n", ret);
		return ret;
	}

	// TODO:
	// bind lmac
	skw_lmac_bind_iface(iface->skw, iface, 0);

	skw_dpd_set_coeff_params(wiphy, ndev, param->chan, param->center_chn1,
				 param->center_chn2, param->chan_width);

	skw_recovery_sap_flush_sta(wiphy, rd, iface, 12, SKW_LEAVE);

	return 0;
}

static int skw_recovery_ibss(struct wiphy *wiphy, struct skw_iface *iface)
{
	return 0;
}

static int skw_recovery_p2p_dev(struct wiphy *wiphy, struct skw_iface *iface)
{
	skw_dbg("done\n");

	return 0;
}

static void
skw_recovery_prepare(struct skw_core *skw, struct skw_recovery_data *rd)
{
	int i, j;
	struct skw_peer_ctx *ctx;
	struct skw_iface *iface;

	skw->cmd.seq = 0;
	skw->skw_event_sn = 0;

	for (i = 0; i < SKW_NR_LMAC; i++)
		atomic_set(&skw->hw.lmac[i].fw_credit, 0);

	if (test_and_set_bit(SKW_FLAG_FW_CHIP_RECOVERY, &skw->flags))
		return;

	mutex_lock(&rd->lock);

	for (i = 0; i < SKW_MAX_PEER_SUPPORT; i++) {
		ctx = &skw->peer_ctx[i];

		skw_peer_ctx_lock(ctx);

		rcu_assign_pointer(ctx->entry, NULL);
		rd->peer[i] = ctx->peer;
		ctx->peer = NULL;

		skw_peer_ctx_unlock(ctx);
	}

	spin_lock_bh(&skw->vif.lock);

	for (i = 0; i < SKW_NR_IFACE; i++) {
		iface = skw->vif.iface[i];
		if (!iface)
			continue;

		for (j = 0; j <= SKW_WMM_AC_MAX; j++) {
			skb_queue_purge(&iface->txq[j]);
			skb_queue_purge(&iface->tx_cache[j]);
		}

		rd->iface[i].peer_map = atomic_read(&iface->peer_map);
		atomic_set(&iface->peer_map, 0);
	}

	spin_unlock_bh(&skw->vif.lock);

	mutex_unlock(&rd->lock);
}

static void skw_recovery_work(struct work_struct *wk)
{
	int i, ret;
	struct skw_chip_info chip;
	struct skw_core *skw = container_of(wk, struct skw_core, recovery_work);
	struct wiphy *wiphy = priv_to_wiphy(skw);
	struct skw_recovery_data *rd = &skw->recovery_data;

	skw_dbg("start\n");

	skw_recovery_prepare(skw, rd);

	skw_wifi_enable();

	ret = skw_register_rx_callback(skw, skw_rx_cb, skw);
	if (ret < 0)
		skw_err("register rx callback failed, ret: %d\n", ret);

	skw_hw_xmit_init(skw, skw->hw.dma);

	clear_bit(SKW_FLAG_FW_ASSERT, &skw->flags);
	clear_bit(SKW_FLAG_BLOCK_TX, &skw->flags);
	clear_bit(SKW_FLAG_FW_MAC_RECOVERY, &skw->flags);
	clear_bit(SKW_FLAG_FW_THERMAL, &skw->flags);

	skw_sync_cmd_event_version(wiphy);

	ret = skw_sync_chip_info(wiphy, &chip);
	if (ret)
		skw_err("sync chip info failed, ret: %d\n", ret);

	ret = skw_calib_download(wiphy, skw->fw.calib_file);
	if (ret)
		skw_err("calib download failed, ret: %d\n", ret);

	for (i = 0; i < SKW_NR_IFACE; i++) {
		struct skw_iface *iface = skw->vif.iface[i];

		if (!iface)
			continue;

		if (test_bit(SKW_FLAG_FW_ASSERT, &skw->flags))
			break;

		skw_info("%s: inst: %d\n",
			 skw_iftype_name(iface->wdev.iftype), i);

		ret = skw_cmd_open_dev(wiphy, iface->id, iface->addr,
				iface->wdev.iftype, 0);
		if (ret) {
			skw_err("open %s failed, inst: %d, ret: %d\n",
				skw_iftype_name(iface->wdev.iftype),
				iface->id, ret);

			skw_hw_assert(skw);

			break;
		}

		mutex_lock(&rd->lock);

		switch (iface->wdev.iftype) {
		case NL80211_IFTYPE_STATION:
		case NL80211_IFTYPE_P2P_CLIENT:
			skw_recovery_sta(wiphy, rd, iface);
			break;

		case NL80211_IFTYPE_AP:
		case NL80211_IFTYPE_P2P_GO:
			skw_recovery_sap(wiphy, rd, iface);
			break;

		case NL80211_IFTYPE_ADHOC:
			skw_recovery_ibss(wiphy, iface);
			break;

		case NL80211_IFTYPE_P2P_DEVICE:
			skw_recovery_p2p_dev(wiphy, iface);
			break;

		default:
			break;
		}

		mutex_unlock(&rd->lock);
	}

	if (!ret) {
		for (i = 0; i < SKW_MAX_PEER_SUPPORT; i++) {
			skw_peer_free(rd->peer[i]);
			rd->peer[i] = NULL;
		}

		clear_bit(SKW_FLAG_FW_CHIP_RECOVERY, &skw->flags);
	}
}

void skw_recovery_del_peer(struct skw_iface *iface, u8 peer_idx)
{
	struct skw_recovery_data *rd = &iface->skw->recovery_data;

	if (!test_bit(SKW_FLAG_FW_CHIP_RECOVERY, &iface->skw->flags))
		return;

	mutex_lock(&rd->lock);

	if (rd->peer[peer_idx])
		rd->peer[peer_idx]->flags |= SKW_PEER_FLAG_DEAUTHED;

	mutex_unlock(&rd->lock);
}

int skw_recovery_data_update(struct skw_iface *iface, void *param, int len)
{
	void *data;
	struct skw_recovery_data *rd = &iface->skw->recovery_data;

	if (!param)
		return 0;

	data = SKW_ALLOC(SKW_2K_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	memcpy(data, param, len);

	mutex_lock(&rd->lock);

	SKW_KFREE(rd->iface[iface->id].param);

	rd->iface[iface->id].param = data;
	rd->iface[iface->id].size = len;

	mutex_unlock(&rd->lock);

	return 0;
}

void skw_recovery_data_clear(struct skw_iface *iface)
{
	struct skw_recovery_data *rd = &iface->skw->recovery_data;

	mutex_lock(&rd->lock);

	rd->iface[iface->id].size = 0;
	rd->iface[iface->id].peer_map = 0;
	SKW_KFREE(rd->iface[iface->id].param);

	mutex_unlock(&rd->lock);
}

int skw_recovery_init(struct skw_core *skw)
{
	mutex_init(&skw->recovery_data.lock);
	INIT_WORK(&skw->recovery_work, skw_recovery_work);

	return 0;
}

void skw_recovery_deinit(struct skw_core *skw)
{
	int i;
	struct skw_recovery_data *rd = &skw->recovery_data;

	mutex_lock(&rd->lock);

	cancel_work_sync(&skw->recovery_work);

	for (i = 0; i < SKW_NR_IFACE; i++)
		SKW_KFREE(rd->iface[i].param);

	for (i = 0; i < SKW_MAX_PEER_SUPPORT; i++) {
		skw_peer_free(rd->peer[i]);
		rd->peer[i] = NULL;
	}

	mutex_unlock(&rd->lock);
}
