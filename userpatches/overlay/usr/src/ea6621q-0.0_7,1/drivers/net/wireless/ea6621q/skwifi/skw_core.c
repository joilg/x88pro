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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/ip.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/inetdevice.h>
#include <net/addrconf.h>
#include <linux/ctype.h>
#include <net/tcp.h>
#include <net/arp.h>
#include <linux/platform_device.h>
#include <linux/if_tunnel.h>
#include <linux/firmware.h>
#include <generated/utsrelease.h>
#include <linux/rfkill-wlan.h>

#include "skw_core.h"
#include "skw_cfg80211.h"
#include "skw_tx.h"
#include "skw_rx.h"
#include "skw_iw.h"
#include "skw_timer.h"
#include "skw_work.h"
#include "version.h"
#include "skw_calib.h"
#include "skw_vendor.h"
#include "skw_regd.h"
#include "skw_recovery.h"
#include "trace.h"

static u8 skw_mac[ETH_ALEN];
static atomic_t skw_chip_idx = ATOMIC_INIT(0);

static const int g_skw_up_to_ac[8] = {
	SKW_WMM_AC_BE,
	SKW_WMM_AC_BK,
	SKW_WMM_AC_BK,
	SKW_WMM_AC_BE,
	SKW_WMM_AC_VI,
	SKW_WMM_AC_VI,
	SKW_WMM_AC_VO,
	SKW_WMM_AC_VO
};

#define SKW_CMD_VER(id, ver)   .cmd[id] = ver
#define SKW_EVENT_VER(id, ver) .event[id] = ver
static const struct skw_version_info skw_drv_ver = {
	SKW_CMD_VER(SKW_CMD_DOWNLOAD_INI, V1),
	SKW_CMD_VER(SKW_CMD_GET_INFO, V1),
	SKW_CMD_VER(SKW_CMD_SYN_VERSION, V1),
	SKW_CMD_VER(SKW_CMD_OPEN_DEV, V1),
	SKW_CMD_VER(SKW_CMD_CLOSE_DEV, V1),
	SKW_CMD_VER(SKW_CMD_START_SCAN, V1),
	SKW_CMD_VER(SKW_CMD_STOP_SCAN, V1),
	SKW_CMD_VER(SKW_CMD_START_SCHED_SCAN, V1),
	SKW_CMD_VER(SKW_CMD_STOP_SCHED_SCAN, V1),
	SKW_CMD_VER(SKW_CMD_JOIN, V1),
	SKW_CMD_VER(SKW_CMD_AUTH, V1),
	SKW_CMD_VER(SKW_CMD_ASSOC, V1),
	SKW_CMD_VER(SKW_CMD_ADD_KEY, V1),
	SKW_CMD_VER(SKW_CMD_DEL_KEY, V1),
	SKW_CMD_VER(SKW_CMD_TX_MGMT, V1),
	SKW_CMD_VER(SKW_CMD_TX_DATA_FRAME, V1),
	SKW_CMD_VER(SKW_CMD_SET_IP, V1),
	SKW_CMD_VER(SKW_CMD_DISCONNECT, V1),
	SKW_CMD_VER(SKW_CMD_RPM_REQ, V1),
	SKW_CMD_VER(SKW_CMD_START_AP, V1),
	SKW_CMD_VER(SKW_CMD_STOP_AP, V1),
	SKW_CMD_VER(SKW_CMD_ADD_STA, V1),
	SKW_CMD_VER(SKW_CMD_DEL_STA, V1),
	SKW_CMD_VER(SKW_CMD_GET_STA, V1),
	SKW_CMD_VER(SKW_CMD_RANDOM_MAC, V1),
	SKW_CMD_VER(SKW_CMD_GET_LLSTAT, V1),
	SKW_CMD_VER(SKW_CMD_SET_MC_ADDR, V1),
	SKW_CMD_VER(SKW_CMD_RESUME, V1),
	SKW_CMD_VER(SKW_CMD_SUSPEND, V1),
	SKW_CMD_VER(SKW_CMD_REMAIN_ON_CHANNEL, V1),
	SKW_CMD_VER(SKW_CMD_BA_ACTION, V1),
	SKW_CMD_VER(SKW_CMD_TDLS_MGMT, V1),
	SKW_CMD_VER(SKW_CMD_TDLS_OPER, V1),
	SKW_CMD_VER(SKW_CMD_TDLS_CHANNEL_SWITCH, V1),
	SKW_CMD_VER(SKW_CMD_SET_CQM_RSSI, V1),
	SKW_CMD_VER(SKW_CMD_NPI_MSG, V1),
	SKW_CMD_VER(SKW_CMD_IBSS_JOIN, V1),
	SKW_CMD_VER(SKW_CMD_SET_IBSS_ATTR, V1),
	SKW_CMD_VER(SKW_CMD_RSSI_MONITOR, V1),
	SKW_CMD_VER(SKW_CMD_SET_IE, V1),
	SKW_CMD_VER(SKW_CMD_SET_MIB, V1),
	SKW_CMD_VER(SKW_CMD_REGISTER_FRAME, V1),
	SKW_CMD_VER(SKW_CMD_ADD_TX_TS, V1),
	SKW_CMD_VER(SKW_CMD_DEL_TX_TS, V1),
	SKW_CMD_VER(SKW_CMD_REQ_CHAN_SWITCH, V1),
	SKW_CMD_VER(SKW_CMD_CHANGE_BEACON, V1),
	SKW_CMD_VER(SKW_CMD_DPD_ILC_GEAR_PARAM, V1),
	SKW_CMD_VER(SKW_CMD_DPD_ILC_MARTIX_PARAM, V1),
	SKW_CMD_VER(SKW_CMD_DPD_ILC_COEFF_PARAM, V1),
	SKW_CMD_VER(SKW_CMD_WIFI_RECOVER, V1),
	SKW_CMD_VER(SKW_CMD_PHY_BB_CFG, V1),
	SKW_CMD_VER(SKW_CMD_SET_REGD, V1),
	SKW_CMD_VER(SKW_CMD_SET_EFUSE, V1),
	SKW_CMD_VER(SKW_CMD_SET_PROBEREQ_FILTER, V1),
	SKW_CMD_VER(SKW_CMD_CFG_ANT, V1),
	SKW_CMD_VER(SKW_CMD_RTT, V1),
	SKW_CMD_VER(SKW_CMD_GSCAN, V1),
	SKW_CMD_VER(SKW_CMD_DFS, V1),
	SKW_CMD_VER(SKW_CMD_SET_SPD_ACTION, V2),
	SKW_CMD_VER(SKW_CMD_SET_MONITOR_PARAM, V1),

	/* event */
};

static int skw_core_show(struct seq_file *seq, void *data)
{
	int mac;
	u64 ts;
	struct rtc_time tm;
	unsigned long rem_nsec;
	struct skw_core *skw = seq->private;

#define FLAG_TEST(n) test_bit(SKW_FLAG_##n, &skw->flags)

	ts = skw->fw.host_timestamp;
	rem_nsec = do_div(ts, 1000000000);
	skw_compat_rtc_time_to_tm(skw->fw.host_seconds, &tm);

	seq_puts(seq, "\n");
	seq_printf(seq, "Timestamp: %u - %lu.%06lu (%d-%02d-%02d %02d:%02d:%02d UTC)\n",
		   skw->fw.timestamp,
		   (unsigned long)ts,
		   rem_nsec / 1000,
		   tm.tm_year + 1900,
		   tm.tm_mon + 1,
		   tm.tm_mday,
		   tm.tm_hour,
		   tm.tm_min,
		   tm.tm_sec);

	seq_puts(seq, "\n");
	seq_printf(seq, "SKW Flags:       \t0x%lx %s\n"
			"TX Packets:      \t%ld\n"
			"RX Packets:      \t%ld\n"
			"txqlen_pending: \t%d\n",
			skw->flags,
			FLAG_TEST(FW_ASSERT) ? "(Assert)" : "",
			skw->tx_packets,
			skw->rx_packets,
			atomic_read(&skw->txqlen_pending));

	for (mac = 0; mac < SKW_NR_LMAC; mac++) {
		seq_printf(seq, "mac[%d] credit:  \t%d\n",
			   mac, skw_get_hw_credit(skw, mac));
	}

	seq_puts(seq, "\n");

	if (!skw->hw_pdata)
		return 0;

	/* Firmware & BSP Info */
	seq_printf(seq, "Calib File:      \t%s\n"
			"FW Build:        \t%s\n"
			"FW Version:      \t%s-%s (BSP-MAC)\n"
			"BUS Type:        \t0x%x (%s)\n"
			"Align Size:      \t%d\n"
			"TX Limit:        \t%d\n",
			skw->fw.calib_file,
			skw->fw.build_time,
			skw->fw.plat_ver,
			skw->fw.wifi_ver,
			skw->hw_pdata->bus_type,
			skw_bus_name(skw->hw.bus),
			skw->hw_pdata->align_value,
			skw->hw.pkt_limit);

	seq_puts(seq, "\n");
#undef FLAG_TEST

	return 0;
}

