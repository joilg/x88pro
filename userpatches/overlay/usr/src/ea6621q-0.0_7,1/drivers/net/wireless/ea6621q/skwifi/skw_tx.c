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

#include <linux/kthread.h>
#include <linux/ip.h>
#include <linux/ctype.h>

#include "skw_core.h"
#include "skw_tx.h"
#include "skw_msg.h"
#include "skw_iface.h"
#include "trace.h"

#define SKW_BASE_VO                    16
#define SKW_BASE_VI                    24
#define SKW_TX_TIMEOUT                 200
#define SKW_TX_RUNING_TIMES            20

struct skw_tx_info {
	int quota;
	bool reset;
	struct sk_buff_head *list;
};

struct skw_tx_lmac {
	bool reset;
	int cred;
	u16 txq_map;
	u16 nr_txq;
	int bk_tx_limit;
	int current_qlen;
	int ac_reset;
	int tx_count_limit;

	struct sk_buff_head tx_list;
	struct skw_tx_info tx[SKW_NR_IFACE];
};

static unsigned int tx_wait_time;

static int skw_tx_time_show(struct seq_file *seq, void *data)
{
	seq_printf(seq, "current tx_wait_time = %dus\n", tx_wait_time);
	return 0;
}

static int skw_tx_time_open(struct inode *inode, struct file *file)
{
	return single_open(file, skw_tx_time_show, inode->i_private);
}

static ssize_t skw_tx_time_write(struct file *fp, const char __user *buf,
				size_t len, loff_t *offset)
{
	int i;
	char cmd[32] = {0};
	unsigned int res = 0;

	for (i = 0; i < 32; i++) {
		char c;

		if (get_user(c, buf))
			return -EFAULT;

		if (c == '\n' || c == '\0')
			break;

		if (isdigit(c) != 0)
			cmd[i] = c;
		else {
			skw_warn("set fail, not number\n");
			return -EFAULT;
		}
		buf++;
	}

	if (kstrtouint(cmd, 10, &res))
		return -EFAULT;

	skw_info("set tx_wait_time = %dus\n", res);
	tx_wait_time = res;

	return len;
}

static const struct file_operations skw_tx_time_fops = {
	.owner = THIS_MODULE,
	.open = skw_tx_time_open,
	.read = seq_read,
	.release = single_release,
	.write = skw_tx_time_write,
};

int skw_pcie_cmd_xmit(struct skw_core *skw, void *data, int data_len)
{
	skw->edma_cmd.hdr[0].data_len = data_len;
	skw_edma_set_data(priv_to_wiphy(skw), &skw->edma_cmd, data, data_len);

	return skw_edma_tx(priv_to_wiphy(skw), &skw->edma_cmd, data_len);
}

int skw_sdio_cmd_xmit(struct skw_core *skw, void *data, int data_len)
{
	int nr = 0, total_len;

	sg_init_table(skw->sgl_cmd, SKW_NR_SGL_CMD);
	sg_set_buf(&skw->sgl_cmd[nr++], data, data_len);
	total_len = data_len;

	skw_set_extra_hdr(skw, skw->eof_blk, skw->hw.cmd_port, skw->hw.align, 0, 1);
	sg_set_buf(&skw->sgl_cmd[nr++], skw->eof_blk, skw->hw.align);
	total_len += skw->hw.align;

	return skw->hw.cmd_xmit(skw, NULL, -1, skw->hw.cmd_port,
			skw->sgl_cmd, nr, total_len);
}

int skw_usb_cmd_xmit(struct skw_core *skw, void *data, int data_len)
{
	int nr = 0, total_len;

	sg_init_table(skw->sgl_cmd, SKW_NR_SGL_CMD);
	sg_set_buf(&skw->sgl_cmd[nr++], data, data_len);
	total_len = data_len;

	return skw->hw.cmd_xmit(skw, NULL, -1, skw->hw.cmd_port,
			skw->sgl_cmd, nr, total_len);
}

static int skw_sync_sdma_tx(struct skw_core *skw, struct sk_buff_head *list,
			int lmac_id, int port, struct scatterlist *sgl,
			int nents, int tx_len)
{
	int total_len;

	if (!skw->sdma_buff) {
		skw_err("invalid buff\n");
		return -ENOMEM;
	}

	total_len = sg_copy_to_buffer(sgl, nents, skw->sdma_buff,
				skw->hw_pdata->max_buffer_size);

	skw->hw_pdata->hw_sdma_tx(port, skw->sdma_buff, total_len);

	if (list) {
		skw_sub_credit(skw, lmac_id, skb_queue_len(list));
		__skb_queue_purge(list);
	}

	return 0;
}

