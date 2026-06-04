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
#include <linux/percpu-defs.h>
#include <linux/skbuff.h>

#include "skw_core.h"
#include "skw_compat.h"
#include "skw_edma.h"
#include "skw_util.h"
#include "skw_log.h"
#include "skw_msg.h"
#include "skw_rx.h"
#include "skw_tx.h"
#include "trace.h"

static struct kmem_cache *skw_edma_node_cache;

static inline void skw_dma_free_coherent(struct skw_core *skw,
		dma_addr_t *dma_handle, void *cpu_addr, size_t size)
{
	struct device *dev = priv_to_wiphy(skw)->dev.parent;

	dma_free_coherent(dev, size, cpu_addr, *dma_handle);
}

static inline void *skw_dma_alloc_coherent(struct skw_core *skw,
		dma_addr_t *dma_handle, size_t size, gfp_t flag)
{
	struct device *dev = priv_to_wiphy(skw)->dev.parent;

	return dma_alloc_coherent(dev, size, dma_handle, flag);
}

struct skw_edma_node *skw_edma_next_node(struct skw_edma_chn *chn)
{
	unsigned long flags;

	chn->current_node->buffer_pa = skw_pci_map_single(chn->context.skw,
				chn->current_node->buffer,
				chn->current_node->buffer_len, DMA_TO_DEVICE);
	spin_lock_irqsave(&chn->edma_chan_lock, flags);
	//skw_dbg("channel:%d prev_cur id:%d used:%d current_node:%p\n",
	//		chn->channel, chn->current_node->node_id,
	//		chn->current_node->used, chn->current_node);
	if (list_is_last(&chn->current_node->list, &chn->node_list)) {
		chn->current_node = list_first_entry(&chn->node_list,
				struct skw_edma_node, list);
	} else {
		chn->current_node = list_next_entry(chn->current_node, list);
	}

	chn->current_node->used = 0;
	chn->tx_node_count++;
	atomic_dec(&chn->nr_node);
	//skw_dbg("channel:%d cur id:%d used:%d, current node:%p\n",
	//		chn->channel, chn->current_node->node_id,
	//		chn->current_node->used, chn->current_node);
	spin_unlock_irqrestore(&chn->edma_chan_lock, flags);
	return chn->current_node;
}

int
skw_edma_set_data(struct wiphy *wiphy, struct skw_edma_chn *edma,
		void *data, int len)
{
	struct skw_edma_node *node = edma->current_node;
	unsigned long flags;
	u8 *buff = NULL;

	spin_lock_irqsave(&edma->edma_chan_lock, flags);
	buff = (u8 *)node->buffer;
	skw_dbg("chan: %d node_id: %d node->used: %d buff: %pad used: %pad\n",
		edma->channel, node->node_id, node->used, (dma_addr_t *)buff,
		(dma_addr_t *)(buff + node->used));
	//skw_dbg("data:%p, data:0x%llx\n", data, (u64) data);
	memcpy(buff + node->used, data, len);
	//skw_dbg("%d channel:%d node:%p\n", __LINE__, edma->channel, node);
	node->used += len;
	edma->hdr[node->node_id].data_len = node->used;
	spin_unlock_irqrestore(&edma->edma_chan_lock, flags);
	BUG_ON(len > node->buffer_len);
	if (node->used + len > node->buffer_len)
		node = skw_edma_next_node(edma);

	return 0;
}

int skw_edma_tx(struct wiphy *wiphy, struct skw_edma_chn *edma, int tx_len)
{
	int tx_count;
	struct skw_core *skw = wiphy_priv(wiphy);
	u64 pa = 0;

	skw_edma_next_node(edma);
	tx_count = edma->tx_node_count;
	pa = edma->hdr->hdr_next;
	//skw_dbg("channel:%d tx_node_count:%d pa:0x%llx\n",
	//	edma->channel, tx_count, pa);
	edma->tx_node_count = 0;

	return  skw->hw_pdata->hw_adma_tx(edma->channel, NULL,
					tx_count, tx_len);
}

