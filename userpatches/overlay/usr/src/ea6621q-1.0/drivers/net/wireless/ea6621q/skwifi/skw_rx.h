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

#ifndef __SKW_RX_H__
#define __SKW_RX_H__

#include "skw_platform_data.h"
#include "skw_iface.h"
#include "skw_core.h"
#include "skw_cfg80211.h"

#define SKW_MAX_AMPDU_BUF_SIZE            0x100 /* 256 */

#define SKW_AMSDU_FLAG_TAINT              BIT(0)
#define SKW_AMSDU_FLAG_VALID              BIT(1)

#define SKW_SDIO_RX_DESC_HDR_OFFSET       0
#define SKW_SDIO_RX_DESC_MSDU_OFFSET      0
#define SKW_USB_RX_DESC_HDR_OFFSET        0
#define SKW_USB_RX_DESC_MSDU_OFFSET       0
#define SKW_PCIE_RX_DESC_HDR_OFFSET       0
#define SKW_PCIE_RX_DESC_MSDU_OFFSET      0

#ifndef ETH_P_80211_RAW
#define ETH_P_80211_RAW (ETH_P_ECONET + 1)
#endif

enum SKW_RELEASE_REASON {
	SKW_RELEASE_INVALID,
	SKW_RELEASE_EXPIRED,
	SKW_RELEASE_OOB,
	SKW_RELEASE_BAR,
	SKW_RELEASE_FREE,
};

struct skw_reorder_cb {
	unsigned long rx_time;
	u16 rx_desc_offset;
	u8 amsdu_bitmap;
	u8 amsdu_mask;
	u16 amsdu_flags;
	u8 skw_created;
};

struct skw_drop_sn_info {
	u16 sn;
	u8 amsdu_idx;
	u8 amsdu_first: 1;
	u8 amsdu_last: 1;
	u8 is_amsdu: 1;
	u8 qos: 1;
	u8 tid: 4;
	u32 peer_idx: 5;
	u32 inst: 2;
	u32 resved: 25;
} __packed;

struct skw_rx_desc {
	u32 priv_used[2];

	/* word 2 */
	u16 msdu_len;
	u16 pkt_len;

	/* word 3 & 4*/
	u8 base_addr[5];
	u8 msdu_offset;
	u8 amsdu_idx;
	u8 msdu_filter;

	/* work 5 */
	u16 csum;
	u8 csum_valid:1;
	u8 rsv:7;

	u8 snap_type:1;
	u8 snap_match:1;
	u8 vlan:1;
	u8 eapol:1;
	u8 ps_mode:1;
	u8 amsdu_first_idx:1;
	u8 first_msdu_in_buff:1;
	u8 amsdu_last_idx:1;

	/* word 6 */
	u16 sn:12; /* seq number */
	u16 frag_num:4;

	u16 more_frag:1;
	u16 need_forward:1;
	u16 is_mc_addr:1;
	u16 is_amsdu:1;
	u16 is_ampdu:1;
	u16 is_qos_data:1;
	u16 retry_frame:1;
	u16 tid:4;
	u16 pm:1;
	u16 eosp:1;
	u16 more_data:1;
	u16 resv:2;

	/* word 7 & 8 */
	u8 pn[6];
	u16 credit;

	/* word 9 */
	u32 rvd8:4;
	u32 ba_session:4;
	u32 inst_id:2;
	u32 inst_resv:2;
	u32 peer_idx:5;
	u32 peer_idx_valid:1;
	u32 resved:14;

	/* word 10 */
	u32 rssi:11;
	u32 rvd9:5;
	u32 snr:6;
	u32 rvd10:10;


	/* word 11 */
	u8 sbw:2;
	u8 dcm:1;
	u8 rvd11:1;
	u8 gi_type:2;
	u8 rvd12:1;
	u8 mac_drop_frag:1;
	u8 ppdu_mode:4;
	u8 rsvd:4;

	u16 data_rate:6;
	u16 pad:2;
	u16 agc_gain:8;

	/* word 12 - 13 */
	u32 unused[2];
} __packed;

static inline void skw_snap_unmatch_handler(struct sk_buff *skb)
{

	/* Add 2 bytes for length field */
	skb_push(skb, 2);
	memmove(skb->data, skb->data + 2, 12);

	skb_reset_mac_header(skb);
	eth_hdr(skb)->h_proto = htons(skb->len & 0xffff);
}

static inline void skw_event_add_credit(struct skw_core *skw, void *data)
{
	skw_add_credit(skw, 0, *((u16 *)data));
}

static inline void skw_data_add_credit(struct skw_core *skw, void *data)
{
	if (((struct skw_rx_desc *)data)->credit)
		skw_add_credit(skw, 0, ((struct skw_rx_desc *)data)->credit);
}

static inline bool skw_is_monitor_data(struct skw_core *skw, void *data)
{
	struct skw_iface *iface;
	struct skw_rx_desc *desc;

	desc = (struct skw_rx_desc *)data;
	iface = to_skw_iface(skw, desc->inst_id);
	if (iface && iface->ndev->ieee80211_ptr->iftype == NL80211_IFTYPE_MONITOR)
		return true;

	return false;
}

static inline void skw_update_peer_rx_rate(struct skw_peer *peer, struct skw_rx_desc *desc)
{
	peer->rx.rssi = desc->rssi;

	skw_desc_get_rx_rate(&peer->rx.rate, desc->sbw, desc->ppdu_mode,
			skw_desc_gi_to_skw_gi(desc->gi_type, desc->ppdu_mode), 1, desc->dcm, desc->data_rate);
}

int skw_rx_thread(void *data);

int skw_add_tid_rx(struct skw_peer *peer, u16 tid, u16 ssn, u16 buf_size);
int skw_update_tid_rx(struct skw_peer *peer, u16 tid, u16 ssn, u16 win_size);
int skw_del_tid_rx(struct skw_peer *peer, u16 tid);

int skw_rx_init(struct skw_core *skw);
int skw_rx_deinit(struct skw_core *skw);
int skw_rx_cb(int port, struct scatterlist *sglist, int nents, void *priv);
int skw_register_rx_callback(struct skw_core *skw, void *func, void *data);

#endif