static int skw_async_sdma_tx(struct skw_core *skw, struct sk_buff_head *list,
			int lmac_id, int port, struct scatterlist *sgl,
			int nents, int tx_len)
{
	void *buff;
	int ret, total_len;

	buff = SKW_ALLOC(tx_len, GFP_KERNEL);
	if (!buff) {
		skw_err("invalid buffer\n");
		return -ENOMEM;
	}

	total_len = sg_copy_to_buffer(sgl, nents, buff, tx_len);
	ret = skw->hw_pdata->hw_sdma_tx_async(port, buff, total_len);
	if (ret < 0)
		SKW_KFREE(buff);

	if (list) {
		skw_sub_credit(skw, lmac_id, skb_queue_len(list));
		__skb_queue_purge(list);
	}

	return 0;
}

static int skw_sync_adma_tx(struct skw_core *skw, struct sk_buff_head *list,
			int lmac_id, int port, struct scatterlist *sgl,
			int nents, int tx_len)
{
	struct sk_buff *skb, *tmp;
	int ret;

	ret = skw->hw_pdata->hw_adma_tx(port, sgl, nents, tx_len);
	skw->trans_start = jiffies;

	if (list) {
		skw_sub_credit(skw, lmac_id, skb_queue_len(list));
		//__skb_queue_purge(list);
		skb_queue_walk_safe(list, skb, tmp) {
			if (likely(0 == ret)) {
				skb->dev->stats.tx_packets++;
				skb->dev->stats.tx_bytes += SKW_SKB_TXCB(skb)->skb_native_len;
			} else
				skb->dev->stats.tx_errors++;

			__skb_unlink(skb, list);
			kfree_skb(skb);
		}
	}

	return 0;
}

static int skw_async_adma_tx(struct skw_core *skw, struct sk_buff_head *list,
			int lmac_id, int port, struct scatterlist *sgl,
			int nents, int tx_len)
{
	int ret, idx;
	struct scatterlist *sg_list, *sg;
	unsigned long *skb_addr, *sg_addr;
	struct sk_buff *skb;

	if (!sgl) {
		ret = -ENOMEM;
		skw_err("sgl is NULL\n");
		goto out;
	}

	sg_list = kcalloc(SKW_NR_SGL_DAT, sizeof(*sg_list), GFP_KERNEL);

	ret = skw->hw_pdata->hw_adma_tx_async(port, sgl, nents, tx_len);
	if (ret < 0) {
		skw_err("failed, ret: %d\n", ret);

		for_each_sg(sgl, sg, nents, idx) {
			sg_addr = (unsigned long *)sg_virt(sg);

			skb_addr = sg_addr - 1;
			skb = (struct sk_buff *)*skb_addr;
			kfree_skb(skb);
		}

		SKW_KFREE(sgl);
	} else {
		atomic_add(nents, &skw->txqlen_pending);
		skw_sub_credit(skw, lmac_id, nents);
	}

	skw->sgl_dat = sg_list;
out:
	return ret;
}

static int skw_async_adma_tx_free(int id, struct scatterlist *sg, int nents,
			   void *data, int status)
{
	struct skw_sg_node node;
	struct skw_core *skw = data;
	struct wiphy *wiphy = priv_to_wiphy(skw);

	node.sg = sg;
	node.nents = nents;
	node.status = status;

	skw_queue_work(wiphy, NULL, SKW_WORK_TX_FREE, &node, sizeof(node));

	return 0;
}

static int skw_async_sdma_tx_free(int id,  void *buffer, int size,
			   void *data, int status)
{
	SKW_KFREE(buffer);

	return 0;
}

int skw_sdio_xmit(struct skw_core *skw, int lmac_id, struct sk_buff_head *txq)
{
	struct sk_buff *skb;
	int nents = 0, tx_bytes = 0;
	struct skw_lmac *lmac = &skw->hw.lmac[lmac_id];

	sg_init_table(skw->sgl_dat, SKW_NR_SGL_DAT);

	skb_queue_walk(txq, skb) {
		int aligned;
		struct skw_packet_header *extra_hdr;

		extra_hdr = (void *)skb_push(skb, SKW_EXTER_HDR_SIZE);

		aligned = round_up(skb->len, skw->hw.align);
		skw_set_extra_hdr(skw, extra_hdr, lmac->lport, aligned, 0, 0);

		sg_set_buf(&skw->sgl_dat[nents++], skb->data, aligned);

		tx_bytes += aligned;
	}

	skw_set_extra_hdr(skw, skw->eof_blk, lmac->lport, skw->hw.align, 0, 1);
	sg_set_buf(&skw->sgl_dat[nents++], skw->eof_blk, skw->hw.align);
	tx_bytes += skw->hw.align;
	skw_detail("nents:%d", nents);

	return skw->hw.dat_xmit(skw, txq, lmac_id, lmac->dport,
				skw->sgl_dat, nents, tx_bytes);
}