static void skw_edma_chn_deinit(struct skw_core *skw, struct skw_edma_chn *edma)
{
	struct skw_edma_node *node = NULL, *tmp = NULL;
	// TODO: stop edma channel transmit

	if (!edma) {
		skw_err("emda is null\n");
		return;
	}

	skw_dbg("chan:%d\n", edma->channel);

	list_for_each_entry_safe(node, tmp, &edma->node_list, list) {
		list_del(&node->list);
		skw_pci_unmap_single(skw, node->buffer_pa, node->buffer_len,
					DMA_TO_DEVICE);
		SKW_KFREE(node->buffer);
		kmem_cache_free(skw_edma_node_cache, node);
	}
	edma->current_node = NULL;
	atomic_set(&edma->nr_node, 0);
	skw_dma_free_coherent(skw, &edma->edma_hdr_pa, edma->hdr,
				edma->edma_hdr_size);
}

static int skw_edma_chn_init(struct skw_core *skw, struct skw_edma_chn *edma,
			int channel, int max_node, int node_buff_len,
			skw_edma_isr isr, skw_edma_empty_isr empty_isr)
{
	u64 tmp_pa;
	int i, size;
	int next_offset;
	struct skw_edma_node *node;
	u64 hdr_next;

	size = max_node * sizeof(struct skw_edma_hdr);
	edma->hdr = skw_dma_alloc_coherent(skw, &edma->edma_hdr_pa,
					size, GFP_DMA);
	if (!edma->hdr)
		return -ENOMEM;

	memset(edma->hdr, 0x6a, size);

	edma->max_node_num = max_node;
	edma->channel = channel;
	edma->tx_node_count = 0;
	spin_lock_init(&edma->edma_chan_lock);
	INIT_LIST_HEAD(&edma->node_list);

	skw_dbg("%d channel: %d edma->edma_hdr_pa: %pad\n",
		__LINE__, channel, (dma_addr_t *)edma->edma_hdr_pa);

	for (i = 0; i < max_node; i++) {
		next_offset = 8 +
			sizeof(struct skw_edma_hdr) * ((i + 1) % max_node);
		edma->hdr[i].hdr_next =
			skw->hw_pdata->phyaddr_to_pcieaddr(edma->edma_hdr_pa) +
			next_offset;
		hdr_next = edma->hdr[i].hdr_next;
		skw_dbg("hdr_next pa:0x%llx\n", hdr_next);

		node = kmem_cache_alloc(skw_edma_node_cache, GFP_KERNEL);
		node->buffer = kzalloc(node_buff_len, GFP_DMA);
		memset(node->buffer, 0x5a, node_buff_len);
		if (!node->buffer)
			goto failed;

		node->used = 0;
		node->node_id = i;
		node->buffer_len = node_buff_len;

		edma->hdr[i].buffer_pa =
			skw->hw_pdata->virtaddr_to_pcieaddr(node->buffer);
		tmp_pa = edma->hdr[i].buffer_pa;
		skw_dbg("channel:%d i:%d buffer pcie addr:0x%llx\n",
			channel, i, tmp_pa);

		INIT_LIST_HEAD(&node->list);
		list_add_tail(&node->list, &edma->node_list);
	}

	edma->edma_hdr_size = size;

	atomic_set(&edma->nr_node, max_node);
	edma->current_node = list_first_entry(&edma->node_list,
				struct skw_edma_node, list);

	edma->isr = isr;
	edma->empty_isr = empty_isr;

	return 0;

failed:
	skw_edma_chn_deinit(skw, edma);

	return -ENOMEM;
}

