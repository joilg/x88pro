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

#ifndef __SKW_EDMA_H__
#define __SKW_EDMA_H__

#define SKW_NR_EDMA_NODE                          32
#define SKW_NR_EDMA_ELEMENT                       64
#define SKW_EDMA_DATA_LEN                         1024

/* EDMA CHANNEL */
#define SKW_EDMA_WIFI_CMD_CHN                     14
#define SKW_EDMA_WIFI_SHORT_EVENT_CHN             15
#define SKW_EDMA_WIFI_LONG_EVENT_CHN              16
#define SKW_EDMA_WIFI_RX0_FITER_CHN               17
#define SKW_EDMA_WIFI_RX1_FITER_CHN               18
#define SKW_EDMA_WIFI_TX0_CHN                     19
#define SKW_EDMA_WIFI_TX1_CHN                     20
#define SKW_EDMA_WIFI_TX0_FREE_CHN                21
#define SKW_EDMA_WIFI_TX1_FREE_CHN                22
#define SKW_EDMA_WIFI_RX0_FREE_CHN                23
#define SKW_EDMA_WIFI_RX1_FREE_CHN                24
#define SKW_EDMA_WIFI_RX0_CHN                     25
#define SKW_EDMA_WIFI_RX1_CHN                     26

#define SKW_EDMA_EVENT_CHN_NODE_NUM               2
#define SKW_EDMA_FILTER_CHN_NODE_NUM              8
#define SKW_EDMA_TX_CHN_NODE_NUM                  64
#define SKW_EDMA_TX_FREE_CHN_NODE_NUM             8
#define SKW_EDMA_RX_CHN_NODE_NUM                  64
#define SKW_EDMA_RX_FREE_CHN_NODE_NUM             9

typedef int (*skw_edma_isr)(void *priv, void *first_pa, void *last_pa, int cnt);
typedef int (*skw_edma_empty_isr)(void *priv);

struct skw_edma_elem {
	u64 pa:40;
	u64 rsv:8;

	u64 eth_type:16;

	u8 id_rsv:2;
	u8 mac_id:2;
	u8 tid:4;

	u8 peer_idx:5;
	u8 prot:1;
	u8 encry_dis:1;
	u8 rate:1;

	u16 msdu_len:12;
	u16 resv:4;
} __packed;

struct skw_edma_hdr {
	u64 buffer_pa:40;
	u64 rsv:24;

	u64 hdr_next:40;
	u64 resv:8;
	u64 data_len:16;
} __packed;

struct skw_edma_node {
	struct list_head list;
	void *buffer;
	dma_addr_t buffer_pa;
	int buffer_len;
	u16 used;
	u16 node_id;
};

struct skw_edma_context {
	void *skw;
	struct skw_channel_cfg *edma_ch_cfg;
	u16 channel;
} __packed;

struct skw_edma_chn {
	struct skw_edma_hdr *hdr;
	dma_addr_t edma_hdr_pa;
	u32 edma_hdr_size;
	struct skw_edma_node *current_node;
	struct list_head node_list;
	u16 max_node_num;
	atomic_t nr_node;
	u16 channel;
	u16 tx_node_count;
	skw_edma_isr isr;
	skw_edma_empty_isr empty_isr;
	struct skw_edma_context context;
	spinlock_t edma_chan_lock;
};

#ifdef CONFIG_SKW_EDMA
int skw_edma_init(struct wiphy *wiphy);
void skw_edma_deinit(struct wiphy *wiphy);
int skw_edma_set_data(struct wiphy *wiphy, struct skw_edma_chn *edma,
			void *data, int len);
int skw_edma_tx(struct wiphy *wiphy, struct skw_edma_chn *edma, int tx_len);
#else
static inline int skw_edma_init(struct wiphy *wiphy)
{
	return 0;
}

static inline void skw_edma_deinit(struct wiphy *wiphy) {}
static inline int skw_edma_set_data(struct wiphy *wiphy,
		struct skw_edma_chn *edma, void *data, int len)
{
	return 0;
}

static inline int skw_edma_tx(struct wiphy *wiphy,
		struct skw_edma_chn *edma, int tx_len)
{
	return 0;
}
#endif

#endif