int skw_usb_xmit(struct skw_core *skw, int lmac_id, struct sk_buff_head *txq)
{
	struct sk_buff *skb, *tmp;
	int nents = 0, tx_bytes = 0;
	unsigned long *skb_addr;
	struct skw_lmac *lmac = &skw->hw.lmac[lmac_id];

	sg_init_table(skw->sgl_dat, SKW_NR_SGL_DAT);

	skb_queue_walk_safe(txq, skb, tmp) {
		int aligned;
		struct skw_packet_header *extra_hdr;

		extra_hdr = (void *)skb_push(skb, SKW_EXTER_HDR_SIZE);

		aligned = round_up(skb->len, skw->hw.align);
		skw_set_extra_hdr(skw, extra_hdr, lmac->lport, aligned, 0, 0);

		sg_set_buf(&skw->sgl_dat[nents++], skb->data, aligned);

		skb_addr = (unsigned long *)skb_push(skb, sizeof(unsigned long));
		*skb_addr = (unsigned long)skb;
		skb_pull(skb, sizeof(unsigned long));

		tx_bytes += aligned;

		if (skb && skw->hw.dma == SKW_ASYNC_ADMA_TX)
			__skb_unlink(skb, txq);
	}

	return skw->hw.dat_xmit(skw, txq, lmac_id, lmac->dport,
				skw->sgl_dat, nents, tx_bytes);
}

int skw_pcie_xmit(struct skw_core *skw, int lmac_id, struct sk_buff_head *txq)
{
	int ret, tx_bytes = 0;
	unsigned long flags;
	struct sk_buff *skb;
	struct skw_lmac *lmac = &skw->hw.lmac[lmac_id];
	struct wiphy *wiphy = priv_to_wiphy(skw);

	skb_queue_walk(txq, skb) {
		skw_edma_set_data(wiphy, &lmac->edma_tx_chn,
				&SKW_SKB_TXCB(skb)->e,
				sizeof(SKW_SKB_TXCB(skb)->e));

		tx_bytes += round_up(skb->len, skw->hw.align);
	}

	skb = skb_peek(txq);

	spin_lock_irqsave(&lmac->edma_free_list.lock, flags);
	skb_queue_splice_tail_init(txq, &lmac->edma_free_list);
	spin_unlock_irqrestore(&lmac->edma_free_list.lock, flags);

	ret = skw_edma_tx(wiphy, &lmac->edma_tx_chn, tx_bytes);
	if (ret < 0) {
		skw_err("failed, ret: %d\n", ret);
		// TODO:
		// release free list
	}

	return ret;
}

static inline int skw_bus_data_xmit(struct skw_core *skw, int mac_id,
			struct sk_buff_head *txq_list)
{
	if (!skb_queue_len(txq_list))
		return 0;

	skw->tx_packets += skb_queue_len(txq_list);

	return skw->hw.bus_dat_xmit(skw, mac_id, txq_list);
}

static inline int skw_bus_cmd_xmit(struct skw_core *skw, void *cmd, int cmd_len)
{
	unsigned long mask = 0;

	if (test_bit(SKW_CMD_FLAG_IGNORE_BLOCK_TX, &skw->cmd.flags))
		mask |= BIT(SKW_FLAG_BLOCK_TX);

	if (!skw_tx_allowed(skw, mask)) {
		skw_warn("cmd: %s[%d] not allowed, flags: 0x%lx, cmd flags: 0x%lx\n",
			 skw->cmd.name, skw->cmd.id, skw->flags, skw->cmd.flags);
		return 0;
	}

	return skw->hw.bus_cmd_xmit(skw, cmd, cmd_len);
}

static inline bool skw_is_same_tcp_stream(struct sk_buff *skb,
					struct sk_buff *next)
{
	return ip_hdr(skb)->saddr == ip_hdr(next)->saddr &&
	       ip_hdr(skb)->daddr == ip_hdr(next)->daddr &&
	       tcp_hdr(skb)->source == tcp_hdr(next)->source &&
	       tcp_hdr(skb)->dest == tcp_hdr(next)->dest;
}

static void skw_merge_pure_ack(struct sk_buff_head *ackq,
				struct sk_buff_head *txq)
{
	int i, drop = 0;
	struct sk_buff *skb, *tmp;

	while ((skb = __skb_dequeue_tail(ackq))) {
		for (i = 0; i < ackq->qlen; i++) {
			tmp = __skb_dequeue(ackq);
			if (!tmp)
				break;

			if (skw_is_same_tcp_stream(skb, tmp)) {
				if (tcp_optlen(tmp) == 0 &&
				    tcp_flag_word(tcp_hdr(tmp)) == TCP_FLAG_ACK) {
					kfree_skb(tmp);
					drop++;
				} else {
					__skb_queue_tail(txq, tmp);
				}
			} else {
				__skb_queue_tail(ackq, tmp);
			}
		}

		__skb_queue_tail(txq, skb);
	}
}