static int
skw_edma_tx_node_isr(void *priv, void *first_pa, void *last_pa, int count)
{
	struct skw_edma_context *context = (struct skw_edma_context *) priv;
	u16 channel = context->channel;
	struct skw_core *skw = context->skw;
	struct skw_edma_chn *edma_chn = NULL;
	struct skw_edma_hdr *edma_hdr = NULL;
	int i = 0;
	u64 pa = 0, hdr_next = 0;
	int offset = 0;
	unsigned long flags;

	//skw_dbg("channel:%d first_pa:%p , count:%d\n",
	//		channel, first_pa, count);

	if (channel == SKW_EDMA_WIFI_TX0_CHN)
		edma_chn = &skw->hw.lmac[0].edma_tx_chn;
	else if (channel == SKW_EDMA_WIFI_TX1_CHN)
		edma_chn  = &skw->hw.lmac[1].edma_tx_chn;
	else if (channel == SKW_EDMA_WIFI_CMD_CHN)
		edma_chn  = &skw->edma_cmd;
	else
		return 0;

	spin_lock_irqsave(&edma_chn->edma_chan_lock, flags);
	hdr_next = edma_chn->hdr->hdr_next;
	//skw_dbg("hdr_pa:0x%llx first_pa:0x%llx  chan:%d, hdr_next:0x%llx\n",
	//		edma_chn->edma_hdr_pa, ((u64 ) (first_pa)),
	//		edma_chn->channel, hdr_next);

	//offset = (u64)first_pa - (edma_chn->hdr->hdr_next - 16);
	offset = skw->hw_pdata->pcieaddr_to_phyaddr((dma_addr_t)first_pa)
					- 8 - edma_chn->edma_hdr_pa;
	//skw_dbg("offset:%d channel:%d\n", offset, edma_chn->channel);
	//edma_hdr = (struct skw_edma_hdr *) (phys_to_virt(first_pa) - 8);
	edma_hdr = (struct skw_edma_hdr *) ((u8 *)edma_chn->hdr + offset);
	//skw_dbg("edma_hdr:%p\n", edma_hdr);
	while (i < count) {
		pa = edma_hdr->buffer_pa; //pcie address
		//skw_dbg("i:%d edma pcie addr:0x%llx, phy addrs:0x%llx\n",
		//		i, pa, skw->hw_pdata->pcieaddr_to_phyaddr(pa));
		skw_pci_unmap_single(skw,
			skw->hw_pdata->pcieaddr_to_phyaddr(edma_hdr->buffer_pa),
			edma_chn->current_node->buffer_len, DMA_TO_DEVICE);
		atomic_inc(&edma_chn->nr_node);
		edma_hdr++;
		i++;
	}
	spin_unlock_irqrestore(&edma_chn->edma_chan_lock, flags);

	//skw_dbg("cur node buffer_pa:0x%llx cur node buffer_len:%d\n",
	//	edma_chn->current_node->buffer_pa,
	//	edma_chn->current_node->buffer_len);
	//skw_pci_unmap_single(skw, edma_chn->current_node->buffer_pa,
	//		edma_chn->current_node->buffer_len, DMA_TO_DEVICE);

	return 0;
}

static void
skw_pci_edma_tx_free(struct skw_core *skw, struct sk_buff_head *free_list,
					void *data, u16 data_len)
{
	int count;
	unsigned long flags;
	struct sk_buff *skb, *tmp;
	struct sk_buff_head qlist;
	u64 *p = (u64 *) data;
	u64 p_data = 0;
	int i = 0, j = 0, m = 0;
	//u64 tmp_out = 0;

	__skb_queue_head_init(&qlist);

	spin_lock_irqsave(&free_list->lock, flags);
	skb_queue_splice_tail_init(free_list, &qlist);
	spin_unlock_irqrestore(&free_list->lock, flags);

	// trace_skw_tx_pcie_edma_free(data_len/8);
	for (count = 0; count < data_len; count = count + 8, p++) {
		p_data = *p & 0xFFFFFFFFFF;
		j++;
		skb_queue_walk_safe(&qlist, skb, tmp) {
			//tmp_out = SKW_SKB_TXCB(skb)->e.pa;
			//skw_dbg("SKW_SKB_TXCB(skb)->e.pa:0x%llx\n", tmp_out);
			//tmp_out = p_data & 0xFFFFFFFFFF;
			//skw_dbg("p_data:0x%llx\n",  tmp_out);
			if (skb && (SKW_SKB_TXCB(skb)->e.pa == (p_data & 0xFFFFFFFFFF))) {
				__skb_unlink(skb, &qlist);
				skw_pci_unmap_single(skw,
					SKW_SKB_TXCB(skb)->skb_data_pa,
					skb->len, DMA_TO_DEVICE);
				//skw_dbg("free skb %p\n", skb->data);
				//kfree_skb(skb);
				dev_kfree_skb_any(skb);
				i++;
				continue;
			}
			m++;
		}
	}

	if (i != j) {
		skw_dbg("i:%d, j:%d\n", j, j);
		//WARN_ON(1);
	}

	//skw_dbg("i:%d, j:%d\n", i, j);
	if (qlist.qlen) {
		spin_lock_irqsave(&free_list->lock, flags);
		skb_queue_splice_tail_init(&qlist, free_list);
		spin_unlock_irqrestore(&free_list->lock, flags);
	}

	//skw_compat_page_frag_free(data);
}