static int skw_core_open(struct inode *inode, struct file *file)
{
	// return single_open(file, skw_core_show, inode->i_private);
	return single_open(file, skw_core_show, skw_pde_data(inode));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops skw_core_fops = {
	.proc_open = skw_core_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations skw_core_fops = {
	.owner = THIS_MODULE,
	.open = skw_core_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int skw_assert_show(struct seq_file *seq, void *data)
{
	return 0;
}

static int skw_assert_open(struct inode *inode, struct file *file)
{
	return single_open(file, skw_assert_show, inode->i_private);
}

static ssize_t skw_assert_write(struct file *fp, const char __user *buf,
				size_t len, loff_t *offset)
{
	struct skw_core *skw = fp->f_inode->i_private;

	skw_hw_assert(skw);

	return len;
}

static const struct file_operations skw_assert_fops = {
	.owner = THIS_MODULE,
	.open = skw_assert_open,
	.read = seq_read,
	.write = skw_assert_write,
	.release = single_release,
};

static int skw_ndo_open(struct net_device *dev)
{
	struct skw_iface *iface = netdev_priv(dev);
	struct skw_core *skw = iface->skw;

	skw_dbg("dev: %s, type: %s\n", netdev_name(dev),
		skw_iftype_name(dev->ieee80211_ptr->iftype));

	if (test_bit(SKW_FLAG_FW_THERMAL, &skw->flags)) {
		skw_warn("disable TX for thermal warnning");
		netif_tx_stop_all_queues(dev);
	}

	netif_carrier_off(dev);

	if (dev->ieee80211_ptr->iftype == NL80211_IFTYPE_MONITOR) {
		dev->type = ARPHRD_IEEE80211;
		netif_tx_stop_all_queues(dev);
	} else
		dev->type = ARPHRD_ETHER;

	return 0;
}

static int skw_ndo_stop(struct net_device *dev)
{
	struct skw_iface *iface = netdev_priv(dev);
	struct skw_core *skw = iface->skw;

	skw_dbg("dev: %s, type: %s\n", netdev_name(dev),
		skw_iftype_name(dev->ieee80211_ptr->iftype));

	//netif_tx_stop_all_queues(dev);

	// fixme:
	// check if sched scan is going on current netdev
	skw_scan_done(skw, iface, true);

	switch (dev->ieee80211_ptr->iftype) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_P2P_CLIENT:
		// if (iface->sta.sm.state !=  SKW_STATE_NONE)
			//skw_disconnect(iface->wdev.wiphy, iface->ndev,
			//		WLAN_REASON_DEAUTH_LEAVING);
		break;

	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_GO:
		// skw_flush_sta_info(iface);
		break;

	case NL80211_IFTYPE_MONITOR:
		skw_cmd_monitor(priv_to_wiphy(skw), NULL, SKW_MONITOR_CLOSE);
		break;

	default:
		break;
	}

	return 0;
}

static bool skw_udp_filter(struct net_device *ndev, struct sk_buff *skb)
{
	bool ret = false;
	struct udphdr *udp = udp_hdr(skb);

#define DHCP_SERVER_PORT   67
#define DHCP_CLIENT_PORT   68
#define DHCPV6_SERVER_PORT 546
#define DHCPV6_CLIENT_PORT 547

	switch (htons(udp->dest)) {
	case DHCP_CLIENT_PORT:
	case DHCP_SERVER_PORT:
#ifdef SKW_DHCP_BROADCAST
		if (ndev->priv_flags & IFF_BRIDGE_PORT) {
			/* set BOOTP flag to broadcast */
			*((u8 *)udp + 18) = 0x80;
			udp->check = 0;
		}
#endif
	case DHCPV6_CLIENT_PORT:
	case DHCPV6_SERVER_PORT:
		ret = true;
		skw_dbg("DHCP, csum: 0x%x\n", udp->check);
		break;

	default:
		ret = false;
		break;
	}

	return ret;
}

static void skw_setup_txba(struct skw_core *skw, struct skw_iface *iface,
			   struct skw_peer *peer, int tid)
{
	int ret;
	struct skw_ba_action tx_ba;

	if (tid >= SKW_NR_TID) {
		skw_warn("tid: %d invalid\n", tid);
		return;
	}

	if ((peer->txba.bitmap | peer->txba.blacklist) & BIT(tid))
		return;

	tx_ba.tid = tid;
	tx_ba.win_size = 64;
	tx_ba.peer_idx = peer->idx;
	tx_ba.action = SKW_ADD_TX_BA;

	ret = skw_queue_work(priv_to_wiphy(skw), iface, SKW_WORK_SETUP_TXBA,
				&tx_ba, sizeof(tx_ba));
	if (!ret)
		peer->txba.bitmap |= BIT(tid);
}

struct skw_ctx_entry *skw_get_ctx_entry(struct skw_core *skw, const u8 *addr)
{
	int i;
	struct skw_ctx_entry *entry;

	for (i = 0; i < SKW_MAX_PEER_SUPPORT; i++) {
		entry = rcu_dereference(skw->peer_ctx[i].entry);
		if (entry && ether_addr_equal(entry->addr, addr))
			return entry;
	}

	return NULL;
}

struct skw_peer_ctx *skw_get_ctx(struct skw_core *skw, u8 idx)
{
	if (idx >= SKW_MAX_PEER_SUPPORT)
		return NULL;

	return &skw->peer_ctx[idx];
}

static int skw_downgrade_ac(struct skw_iface *iface, int aci)
{
#if 1
	while (iface->wmm.acm & BIT(aci)) {
		if (aci == SKW_WMM_AC_BK)
			break;
		aci++;
	}
#else
	if (iface->wmm.acm & BIT(aci)) {
		int i;
		int acm = SKW_WMM_AC_BK;

		for (i = SKW_WMM_AC_BK; i >= 0; i--) {
			if (!(iface->wmm.acm & BIT(i))) {
				acm = i;
				break;
			}
		}

		aci = acm;
	}
#endif

	return aci;
}

static inline bool skw_is_tcp_pure_ack(struct sk_buff *skb)
{
	return tcp_hdr(skb)->ack &&
	       ntohs(ip_hdr(skb)->tot_len) == ip_hdrlen(skb) + tcp_hdrlen(skb);
}

static netdev_tx_t skw_ndo_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	int ret = -1;
	u8 tid;
	u8 fixed_rate = 0;
	u8 fixed_tid = SKW_INVALID_ID;
	u8 peer_index = SKW_INVALID_ID;
	u8 ac_idx = 0, padding = 0;
	u8 prot = 0, tcp_pkt = 0, do_csum = 0;
	bool is_prot_filter = false;
	bool is_udp_filter = false;
	bool is_802_3_frame = false;
	bool pure_tcp_ack = false;
	const u8 tid_map[] = {6, 4, 0, 1};
	int l4_hdr_offset = 0, reset_l4_offset = 0;
	int msdu_len, align_len;
	int nhead, ntail, nroom, txq_len;
	bool is_completed = true;

	struct netdev_queue *txq;
	struct skw_peer_ctx *ctx;
	struct skw_iface *iface = netdev_priv(ndev);
	struct ethhdr *eth = eth_hdr(skb);
	struct skw_ctx_entry *entry = NULL;
	struct sk_buff_head *qlist;
	struct skw_tx_desc_hdr *desc_hdr;
	struct skw_tx_desc_conf *desc_conf;
	struct skw_core *skw = iface->skw;

	/* Mini frame size that HW support */
	if (unlikely(skb->len <= 16)) {
		skw_dbg("current: %s\n", current->comm);
		skw_hex_dump("short skb", skb->data, skb->len, true);

		if (skb->len == ETH_HLEN && eth->h_proto == htons(ETH_P_IP))
			goto free;
		else
			SKW_BUG_ON(1);
	}

	if (skb_linearize(skb))
		goto free;

	msdu_len = skb->len;
	SKW_SKB_TXCB(skb)->skb_native_len = skb->len;
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		l4_hdr_offset = skb_checksum_start_offset(skb);
		do_csum = 1;
	}

	switch (eth->h_proto) {
	case htons(ETH_P_IP):
		prot = ip_hdr(skb)->protocol;
		reset_l4_offset = ETH_HLEN + ip_hdrlen(skb);

		if (prot == IPPROTO_TCP)
			pure_tcp_ack = skw_is_tcp_pure_ack(skb);

		break;

	case htons(ETH_P_IPV6):
		prot = ipv6_hdr(skb)->nexthdr;
		// fixme:
		// get tcp/udp head offset
		reset_l4_offset = ETH_HLEN + sizeof(struct ipv6hdr);
		break;

	case htons(ETH_P_ARP):
		if (test_bit(SKW_FLAG_FW_FILTER_ARP, &skw->flags))
			is_prot_filter = true;

#ifdef SKW_ARP_NAT
		if (ndev->priv_flags & IFF_BRIDGE_PORT) {
			if (iface->wdev.iftype == NL80211_IFTYPE_STATION) {
				struct sk_buff *arp_skb;
				struct skw_ctx_entry *e;
				struct skw_arphdr *arp = skw_arp_hdr(skb);

				rcu_read_lock();
				e = skw_get_ctx_entry(iface->skw, arp->ar_sha);
				if (e)
					e->peer->ip_addr = arp->ar_sip;

				rcu_read_unlock();

				arp_skb = arp_create(ntohs(arp->ar_op),
						ETH_P_ARP, arp->ar_tip,
						iface->ndev,
						arp->ar_sip, eth->h_dest,
						iface->addr, arp->ar_tha);

				kfree_skb(skb);

				skb = arp_skb;
				if (!skb)
					return NETDEV_TX_OK;

				eth = skw_eth_hdr(skb);
			}
		}
#endif

		//fixed_rate = 1;
		//fixed_tid = 4;
		break;

	case htons(ETH_P_PAE):
	case htons(SKW_ETH_P_WAPI):
	case htons(ETH_P_TDLS):
		is_prot_filter = true;
		break;

	default:
		if (eth->h_proto < ETH_P_802_3_MIN)
			is_802_3_frame = true;

		break;
	}

	if (!do_csum && (prot == IPPROTO_UDP || prot == IPPROTO_TCP))
		skb_set_transport_header(skb, reset_l4_offset);

	// fixme:
	/* enable checksum for TCP & UDP frame, except framgment frame */
	switch (prot) {
	case IPPROTO_UDP:
		is_udp_filter = skw_udp_filter(ndev, skb);
		if (udp_hdr(skb)->check == 0)
			do_csum = 0;

		break;

	case IPPROTO_TCP:
		tcp_pkt = 1;

		break;

	default:
		break;
	}

	ac_idx = skw_downgrade_ac(iface, g_skw_up_to_ac[skb->priority]);
	tid = fixed_tid != SKW_INVALID_ID ? fixed_tid : tid_map[ac_idx];

	rcu_read_lock();

	switch (iface->wdev.iftype) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_P2P_GO:
		if (is_unicast_ether_addr(eth->h_dest)) {
			entry = skw_get_ctx_entry(skw, eth->h_dest);
			peer_index = entry ? entry->idx : SKW_INVALID_ID;

			if (entry && entry->peer->sm.state != SKW_STATE_COMPLETED)
				is_completed = false;
		} else {
			fixed_rate = 1;
			peer_index = iface->default_multicast;
		}

		break;

	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_P2P_CLIENT:
	case NL80211_IFTYPE_ADHOC:

		if (atomic_read(&iface->actived_ctx) > 1)
			entry = skw_get_ctx_entry(skw, eth->h_dest);

		if (!entry) {
			ctx = skw_get_ctx(skw, iface->sta.core.bss.ctx_idx);
			if (ctx)
				entry = rcu_dereference(ctx->entry);
		}

		peer_index = entry ? entry->idx : SKW_INVALID_ID;

		if (iface->sta.core.sm.state != SKW_STATE_COMPLETED)
			is_completed = false;

		break;

	default:
		peer_index = SKW_INVALID_ID;
		break;
	}

	if (entry && is_completed)
		skw_setup_txba(skw, iface, entry->peer, tid);

	rcu_read_unlock();

	if (peer_index == SKW_INVALID_ID) {
		skw_dbg("%s drop dst: %pM, proto: 0x%x\n",
			netdev_name(ndev), eth->h_dest, htons(eth->h_proto));

		goto free;
	}

	padding = (long)(skb->data - skw->skb_headroom) & SKW_DATA_ALIGN_MASK;
	if (padding)
		padding = SKW_DATA_ALIGN_SIZE - padding;

	nroom = skb_headroom(skb);
	nhead = skw->skb_headroom > nroom ? skw->skb_headroom - nroom : 0;

	nroom = skb->len + skw->skb_headroom + padding - SKW_DATA_ALIGN_SIZE;
	align_len = round_up(nroom, skw->hw.align);
	nroom = align_len - nroom;
	ntail = nroom > skb_tailroom(skb) ? nroom - skb_tailroom(skb) : 0;

	if (nhead || ntail || skb_cloned(skb)) {
		if (unlikely(pskb_expand_head(skb, nhead, ntail, GFP_ATOMIC))) {
			skw_dbg("failed, nhead: %d, ntail: %d\n", nhead, ntail);
			goto free;
		}

		eth = skw_eth_hdr(skb);
	}

	desc_conf = (void *)skb_push(skb, sizeof(*desc_conf));
	desc_conf->csum = do_csum;
	desc_conf->ip_prot = tcp_pkt;
	desc_conf->l4_hdr_offset = l4_hdr_offset;

	if (skw->hw.bus == SKW_BUS_PCIE) {
		dma_addr_t pa;

		SKW_SKB_TXCB(skb)->e.eth_type = eth->h_proto;
		SKW_SKB_TXCB(skb)->e.mac_id = iface->id;
		SKW_SKB_TXCB(skb)->e.tid = tid;
		SKW_SKB_TXCB(skb)->e.peer_idx = peer_index;
		SKW_SKB_TXCB(skb)->e.prot = SKW_ETHER_FRAME;
		SKW_SKB_TXCB(skb)->e.encry_dis = 0;
		SKW_SKB_TXCB(skb)->e.rate = fixed_rate;
		SKW_SKB_TXCB(skb)->e.msdu_len = msdu_len;

		SKW_SKB_TXCB(skb)->skb_data_pa = skw_pci_map_single(skw,
			skb->data, skb->len, DMA_TO_DEVICE);
		pa = skw->hw_pdata->virtaddr_to_pcieaddr(skb->data);
		SKW_SKB_TXCB(skb)->e.pa = (pa + 2) & 0xFFFFFFFFFF;
	} else {
		if (padding)
			skb_push(skb, padding);

		desc_hdr = (void *)skb_push(skb, sizeof(*desc_hdr));
		desc_hdr->padding_gap = padding;
		desc_hdr->inst = iface->id & 0x3;

		desc_hdr->tid = tid;
		desc_hdr->peer_lut = peer_index;
		desc_hdr->frame_type = SKW_ETHER_FRAME;
		skw_set_tx_desc_eth_type(desc_hdr, eth->h_proto);

		desc_hdr->encry_dis = 0;
		desc_hdr->msdu_len = msdu_len;
		desc_hdr->rate = fixed_rate;
	}
	__net_timestamp(skb);

	if (unlikely(is_prot_filter || is_udp_filter || is_802_3_frame)) {
		skw_dbg("proto: 0x%x, udp filter: %d, 802.3 frame: %d\n",
			htons(eth->h_proto), is_udp_filter, is_802_3_frame);
		if (skw->hw.bus == SKW_BUS_PCIE) {
			desc_hdr = (void *)skb_push(skb, sizeof(*desc_hdr));
			desc_hdr->padding_gap = padding;
			desc_hdr->inst = iface->id & 0x3;

			desc_hdr->tid = tid;
			desc_hdr->peer_lut = peer_index;
			desc_hdr->frame_type = SKW_ETHER_FRAME;

			desc_hdr->encry_dis = 0;
			desc_hdr->msdu_len = msdu_len;
			desc_hdr->rate = fixed_rate;
			skw_dbg("Add desc hdr for spec data\n");
		}
		ret = skw_msg_try_send(skw, iface->id, SKW_CMD_TX_DATA_FRAME,
				       skb->data, skb->len, NULL, 0,
				       "SKW_CMD_TX_DATA_FRAME");
		if (ret < 0) {
			if (SKW_SKB_TXCB(skb)->tx_retry++ > 3) {
				skw_queue_work(priv_to_wiphy(skw), iface,
					       SKW_WORK_TX_ETHER_DATA,
					       skb->data, skb->len);
			} else {
				skb_pull(skb, skb->len - msdu_len);
				return NETDEV_TX_BUSY;
			}
		}

		goto free;
	}

	SKW_SKB_TXCB(skb)->peer_idx = peer_index;

	if (pure_tcp_ack)
		ac_idx = SKW_ACK_TXQ;

	qlist = &iface->txq[ac_idx];

	skb_queue_tail(qlist, skb);
	skw_wakeup_tx(skw);

	trace_skw_tx_xmit(eth->h_dest, peer_index, prot, fixed_rate,
			  do_csum, ac_idx, skb_queue_len(qlist));

	txq_len = skb_queue_len(qlist) +
		skb_queue_len(&iface->tx_cache[ac_idx]);
	if (txq_len >= SKW_TXQ_HIGH_THRESHOLD) {
		txq = netdev_get_tx_queue(ndev, ac_idx);
		if (!netif_tx_queue_stopped(txq))
			netif_tx_stop_queue(txq);
	}

	return NETDEV_TX_OK;