static bool skw_is_peer_data_valid(struct skw_core *skw, struct sk_buff *skb)
{
	struct skw_ctx_entry *entry;
	bool valid = true;
	int peer_idx = SKW_SKB_TXCB(skb)->peer_idx;

	rcu_read_lock();

	entry = rcu_dereference(skw->peer_ctx[peer_idx].entry);
	if (entry) {
		if (entry->peer) {
			entry->peer->tx.bytes += skb->len;
			entry->peer->tx.pkts++;

			if (entry->peer->flags & SKW_PEER_FLAG_DEAUTHED)
				valid = false;
		} else {
			if (is_unicast_ether_addr(eth_hdr(skb)->h_dest))
				valid = false;
		}
	}

	rcu_read_unlock();

	return valid;
}

static inline void skw_reset_ac(struct skw_tx_lmac *txlp)
{
	int i;

	for (i = 0; i < SKW_NR_LMAC; i++) {
		txlp->ac_reset = 0xF;
		txlp++;
	}
}

#ifdef CONFIG_SKW_TX_WORKQUEUE
void skw_tx_worker(struct work_struct *work)
{
	int i, ac, mac;
	int base = 0;
	unsigned long flags;
	int lmac_tx_capa;
	int qlen, pending_qlen = 0;
	int max_tx_count_limit = 0;
	int lmac_tx_map = 0;
	struct sk_buff *skb;
	struct skw_iface *iface;
	struct sk_buff_head *qlist;
	struct netdev_queue *txq;
	struct skw_tx_lmac txl[SKW_NR_LMAC];
	struct skw_tx_lmac *txlp;
	struct sk_buff_head pure_ack_list;
	int xmit_tx_flag;
	int all_credit;
	struct skw_core *skw = container_of(work, struct skw_core, tx_worker);

	memset(txl, 0, sizeof(txl));
	skw_reset_ac(txl);

	max_tx_count_limit = skw->hw.pkt_limit;

	/* reserve one for eof block */
	if (skw->hw.bus == SKW_BUS_SDIO)
		max_tx_count_limit--;

	for (i = 0; i < SKW_NR_LMAC; i++) {
		__skb_queue_head_init(&txl[i].tx_list);
		txl[i].tx_count_limit = max_tx_count_limit;
	}

	while (!atomic_read(&skw->exit)) {

		// TODO:
		/* CPU bind */
		/* check if frame in pending queue is timeout */

		if (test_and_clear_bit(SKW_CMD_FLAG_XMIT, &skw->cmd.flags))
			skw_bus_cmd_xmit(skw, skw->cmd.data, skw->cmd.data_len);

		if (!skw_tx_allowed(skw, 0)) {
			pending_qlen = 0;

			return;
		}

		pending_qlen = 0;
		lmac_tx_map = 0;

		all_credit = 0;
		for (mac = 0; mac < SKW_NR_LMAC; mac++)
			all_credit += skw_get_hw_credit(skw, mac);

		if (0 == all_credit) {
			if (jiffies_to_usecs(jiffies - skw->trans_start) < tx_wait_time)
				continue;
			else
				return;
		}

		for (ac = 0; ac < SKW_WMM_AC_MAX; ac++) {
			int ac_qlen = 0;

			for (i = 0; i < SKW_NR_IFACE; i++) {
				iface = skw->vif.iface[i];
				// if (!iface || skw_lmac_is_actived(skw, iface->lmac_id))
				if (!iface || !iface->ndev)
					continue;

				if (ac == SKW_WMM_AC_BE && iface->txq[SKW_ACK_TXQ].qlen) {
					qlist = &iface->txq[SKW_ACK_TXQ];

					__skb_queue_head_init(&pure_ack_list);

					spin_lock_irqsave(&qlist->lock, flags);
					skb_queue_splice_tail_init(&iface->txq[SKW_ACK_TXQ],
								&pure_ack_list);
					spin_unlock_irqrestore(&qlist->lock, flags);

					skw_merge_pure_ack(&pure_ack_list,
							&iface->tx_cache[ac]);
				}

				qlist = &iface->txq[ac];
				if (!skb_queue_empty(qlist)) {
					spin_lock_irqsave(&qlist->lock, flags);
					skb_queue_splice_tail_init(qlist,
							&iface->tx_cache[ac]);
					spin_unlock_irqrestore(&qlist->lock, flags);
				}

				if (READ_ONCE(iface->flags) & SKW_IFACE_FLAG_DEAUTH)
					__skb_queue_purge(&iface->tx_cache[ac]);

				qlen = skb_queue_len(&iface->tx_cache[ac]);
				if (qlen) {
					if (qlen < SKW_TXQ_LOW_THRESHOLD) {
						txq = netdev_get_tx_queue(iface->ndev, ac);
						if (netif_tx_queue_stopped(txq)) {
							netif_tx_start_queue(txq);
							netif_schedule_queue(txq);
						}
					}

					txlp = &txl[iface->lmac_id];
					txlp->current_qlen += qlen;

					txlp->txq_map |= BIT(txlp->nr_txq);

					txlp->tx[txlp->nr_txq].list = &iface->tx_cache[ac];
					txlp->tx[txlp->nr_txq].reset = true;
					txlp->reset = true;
					if (txlp->ac_reset & BIT(ac)) {
						txlp->tx[txlp->nr_txq].quota = iface->wmm.factor[ac];
						txlp->ac_reset ^= BIT(ac);
					}

					txlp->nr_txq++;
					ac_qlen += qlen;
				}
			}

			if (!ac_qlen)
				continue;

			pending_qlen += ac_qlen;

			lmac_tx_capa = 0;

			for (mac = 0; mac < SKW_NR_LMAC; mac++) {
				int credit;

				txlp = &txl[mac];
				if (!txlp->txq_map)
					goto reset;

				credit = txlp->cred = skw_get_hw_credit(skw, mac);
				if (!txlp->cred)
					goto reset;

				if (txlp->reset) {
					switch (ac) {
					case SKW_WMM_AC_VO:
						base = SKW_BASE_VO;
						break;

					case SKW_WMM_AC_VI:
						base = SKW_BASE_VI;
						break;

					case SKW_WMM_AC_BK:
						if (txlp->bk_tx_limit) {
							base = min(txlp->cred, txlp->bk_tx_limit);
							txlp->bk_tx_limit = 0;
						} else {
							base = txlp->cred;
						}

						base = base / txlp->nr_txq;
						break;

					default:
						base = min(txlp->cred, txlp->current_qlen);
						base = base / txlp->nr_txq;
						txlp->bk_tx_limit = (txlp->cred + 1) >> 1;
						break;
					}

					base = base ? base : 1;
					txlp->reset = false;
				} else
					base = 0;

				for (i = 0; txlp->txq_map != 0; i++) {

					i = i % txlp->nr_txq;
					if (!(txlp->txq_map & BIT(i)))
						continue;

					if (!txlp->cred)
						break;

					if (txlp->tx[i].reset) {
						txlp->tx[i].quota += base;

						if (txlp->tx[i].quota < 0)
							txlp->tx[i].quota = 0;

						txlp->tx[i].reset = false;
					}

					skb = skb_peek(txlp->tx[i].list);
					if (!skb) {
						txlp->txq_map ^= BIT(i);
						continue;
					}

					if (!skw_is_peer_data_valid(skw, skb)) {

						skw_detail("drop dest: %pM\n",
							   eth_hdr(skb)->h_dest);

						__skb_unlink(skb, txlp->tx[i].list);
						kfree_skb(skb);

						continue;
					}

					if (!txlp->tx_count_limit--)
						break;

					if (txlp->tx[i].quota) {
						txlp->tx[i].quota--;
					} else {
						txlp->txq_map ^= BIT(i);
						continue;
					}

					if ((long)skb->data & SKW_DATA_ALIGN_MASK)
						skw_warn("address unaligned\n");

#if 0
					if (skb->len % skw->hw_pdata->align_value)
						skw_warn("len: %d unaligned\n", skb->len);
#endif

					__skb_unlink(skb, txlp->tx[i].list);
					__skb_queue_tail(&txlp->tx_list, skb);

					txlp->cred--;
				}

				pending_qlen = pending_qlen - credit + txlp->cred;

				trace_skw_tx_info(mac, ac, credit, credit - txlp->cred, txlp->current_qlen);
				atomic_set(&skw->xmitting_data, 1);
				skw_bus_data_xmit(skw, mac, &txlp->tx_list);
				atomic_set(&skw->xmitting_data, 0);
				txlp->tx_count_limit = max_tx_count_limit;

				lmac_tx_map |= BIT(mac);

				if (txlp->cred)
					lmac_tx_capa |= BIT(mac);

reset:
				txlp->nr_txq = 0;
				txlp->txq_map = 0;
				txlp->current_qlen = 0;
			}

			if (!lmac_tx_capa)
				break;
		}

		if (ac == SKW_WMM_AC_MAX)
			skw_reset_ac(txl);

		if (pending_qlen == 0) {
			xmit_tx_flag = 0;

			for (ac = 0; ac < SKW_WMM_AC_MAX; ac++) {
				for (i = 0; i < SKW_NR_IFACE; i++) {
					iface = skw->vif.iface[i];
					if (!iface || !iface->ndev)
						continue;

					if (skb_queue_len(&iface->tx_cache[ac]) != 0) {
						xmit_tx_flag = 1;
						goto need_running;
					}

					spin_lock_irqsave(&iface->txq[ac].lock, flags);

					if (skb_queue_len(&iface->txq[ac]) != 0) {
						xmit_tx_flag = 1;
						spin_unlock_irqrestore(&iface->txq[ac].lock, flags);
						goto need_running;

					}

					spin_unlock_irqrestore(&iface->txq[ac].lock, flags);
				}
			}
need_running:
			if (test_bit(SKW_CMD_FLAG_XMIT, &skw->cmd.flags))
				xmit_tx_flag = 1;

			if (xmit_tx_flag == 0) {
				//skw_start_dev_queue(skw);
				return;
			}
		}
	}
}