static void skw_pci_edma_rx_data(struct skw_core *skw, void *data, int data_len)
{
#if 0
	struct skw_rx_desc *desc = NULL;
	struct sk_buff *skb;
	int i, total_len;
	u64 p_data = 0;
	u64 *p = NULL;
	u16 pkt_len = 0;

	for (i = 0; i < data_len; i += 8) {
		p = (u64 *)((u8 *)data + i);
		p_data = skw->hw_pdata->pcieaddr_to_virtaddr(*p & 0xFFFFFFFFFF);

		desc = (struct skw_rx_desc *) ((u8 *) (p_data + 52));

		//FW use this way to return unused buff
		if (unlikely(!desc->msdu_len)) {
			skw_compat_page_frag_free((void *)p_data);
			continue;
		}

		//msdu_len+desc_len(72)+eth_hdr_len(14)+pad(2)-snap_hdr(8)
		if (desc->snap_match)
			pkt_len = desc->msdu_len + 80;
		else
			pkt_len = desc->msdu_len + 88;

		total_len = SKB_DATA_ALIGN(pkt_len) + skw->skb_share_len;

		if (unlikely(total_len > SKW_ADMA_BUFF_LEN)) {
			skw_hw_assert(skw);
			skw_warn("total len: %d\n", total_len);

			skw_compat_page_frag_free((void *)p_data);
			continue;
		}

		skb = build_skb((void *)p_data, total_len);
		if (!skb) {
			skw_err("build skb failed, len: %d\n", total_len);

			skw_compat_page_frag_free((void *)p_data);
			continue;
		}

		skb_put(skb, pkt_len);
		skb_pull(skb, 8);
		skb_queue_tail(&skw->rx_dat_q, skb);
		skw->rx_packets++;
		skw_wakeup_rx(skw);
	}
#endif
}

static void skw_pci_edma_rx_filter_data(struct skw_core *skw, void *data, int data_len)
{
	struct sk_buff *skb;
	int total_len;

	total_len = SKB_DATA_ALIGN(data_len) + skw->skb_share_len;

	if (unlikely(total_len > SKW_ADMA_BUFF_LEN)) {
		skw_warn("total_len: %d\n", total_len);
		skw_compat_page_frag_free(data);
		return;
	}

	skb = build_skb((void *)data, total_len);
	if (!skb) {
		skw_err("build skb failed, len: %d\n", total_len);

		skw_compat_page_frag_free(data);
		return;
	}

	skb_put(skb, data_len);

	skb_queue_tail(&skw->rx_dat_q, skb);
	skw->rx_packets++;
	skw_wakeup_rx(skw);
}