free:
	if (ret != 0)
		ndev->stats.tx_dropped++;

	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

static u16 skw_ndo_select(struct net_device *dev, struct sk_buff *skb
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
	, void *accel_priv
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
	, void *accel_priv, select_queue_fallback_t fallback
#else
	, struct net_device *ndev
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 2, 0)
	, select_queue_fallback_t cb
#endif
#endif
	SKW_NULL)
{
	struct skw_iface *iface = netdev_priv(dev);

	if (!iface->wmm.qos_enabled) {
		skb->priority = 0;
		return SKW_WMM_AC_BE;
	}

	skb->priority = skw_compat_classify8021d(skb, iface->qos_map);

	return g_skw_up_to_ac[skb->priority];
}

static int skw_android_cmd(struct net_device *dev, void __user *priv_data, int cmd)
{
	char command[128], *data;
	struct android_wifi_priv_cmd priv_cmd;
	bool compat_task = false;

	if (!priv_data)
		return  -EINVAL;

#ifdef CONFIG_COMPAT
	if (SKW_IS_COMPAT_TASK()) {
		struct compat_android_wifi_priv_cmd compat;

		if (copy_from_user(&compat, priv_data, sizeof(compat)))
			return -EFAULT;

		priv_cmd.buf = compat_ptr(compat.buf);
		priv_cmd.used_len = compat.used_len;
		priv_cmd.total_len = compat.total_len;

		compat_task = true;
	}
#endif

	if (!compat_task &&
	    copy_from_user(&priv_cmd, priv_data, sizeof(priv_cmd)))
		return -EFAULT;

	if (copy_from_user(command, priv_cmd.buf, sizeof(command)))
		return -EFAULT;

	command[127] = 0;

	skw_dbg("%s: %s\n", netdev_name(dev), command);

#define IS_SKW_CMD(c, k)        \
	!strncasecmp(c, SKW_ANDROID_PRIV_##k, strlen(SKW_ANDROID_PRIV_##k))
#define SKW_CMD_DATA(c, k)     \
	(c + strlen(SKW_ANDROID_PRIV_##k) + 1)
#define SKW_CMD_DATA_LEN(c, k) \
	(sizeof(command) - strlen(SKW_ANDROID_PRIV_##k) - 1)

	if (IS_SKW_CMD(command, COUNTRY)) {
		data = SKW_CMD_DATA(command, COUNTRY);
		skw_set_regdom(dev->ieee80211_ptr->wiphy, data);
	} else if (IS_SKW_CMD(command, BTCOEXSCAN_STOP)) {
	} else if (IS_SKW_CMD(command, RXFILTER_START)) {
	} else if (IS_SKW_CMD(command, RXFILTER_STOP)) {
	} else if (IS_SKW_CMD(command, RXFILTER_ADD)) {
	} else if (IS_SKW_CMD(command, RXFILTER_REMOVE)) {
	} else if (IS_SKW_CMD(command, SETSUSPENDMODE)) {
	} else if (IS_SKW_CMD(command, BTCOEXMODE)) {
	} else if (IS_SKW_CMD(command, SET_AP_WPS_P2P_IE)) {
	} else {
		skw_info("Unsupport cmd: %s - ignored\n", command);
	}

#undef IS_SKW_CMD

	return 0;
}

static int skw_ndo_ioctl(struct net_device *dev, struct ifreq *ifr,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
			void __user *data,
#endif
			int cmd)
{
	int ret;
	void __user *priv = ifr->ifr_data;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	priv = data;
#endif

	switch (cmd) {
	case SIOCDEVPRIVATE + 1:
		ret = skw_android_cmd(dev, priv, cmd);
		break;

	default:
		ret = -ENOTSUPP;
		skw_warn("%s, unsupport cmd: 0x%x\n", netdev_name(dev), cmd);

		break;
	}

	return ret;
}

static void skw_ndo_tx_timeout(struct net_device *dev
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
				, unsigned int txqueue
#endif
				)
{
	struct skw_iface *iface = (struct skw_iface *)netdev_priv(dev);
	struct skw_core *skw;

	if (!iface) {
		skw_err("iface NULL\n");
		return;
	}

	skw = iface->skw;

	skw_warn("ndev:%s,flag:0x%lx\n", netdev_name(dev), skw->flags);
}

static void skw_ndo_set_rx_mode(struct net_device *dev)
{
	int count, total_len;
	struct skw_mc_list *mc;
	struct netdev_hw_addr *ha;

	skw_dbg("%s, mc: %d, uc: %d\n", netdev_name(dev),
		netdev_mc_count(dev), netdev_uc_count(dev));

	count = netdev_mc_count(dev);
	if (!count)
		return;

	total_len = sizeof(*mc) + sizeof(struct mac_address) * count;
	mc = SKW_ALLOC(total_len, GFP_ATOMIC);
	if (!mc) {
		skw_err("alloc failed, mc count: %d, total_len: %d\n",
			count, total_len);
		return;
	}

	mc->count = count;

	count = 0;
	netdev_for_each_mc_addr(ha, dev)
		ether_addr_copy(mc->mac[count++].addr, ha->addr);

	skw_queue_work(dev->ieee80211_ptr->wiphy, netdev_priv(dev),
		SKW_WORK_SET_MC_ADDR, mc, total_len);

	SKW_KFREE(mc);
}
#if 0
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
static struct rtnl_link_stats64 *
#else
static void
#endif
skw_ndo_get_stats64(struct net_device *dev,
		    struct rtnl_link_stats64 *stats)
{
	int i;

	for_each_possible_cpu(i) {
		const struct pcpu_sw_netstats *tstats;
		u64 rx_packets, rx_bytes, tx_packets, tx_bytes;
		unsigned int start;

		tstats = per_cpu_ptr(dev->tstats, i);

		do {
			start = u64_stats_fetch_begin_irq(&tstats->syncp);
			rx_packets = tstats->rx_packets;
			tx_packets = tstats->tx_packets;
			rx_bytes = tstats->rx_bytes;
			tx_bytes = tstats->tx_bytes;
		} while (u64_stats_fetch_retry_irq(&tstats->syncp, start));

		stats->rx_packets += rx_packets;
		stats->tx_packets += tx_packets;
		stats->rx_bytes   += rx_bytes;
		stats->tx_bytes   += tx_bytes;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
	return stats;
#endif
}
#endif

static int
skw_ndo_set_mac_address(struct net_device *dev, void *addr)
{
	struct skw_iface *iface = (struct skw_iface *)netdev_priv(dev);
	struct sockaddr *sa = addr;
	int ret = 0;

	skw_dbg("mac: %pM\n", sa->sa_data);

	ret = eth_mac_addr(dev, sa);
	if (ret) {
		skw_err("failed, addr: %pM, ret: %d\n", sa->sa_data, ret);
		return ret;
	}

	ret = skw_send_msg(iface->wdev.wiphy, dev, SKW_CMD_RANDOM_MAC,
			sa->sa_data, ETH_ALEN, NULL, 0);
	if (ret) {
		skw_err("set mac: %pM failed, ret: %d\n",
			sa->sa_data, ret);

		eth_mac_addr(dev, iface->addr);

		return ret;
	}

	ether_addr_copy(iface->addr, sa->sa_data);

	return 0;
}

#if 0
static void skw_ndo_uninit(struct net_device *ndev)
{
	struct skw_iface *iface = netdev_priv(ndev);
	struct wiphy *wiphy = ndev->ieee80211_ptr->wiphy;

	skw_dbg("%s\n", netdev_name(ndev));

	free_percpu(ndev->tstats);

	skw_del_vif(wiphy, iface);
	skw_iface_teardown(wiphy, iface);
	skw_release_inst(wiphy, iface->id);
}
#endif

static const struct net_device_ops skw_netdev_ops = {
	// .ndo_uninit = skw_ndo_uninit,
	.ndo_open = skw_ndo_open,
	.ndo_stop = skw_ndo_stop,
	.ndo_start_xmit = skw_ndo_xmit,
	.ndo_select_queue = skw_ndo_select,
	.ndo_tx_timeout = skw_ndo_tx_timeout,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	.ndo_siocdevprivate = skw_ndo_ioctl,
#else
	.ndo_do_ioctl = skw_ndo_ioctl,
#endif
	//.ndo_get_stats64 = skw_ndo_get_stats64,
	.ndo_set_rx_mode = skw_ndo_set_rx_mode,
	.ndo_set_mac_address = skw_ndo_set_mac_address,
};

int skw_netdev_init(struct wiphy *wiphy, struct net_device *ndev, u8 *addr)
{
	struct skw_core *skw;
	struct skw_iface *iface;

	if (!ndev)
		return -EINVAL;

	skw = wiphy_priv(wiphy);
	iface = netdev_priv(ndev);
	SET_NETDEV_DEV(ndev, wiphy_dev(wiphy));

	ndev->features = NETIF_F_GRO |
			 NETIF_F_IP_CSUM |
			 NETIF_F_IPV6_CSUM;

	ndev->ieee80211_ptr = &iface->wdev;
	ndev->netdev_ops = &skw_netdev_ops;
	ndev->watchdog_timeo = 3 * HZ;
	ndev->needed_headroom =	skw->skb_headroom;
	ndev->needed_tailroom = skw->hw_pdata->align_value;
	// ndev->priv_destructor = skw_netdev_deinit;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
	ndev->destructor = free_netdev;
#else
	ndev->needs_free_netdev = true;
#endif

	skw_set_hw_addr(ndev, addr);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
	ndev->tstats = netdev_alloc_pcpu_stats(struct pcpu_tstats);
#else
	ndev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
#endif
	if (!ndev->tstats)
		return -ENOMEM;

#ifdef CONFIG_WIRELESS_EXT
	ndev->wireless_handlers = skw_iw_handlers();
#endif

	return 0;
}

#if 0
void skw_netdev_deinit(struct net_device *dev)
{
	struct skw_iface *iface = netdev_priv(dev);

	skw_dbg("%s\n", netdev_name(dev));

#if 0 //move these actions to ndo_uninit
	free_percpu(dev->tstats);
	skw_cmd_close_dev(iface->wdev.wiphy, iface->id);
	skw_del_vif(iface->skw, iface);
	skw_iface_teardown(iface);
#endif
}
#endif

int skw_sync_cmd_event_version(struct wiphy *wiphy)
{
	int i, ret = 0;
	struct skw_version_info skw_fw_ver;

	ret = skw_msg_xmit(wiphy, 0, SKW_CMD_SYN_VERSION,
			   NULL, 0, &skw_fw_ver, sizeof(skw_fw_ver));
	if (ret) {
		skw_err("ret: %d\n", ret);
		return ret;
	}

	for (i = 0; i < SKW_MAX_MSG_ID; i++) {
		if (skw_fw_ver.cmd[i] == 0 || skw_drv_ver.cmd[i] == 0)
			continue;

		if (skw_drv_ver.cmd[i] != skw_fw_ver.cmd[i]) {
			skw_warn("cmd: %d, drv ver: %d, fw ver: %d (mismatch, continuing)\n",
				 i, skw_drv_ver.cmd[i], skw_fw_ver.cmd[i]);
		}
	}

	return ret;

}

static int skw_set_capa(struct skw_core *skw, int capa)
{
	int idx, bit;
	int size = sizeof(skw->ext_capa);

	idx = capa / BITS_PER_BYTE;
	bit = capa % BITS_PER_BYTE;

	BUG_ON(idx >= size);

	skw->ext_capa[idx] |= BIT(bit);

	return 0;
}

static int skw_set_ext_capa(struct skw_core *skw, struct skw_chip_info *chip)
{
	skw_set_capa(skw, SKW_EXT_CAPA_BSS_TRANSITION);
	skw_set_capa(skw, SKW_EXT_CAPA_MBSSID);
	skw_set_capa(skw, SKW_EXT_CAPA_TDLS_SUPPORT);
	skw_set_capa(skw, SKW_EXT_CAPA_TWT_REQ_SUPPORT);

	return 0;
}

static const char *skw_get_chip_id(u32 chip_type)
{
	if (strlen(CONFIG_SKW_CHIP_ID))
		return CONFIG_SKW_CHIP_ID;

	switch (chip_type) {
	case 0x100:
		return "EA6621Q";

	case 0x101:
		return "EA6521QF";

	case 0x102:
		return "EA6621QT";

	case 0x103:
		return "EA6521QT";

	default:
		skw_err("Unsupport chip type: 0x%x\n", chip_type);
		break;
	}

	return NULL;
}

int skw_sync_chip_info(struct wiphy *wiphy, struct skw_chip_info *chip)
{
	int ret;
	const char *chipid;
	int vendor, revision;
	u64 ts = local_clock();
	struct skw_core *skw = wiphy_priv(wiphy);

	skw->fw.host_timestamp = ts;
	skw->fw.host_seconds = skw_get_seconds();

	do_div(ts, 1000000);
	ret = skw_msg_xmit(wiphy, 0, SKW_CMD_GET_INFO, &ts, sizeof(ts),
			   chip, sizeof(*chip));
	if (ret) {
		skw_err("ret: %d\n", ret);
		return ret;
	}

	if (chip->priv_filter_arp)
		set_bit(SKW_FLAG_FW_FILTER_ARP, &skw->flags);

	if (chip->priv_ignore_cred)
		set_bit(SKW_FLAG_FW_IGNORE_CRED, &skw->flags);

	memcpy(skw->fw.build_time, chip->fw_build_time,
	       sizeof(skw->fw.build_time));
	memcpy(skw->fw.plat_ver, chip->fw_plat_ver, sizeof(skw->fw.plat_ver));
	memcpy(skw->fw.wifi_ver, chip->fw_wifi_ver, sizeof(skw->fw.wifi_ver));

	skw->fw.timestamp = chip->fw_timestamp;
	skw->fw.max_num_sta = chip->max_sta_allowed;
	skw->fw.fw_bw_capa = chip->fw_bw_capa;

	chipid = skw_get_chip_id(chip->fw_chip_type);
	if (!chipid) {
		skw_err("chip id 0x%x not support\n", chip->fw_chip_type);
		return -ENOTSUPP;
	}

	/* BIT[0:1] Reserved
	 * BIT[2:3] BUS Type
	 * BIT[4:7] Vendor ID
	 */
	vendor = 0;

#ifdef CONFIG_SKW6316_CALIB_BUS_SENSITIVE
	vendor |= ((skw->hw.bus & 0x3) << 2);
#endif

	revision = skw->hw_pdata->chipid[15];

	snprintf(skw->fw.calib_file, sizeof(skw->fw.calib_file),
		"%s_%s_R%02x%03x.bin", chipid, CONFIG_SKW_PROJECT_NAME,
		vendor & 0xff, revision);

	skw_set_ext_capa(skw, chip);
	/* HT capa */

	return 0;
}

static void skw_setup_mac_address(struct wiphy *wiphy, u8 *user_mac, u8 *hw_mac)
{
	int i;
	u8 addr[ETH_ALEN] = {0};
	struct skw_core *skw = wiphy_priv(wiphy);

	if (user_mac && is_valid_ether_addr(user_mac)) {
		ether_addr_copy(addr, user_mac);
	} else if (hw_mac && is_valid_ether_addr(hw_mac)) {
		ether_addr_copy(addr, hw_mac);
	} else {
		eth_random_addr(addr);
		addr[0] = 0xFE;
		addr[1] = 0xFD;
		addr[2] = 0xFC;
	}

	for (i = 0; i < SKW_NR_IFACE; i++) {
		ether_addr_copy(skw->address[i].addr, addr);

		if (i != 0) {
			skw->address[i].addr[0] |= BIT(1);
			skw->address[i].addr[3] ^= BIT(i);
		}

		skw_dbg("addr[%d]: %pM\n", i, skw->address[i].addr);
	}
}

static int skw_buffer_init(struct skw_core *skw)
{
	int ret, size;

	if (skw->hw.bus == SKW_BUS_PCIE) {
		ret = skw_edma_init(priv_to_wiphy(skw));
		if (ret < 0) {
			skw_err("edma init failed, ret: %d\n", ret);
			return ret;
		}

		skw->cmd.data = skw->edma_cmd.current_node->buffer;
	} else {
		size = sizeof(struct scatterlist);

		skw->sgl_dat = kcalloc(SKW_NR_SGL_DAT, size, GFP_KERNEL);
		if (!skw->sgl_dat) {
			skw_err("sg list malloc failed, sg length: %d\n",
				SKW_NR_SGL_DAT);

			return -ENOMEM;
		}

		skw->sgl_cmd = kcalloc(SKW_NR_SGL_CMD, size, GFP_KERNEL);
		if (!skw->sgl_cmd) {
			skw_err("sg list malloc failed, sg length: %d\n",
				SKW_NR_SGL_CMD);

			SKW_KFREE(skw->sgl_dat);
			return -ENOMEM;
		}

		skw->cmd.data = SKW_ALLOC(SKW_MSG_BUFFER_LEN, GFP_KERNEL);
		if (!skw->cmd.data) {
			skw_err("mallc data buffer failed\n");
			SKW_KFREE(skw->sgl_dat);
			SKW_KFREE(skw->sgl_cmd);
			return -ENOMEM;
		}
	}

	return 0;
}

static void skw_buffer_deinit(struct skw_core *skw)
{
	if (skw->hw.bus == SKW_BUS_PCIE) {
		skw_edma_deinit(priv_to_wiphy(skw));
	} else {
		SKW_KFREE(skw->sgl_dat);
		SKW_KFREE(skw->sgl_cmd);
		SKW_KFREE(skw->cmd.data);

		//TODO : recycle txqlen_pending if BSP support API
		if (unlikely(atomic_read(&skw->txqlen_pending)))
			skw_err("txqlen_pending:%d remind\n", atomic_read(&skw->txqlen_pending));
	}
}

static struct wakeup_source *skw_wakeup_source_init(const char *name)
{
	struct wakeup_source *ws;

	ws = wakeup_source_create(name);
	if (ws)
		wakeup_source_add(ws);

	return ws;
}

static void skw_wakeup_source_deinit(struct wakeup_source *ws)
{
	if (ws) {
		wakeup_source_remove(ws);
		wakeup_source_destroy(ws);
	}
}

static void skw_lmac_port_init(struct skw_core *skw, int port, bool multi_dport)
{
	int i, lport, dport;

	for (i = 0; i < SKW_NR_LMAC; i++) {
		if (multi_dport) {
			dport = ((port >> (i * 4)) & 0xf);
			lport = dport;
		} else {
			lport = skw->hw.lmac[i].id;
			dport = port;
		}

		skw->hw.lmac[i].lport = lport;
		skw->hw.lmac[i].dport = dport;
	}
}

static void skw_hw_info_init(struct skw_core *skw, struct sv6160_platform_data *pdata)
{
	int i, bus;
	bool extra_v2;

	atomic_set(&skw->hw.credit, 0);
	skw->hw.align = pdata->align_value;
	skw->hw.cmd_port = pdata->cmd_port;
	skw->hw.pkt_limit = pdata->max_buffer_size / SKW_TX_PACK_SIZE;
	skw->hw.dma = (pdata->bus_type >> 3) & 0x3;

	if (skw->hw.pkt_limit >= SKW_NR_SGL_DAT) {
		WARN_ONCE(1, "pkt limit(%d) larger than buffer", skw->hw.pkt_limit);
		skw->hw.pkt_limit = SKW_NR_SGL_DAT - 1;
	}

	for (i = 0; i < SKW_NR_LMAC; i++) {
		skw->hw.lmac[i].id = i;
		skw->hw.lmac[i].lport = -1;
		skw->hw.lmac[i].dport = -1;
		skw->hw.lmac[i].flags = SKW_LMAC_FLAG_INIT;
		atomic_set(&skw->hw.lmac[i].fw_credit, 0);
	}

	bus = pdata->bus_type & SKW_BUS_TYPE_MASK;
	if (bus == SKW_BUS_SDIO2) {
		bus = SKW_BUS_SDIO;
		extra_v2 = true;
	} else if (bus == SKW_BUS_USB2) {
		bus = SKW_BUS_USB;
		extra_v2 = true;
	} else {
		extra_v2 = false;
	}

	switch (bus) {
	case  SKW_BUS_SDIO:
		skw->hw.bus = SKW_BUS_SDIO;
		skw->hw.bus_dat_xmit = skw_sdio_xmit;
		skw->hw.bus_cmd_xmit = skw_sdio_cmd_xmit;

		skw->hw.extra.hdr_len = SKW_EXTER_HDR_SIZE;
		if (extra_v2)
			skw->hw.extra.len_offset = 0;
		else
			skw->hw.extra.len_offset = 7;

		skw->hw.extra.eof_offset = 23;
		skw->hw.extra.chn_offset = 24;

		skw->hw.rx_desc.hdr_offset = SKW_SDIO_RX_DESC_HDR_OFFSET;
		skw->hw.rx_desc.msdu_offset = SKW_SDIO_RX_DESC_MSDU_OFFSET;

		skw->hw.flags = SKW_HW_FLAG_SDIO_V2 | SKW_HW_FLAG_EXTRA_HDR;
		skw_lmac_port_init(skw, pdata->data_port, true);

		break;

	case SKW_BUS_USB:
		skw->hw.bus = SKW_BUS_USB;
		skw->hw.bus_dat_xmit = skw_usb_xmit;
		skw->hw.bus_cmd_xmit = skw_usb_cmd_xmit;
		skw->hw.flags = SKW_HW_FLAG_EXTRA_HDR;
		skw->hw.extra.hdr_len = SKW_EXTER_HDR_SIZE;
		if (extra_v2)
			skw->hw.extra.len_offset = 0;
		else
			skw->hw.extra.len_offset = 7;

		skw->hw.extra.eof_offset = 23;
		skw->hw.extra.chn_offset = 24;

		skw->hw.rx_desc.hdr_offset = SKW_USB_RX_DESC_HDR_OFFSET;
		skw->hw.rx_desc.msdu_offset = SKW_USB_RX_DESC_MSDU_OFFSET;

		skw_lmac_port_init(skw, pdata->data_port, false);

		break;

	case SKW_BUS_PCIE:
		skw->hw.bus = SKW_BUS_PCIE;
		skw->hw.bus_dat_xmit = skw_pcie_xmit;
		skw->hw.bus_cmd_xmit = skw_pcie_cmd_xmit;
		skw->hw.dma = SKW_ASYNC_EDMA_TX;

		skw->hw.rx_desc.hdr_offset = SKW_PCIE_RX_DESC_HDR_OFFSET;
		skw->hw.rx_desc.msdu_offset = SKW_PCIE_RX_DESC_MSDU_OFFSET;

		skw->hw.cmd_port = -1;

		break;

	default:
		break;
	}
}

static int skw_core_init(struct skw_core *skw, struct platform_device *pdev, int idx)
{
	int i, ret;
	char name[32] = {0};

	skw->hw_pdata = dev_get_platdata(&pdev->dev);
	if (!skw->hw_pdata) {
		skw_err("get drv data failed\n");
		return -ENODEV;
	}

	skw->idx = idx;
	skw_hw_info_init(skw, skw->hw_pdata);

	snprintf(name, sizeof(name), "chip%d.%s", idx, skw_bus_name(skw->hw.bus));
	skw->dentry = skw_debugfs_subdir(name, NULL);
	skw->pentry = skw_procfs_subdir(name, NULL);

	mutex_init(&skw->lock);

#ifdef CONFIG_SKW_SAP_SME_EXT
	set_bit(SKW_FLAG_SAP_SME_EXTERNAL, &skw->flags);
#endif

#ifdef CONFIG_SKW_STA_SME_EXT
	set_bit(SKW_FLAG_STA_SME_EXTERNAL, &skw->flags);
#endif

#ifdef CONFIG_SKW_LEGACY_P2P
	set_bit(SKW_FLAG_LEGACY_P2P, &skw->flags);
#endif

	skw->regd = NULL;
	skw->country[0] = '0';
	skw->country[1] = '0';

	atomic_set(&skw->exit, 0);
	atomic_set(&skw->tx_wake, 0);
	atomic_set(&skw->rx_wake, 0);
	atomic_set(&skw->xmitting_data, 0);

	init_waitqueue_head(&skw->tx_wait_q);
	init_waitqueue_head(&skw->rx_wait_q);

	skb_queue_head_init(&skw->rx_dat_q);
	atomic_set(&skw->txqlen_pending, 0);

	mutex_init(&skw->cmd.lock);
	init_waitqueue_head(&skw->cmd.wq);
	skw->cmd.ws = skw_wakeup_source_init("skwifi_cmd");

	spin_lock_init(&skw->vif.lock);

	for (i = 0; i < SKW_MAX_PEER_SUPPORT; i++) {
		skw->peer_ctx[i].idx = i;
		mutex_init(&skw->peer_ctx[i].lock);
	}

	skw_event_work_init(&skw->event_work, skw_default_event_work);
	skw->event_wq = alloc_workqueue("skw_evt_wq.%d", WQ_UNBOUND | WQ_MEM_RECLAIM, 1, idx);
	if (!skw->event_wq) {
		ret = PTR_ERR(skw->event_wq);
		skw_err("alloc event wq failed, ret: %d\n", ret);

		goto deinit_ws;
	}

	ret = skw_dpd_init(&skw->dpd);
	if (ret < 0)
		goto deinit_wq;

	ret = skw_buffer_init(skw);
	if (ret < 0)
		goto deinit_dpd;

	ret = skw_rx_init(skw);
	if (ret < 0) {
		skw_err("rx init failed, ret: %d\n", ret);
		goto deinit_buff;
	}

	ret = skw_tx_init(skw);
	if (ret < 0) {
		skw_err("tx init failed, ret: %d\n", ret);
		goto deinit_rx;
	}

	return 0;

deinit_rx:
	skw_rx_deinit(skw);

deinit_buff:
	skw_buffer_deinit(skw);

deinit_dpd:
	skw_dpd_deinit(&skw->dpd);

deinit_wq:
	destroy_workqueue(skw->event_wq);
	skw_event_work_deinit(&skw->event_work);

deinit_ws:
	skw_wakeup_source_deinit(skw->cmd.ws);

	debugfs_remove_recursive(skw->dentry);
	proc_remove(skw->pentry);

	return ret;
}

static void skw_core_deinit(struct skw_core *skw)
{
	skw_tx_deinit(skw);

	skw_rx_deinit(skw);

	skw_buffer_deinit(skw);

	skw_dpd_deinit(&skw->dpd);

	flush_workqueue(skw->event_wq);
	destroy_workqueue(skw->event_wq);

	skw_event_work_deinit(&skw->event_work);

	skw_wakeup_source_deinit(skw->cmd.ws);

	debugfs_remove_recursive(skw->dentry);
	proc_remove(skw->pentry);
}

int skw_set_ip(struct wiphy *wiphy, struct net_device *ndev,
		struct skw_setip_param *setip_param, int size)
{
	int ret = 0;
	//int size = 0;

	//size = sizeof(struct skw_setip_param);
	ret = skw_queue_work(wiphy, netdev_priv(ndev),
			SKW_WORK_SET_IP, setip_param, size);
	if (ret)
		skw_err("Set IP failed\n");

	return ret;
}

/*
 * Get the IP address for both IPV4 and IPV6, set it
 * to firmware while they are valid.
 */
void skw_set_ip_to_fw(struct wiphy *wiphy, struct net_device *ndev)
{
	struct in_device *in_dev;
	struct in_ifaddr *ifa;

	struct inet6_dev *idev;
	struct inet6_ifaddr *ifp;

	struct skw_setip_param setip_param[SKW_FW_IPV6_COUNT_LIMIT+1];
	int ip_count = 0, ipv6_count = 0;

	in_dev = __in_dev_get_rtnl(ndev);
	if (!in_dev)
		goto ipv6;

	ifa = in_dev->ifa_list;
	if (!ifa || !ifa->ifa_local)
		goto ipv6;

	skw_info("ip addr: %pI4\n", &ifa->ifa_local);
	setip_param[ip_count].ip_type = SKW_IP_IPV4;
	setip_param[ip_count].ipv4 = ifa->ifa_local;
	ip_count++;

ipv6:

	idev = __in6_dev_get(ndev);
	if (!idev)
		goto ip_apply;

	list_for_each_entry_reverse(ifp, &idev->addr_list, if_list) {
		skw_info("ip addr: %pI6\n", &ifp->addr);
		if (++ipv6_count > SKW_FW_IPV6_COUNT_LIMIT) {
			skw_warn("%s set first %d of %d ipv6 address\n",
				ndev->name, SKW_FW_IPV6_COUNT_LIMIT, ipv6_count);

			goto ip_apply;
		}

		setip_param[ip_count].ip_type = SKW_IP_IPV6;
		memcpy(&setip_param[ip_count].ipv6, &ifp->addr, 16);
		ip_count++;
	}

ip_apply:
	if (ip_count)
		skw_set_ip(wiphy, ndev, setip_param, ip_count*sizeof(struct skw_setip_param));

}

#ifdef CONFIG_INET
static int skw_ifa4_notifier(struct notifier_block *nb,
			unsigned long action, void *data)
{
	struct in_ifaddr *ifa = data;
	struct net_device *ndev;
	struct wireless_dev *wdev;
	struct skw_core *skw = container_of(nb, struct skw_core, ifa4_nf);

	skw_dbg("action: %ld\n", action);

	if (!ifa->ifa_dev)
		return NOTIFY_DONE;

	ndev = ifa->ifa_dev->dev;
	wdev = ndev->ieee80211_ptr;

	if (!wdev || wdev->wiphy != priv_to_wiphy(skw))
		return NOTIFY_DONE;

	if (ndev->ieee80211_ptr->iftype != NL80211_IFTYPE_STATION &&
	    ndev->ieee80211_ptr->iftype != NL80211_IFTYPE_P2P_CLIENT)
		return NOTIFY_DONE;

	switch (action) {
	case NETDEV_UP:
//	case NETDEV_DOWN:

		skw_set_ip_to_fw(wdev->wiphy, ndev);

		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}

#endif

#ifdef CONFIG_IPV6
static int skw_ifa6_notifier(struct notifier_block *nb,
			unsigned long action, void *data)
{
	struct inet6_ifaddr *ifa6 = data;
	struct skw_core *skw = container_of(nb, struct skw_core, ifa6_nf);

	struct net_device *ndev;
	struct wireless_dev *wdev;

	skw_dbg("action: %ld\n", action);

	if (!ifa6->idev || !ifa6->idev->dev)
		return NOTIFY_DONE;

	ndev = ifa6->idev->dev;
	wdev = ndev->ieee80211_ptr;

	if (!wdev || wdev->wiphy != priv_to_wiphy(skw))
		return NOTIFY_DONE;

	switch (action) {
	case NETDEV_UP:
	// case NETDEV_DOWN:

		skw_set_ip_to_fw(wdev->wiphy, ndev);

		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}

#endif

static int skw_bsp_notifier(struct notifier_block *nb,
			unsigned long action, void *data)
{
	struct skw_core *skw = container_of(nb, struct skw_core, bsp_nf);

	skw_dbg("action: %ld, skw flags: 0x%lx\n", action, skw->flags);

	switch (action) {
	case SKW_BSP_NF_ASSERT:
	case SKW_BSP_NF_BLOCKED:
		set_bit(SKW_FLAG_FW_ASSERT, &skw->flags);
		skw_abort_cmd(skw);
		cancel_work_sync(&skw->recovery_work);
		skw_wifi_disable();
		break;

	case SKW_BSP_NF_READY:
		schedule_work(&skw->recovery_work);
		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}

static void skw_ifa_notifier_register(struct skw_core *skw)
{
#ifdef CONFIG_INET
	skw->ifa4_nf.notifier_call = skw_ifa4_notifier;
	register_inetaddr_notifier(&skw->ifa4_nf);
#endif

#ifdef CONFIG_IPV6
	skw->ifa6_nf.notifier_call = skw_ifa6_notifier;
	register_inet6addr_notifier(&skw->ifa6_nf);
#endif

	skw->bsp_nf.notifier_call = skw_bsp_notifier;
	skw_register_bsp_notifier(skw, &skw->bsp_nf);
}

static void skw_ifa_notifier_unregister(struct skw_core *skw)
{
#ifdef CONFIG_INET
	unregister_inetaddr_notifier(&skw->ifa4_nf);
#endif

#ifdef CONFIG_IPV6
	unregister_inet6addr_notifier(&skw->ifa6_nf);
#endif

	skw_unregister_bsp_notifier(skw, &skw->bsp_nf);
}

int skw_lmac_bind_iface(struct skw_core *skw, struct skw_iface *iface, int lmac_id)
{
	struct skw_lmac *lmac;

	BUG_ON(lmac_id >= SKW_NR_LMAC);

	lmac = &skw->hw.lmac[lmac_id];

	iface->lmac_id = lmac_id;

	BUG_ON(!(lmac->flags & SKW_LMAC_FLAG_INIT));

	if (skw->hw.bus == SKW_BUS_PCIE) {
		// TODO:
		// register edma channel
		// skw_edma_enable_channel(&lmac->edma_tx_chn, isr);
	}

	SKW_SET(lmac->iface_bitmap, BIT(iface->id));
	SKW_SET(lmac->flags, SKW_LMAC_FLAG_ACTIVED);

	return 0;
}

int skw_lmac_unbind_iface(struct skw_core *skw, int lmac_id, int iface_id)
{
	struct skw_lmac *lmac;

	BUG_ON(lmac_id >= SKW_NR_LMAC);
	lmac = &skw->hw.lmac[lmac_id];

	SKW_CLEAR(lmac->iface_bitmap, BIT(iface_id));

	if (lmac->iface_bitmap)
		return 0;

	// TODO:
	// unregister edma channel

	SKW_CLEAR(lmac->flags, SKW_LMAC_FLAG_ACTIVED);

	return 0;
}

void skw_add_credit(struct skw_core *skw, int lmac_id, int cred)
{
	trace_skw_tx_add_credit(lmac_id, cred);

	atomic_add(cred, &skw->hw.lmac[lmac_id].fw_credit);
	smp_wmb();

	skw_wakeup_tx(skw);
	skw_detail("lmac_id:%d cred:%d", lmac_id, cred);
}

static int skw_add_default_iface(struct wiphy *wiphy)
{
	int ret = 0;
	struct skw_iface *p2p = NULL;
	struct skw_iface *iface = NULL;
	struct skw_core *skw = wiphy_priv(wiphy);

	rtnl_lock();

	iface = skw_add_iface(wiphy, "wlan%d", NL80211_IFTYPE_STATION,
				skw_mac, SKW_INVALID_ID, true);
	if (IS_ERR(iface)) {
		ret = PTR_ERR(iface);
		skw_err("add iface failed, ret: %d\n", ret);

		goto unlock;
	}

	if (test_bit(SKW_FLAG_LEGACY_P2P, &skw->flags)) {
		p2p = skw_add_iface(wiphy, "p2p%d", NL80211_IFTYPE_P2P_DEVICE,
					NULL, SKW_LAST_IFACE_ID, true);
		if (IS_ERR(p2p)) {
			ret = PTR_ERR(p2p);
			skw_err("add p2p interface failed, ret: %d\n", ret);

			skw_del_iface(wiphy, iface);
		}

		p2p->flags |= SKW_IFACE_FLAG_LEGACY_P2P;
	}

unlock:
	rtnl_unlock();

	return ret;
}

static int skw_calib_bin_download(struct wiphy *wiphy, const u8 *data, u32 size)
{
	int i = 0, ret = 0;
	int buf_size, remain = size;
	struct skw_calib_param calib;

	skw_dbg("file size: %d\n", size);

	buf_size = sizeof(calib.data);

	while (remain > 0) {
		calib.len = (remain < buf_size) ? remain : buf_size;
		calib.seq = i;
		remain -= calib.len;

		memcpy(calib.data, data, calib.len);

		if (!remain)
			calib.end = 1;
		else
			calib.end = 0;

		skw_dbg("bb_file remain: %d, seq: %d len: %d end: %d\n",
			remain, calib.seq, calib.len, calib.end);

		ret = skw_msg_xmit(wiphy, 0, SKW_CMD_PHY_BB_CFG, &calib,
				sizeof(calib), NULL, 0);
		if (ret) {
			skw_err("failed, ret: %d,  seq: %d\n", ret, i);
			break;
		}

		i++;
		data += calib.len;
	}

	return ret;
}

int skw_calib_download(struct wiphy *wiphy, const char *fname)
{
	int ret = 0;
	const struct firmware *fw;

	skw_dbg("fw file: %s\n", fname);

	ret = request_firmware(&fw, fname, &wiphy->dev);
	if (ret) {
		skw_err("load %s failed, ret: %d\n", fname, ret);
		return ret;
	}

	ret = skw_calib_bin_download(wiphy, fw->data, fw->size);
	if (ret != 0)
		skw_err("bb_file cali msg fail\n");

	release_firmware(fw);

	return ret;
}

static int skw_drv_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct skw_chip_info chip;
	struct wiphy *wiphy = NULL;
	struct skw_core *skw = NULL;
	int idx = atomic_inc_return(&skw_chip_idx);

	skw_info("chip: %d, MAC: %pM\n", idx, skw_mac);

	wiphy = skw_alloc_wiphy(sizeof(struct skw_core));
	if (!wiphy) {
		ret = -ENOMEM;
		skw_err("malloc wiphy failed\n");

		goto failed;
	}

	set_wiphy_dev(wiphy, pdev->dev.parent);

	skw = wiphy_priv(wiphy);

	ret = skw_core_init(skw, pdev, idx);
	if (ret) {
		skw_err("core init failed, ret: %d\n", ret);
		goto free_wiphy;
	}

	skw_regd_init(wiphy);
	skw_vendor_init(wiphy);
	skw_work_init(wiphy);
	skw_timer_init(skw);
	skw_recovery_init(skw);

	skw_wifi_enable();

	if (skw_sync_cmd_event_version(wiphy) ||
	    skw_sync_chip_info(wiphy, &chip))
		goto core_deinit;

	if (!is_valid_ether_addr(skw_mac))
		rockchip_wifi_mac_addr(skw_mac);

	skw_setup_mac_address(wiphy, skw_mac, chip.mac);

	skw_calib_download(wiphy, skw->fw.calib_file);

	skw_dpd_download(wiphy, &skw->dpd);

	ret = skw_setup_wiphy(wiphy, &chip);
	if (ret) {
		skw_err("setup wiphy failed, ret: %d\n", ret);
		goto core_deinit;
	}

	rtnl_lock();

	skw_set_wiphy_regd(wiphy, skw->country);

#ifdef CONFIG_SKW_DEFAULT_COUNTRY
	if (strlen(CONFIG_SKW_DEFAULT_COUNTRY) == 2)
		skw_set_regdom(wiphy, CONFIG_SKW_DEFAULT_COUNTRY);
#endif

	rtnl_unlock();

	ret = skw_add_default_iface(wiphy);
	if (ret)
		goto unregister_wiphy;

	skw_ifa_notifier_register(skw);

	platform_set_drvdata(pdev, skw);

	skw_procfs_file(skw->pentry, "core", 0444, &skw_core_fops, skw);
	skw_debugfs_file(SKW_WIPHY_DENTRY(wiphy), "assert", 0444,
			 &skw_assert_fops, skw);

	return 0;

unregister_wiphy:
	wiphy_unregister(wiphy);

core_deinit:
	skw_wifi_disable();
	skw_recovery_deinit(skw);
	skw_timer_deinit(skw);
	skw_work_deinit(wiphy);
	skw_vendor_deinit(wiphy);
	skw_core_deinit(skw);

free_wiphy:
	wiphy_free(wiphy);

failed:
	atomic_dec(&skw_chip_idx);

	return ret;
}

static int skw_drv_remove(struct platform_device *pdev)
{
	int i;
	struct wiphy *wiphy;
	struct skw_core *skw = platform_get_drvdata(pdev);

	skw_info("%s\n", pdev->name);

	if (!skw)
		return 0;

	wiphy = priv_to_wiphy(skw);

	skw_ifa_notifier_unregister(skw);

	rtnl_lock();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
	cfg80211_shutdown_all_interfaces(wiphy);
#endif

	for (i = 0; i < SKW_NR_IFACE; i++)
		skw_del_iface(wiphy, skw->vif.iface[i]);

	rtnl_unlock();

	wiphy_unregister(wiphy);

	skw_wifi_disable();

	skw_recovery_deinit(skw);

	skw_timer_deinit(skw);

	skw_work_deinit(wiphy);

	skw_vendor_deinit(wiphy);

	skw_core_deinit(skw);

	wiphy_free(wiphy);

	atomic_dec(&skw_chip_idx);

	return 0;
}

static struct platform_driver skw_drv = {
	.probe = skw_drv_probe,
	.remove = skw_drv_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "sv6160_wireless1",
	},
};

static int __init skw_module_init(void)
{
	int ret = 0;

	pr_info("[SKWIFI INFO] VERSION: %s (%s)\n",
		SKW_VERSION, UTS_RELEASE);

	skw_dentry_init();
	skw_log_level_init();

	skw_power_on_chip();

	ret = platform_driver_register(&skw_drv);
	if (ret)
		skw_err("register %s failed, ret: %d\n",
			skw_drv.driver.name, ret);

	return ret;
}

static void __exit skw_module_exit(void)
{
	skw_info("unload\n");

	platform_driver_unregister(&skw_drv);

	skw_power_off_chip();

	skw_dentry_deinit();
}

module_init(skw_module_init);
module_exit(skw_module_exit);

module_param_array_named(mac, skw_mac, byte, NULL, 0444);
MODULE_PARM_DESC(mac, "config mac address");

MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0.0");
MODULE_AUTHOR("seekwavetech");