static int __skw_tx_init(struct skw_core *skw)
{
	//struct workqueue_attrs wq_attrs;

	skw->tx_wq = alloc_workqueue("skw_txwq.%d",
			WQ_UNBOUND | WQ_CPU_INTENSIVE | WQ_HIGHPRI | WQ_SYSFS,
			0, skw->idx);
	if (!skw->tx_wq) {
		skw_err("alloc skwtx_workqueue failed\n");
		return -EFAULT;
	}

	//memset(&wq_attrs, 0, sizeof(wq_attrs));

	//wq_attrs.nice = MIN_NICE;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0) && LINUX_VERSION_CODE <= KERNEL_VERSION(5, 2, 0)
	//apply_workqueue_attrs(skw->tx_wq, &wq_attrs);
#endif

	INIT_WORK(&skw->tx_worker, skw_tx_worker);
	queue_work(skw->tx_wq, &skw->tx_worker);
	//queue_work_on(cpumask_last(cpu_online_mask), skw->tx_wq, &skw->tx_worker);
	skw->trans_start = 0;

	return 0;
}

static void __skw_tx_deinit(struct skw_core *skw)
{
	atomic_set(&skw->exit, 1);
	cancel_work_sync(&skw->tx_worker);
	destroy_workqueue(skw->tx_wq);
}