void skw_pcie_edma_rx_cb(void *priv, void *data, u16 data_len)
{
	u16 channel = 0;
	int ret = 0, total_len = 0;
	struct skw_edma_context *context = (struct skw_edma_context *) priv;
	struct skw_core *skw = (struct skw_core *) context->skw;
	struct skw_iface *iface = NULL;
	struct skw_event_work *work = NULL;
	struct sk_buff *skb = NULL;
	struct skw_msg *msg = NULL;

	channel = context->channel;

	//skw_dbg("phy data:0x%llx len:%u\n", virt_to_phys(data), data_len);
	//short & long event channel
	//skw_dbg("channel:%d\n", channel);
	if (channel == SKW_EDMA_WIFI_SHORT_EVENT_CHN || channel == SKW_EDMA_WIFI_LONG_EVENT_CHN) {
		//skw_hex_dump("rx_cb data", data, 16, 1);

		total_len = SKB_DATA_ALIGN(data_len) + skw->skb_share_len;
		if (unlikely(total_len > SKW_ADMA_BUFF_LEN)) {
			skw_warn("data: %d\n", data_len);
			skw_compat_page_frag_free(data);
			return;
		}

		skb = build_skb(data, total_len);
		if (!skb) {
			skw_compat_page_frag_free(data);
			skw_err("build skb failed, len: %d\n", data_len);
			return;
		}

		skb_put(skb, data_len);
		//skw_dbg("data len:%d\n", skb->len);
		//skw_hex_dump("event content", skb->data, 16, 1);
		msg = (struct skw_msg *) skb->data;
		switch (msg->type) {
		case SKW_MSG_CMD_ACK:
			skw_cmd_ack_handler(skw, skb->data, skb->len);
			kfree_skb(skb);
			break;

		case SKW_MSG_EVENT:
			if (++skw->skw_event_sn != msg->seq) {
				skw_warn("invalid event seq:%d, expect:%d\n",
					 msg->seq, skw->skw_event_sn);

				//skw_hw_assert(skw);
				//kfree_skb(skb);
				//break;
			}

			if (msg->id == SKW_EVENT_CREDIT_UPDATE) {
				skw_warn("PCIE doesn't support CREDIT");
				kfree_skb(skb);
				break;
			}

			iface = to_skw_iface(skw, msg->inst_id);
			if (iface)
				work = &iface->event_work;
			else
				work = &skw->event_work;

			ret = skw_queue_event_work(priv_to_wiphy(skw),
						work, skb);
			if (ret < 0) {
				skw_err("inst: %d, drop event %d\n",
					msg->inst_id, msg->id);
				kfree_skb(skb);
			}
			break;

		default:
			skw_warn("invalid: type: %d, id: %d, seq: %d\n",
						msg->type, msg->id, msg->seq);
			kfree_skb(skb);
			break;
		}
	} else if (channel == SKW_EDMA_WIFI_TX0_FREE_CHN ||
			channel == SKW_EDMA_WIFI_TX1_FREE_CHN) {
		struct sk_buff_head *edma_free_list = NULL;

		//skw_dbg("channel:%d received tx free data\n", channel);
		if (channel ==  SKW_EDMA_WIFI_TX1_FREE_CHN)
			edma_free_list = &skw->hw.lmac[1].edma_free_list;
		else
			edma_free_list = &skw->hw.lmac[0].edma_free_list;

		skw_pci_edma_tx_free(skw, edma_free_list, data, data_len);

	} else if (channel == SKW_EDMA_WIFI_RX0_CHN ||
			channel == SKW_EDMA_WIFI_RX1_CHN) {
		//skw_dbg("channel:%d received data\n", channel);
		skw_pci_edma_rx_data(skw, data, data_len);
	} else if (channel == SKW_EDMA_WIFI_RX0_FITER_CHN ||
			channel == SKW_EDMA_WIFI_RX1_FITER_CHN) {
		//skw_dbg("channel:%d received filter data\n", channel);
		//skw_hex_dump("filter data", data, data_len, 1);
		skw_pci_edma_rx_filter_data(skw, data, data_len);
	}
}

static int skw_edma_cache_init(struct skw_core *skw)
{
	if (skw->hw.bus != SKW_BUS_PCIE)
		return 0;

	skw_edma_node_cache = kmem_cache_create("skw_edma_node_cache",
						sizeof(struct skw_edma_node),
						0, 0, NULL);
	if (skw_edma_node_cache == NULL)
		return -ENOMEM;

	return 0;
}

static void skw_edma_cache_deinit(struct skw_core *skw)
{
	if (skw->hw.bus == SKW_BUS_PCIE)
		kmem_cache_destroy(skw_edma_node_cache);
}

int skw_edma_cfg_chan(struct skw_core *skw, struct skw_edma_chn *edma_ch,
	struct skw_channel_cfg *cfg)
{
	int ret = 0;

	edma_ch->context.skw = skw;
	edma_ch->context.channel = edma_ch->channel;
	edma_ch->context.edma_ch_cfg = cfg;

