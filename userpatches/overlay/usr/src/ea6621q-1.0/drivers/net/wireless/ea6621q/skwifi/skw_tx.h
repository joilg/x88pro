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

#ifndef __SKW_TX_H__
#define __SKW_TX_H__

#include "skw_platform_data.h"

/* used for tx descriptor */
#define SKW_ETHER_FRAME            0
#define SKW_80211_FRAME            1

#define SKW_SNAP_HDR_LEN           8

#define SKW_TXQ_HIGH_THRESHOLD     1000
#define SKW_TXQ_LOW_THRESHOLD      256

#define SKW_TX_WAIT_TIME 1000000

struct skw_tx_desc_hdr {
	/* pading bytes for gap */
	u16 padding_gap:2;
	u16 inst:2;
	u16 tid:4;
	u16 peer_lut:5;

	/* frame_type:
	 * 0: ethernet frame
	 * 1: 80211 frame
	 */
	u16 frame_type:1;
	u16 encry_dis:1;

	/* rate: 0: auto, 1: sw config */
	u16 rate:1;

	u16 msdu_len:12;
	u16 rsv:4;

	/* pading for address align */
	u8 gap[0];
} __packed;

struct skw_tx_desc_conf {
	u16 l4_hdr_offset:10;
	u16 csum:1;

	/* ip_prot: 0: UDP, 1: TCP */
	u16 ip_prot:1;
	u16 rsv:4;
} __packed;

struct skw_tx_cb {
	u8 peer_idx;
	u8 tx_retry;
	u16 skb_native_len;
	dma_addr_t skb_data_pa;
	struct skw_edma_elem e;
};

static inline void
skw_set_tx_desc_eth_type(struct skw_tx_desc_hdr *desc_hdr, u16 proto)
{
}

static inline bool skw_tx_allowed(struct skw_core *skw, unsigned long mask)
{
	unsigned long flags = BIT(SKW_FLAG_FW_ASSERT) |
			      BIT(SKW_FLAG_BLOCK_TX) |
			      BIT(SKW_FLAG_MP_MODE) |
			      BIT(SKW_FLAG_FW_THERMAL) |
			      BIT(SKW_FLAG_FW_MAC_RECOVERY);

	if (unlikely((READ_ONCE(skw->flags) & ~mask) & flags))
		return false;

	return true;
}


int skw_pcie_cmd_xmit(struct skw_core *skw, void *data, int data_len);
int skw_pcie_xmit(struct skw_core *skw, int lmac_id, struct sk_buff_head *txq);

int skw_sdio_cmd_xmit(struct skw_core *skw, void *data, int data_len);
int skw_sdio_xmit(struct skw_core *skw, int lmac_id, struct sk_buff_head *txq);

int skw_usb_cmd_xmit(struct skw_core *skw, void *data, int data_len);
int skw_usb_xmit(struct skw_core *skw, int lmac_id, struct sk_buff_head *txq);

int skw_hw_xmit_init(struct skw_core *skw, int dma);
int skw_tx_init(struct skw_core *skw);
int skw_tx_deinit(struct skw_core *skw);

#endif