#else

static int skw_tx_thread(void *data)
{
	struct skw_core *skw = data;
	int i, ac, mac;
	int base = 0;
	unsigned long flags;
	int lmac_tx_capa;
	int qlen, pending_qlen = 0;
	int max_tx_count_limit = 0;
	int lmac_tx_map = 0;
	struct sk_buff *skb;
	struct skw_iface *iface;
	struct sk_buff_head *qlist;
	struct skw_tx_lmac txl[SKW_NR_LMAC];
	struct skw_tx_lmac *txlp;
	struct sk_buff_head pure_ack_list;
	int xmit_tx_flag;
	int all_credit;

	memset(txl, 0, sizeof(txl));
	skw_reset_ac(txl);

	max_tx_count_limit = skw->hw.pkt_limit;

	/* reserve one for eof block */
	if (skw->hw.bus == SKW_BUS_SDIO)
		max_tx_count_limit--;

	for (i = 0; i < SKW_NR_LMAC; i++) {
		__skb_queue_head_init(&txl[i].tx_list);
		txl[i].tx_count_limit = max_tx_count_limit;
	}

	while (!kthread_should_stop()) {
		// TODO:
		/* CPU bind */
		/* check if frame in pending queue is timeout */

		if (test_and_clear_bit(SKW_CMD_FLAG_XMIT, &skw->cmd.flags))
			skw_bus_cmd_xmit(skw, skw->cmd.data, skw->cmd.data_len);

		if (!skw_tx_allowed(skw, 0)) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(msecs_to_jiffies(200));
			continue;
		}

		pending_qlen = 0;
		lmac_tx_map = 0;

		for (ac = 0; ac < SKW_WMM_AC_MAX; ac++) {
			int ac_qlen = 0;

			for (i = 0; i < SKW_NR_IFACE; i++) {
				iface = skw->vif.iface[i];
				// if (!iface || skw_lmac_is_actived(skw, iface->lmac_id))
				if (!iface || !iface->ndev)
					continue;

				if (ac == SKW_WMM_AC_BE && iface->txq[SKW_ACK_TXQ].qlen) {
					qlist = &iface->txq[SKW_ACK_TXQ];
					__skb_queue_head_init(&pure_ack_list);

					spin_lock_irqsave(&qlist->lock, flags);
					skb_queue_splice_tail_init(&iface->txq[SKW_ACK_TXQ],
								&pure_ack_list);
					spin_unlock_irqrestore(&qlist->lock, flags);

					skw_merge_pure_ack(&pure_ack_list, &iface->tx_cache[ac]);
				}

				qlist = &iface->txq[ac];
				if (!skb_queue_empty(qlist)) {
					spin_lock_irqsave(&qlist->lock, flags);
					skb_queue_splice_tail_init(qlist, &iface->tx_cache[ac]);
					spin_unlock_irqrestore(&qlist->lock, flags);
				}

				qlen = skb_queue_len(&iface->tx_cache[ac]);
				if (qlen) {
					txlp = &txl[iface->lmac_id];
					txlp->current_qlen += qlen;
					txlp->txq_map |= BIT(txlp->nr_txq);
					txlp->tx[txlp->nr_txq].list = &iface->tx_cache[ac];

					if (txlp->ac_reset & BIT(ac)) {
						txlp->tx[txlp->nr_txq].quota = iface->wmm.factor[ac];
						txlp->tx[txlp->nr_txq].reset = true;
						txlp->reset = true;
						txlp->ac_reset ^= BIT(ac);
					}

					txlp->nr_txq++;
					ac_qlen += qlen;
				}
			}

			if (!ac_qlen)
				continue;

			pending_qlen += ac_qlen;
			lmac_tx_capa = 0;

			all_credit = 0;
			for (mac = 0; mac < SKW_NR_LMAC; mac++)
				all_credit += skw_get_hw_credit(skw, mac);

			if (all_credit == 0) {
				skw_stop_dev_queue(skw);
				wait_event_interruptible_exclusive(skw->tx_wait_q,
					atomic_xchg(&skw->tx_wake, 0) || atomic_xchg(&skw->exit, 0));
				skw_start_dev_queue(skw);
			}

			for (mac = 0; mac < SKW_NR_LMAC; mac++) {
				int credit;

				txlp = &txl[mac];
				if (!txlp->txq_map)
					goto reset;

				credit = txlp->cred = skw_get_hw_credit(skw, mac);
				if (!txlp->cred)
					goto reset;

				if (txlp->reset) {
					switch (ac) {
					case SKW_WMM_AC_VO:
						base = SKW_BASE_VO;
						break;

					case SKW_WMM_AC_VI:
						base = SKW_BASE_VI;
						break;

					case SKW_WMM_AC_BK:
						if (txlp->bk_tx_limit) {
							base = min(txlp->cred, txlp->bk_tx_limit);
							txlp->bk_tx_limit = 0;
						} else {
							base = txlp->cred;
						}

						base = base / txlp->nr_txq;

						break;
					default:
						base = min(txlp->cred, txlp->current_qlen);
						base = base / txlp->nr_txq;
						txlp->bk_tx_limit = (txlp->cred + 1) >> 1;

						break;
					}

					base = base ? base : 1;
					txlp->reset = false;
				}

				for (i = 0; txlp->txq_map != 0; i++) {
					i = i % txlp->nr_txq;

					if (!(txlp->txq_map & BIT(i)))
						continue;

					if (!txlp->cred)
						break;

					if (txlp->tx[i].reset) {
						txlp->tx[i].quota += base;

						if (txlp->tx[i].quota < 0)
							txlp->tx[i].quota = 0;

						txlp->tx[i].reset = false;
					}

					skb = skb_peek(txlp->tx[i].list);
					if (!skb) {
						txlp->txq_map ^= BIT(i);
						continue;
					}

					if (!skw_is_peer_data_valid(skw, skb)) {
						skw_detail("drop dest: %pM\n",
							   eth_hdr(skb)->h_dest);

						__skb_unlink(skb, txlp->tx[i].list);
						kfree_skb(skb);

						continue;
					}

					if (!txlp->tx_count_limit--)
						break;

					if (txlp->tx[i].quota) {
						txlp->tx[i].quota--;
					} else {
						txlp->txq_map ^= BIT(i);
						continue;
					}

					if ((long)skb->data & SKW_DATA_ALIGN_MASK)
						skw_warn("address unaligned\n");
#if 0
					if (skb->len % skw->hw_pdata->align_value)
						skw_warn("len: %d unaligned\n", skb->len);
#endif
					__skb_unlink(skb, txlp->tx[i].list);
					__skb_queue_tail(&txlp->tx_list, skb);
					txlp->cred--;
				}

				pending_qlen = pending_qlen - credit + txlp->cred;

				trace_skw_tx_info(mac, ac, credit, credit - txlp->cred, txlp->current_qlen);

				// skw_lmac_tx(skw, mac, &txlp->tx_list);
				skw_bus_data_xmit(skw, mac, &txlp->tx_list);
				txlp->tx_count_limit = max_tx_count_limit;

				lmac_tx_map |= BIT(mac);

				if (txlp->cred)
					lmac_tx_capa |= BIT(mac);
reset:
				txlp->nr_txq = 0;
				txlp->txq_map = 0;
				txlp->current_qlen = 0;
			}

			if (!lmac_tx_capa)
				break;
		}

		if (ac == SKW_WMM_AC_MAX)
			skw_reset_ac(txl);

		if (pending_qlen == 0) {
			xmit_tx_flag = 0;

			for (ac = 0; ac < SKW_WMM_AC_MAX; ac++) {
				for (i = 0; i < SKW_NR_IFACE; i++) {
					iface = skw->vif.iface[i];
					if (!iface || !iface->ndev)
						continue;

					if (skb_queue_len(&iface->tx_cache[ac]) != 0) {
						xmit_tx_flag = 1;
						goto need_running;
					}

					spin_lock_irqsave(&iface->txq[ac].lock, flags);
					if (skb_queue_len(&iface->txq[ac]) != 0) {
						xmit_tx_flag = 1;
						spin_unlock_irqrestore(&iface->txq[ac].lock, flags);
						goto need_running;

					}
					spin_unlock_irqrestore(&iface->txq[ac].lock, flags);
				}
			}
need_running:

			if (xmit_tx_flag == 0) {
				//skw_start_dev_queue(skw);
				wait_event_interruptible_exclusive(skw->tx_wait_q,
					atomic_xchg(&skw->tx_wake, 0) || atomic_xchg(&skw->exit, 0));
			}
		}
	}

	skw_info("exit");

	return 0;
}