	cfg->node_count = edma_ch->max_node_num;
	cfg->header = edma_ch->hdr[cfg->node_count - 1].hdr_next;
	skw_dbg("channel: %d header pa: %pad\n",
		edma_ch->context.channel, (dma_addr_t *)cfg->header);
	cfg->complete_callback = skw->edma_cmd.isr;
	cfg->rx_callback = skw_pcie_edma_rx_cb;
	cfg->context = &skw->edma_cmd.context;
	ret = skw->hw_pdata->hw_channel_init(edma_ch->channel, cfg, NULL);

	return ret;
}

int skw_edma_init(struct wiphy *wiphy)
{
	int ret, i;
	struct skw_channel_cfg ch_cfg;
	struct skw_core *skw = wiphy_priv(wiphy);
	struct skw_lmac *lmac = NULL;

	ret = skw_edma_cache_init(skw);
	if (ret < 0) {
		skw_err("edma cached init failed, ret: %d\n", ret);
		return ret;
	}

	//cmd channel
	skw_edma_chn_init(skw, &skw->edma_cmd,
		SKW_EDMA_WIFI_CMD_CHN, 1,
		SKW_MSG_BUFFER_LEN, skw_edma_tx_node_isr, NULL);
	memset(&ch_cfg, 0, sizeof(struct skw_channel_cfg));
	ch_cfg.direction = 0;
	ch_cfg.priority = 0;
	ch_cfg.split = 1;
	ch_cfg.ring = 1;
	ch_cfg.req_mode = 1;
	ch_cfg.irq_threshold = 1;
	skw_edma_cfg_chan(skw, &skw->edma_cmd, &ch_cfg);

	//short event channel
	skw_edma_chn_init(skw, &skw->edma_short_event,
		SKW_EDMA_WIFI_SHORT_EVENT_CHN,
		SKW_EDMA_EVENT_CHN_NODE_NUM,
		SKW_MSG_BUFFER_LEN, NULL, NULL);
	memset(&ch_cfg, 0, sizeof(struct skw_channel_cfg));
	ch_cfg.direction = 1;
	ch_cfg.priority = 0;
	ch_cfg.split = 1;
	ch_cfg.ring = 0;
	ch_cfg.req_mode = 1;
	ch_cfg.irq_threshold = 1;
	skw_edma_cfg_chan(skw, &skw->edma_short_event, &ch_cfg);

	//long event channel
	skw_edma_chn_init(skw, &skw->edma_long_event,
		SKW_EDMA_WIFI_LONG_EVENT_CHN,
		SKW_EDMA_EVENT_CHN_NODE_NUM,
		SKW_MSG_BUFFER_LEN, NULL, NULL);

	memset(&ch_cfg, 0, sizeof(struct skw_channel_cfg));
	ch_cfg.direction = 1;
	ch_cfg.priority = 0;
	ch_cfg.split = 1;
	ch_cfg.ring = 0;
	ch_cfg.req_mode = 1;
	ch_cfg.irq_threshold = 1;
	skw_edma_cfg_chan(skw, &skw->edma_long_event, &ch_cfg);

	// data tx/rx channel
	for (i = 0; i < SKW_NR_LMAC; i++) {
		lmac = &skw->hw.lmac[i];

		// RX filter channel
		skw_edma_chn_init(skw,  &lmac->edma_filter_ch,
			SKW_EDMA_WIFI_RX0_FITER_CHN + i,
			SKW_EDMA_FILTER_CHN_NODE_NUM,
			SKW_MSG_BUFFER_LEN, NULL, NULL);
		memset(&ch_cfg, 0, sizeof(struct skw_channel_cfg));
		ch_cfg.direction = 1;
		ch_cfg.priority = 0;
		ch_cfg.split = 1;
		ch_cfg.ring = 0;
		ch_cfg.req_mode = 1;
		ch_cfg.irq_threshold = 1;
		skw_edma_cfg_chan(skw, &lmac->edma_filter_ch, &ch_cfg);

		//TX chan
		skw_edma_chn_init(skw, &lmac->edma_tx_chn,
			SKW_EDMA_WIFI_TX0_CHN + i,
			SKW_EDMA_TX_CHN_NODE_NUM,
			SKW_EDMA_DATA_LEN, skw_edma_tx_node_isr, NULL);
		memset(&ch_cfg, 0, sizeof(struct skw_channel_cfg));
		ch_cfg.direction = 0;
		ch_cfg.priority = 0;
		ch_cfg.split = 1;
		ch_cfg.ring = 1;
		ch_cfg.req_mode = 1;
		ch_cfg.irq_threshold = 1;
		skw_edma_cfg_chan(skw, &lmac->edma_tx_chn, &ch_cfg);

		//TX free chan
		skb_queue_head_init(&lmac->edma_free_list);

		skw_edma_chn_init(skw, &lmac->edma_tx_resp_chn,
			SKW_EDMA_WIFI_TX0_FREE_CHN + i,
			SKW_EDMA_TX_FREE_CHN_NODE_NUM,
			SKW_EDMA_DATA_LEN, NULL, NULL);
		memset(&ch_cfg, 0, sizeof(struct skw_channel_cfg));
		ch_cfg.direction = 1;
		ch_cfg.priority = 0;
		ch_cfg.split = 1;
		ch_cfg.ring = 1;
		ch_cfg.req_mode = 1;
		ch_cfg.irq_threshold = 1;
		skw_edma_cfg_chan(skw, &lmac->edma_tx_resp_chn, &ch_cfg);

		//RX chan
		skw_edma_chn_init(skw, &lmac->edma_rx_chn,
			SKW_EDMA_WIFI_RX0_CHN + i,
			SKW_EDMA_RX_CHN_NODE_NUM,
			SKW_EDMA_DATA_LEN, NULL, NULL);
		memset(&ch_cfg, 0, sizeof(struct skw_channel_cfg));
		ch_cfg.direction = 1;
		ch_cfg.priority = 0;
		ch_cfg.split = 1;
		ch_cfg.ring = 1;
		ch_cfg.req_mode = 1;
		ch_cfg.irq_threshold = 1;
		skw_edma_cfg_chan(skw, &lmac->edma_rx_chn, &ch_cfg);

		//RX free chan
		skw_edma_chn_init(skw, &lmac->edma_rx_req_chn,
			SKW_EDMA_WIFI_RX0_FREE_CHN + i,
			SKW_EDMA_RX_FREE_CHN_NODE_NUM,
			SKW_EDMA_DATA_LEN, NULL, NULL);
		memset(&ch_cfg, 0, sizeof(struct skw_channel_cfg));
		ch_cfg.direction = 0;
		ch_cfg.priority = 0;
		ch_cfg.split = 1;
		ch_cfg.ring = 1;
		ch_cfg.req_mode = 1;
		//mac0_rx_free_ch_cfg.irq_threshold = 1;
		skw_edma_cfg_chan(skw, &lmac->edma_rx_req_chn, &ch_cfg);

		lmac->flags = SKW_LMAC_FLAG_INIT;
	}

	return 0;
}
//TBD: Use macro to define the node number for each channel

void skw_edma_deinit(struct wiphy *wiphy)
{
	struct skw_core *skw = wiphy_priv(wiphy);
	int i = 0;
	struct skw_lmac *lmac = NULL;

	if (skw->hw.bus != SKW_BUS_PCIE)
		return;

	skw_edma_chn_deinit(skw, &skw->edma_cmd);
	skw_edma_chn_deinit(skw, &skw->edma_short_event);
	skw_edma_chn_deinit(skw, &skw->edma_long_event);

	for (i = 0; i < SKW_NR_LMAC; i++) {
		lmac = &skw->hw.lmac[i];
		skw_edma_chn_deinit(skw, &lmac->edma_tx_chn);
		skw_edma_chn_deinit(skw, &lmac->edma_tx_resp_chn);
		skw_edma_chn_deinit(skw, &lmac->edma_rx_chn);
		skw_edma_chn_deinit(skw, &lmac->edma_rx_req_chn);
		skb_queue_purge(&lmac->edma_free_list);
	}

	skw_edma_cache_deinit(skw);
}