static int __skw_tx_init(struct skw_core *skw)
{
	skw->tx_thread = kthread_create(skw_tx_thread, skw, "skw_tx.%d", skw->idx);
	if (IS_ERR(skw->tx_thread)) {
		skw_err("create tx thread failed\n");
		return  PTR_ERR(skw->tx_thread);
	}

	//kthread_bind(skw->tx_thread, cpumask_last(cpu_online_mask));

	skw_set_thread_priority(skw->tx_thread, SCHED_RR, 1);
	set_user_nice(skw->tx_thread, MIN_NICE);
	wake_up_process(skw->tx_thread);

	return 0;
}

static void __skw_tx_deinit(struct skw_core *skw)
{
	if (skw->tx_thread) {
		atomic_set(&skw->exit, 1);
		kthread_stop(skw->tx_thread);
		skw->tx_thread = NULL;
	}
}

#endif

static int skw_register_tx_callback(struct skw_core *skw, void *func, void *data)
{
	int i, ret = 0;

	for (i = 0; i < SKW_NR_LMAC; i++) {
		if (skw->hw.lmac[i].dport < 0)
			continue;

		skw_dbg("data port: %d func:%p data:%p\n", skw->hw.lmac[i].dport, func, data);
		ret = skw_register_tx_cb(skw, skw->hw.lmac[i].dport, func, data);
		if (ret < 0) {
			skw_err("chip: %d, hw mac: %d, port: %d failed, ret: %d\n",
				skw->idx, i, skw->hw.lmac[i].dport, ret);

			break;
		}
	}

	return ret;
}

int skw_hw_xmit_init(struct skw_core *skw, int dma)
{
	int ret = 0;

	skw_dbg("dma: %d\n", dma);

	switch (dma) {
	case SKW_SYNC_ADMA_TX:
		skw->hw.dat_xmit = skw_sync_adma_tx;
		skw->hw.cmd_xmit = skw_sync_adma_tx;
		break;

	case SKW_SYNC_SDMA_TX:
		skw->hw.dat_xmit = skw_sync_sdma_tx;
		skw->hw.cmd_xmit = skw_sync_sdma_tx;

		if (skw->sdma_buff)
			break;
		skw->sdma_buff = SKW_ALLOC(skw->hw_pdata->max_buffer_size, GFP_KERNEL);
		if (!skw->sdma_buff)
			ret = -ENOMEM;

		break;

	case SKW_ASYNC_ADMA_TX:
		skw->hw.dat_xmit = skw_async_adma_tx;
		skw->hw.cmd_xmit = skw_sync_adma_tx;

		ret = skw_register_tx_callback(skw, skw_async_adma_tx_free, skw);
		break;

	case SKW_ASYNC_SDMA_TX:
		skw->hw.dat_xmit = skw_async_sdma_tx;
		skw->hw.cmd_xmit = skw_sync_sdma_tx;

		ret = skw_register_tx_callback(skw, skw_async_sdma_tx_free, skw);
		break;

	case SKW_ASYNC_EDMA_TX:
		// skw->hw.dat_xmit = skw_async_edma_tx;
		skw->hw.cmd_xmit = skw_sync_adma_tx;
		break;

	default:
		ret = -EINVAL;
		skw->hw.dat_xmit = NULL;
		skw->hw.cmd_xmit = NULL;
		break;
	}

	return ret;
}

int skw_tx_init(struct skw_core *skw)
{
	int ret;

	skw->skb_share_len = SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
	skw->skb_headroom = sizeof(struct skw_tx_desc_hdr) +
			    sizeof(struct skw_tx_desc_conf) +
			    skw->hw.extra.hdr_len +
			    SKW_DATA_ALIGN_SIZE + sizeof(unsigned long);

	if (skw->hw.bus == SKW_BUS_SDIO) {
		skw->eof_blk = SKW_ALLOC(skw->hw.align, GFP_KERNEL);
		if (!skw->eof_blk)
			return -ENOMEM;
	} else if (skw->hw.bus == SKW_BUS_PCIE) {
		skw->skb_headroom = sizeof(struct skw_tx_desc_conf) +
				    SKW_DATA_ALIGN_SIZE;
	}

	ret = skw_hw_xmit_init(skw, skw->hw.dma);
	if (ret < 0) {
		SKW_KFREE(skw->eof_blk);
		return ret;
	}

	ret = __skw_tx_init(skw);
	if (ret < 0) {
		SKW_KFREE(skw->eof_blk);
		SKW_KFREE(skw->sdma_buff);
	}

	tx_wait_time = SKW_TX_WAIT_TIME;
	skw_debugfs_file(skw->dentry, "tx_wait_time", 0666, &skw_tx_time_fops, NULL);

	return ret;
}

int skw_tx_deinit(struct skw_core *skw)
{
	__skw_tx_deinit(skw);

	skw_register_tx_callback(skw, NULL, NULL);

	SKW_KFREE(skw->eof_blk);
	SKW_KFREE(skw->sdma_buff);

	return 0;
}
