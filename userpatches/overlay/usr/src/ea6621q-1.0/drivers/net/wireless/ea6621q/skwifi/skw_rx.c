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

#include <linux/timer.h>
#include <linux/skbuff.h>
#include <linux/kthread.h>
#include <linux/cpumask.h>
#include <linux/ctype.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <net/ip6_checksum.h>

#include "skw_core.h"
#include "skw_msg.h"
#include "skw_cfg80211.h"
#include "skw_rx.h"
#include "skw_work.h"
#include "skw_tx.h"
#include "trace.h"

#define SKW_PN_U48(x)          (*(u64 *)(x) & 0xffffffffffff)
#define SKW_MSDU_HDR_LEN       6 /* ETH_HLEN - SKW_SNAP_HDR_LEN */

static u8 rx_reorder_flag;

static inline void skw_wake_lock_timeout(struct skw_core *skw, long timeout)
{
#ifdef CONFIG_HAS_WAKELOCK
	if (!wake_lock_active(&skw->rx_wlock))
		wake_lock_timeout(&skw->rx_wlock, msecs_to_jiffies(timeout));
#endif
}

static inline void skw_wake_lock_init(struct skw_core *skw, int type, const char *name)
{
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_init(&skw->rx_wlock, type, name);
#endif
}

static inline void skw_wake_lock_deinit(struct skw_core *skw)
{
#ifdef CONFIG_HAS_WAKELOCK
	wake_lock_destroy(&skw->rx_wlock);
#endif
}

static int skw_rx_reorder_show(struct seq_file *seq, void *data)
{
	if (rx_reorder_flag)
		seq_puts(seq, "enable\n");
	else
		seq_puts(seq, "disable\n");

	return 0;
}

static int skw_rx_reorder_open(struct inode *inode, struct file *file)
{
	return single_open(file, skw_rx_reorder_show, inode->i_private);
}

static ssize_t skw_rx_reorder_write(struct file *fp, const char __user *buf,
				size_t len, loff_t *offset)
{
	int i;
	char cmd[32] = {0};

	for (i = 0; i < len; i++) {
		char c;

		if (get_user(c, buf))
			return -EFAULT;

		if (c == '\n' || c == '\0')
			break;

		cmd[i] = tolower(c);
		buf++;
	}

	if (strcmp(cmd, "enable") == 0)
		rx_reorder_flag = true;
	else if (strcmp(cmd, "disable") == 0)
		rx_reorder_flag = false;
	else
		skw_warn("rx_reorder support setting values of \"enable\" or \"disbale\"\n");

	return len;
}

static const struct file_operations skw_rx_reorder_fops = {
	.owner = THIS_MODULE,
	.open = skw_rx_reorder_open,
	.read = seq_read,
	.release = single_release,
	.write = skw_rx_reorder_write,
};

static inline struct skw_reorder_cb *SKW_SKB_RXCB(struct sk_buff *skb)
{
	return (struct skw_reorder_cb *)skb->cb;
}

static inline struct skw_rx_desc *skw_rx_desc_hdr(struct sk_buff *skb)
{
	return (struct skw_rx_desc *)(skb->data - SKW_SKB_RXCB(skb)->rx_desc_offset);
}

/*
 * To verify HW checksum for ipv6
 */
static void skw_csum_verify(struct skw_rx_desc *desc, struct sk_buff *skb)
{
	u16 data_len;
	__sum16 csum;
	unsigned int tcphoff;
	struct iphdr *iph;
	struct ipv6hdr *ip6h;
	struct tcphdr *tcph;
	struct ethhdr *eth = eth_hdr(skb);

	if (!skb->csum)
		return;

	switch (eth->h_proto) {
	case htons(ETH_P_IPV6):
		ip6h = (struct ipv6hdr *)(skb->data);
		tcphoff = sizeof(struct ipv6hdr);
		tcph = (struct tcphdr *)(skb->data + tcphoff);

		// fixme:
		// minus the length of any extension headers present between the IPv6
		// header and the upper-layer header
		data_len = ntohs(ip6h->payload_len);

		if (skb->len != data_len + tcphoff) {
			skw_detail("ipv6 dummy pending: rx len: %d, tot_len: %d",
				   skb->len, data_len);

			skb->csum = csum_partial(skb->data + tcphoff,
						data_len, 0);

			skb_trim(skb, data_len + tcphoff);
		}

		csum = csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, data_len,
					ip6h->nexthdr, skb->csum);
		if (csum) {
			skw_detail("sa: %pI6, da: %pI6, proto: 0x%x, seq: %d, csum: 0x%x, result: 0x%x\n",
				&ip6h->saddr, &ip6h->daddr, ip6h->nexthdr,
				desc->sn, skb->csum, csum);

			skw_hex_dump("csum failed", skb->data, skb->len, false);

		} else {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		}

		break;

	case htons(ETH_P_IP):
		iph = (struct iphdr *)(skb->data);
		tcphoff = iph->ihl * 4;
		tcph = (struct tcphdr *)(skb->data + tcphoff);

		data_len = ntohs(iph->tot_len);

		if (skb->len != data_len) {
			skw_detail("ipv4 dummy pending: rx len: %d, tot_len: %d",
				   skb->len, data_len);

			skb->csum = csum_partial(skb->data + tcphoff,
					data_len - tcphoff, 0);

			skb_trim(skb, data_len);
		}

		csum = csum_tcpudp_magic(iph->saddr, iph->daddr,
					data_len - tcphoff,
					iph->protocol, skb->csum);
		if (csum) {
			skw_detail("sa: %pI4, da: %pI4, proto: 0x%x, seq: %d, csum: 0x%x, result: 0x%x\n",
				&iph->saddr, &iph->daddr, iph->protocol,
				desc->sn, skb->csum, csum);

			skw_hex_dump("csum failed", skb->data, skb->len, false);
		}

		break;

	default:
		break;
	}
}

#ifdef CONFIG_RPS
int skw_init_rps_map(struct netdev_rx_queue *queue, int unmask)
{
	int i, cpu;
	struct rps_map *map, *old_map;
	static DEFINE_SPINLOCK(rps_map_lock);

	map = kzalloc(max_t(unsigned int,
			    RPS_MAP_SIZE(cpumask_weight(cpu_online_mask)), L1_CACHE_BYTES),
		      GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	i = 0;
	for_each_cpu(cpu, cpu_online_mask)
		if (cpu != unmask)
			map->cpus[i++] = cpu;

	if (i) {
		map->len = i;
	} else {
		kfree(map);
		map = NULL;
	}

	spin_lock(&rps_map_lock);
	old_map = rcu_dereference_protected(queue->rps_map,
					    lockdep_is_held(&rps_map_lock));
	rcu_assign_pointer(queue->rps_map, map);
	spin_unlock(&rps_map_lock);

	if (map) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0))
		static_key_slow_inc(&rps_needed.key);
#else
		static_key_slow_inc(&rps_needed);
#endif
	}

	if (old_map) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0))
		static_key_slow_dec(&rps_needed.key);
#else
		static_key_slow_dec(&rps_needed);
#endif
		kfree_rcu(old_map, rcu);
	}

	return 0;
}
#endif

static void skw_deliver_skb(struct skw_iface *iface, struct sk_buff *skb)
{
	struct sk_buff *tx_skb = NULL;
	struct ethhdr *eth = (struct ethhdr *)skb->data;
	struct skw_rx_desc *desc = skw_rx_desc_hdr(skb);
	int mcast;
	unsigned int len = skb->len;
	int ret = NET_RX_DROP;

	if (ether_addr_equal(eth->h_source, iface->addr))
		goto stats;

	if (unlikely(!desc->snap_match)) {
		skw_detail("snap unmatch, sn: %d\n", desc->sn);

		skw_snap_unmatch_handler(skb);
	}

	/* forward for ap mode */
	mcast = is_multicast_ether_addr(skb->data);
	if (desc->need_forward && skw_is_ap_mode(iface)) {
		if (mcast) {
			tx_skb = skb_copy(skb, GFP_ATOMIC);
		} else if (skw_peer_ctx(iface, skb->data) != NULL) {
			tx_skb = skb;
			skb = NULL;
		}

		if (tx_skb) {
			tx_skb->priority += 256;
			tx_skb->protocol = htons(ETH_P_802_3);
			skb_reset_network_header(tx_skb);
			skb_reset_mac_header(tx_skb);
			dev_queue_xmit(tx_skb);
		}

		if (!skb) {
			ret = NET_RX_SUCCESS;
			goto stats;
			//return;
		}
	}

#ifdef SKW_ARP_NAT
	if (eth->h_proto == ntohs(ETH_P_ARP) &&
	    skw_is_sta_mode(iface) &&
	    iface->ndev->priv_flags & IFF_BRIDGE_PORT) {
		struct skw_arphdr *arp = skw_arp_hdr(skb);

		if (arp->ar_op == ntohs(ARPOP_REPLY)) {
			int i;
			bool found = false;
			struct skw_peer_ctx *ctx;

			for (i = 0; i < SKW_MAX_PEER_SUPPORT; i++) {
				ctx = &iface->skw->peer_ctx[i];

				mutex_lock(&ctx->lock);

				if (ctx->peer
					&& ctx->peer->ip_addr == arp->ar_tip) {
					ether_addr_copy(eth->h_dest,
							ctx->peer->addr);
					ether_addr_copy(arp->ar_tha,
							ctx->peer->addr);
					found = true;
				}

				mutex_unlock(&ctx->lock);

				if (found)
					break;
			}
		}
	}
#endif

	skb->protocol = eth_type_trans(skb, iface->ndev);
	// TODO:
	// ipv6 csum check
	if (desc->csum_valid) {
		skb->csum = desc->csum;
		skb->ip_summed = CHECKSUM_COMPLETE;
		skw_csum_verify(desc, skb);
	} else {
		skb->csum = 0;
		skb->ip_summed = CHECKSUM_NONE;
	}

	ret = NET_RX_SUCCESS;


#ifdef CONFIG_RPS
	if (iface->cpu_id != smp_processor_id()) {
		iface->cpu_id = smp_processor_id();
		skw_init_rps_map(iface->ndev->_rx, iface->cpu_id);
	}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
	netif_rx_ni(skb);
#else
	netif_rx(skb);
#endif

stats:
	if (unlikely(ret == NET_RX_DROP)) {
		iface->ndev->stats.rx_dropped++;
		dev_kfree_skb(skb);
	} else {
		iface->ndev->stats.rx_packets++;
		iface->ndev->stats.rx_bytes += len;
		if (mcast)
			iface->ndev->stats.multicast++;
	}
}

/*
 * get fragment entry
 * @tid & @sn as fragment entry match id
 * @active, if false, check duplicat first, then get an inactive fragment,
 *          else return the oldest active entry
 */
static struct skw_frag_entry *
skw_frag_get_entry(struct skw_iface *iface, u8 tid, u16 sn, bool active)
{
	int i;
	struct skw_frag_entry *entry = NULL, *oldest = NULL;
	struct skw_frag_entry *inactive = NULL;

	for (i = 0; i < SKW_MAX_DEFRAG_ENTRY; i++) {
		struct skw_frag_entry *e = &iface->frag[i];

		if (e->sn == sn && e->tid == tid) {
			entry = e;
			break;
		}

		if (!active) {

			// skw_dbg("i: %d,entry tid: %d, sn: %d, status: %d\n",
			//        i, e->tid, e->sn, e->status);

			if (!(e->status & SKW_FRAG_STATUS_ACTIVE)) {
				inactive = e;
				continue;
			}

			if (!oldest) {
				oldest = e;
				continue;
			}

			if (time_after(oldest->start, e->start))
				oldest = e;
		}
	}

	if (!active && !entry)
		entry = inactive ? inactive : oldest;

	return entry;
}
// Firmware will cover the exception that receiving a fragment
// frame while in a ba session
// Firmware will split A-MSDU frame to MSDU to Wi-Fi driver

static void skw_frag_init_entry(struct skw_iface *iface, struct sk_buff *skb)
{
	struct skw_frag_entry *entry = NULL;
	struct skw_rx_desc *desc = skw_rx_desc_hdr(skb);

	entry = skw_frag_get_entry(iface, desc->tid, desc->sn, false);
	if (entry->status & SKW_FRAG_STATUS_ACTIVE) {
		skw_warn("overwrite, entry: %d, tid: %d, sn: %d, time: %d ms\n",
			 entry->id, entry->tid, entry->sn,
			 jiffies_to_msecs(jiffies - entry->start));
	}

	if (!skb_queue_empty(&entry->skb_list))
		__skb_queue_purge(&entry->skb_list);

	entry->status = SKW_FRAG_STATUS_ACTIVE;
	entry->pending_len = 0;
	entry->start = jiffies;
	entry->tid = desc->tid;
	entry->sn = desc->sn;
	entry->frag_num = 0;

	if (iface->key_conf.skw_cipher == SKW_CIPHER_SUITE_CCMP ||
	    iface->key_conf.skw_cipher == SKW_CIPHER_SUITE_CCMP_256 ||
	    iface->key_conf.skw_cipher == SKW_CIPHER_SUITE_GCMP ||
	    iface->key_conf.skw_cipher == SKW_CIPHER_SUITE_GCMP_256) {
		memcpy(entry->last_pn, desc->pn, IEEE80211_CCMP_PN_LEN);
		SKW_SET(entry->status, SKW_FRAG_STATUS_CHK_PN);
	}

	__skb_queue_tail(&entry->skb_list, skb);
}

/*
 * if @skb is a fragment frame, start to defragment.
 * return skb buffer if all fragment frames have received, else return NULL
 */
static struct sk_buff *
skw_rx_defragment(struct skw_core *skw, struct skw_iface *iface,
				struct sk_buff *skb)
{
	struct sk_buff *pskb;
	struct skw_frag_entry *entry;
	struct skw_rx_desc *desc = skw_rx_desc_hdr(skb);

	if (likely(!desc->more_frag && !desc->frag_num))
		return skb;

	//skw_dbg("peer: %d, tid: %d, sn: %d, more frag: %d, frag num: %d\n",
	//	desc->peer_idx, desc->tid, desc->sn,
	//	desc->more_frag, desc->frag_num);


	if (desc->frag_num == 0) {
		desc->csum_valid = 0;
		desc->csum = 0;
		skw_frag_init_entry(iface, skb);

		return NULL;
	}

	entry = skw_frag_get_entry(iface, desc->tid, desc->sn, true);
	if (!entry || (entry->frag_num + 1 != desc->frag_num)) {
		//TBD: the frag num increased by 2 when it is WAPI
		skw_dbg("drop, entry: %d, tid: %d, sn: %d, frag num: %d\n",
			entry ? entry->id : -1, desc->tid,
			desc->sn, desc->frag_num);

		dev_kfree_skb(skb);
		return NULL;
	}

	/* check fragment frame PN if cipher is CCMP
	 * The PN shall be incremented in steps of 1 for constituent
	 * MPDUs of fragmented MSDUs and MMPDUs
	 */
	if (entry->status & SKW_FRAG_STATUS_CHK_PN) {
		if (SKW_PN_U48(entry->last_pn) + 1 != SKW_PN_U48(desc->pn)) {
			skw_dbg("drop frame last pn:%llu desc_pn:%llu\n",
				SKW_PN_U48(entry->last_pn),
				SKW_PN_U48(desc->pn));
			dev_kfree_skb(skb);
			return NULL;
		}

		memcpy(entry->last_pn, desc->pn, IEEE80211_CCMP_PN_LEN);
	}

	entry->frag_num++;

	/* remove mac address header -- SA & DA */
	skb_pull(skb, 12);

	entry->pending_len += skb->len;

	__skb_queue_tail(&entry->skb_list, skb);

	if (desc->more_frag)
		return NULL;

	pskb = __skb_dequeue(&entry->skb_list);
	if (skb_tailroom(pskb) < entry->pending_len) {
		if (unlikely(pskb_expand_head(pskb, 0, entry->pending_len,
						GFP_ATOMIC))) {

			skw_warn("drop: tailroom: %d, needed: %d\n",
				 skb_tailroom(pskb), entry->pending_len);

			__skb_queue_purge(&entry->skb_list);
			dev_kfree_skb(pskb);
			entry->status = 0;
			entry->tid = SKW_INVALID_ID;

			return NULL;
		}
	}

	while ((skb = __skb_dequeue(&entry->skb_list))) {
		/* snap unmatch */
		skw_put_skb_data(pskb, skb->data, skb->len);
		dev_kfree_skb(skb);
	}

	entry->status = 0;
	entry->tid = SKW_INVALID_ID;

	// Remove the mic value in the final fragment when encryption is TKIP
	if (iface->key_conf.skw_cipher == SKW_CIPHER_TYPE_TKIP)
		skb_trim(pskb, pskb->len - 8);

	return pskb;
}

static int skw_pn_allowed(struct skw_key *key, struct skw_rx_desc *desc, int queue)
{
	int ret;

	if (!key)
		return -EINVAL;

	ret = SKW_PN_U48(desc->pn) - SKW_PN_U48(key->rx_pn[queue]);
	if (ret < 0 || (!ret && !desc->is_amsdu && SKW_PN_U48(desc->pn) != 0)) {
		/* SKW_PN_U48(desc->pn) = 0 allow workaround some devices pn=0*/

		/* failed that PN less than or equal to rx_pn */
		skw_warn("seq: %d, pn: %llu, rx pn: %llu\n",
			 desc->sn, SKW_PN_U48(key->rx_pn[queue]),
			 SKW_PN_U48(desc->pn));

		return -EINVAL;
	}

	return 0;
}

static int skw_replay_detect(struct skw_core *skw, struct skw_iface *iface,
			struct skw_rx_desc *desc)
{
	int64_t ret = 0;
	int key_idx, queue = -1;
	struct skw_key *key;
	struct skw_key_conf *conf;
	struct skw_peer_ctx *ctx;

	// fixme:
	ctx = &skw->peer_ctx[desc->peer_idx];
	if (!ctx->peer)
		return -EINVAL;

	if (desc->is_mc_addr) {
		conf = &iface->key_conf;
		if (!conf->installed_bitmap)
			conf = &ctx->peer->gtk_conf;
	} else {
		conf = &ctx->peer->ptk_conf;
	}

	key_idx = skw_key_idx(conf->installed_bitmap);
	if (key_idx == SKW_INVALID_ID)
		return 0;

	switch (iface->key_conf.skw_cipher) {
	case SKW_CIPHER_TYPE_CCMP:
	case SKW_CIPHER_TYPE_CCMP_256:
	case SKW_CIPHER_TYPE_GCMP:
	case SKW_CIPHER_TYPE_GCMP_256:
		queue = desc->tid;
		break;

	case SKW_CIPHER_TYPE_TKIP:
	case SKW_CIPHER_TYPE_AES_CMAC:
	case SKW_CIPHER_TYPE_BIP_CMAC_256:
	case SKW_CIPHER_TYPE_BIP_GMAC_128:
	case SKW_CIPHER_TYPE_BIP_GMAC_256:
		queue = 0;
		break;

	default:
		queue = -1;
		break;
	}

	if (queue < 0)
		return 0;

	rcu_read_lock();

	key = rcu_dereference(conf->key[key_idx]);
	ret = skw_pn_allowed(key, desc, queue);
	if (!ret)
		memcpy(key->rx_pn[queue], desc->pn, SKW_PN_LEN);

	rcu_read_unlock();

	return ret;
}

static void skw_rx_handler(struct skw_core *skw, struct sk_buff_head *list)
{
	struct sk_buff *skb;
	struct skw_iface *iface;
	struct skw_rx_desc *desc;
	struct sk_buff_head deliver_list;

	__skb_queue_head_init(&deliver_list);

	spin_lock_bh(&skw->rx_lock);

	while ((skb = __skb_dequeue(list))) {
		if (SKW_SKB_RXCB(skb)->skw_created) {
			dev_kfree_skb(skb);
			continue;
		}

		desc = skw_rx_desc_hdr(skb);

		trace_skw_rx_handler_seq(desc->sn, desc->msdu_filter);

		iface = to_skw_iface(skw, desc->inst_id);
		if (iface == NULL) {
			dev_kfree_skb(skb);
			continue;
		}

		if (skw_replay_detect(skw, iface, desc) < 0) {
			dev_kfree_skb(skb);
			continue;
		}

		skb = skw_rx_defragment(skw, iface, skb);
		if (!skb)
			continue;

		skb->dev = iface->ndev;
		__skb_queue_tail(&deliver_list, skb);
	}

	spin_unlock_bh(&skw->rx_lock);

	while ((skb = __skb_dequeue(&deliver_list)))
		skw_deliver_skb(netdev_priv(skb->dev), skb);
}

static void skw_set_reorder_timer(struct skw_tid_rx *tid_rx, u16 sn)
{
	u16 index;
	struct sk_buff_head *list;
	unsigned long timeout = 0;
	struct skw_reorder_rx *reorder = tid_rx->reorder;

	smp_rmb();

	if (timer_pending(&reorder->timer) ||
	    atomic_read(&reorder->ref_cnt) != tid_rx->ref_cnt)
		return;

	index = sn % tid_rx->win_size;
	list = &tid_rx->reorder_buf[index];
	if (!list || skb_queue_empty(list)) {
		//skw_warn("invalid rx list, sn: %d\n", sn);
		return;
	}

	timeout = SKW_SKB_RXCB(skb_peek(list))->rx_time +
		  msecs_to_jiffies(CONFIG_SKW_RX_REORDER_TIMEOUT);

	trace_skw_rx_set_reorder_timer(reorder->inst, reorder->peer_idx,
				reorder->tid, sn, jiffies, timeout);

	if (time_before(jiffies, timeout)) {
		reorder->expired.sn = sn;
		reorder->expired.ref_cnt = tid_rx->ref_cnt;
		mod_timer(&reorder->timer, timeout);
	} else {

		spin_lock_bh(&reorder->todo.lock);

		if (!reorder->todo.actived) {
			reorder->todo.seq = sn;
			reorder->todo.actived = true;
			reorder->todo.reason = SKW_RELEASE_EXPIRED;
			skw_list_add(&reorder->skw->rx_todo_list, &reorder->todo.list);

		}

		spin_unlock_bh(&reorder->todo.lock);

		skw_wakeup_rx(reorder->skw);
	}
}

static inline bool skw_is_release_ready(struct sk_buff_head *list)
{
	struct sk_buff *skb = skb_peek(list);
	struct skw_reorder_cb *cb = NULL;

	if (!skb)
		return false;

	cb = SKW_SKB_RXCB(skb);
	if ((cb->amsdu_flags & SKW_AMSDU_FLAG_VALID) &&
	    (cb->amsdu_bitmap != cb->amsdu_mask))
		return false;

	return true;
}

static inline bool skw_is_msdu_timeout(struct sk_buff_head *list)
{
	struct sk_buff *skb;
	unsigned long timeout = 0;

	skb = skb_peek(list);
	if (skb) {
		timeout = SKW_SKB_RXCB(skb)->rx_time + CONFIG_SKW_RX_REORDER_TIMEOUT;
		if (time_after(jiffies, timeout))
			return true;
	}

	return false;
}

/* Force release frame in reorder buffer to to_sn*/
static void skw_reorder_force_release(struct skw_tid_rx *tid_rx,
		u16 to_sn, struct sk_buff_head *release_list, int reason)
{
	u16 index, target;
	struct sk_buff *skb, *pskb;

	if (!tid_rx)
		return;

	target = ieee80211_sn_inc(to_sn);

	smp_rmb();

	if (timer_pending(&tid_rx->reorder->timer) &&
	    atomic_read(&tid_rx->reorder->ref_cnt) == tid_rx->ref_cnt &&
	    (ieee80211_sn_less(tid_rx->reorder->expired.sn, to_sn) ||
	     ieee80211_sn_less(to_sn, tid_rx->win_start)))
		del_timer(&tid_rx->reorder->timer);

	while (ieee80211_sn_less(tid_rx->win_start, target)) {
		struct sk_buff_head *list;

		index = tid_rx->win_start % tid_rx->win_size;
		list = &tid_rx->reorder_buf[index];

		if (!tid_rx->stored_num) {
			tid_rx->win_start = to_sn;
			break;
		}

		skb = skb_peek(list);
		if (skb) {
			if (!skw_is_release_ready(list)) {
				skw_dbg("warn, seq: %d, amsdu bitmap: 0x%x\n",
					skw_rx_desc_hdr(skb)->sn,
					SKW_SKB_RXCB(skb)->amsdu_bitmap);
			}

			if (SKW_SKB_RXCB(skb)->amsdu_flags
					& SKW_AMSDU_FLAG_TAINT) {
				__skb_queue_purge(list);
			} else {
				while ((pskb = __skb_dequeue(list)))
					__skb_queue_tail(release_list, pskb);
			}

			tid_rx->stored_num--;
		}

		WARN_ON(!skb_queue_empty(list));

		tid_rx->win_start = ieee80211_sn_inc(tid_rx->win_start);

		trace_skw_rx_force_release(tid_rx->reorder->inst,
					tid_rx->reorder->peer_idx,
					tid_rx->reorder->tid,
					index, tid_rx->win_start, target,
					tid_rx->stored_num, reason);
	}
}

/*
 * release all ready skb in reorder buffer until a gap
 * if first ready skb is timeout, release all skb in reorder buffer,
 * else reset timer
 */
static void skw_reorder_release(struct skw_reorder_rx *reorder,
			struct sk_buff_head *release_list)
{
	bool release = true;

	u16 i, index;
	u16 win_start;
	struct sk_buff *skb;
	struct sk_buff_head *list;
	struct skw_tid_rx *tid_rx;

	tid_rx = rcu_dereference(reorder->tid_rx);
	if (!tid_rx)
		return;

	win_start = tid_rx->win_start;

	for (i = 0; i < tid_rx->win_size; i++) {
		if (tid_rx->stored_num == 0) {
			if (timer_pending(&reorder->timer))
				del_timer(&reorder->timer);

			break;
		}

		index = (win_start + i) % tid_rx->win_size;
		list = &tid_rx->reorder_buf[index];

		if (!skb_queue_len(list)) {
			if (timer_pending(&reorder->timer))
				break;

			release = false;
			continue;
		}

		/* release timeout skb and reset reorder timer */
		if (!release) {
			if (!skw_is_msdu_timeout(list)) {
				skw_set_reorder_timer(tid_rx, win_start + i);
				break;
			}

			skw_reorder_force_release(tid_rx, win_start + i,
					release_list, SKW_RELEASE_EXPIRED);
			release = true;
			continue;
		}

		if (release) {
			skb = skb_peek(list);

			if (timer_pending(&reorder->timer) &&
			    reorder->expired.sn == tid_rx->win_start)
				del_timer(&reorder->timer);

			if (SKW_SKB_RXCB(skb)->amsdu_flags & SKW_AMSDU_FLAG_TAINT) {
				__skb_queue_purge(list);
				release = false;
				continue;
			}

			if (skw_is_release_ready(list) || skw_is_msdu_timeout(list)) {
				struct sk_buff *pskb;

				while ((pskb = __skb_dequeue(list)))
					__skb_queue_tail(release_list, pskb);

				tid_rx->win_start = ieee80211_sn_inc(tid_rx->win_start);
				tid_rx->stored_num--;

				trace_skw_rx_reorder_release(reorder->inst,
						reorder->peer_idx, reorder->tid,
						win_start, win_start + i,
						index, tid_rx->win_start,
						tid_rx->stored_num);

			} else {
				/* AMSDU not ready and expired */
				if (!timer_pending(&reorder->timer))
					skw_set_reorder_timer(tid_rx, win_start + i);

				break;
			}
		}
	}
}

static void skw_ampdu_reorder(struct skw_core *skw, struct skw_rx_desc *desc,
			struct sk_buff *skb, struct sk_buff_head *release_list)
{
	u32 filter;
	u16 win_start, win_size;
	struct skw_ctx_entry *entry;
	struct skw_tid_rx *tid_rx;
	struct sk_buff_head *list = NULL;
	struct sk_buff *pskb;
	struct skw_peer *peer;
	struct skw_reorder_rx *reorder;
	bool release = false, drop = false;
	const u8 snap_hdr[] = {0xAA, 0xAA, 0x03, 0x0, 0x0, 0x0};

#define SKW_RXCB_AMSDU_LAST    BIT(0)

	if (!rx_reorder_flag) {
		__skb_queue_tail(release_list, skb);
		return;
	}

	entry = rcu_dereference(skw->peer_ctx[desc->peer_idx].entry);
	if (!entry) {
		__skb_queue_tail(release_list, skb);
		return;
	}

	peer = entry->peer;
	filter = atomic_read(&peer->rx_filter);
	if (filter && !(filter & BIT(desc->msdu_filter & 0x1F))) {
		skw_dbg("warn: rx filter: 0x%x, msdu filter: 0x%x\n",
			filter, desc->msdu_filter);

		kfree_skb(skb);
		return;
	}


	skw_update_peer_rx_rate(peer, desc);

	entry->peer->rx.bytes += skb->len;
	entry->peer->rx.pkts++;

	if (!desc->is_qos_data || desc->is_mc_addr) {
		__skb_queue_tail(release_list, skb);
		return;
	}

	/* if this mpdu is fragmented, skip reorder */
	if (desc->more_frag || desc->frag_num) {
		__skb_queue_tail(release_list, skb);
		return;
	}

	reorder = &peer->reorder[desc->tid];
	tid_rx = rcu_dereference(reorder->tid_rx);
	if (!tid_rx) {
		__skb_queue_tail(release_list, skb);
		return;
	}

	win_start = tid_rx->win_start;
	win_size = tid_rx->win_size;
	/* case:
	 * frame seqence number less than window start
	 */
	if (ieee80211_sn_less(desc->sn, win_start)) {
		skw_detail("drop: peer: %d, tid: %d, ssn: %d, seq: %d, amsdu idx: %d, filter: %d\n",
			   desc->peer_idx, desc->tid, win_start,
			   desc->sn, desc->amsdu_idx, desc->msdu_filter);

		drop = true;
		goto out;
	}

	/* case:
	 * frame sequence number exceeds window size
	 */
	if (!ieee80211_sn_less(desc->sn, win_start + win_size)) {
		win_start = ieee80211_sn_sub(desc->sn, win_size);

		skw_reorder_force_release(tid_rx, win_start, release_list,
						SKW_RELEASE_OOB);
		release = true;
		win_start = tid_rx->win_start;
	}

	/* dup check
	 */
	// index = desc->sn % win_size;
	list = &tid_rx->reorder_buf[desc->sn % win_size];
	pskb = skb_peek(list);

	if (desc->is_amsdu) {
		struct skw_reorder_cb *cb;

		if (!pskb) {
			pskb = skb;
			tid_rx->stored_num++;
		}

		cb = SKW_SKB_RXCB(pskb);
		if (cb->amsdu_bitmap & BIT(desc->amsdu_idx)) {
			drop = true;
			goto out;
		}

		cb->amsdu_bitmap |= BIT(desc->amsdu_idx);
		cb->amsdu_flags |= SKW_AMSDU_FLAG_VALID;
		__skb_queue_tail(list, skb);

		if (desc->amsdu_first_idx &&
		    ether_addr_equal(skb->data, snap_hdr)) {
			cb->amsdu_flags |= SKW_AMSDU_FLAG_TAINT;
			skw_hex_dump("attack", skb->data, 14, true);
		}

		if (desc->amsdu_last_idx) {
			cb->amsdu_mask = BIT(desc->amsdu_idx + 1) - 1;
			cb->amsdu_bitmap |= SKW_RXCB_AMSDU_LAST;
		}

		if (cb->amsdu_bitmap != cb->amsdu_mask)
			goto out;

		/* amsdu ready to release */
		tid_rx->stored_num--;

		if (cb->amsdu_flags & SKW_AMSDU_FLAG_TAINT) {
			__skb_queue_purge(list);
			tid_rx->win_start = ieee80211_sn_inc(tid_rx->win_start);
			drop = true;
			skb = NULL;

			goto out;
		}

	} else {
		if (pskb) {
			drop = true;
			goto out;
		}

		__skb_queue_tail(list, skb);
	}

	if (desc->sn == win_start) {
		while ((pskb = __skb_dequeue(list)))
			__skb_queue_tail(release_list, pskb);

		if (timer_pending(&reorder->timer) &&
			reorder->expired.sn == tid_rx->win_start)
			del_timer(&reorder->timer);

		tid_rx->win_start = ieee80211_sn_inc(tid_rx->win_start);

		release = true;

	} else {
		tid_rx->stored_num++;
	}

out:
	trace_skw_rx_reorder(desc->inst_id, desc->peer_idx, desc->tid,
			     desc->sn, desc->is_amsdu, desc->amsdu_idx,
			     tid_rx->win_size, tid_rx->win_start,
			     tid_rx->stored_num, release, drop);

	if (drop && skb)
		dev_kfree_skb(skb);

	if (tid_rx->stored_num) {
		if (release)
			skw_reorder_release(reorder, release_list);
		else
			skw_set_reorder_timer(tid_rx, desc->sn);
	} else {
		if (timer_pending(&reorder->timer))
			del_timer(&reorder->timer);
	}
}

static void skw_rx_todo(struct skw_list *todo_list)
{
	// u16 target;
	LIST_HEAD(list);
	struct sk_buff_head release;
	struct skw_reorder_rx *reorder;
	struct skw_tid_rx *tid_rx;

	if (likely(!todo_list->count))
		return;

	INIT_LIST_HEAD(&list);
	__skb_queue_head_init(&release);

	spin_lock_bh(&todo_list->lock);

	list_splice_init(&todo_list->list, &list);
	todo_list->count = 0;

	spin_unlock_bh(&todo_list->lock);

	rcu_read_lock();

	while (!list_empty(&list)) {
		reorder = list_first_entry(&list, struct skw_reorder_rx,
					   todo.list);

		spin_lock_bh(&reorder->todo.lock);

		list_del(&reorder->todo.list);

		tid_rx = rcu_dereference(reorder->tid_rx);
		skw_reorder_force_release(tid_rx, reorder->todo.seq,
					&release, reorder->todo.reason);

		reorder->todo.actived = false;

		spin_unlock_bh(&reorder->todo.lock);

		skw_reorder_release(reorder, &release);
		skw_rx_handler(reorder->skw, &release);

		trace_skw_rx_expired_release(reorder->inst, reorder->peer_idx,
					reorder->tid, reorder->todo.seq);
	}

	rcu_read_unlock();
}

static void skw_rx_handler_drop_info(struct skw_core *skw, void *buff,
			int buff_len, struct sk_buff_head *release_list)
{
	int i;
	int total_drop_sn;
	struct sk_buff *skb;
	struct skw_rx_desc *new_desc;
	struct skw_drop_sn_info *sn_info;
	static unsigned long j;

	total_drop_sn = *(int *)buff;
	if (total_drop_sn > buff_len / sizeof(*sn_info)) {
		if (printk_timed_ratelimit(&j, 5000))
			skw_hex_dump("dump", buff, buff_len, true);

		//skw_hw_assert(skw);
		return;
	}

	sn_info = (struct skw_drop_sn_info *)(buff + 4);
	for (i = 0; i < total_drop_sn; i++) {
		trace_skw_rx_data(sn_info[i].inst, sn_info[i].peer_idx,
				  sn_info[i].tid, 0,
				  sn_info[i].sn, sn_info[i].qos,
				  0, sn_info[i].is_amsdu,
				  sn_info[i].amsdu_idx, sn_info[i].amsdu_first,
				  sn_info[i].amsdu_last, true);

		if (!sn_info[i].qos)
			continue;

		skb = dev_alloc_skb(sizeof(struct skw_rx_desc));
		if (skb) {
			SKW_SKB_RXCB(skb)->skw_created = 1;
			SKW_SKB_RXCB(skb)->rx_time = jiffies;

			new_desc = skw_put_skb_zero(skb, sizeof(struct skw_rx_desc));
			new_desc->inst_id = sn_info[i].inst;
			new_desc->peer_idx = sn_info[i].peer_idx;
			new_desc->tid = sn_info[i].tid;
			new_desc->is_qos_data = sn_info[i].qos;
			new_desc->sn = sn_info[i].sn;
			new_desc->is_amsdu = sn_info[i].is_amsdu;
			new_desc->amsdu_idx = sn_info[i].amsdu_idx;
			new_desc->amsdu_first_idx = sn_info[i].amsdu_first;
			new_desc->amsdu_last_idx = sn_info[i].amsdu_last;

			rcu_read_lock();
			skw_ampdu_reorder(skw, new_desc, skb, release_list);
			rcu_read_unlock();

			skw_rx_handler(skw, release_list);
		}
	}
}

static void skw_netif_monitor_rx(struct skw_core *skw, struct sk_buff *skb)
{
	struct skw_iface *iface;
	struct skw_rx_desc *desc;

	desc = (struct skw_rx_desc *) skb->data;
	if (unlikely(!desc->msdu_len)) {
		skw_detail("strip invalid pakcet\n");
		kfree_skb(skb);
		return;
	}

	skb_pull(skb, sizeof(struct skw_rx_desc));
	iface = to_skw_iface(skw, desc->inst_id);
	if (unlikely(!iface)) {
		skw_err("iface not valid\n");
		kfree_skb(skb);

		return;
	}

	__skb_trim(skb, desc->msdu_len);

	skb->dev = iface->ndev;
	skb_reset_mac_header(skb);
	skb->ip_summed = CHECKSUM_NONE;
	skb->pkt_type = PACKET_OTHERHOST;
	skb->protocol = htons(ETH_P_80211_RAW);
	netif_receive_skb(skb);
}

static void skw_rx_data_handler(struct skw_core *skw,
				struct sk_buff_head *rx_list)
{
	struct sk_buff_head release_list;
	struct skw_rx_desc *desc;
	struct sk_buff *skb;

	__skb_queue_head_init(&release_list);

	while ((skb = __skb_dequeue(rx_list))) {
		int msdu_offset = 0, msdu_len = 0;

		desc = (struct skw_rx_desc *)skb_pull(skb, skw->hw.rx_desc.hdr_offset);

		if (skw_is_monitor_data(skw, skb->data)) {
			skw_netif_monitor_rx(skw, skb);
			continue;
		}

		trace_skw_rx_data(desc->inst_id, desc->peer_idx, desc->tid,
				  desc->msdu_filter, desc->sn, desc->is_qos_data,
				  desc->retry_frame, desc->is_amsdu,
				  desc->amsdu_idx, desc->amsdu_first_idx,
				  desc->amsdu_last_idx, false);

		if (desc->peer_idx >= SKW_MAX_PEER_SUPPORT ||
		    desc->tid >= SKW_NR_TID) {
			skw_warn("invlid peer: %d, tid: %d\n",
				desc->peer_idx, desc->tid);

			kfree_skb(skb);
			continue;
		}

		if (BIT(desc->msdu_filter & 0x1f) & SKW_RX_FILTER_DBG)
			skw_dbg("filter: %d, sn: %d, sa: %pM\n",
				desc->msdu_filter, desc->sn,
				skw_eth_hdr(skb)->h_source);

		msdu_offset = desc->msdu_offset -
			      skw->hw.rx_desc.msdu_offset -
			      skw->hw.rx_desc.hdr_offset;

		skb_pull(skb, msdu_offset);
		SKW_SKB_RXCB(skb)->rx_desc_offset = msdu_offset;
		SKW_SKB_RXCB(skb)->rx_time = jiffies;

		msdu_len = desc->msdu_len + SKW_MSDU_HDR_LEN;

		if (desc->mac_drop_frag) {
			int offset = round_up(msdu_len + desc->msdu_offset, 4);

			offset -= desc->msdu_offset;
			skw_rx_handler_drop_info(skw, skb->data + offset,
					skb->len - offset, &release_list);
		}

		__skb_trim(skb, msdu_len);

		rcu_read_lock();

		skw_ampdu_reorder(skw, desc, skb, &release_list);

		rcu_read_unlock();

		skw_rx_handler(skw, &release_list);

		skw_rx_todo(&skw->rx_todo_list);
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
static void skw_reorder_timeout(struct timer_list *timer)
#else
static void skw_reorder_timeout(unsigned long timer)
#endif
{
	struct skw_reorder_rx *reorder;

	reorder = container_of((void *)timer, struct skw_reorder_rx, timer);

	skw_dbg("tid: %d, expired sn: %d\n", reorder->tid, reorder->expired.sn);

	if (atomic_read(&reorder->ref_cnt) != reorder->expired.ref_cnt)
		return;

	trace_skw_rx_reorder_timeout(reorder->inst, reorder->peer_idx,
				reorder->tid, reorder->expired.sn);

	spin_lock_bh(&reorder->todo.lock);

	if (!reorder->todo.actived) {
		reorder->todo.actived = true;
		reorder->todo.seq = reorder->expired.sn;
		reorder->todo.reason = SKW_RELEASE_EXPIRED;
		INIT_LIST_HEAD(&reorder->todo.list);

		skw_list_add(&reorder->skw->rx_todo_list, &reorder->todo.list);
	}

	spin_unlock_bh(&reorder->todo.lock);

	skw_wakeup_rx(reorder->skw);
}

static void skw_free_tid_rx(struct rcu_head *head)
{
	u16 win_end;
	struct skw_tid_rx *tid_rx;
	struct sk_buff_head release_list;

	tid_rx = container_of(head, struct skw_tid_rx, rcu_head);

	__skb_queue_head_init(&release_list);
	win_end = ieee80211_sn_add(tid_rx->win_start, tid_rx->win_size - 1);

	rcu_read_lock();

	skw_reorder_force_release(tid_rx, win_end, &release_list,
					SKW_RELEASE_FREE);

	rcu_read_unlock();

	skw_rx_handler(tid_rx->reorder->skw, &release_list);

	SKW_KFREE(tid_rx->reorder_buf);
	SKW_KFREE(tid_rx);
}

int skw_update_tid_rx(struct skw_peer *peer, u16 tid, u16 ssn, u16 win_size)
{
	struct skw_tid_rx *tid_rx;
	struct skw_reorder_rx *reorder;

	trace_skw_rx_update_ba(peer->iface->id, peer->idx, tid, ssn);

	rcu_read_lock();

	reorder = &peer->reorder[tid];
	tid_rx = rcu_dereference(reorder->tid_rx);
	if (!tid_rx)
		goto unlock;

	spin_lock_bh(&reorder->todo.lock);

	/* force to update rx todo list */
	reorder->todo.seq = ssn;
	reorder->todo.reason = SKW_RELEASE_BAR;

	if (!reorder->todo.actived) {
		reorder->todo.actived = true;
		INIT_LIST_HEAD(&reorder->todo.list);
		skw_list_add(&reorder->skw->rx_todo_list, &reorder->todo.list);
	}

	spin_unlock_bh(&reorder->todo.lock);

	skw_wakeup_rx(reorder->skw);

unlock:
	rcu_read_unlock();

	return 0;
}

int skw_add_tid_rx(struct skw_peer *peer, u16 tid, u16 ssn, u16 buf_size)
{
	int i;
	u32 win_sz;
	struct skw_tid_rx *tid_rx;
	struct skw_reorder_rx *reorder;

	skw_dbg("peer: %d, tid: %d, ssn: %d, win size: %d\n",
		peer->idx, tid, ssn, buf_size);

	reorder = &peer->reorder[tid];

	tid_rx = rcu_dereference(reorder->tid_rx);
	if (tid_rx)
		return 0;

	win_sz = buf_size ? buf_size : 64;
	win_sz <<= 1;

	trace_skw_rx_add_ba(peer->iface->id, peer->idx, tid, ssn, win_sz);

	tid_rx = SKW_ALLOC(sizeof(*tid_rx), GFP_KERNEL);
	if (!tid_rx) {
		skw_err("alloc failed, len: %ld\n", (long)(sizeof(*tid_rx)));
		return -ENOMEM;
	}

	tid_rx->reorder_buf = kcalloc(win_sz, sizeof(struct sk_buff_head),
				      GFP_KERNEL);
	if (!tid_rx->reorder_buf) {
		SKW_KFREE(tid_rx);
		return -ENOMEM;
	}

	for (i = 0; i < win_sz; i++)
		__skb_queue_head_init(&tid_rx->reorder_buf[i]);

	tid_rx->win_start = ssn;
	tid_rx->win_size = win_sz;
	tid_rx->stored_num = 0;
	tid_rx->reorder = reorder;
	tid_rx->ref_cnt = atomic_read(&reorder->ref_cnt);

	reorder->inst = peer->iface->id;
	reorder->peer_idx = peer->idx;
	reorder->tid = tid;

	reorder->todo.seq = 0;
	reorder->todo.actived = false;
	reorder->todo.reason = SKW_RELEASE_INVALID;
	INIT_LIST_HEAD(&reorder->todo.list);
	spin_lock_init(&reorder->todo.lock);

	reorder->skw = peer->iface->skw;
	spin_lock_init(&reorder->lock);

	skw_compat_setup_timer(&reorder->timer, skw_reorder_timeout);

	rcu_assign_pointer(reorder->tid_rx, tid_rx);

	return 0;
}

int skw_del_tid_rx(struct skw_peer *peer, u16 tid)
{
	struct skw_tid_rx *tid_rx;
	struct skw_reorder_rx *reorder;
	struct sk_buff_head release_list;

	reorder = &peer->reorder[tid];

	__skb_queue_head_init(&release_list);

	trace_skw_rx_del_ba(tid);

	spin_lock_bh(&reorder->lock);
	tid_rx = rcu_dereference_protected(reorder->tid_rx,
			lockdep_is_held(&reorder->lock));

	RCU_INIT_POINTER(reorder->tid_rx, NULL);

	atomic_inc(&reorder->ref_cnt);

	smp_wmb();

	del_timer_sync(&reorder->timer);

	if (tid_rx) {
#ifdef CONFIG_SKW_GKI_DRV
		skw_call_rcu(peer->iface->skw, &tid_rx->rcu_head, skw_free_tid_rx);
#else
		call_rcu(&tid_rx->rcu_head, skw_free_tid_rx);
#endif
	}

	spin_unlock_bh(&reorder->lock);

	return 0;
}

#ifdef SKW_RX_WORKQUEUE

void skw_rx_worker(struct work_struct *work)
{
	unsigned long flags;
	struct skw_core *skw;
	struct sk_buff_head qlist;

	skw = container_of(work, struct skw_core, rx_worker);
	__skb_queue_head_init(&qlist);

	while (skw->rx_todo_list.count || skb_queue_len(&skw->rx_dat_q)) {

		skw_rx_todo(&skw->rx_todo_list);

		if (skb_queue_empty(&skw->rx_dat_q))
			return;

		/*
		 * data frame format:
		 * RX_DESC_HEADER + ETHERNET
		 */
		spin_lock_irqsave(&skw->rx_dat_q.lock, flags);
		skb_queue_splice_tail_init(&skw->rx_dat_q, &qlist);
		spin_unlock_irqrestore(&skw->rx_dat_q.lock, flags);

		skw_rx_data_handler(skw, &qlist);
	}
}

static int __skw_rx_init(struct skw_core *skw)
{
	int cpu;
	struct workqueue_attrs wq_attrs;

	skw->rx_wq = alloc_workqueue("skw_rxwq.%d", WQ_UNBOUND | __WQ_ORDERED, 1, skw->idx);
	if (!skw->rx_wq) {
		skw_err("alloc skwrx_workqueue failed\n");
		return -EFAULT;
	}

	memset(&wq_attrs, 0, sizeof(wq_attrs));
	wq_attrs.nice = MIN_NICE;

	apply_workqueue_attrs(skw->rx_wq, &wq_attrs);

	INIT_WORK(&skw->rx_worker, skw_rx_worker);

	queue_work(skw->rx_wq, &skw->rx_worker);

	return 0;
}

static void __skw_rx_deinit(struct skw_core *skw)
{
	atomic_set(&skw->exit, 1);
	cancel_work_sync(&skw->rx_worker);
	destroy_workqueue(skw->rx_wq);
}

#else

/* RX data thread entry */
int skw_rx_thread(void *data)
{
	unsigned long flags;
	struct skw_core *skw;
	struct sk_buff_head qlist;

	skw = (struct skw_core *)data;
	__skb_queue_head_init(&qlist);

	while (!kthread_should_stop()) {
		skw_rx_todo(&skw->rx_todo_list);

		if (skb_queue_empty(&skw->rx_dat_q)) {
			set_current_state(TASK_IDLE);
			schedule_timeout(msecs_to_jiffies(1));
		}

		/*
		 * data frame format:
		 * RX_DESC_HEADER + ETHERNET
		 */
		spin_lock_irqsave(&skw->rx_dat_q.lock, flags);
		skb_queue_splice_tail_init(&skw->rx_dat_q, &qlist);
		spin_unlock_irqrestore(&skw->rx_dat_q.lock, flags);

		skw_rx_data_handler(skw, &qlist);
	}

	skw_info("exit\n");

	return 0;
}

static int __skw_rx_init(struct skw_core *skw)
{
	//int cpu;

	skw->rx_thread = kthread_create(skw_rx_thread, skw, "skw_rx.%d", skw->idx);
	if (IS_ERR(skw->rx_thread)) {
		skw_err("create rx thread failed\n");

		return PTR_ERR(skw->rx_thread);
	}

	skw_set_thread_priority(skw->rx_thread, SCHED_RR, 1);
	set_user_nice(skw->rx_thread, MIN_NICE);
	wake_up_process(skw->rx_thread);

	return 0;
}

static void __skw_rx_deinit(struct skw_core *skw)
{
	if (skw->rx_thread) {
		atomic_set(&skw->exit, 1);
		kthread_stop(skw->rx_thread);
		skw->rx_thread = NULL;
	}
}

#endif

/*
 * callback function, invoked by bsp
 */
int skw_rx_cb(int port, struct scatterlist *sglist,
		     int nents, void *priv)
{
	int ret;
	bool rx_sdma;
	void *sg_addr;
	int idx, total_len;
	struct sk_buff *skb;
	struct scatterlist *sg;
	struct skw_msg *msg;
	struct skw_iface *iface;
	struct skw_event_work *work;
	struct skw_core *skw = (struct skw_core *)priv;

	rx_sdma = skw->hw_pdata->bus_type & RX_SDMA;

	for_each_sg(sglist, sg, nents, idx) {
		if (sg == NULL || !sg->length) {
			skw_warn("sg: 0x%p, nents: %d, idx: %d, len: %d\n",
				sg, nents, idx, sg ? sg->length : 0);
			break;
		}

		sg_addr = sg_virt(sg);

		if (rx_sdma) {
			skb = dev_alloc_skb(sg->length);
			if (!skb) {
				skw_err("alloc skb failed, len: %d\n",
							sg->length);
				continue;
			}

			skw_put_skb_data(skb, sg_addr, sg->length);
		} else {
			total_len = SKB_DATA_ALIGN(sg->length) + skw->skb_share_len;
			if (unlikely(total_len > SKW_ADMA_BUFF_LEN)) {
				skw_warn("sg->length: %d, rx buff: %lu, share info: %d\n",
					 sg->length, (long)SKW_ADMA_BUFF_LEN, skw->skb_share_len);

				skw_compat_page_frag_free(sg_addr);
				continue;
			}

			skb = build_skb(sg_addr, total_len);
			if (!skb) {
				skw_err("build skb failed, len: %d\n",
							total_len);

				skw_compat_page_frag_free(sg_addr);
				continue;
			}

			skb_put(skb, sg->length);
		}

		trace_skw_rx_irq(nents, idx, port, sg->length);

		if (skw->hw.bus == SKW_BUS_SDIO)
			skb_pull(skb, 4);

		if (port == skw->hw_pdata->cmd_port) {
			msg = (struct skw_msg *)skb_pull(skb, 12);
			if (!msg) {
				dev_kfree_skb(skb);
				continue;
			}

			trace_skw_msg_rx(msg->inst_id, msg->type, msg->id,
					msg->seq, msg->total_len);

			switch (msg->type) {
			case SKW_MSG_CMD_ACK:
				skw_cmd_ack_handler(skw, skb->data, skb->len);
				kfree_skb(skb);

				break;

			case SKW_MSG_EVENT:
				if (++skw->skw_event_sn != msg->seq) {
					skw_warn("invalid event seq: %d, expect: %d\n",
						 msg->seq, skw->skw_event_sn);

					skw_hw_assert(skw);
					kfree_skb(skb);

					continue;
				}

				if (msg->id == SKW_EVENT_CREDIT_UPDATE) {
					skw_event_add_credit(skw, msg + 1);
					smp_wmb();
					kfree_skb(skb);

					continue;
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

		} else {

			skw_data_add_credit(skw, skb->data);
			skb_queue_tail(&skw->rx_dat_q, skb);

			skw->rx_packets++;
			set_cpus_allowed_ptr(skw->rx_thread, cpumask_of(task_cpu(current)));
			skw_wakeup_rx(skw);

			skw_wake_lock_timeout(skw, 400);
		}
	}

	return 0;
}

int skw_register_rx_callback(struct skw_core *skw, void *func, void *data)
{
	int i, ret = 0;

	if (skw->hw.cmd_port >= 0) {
		skw_dbg("cmd port: %d func:%p data:%p\n", skw->hw.cmd_port, func, data);
		ret = skw_register_rx_cb(skw, skw->hw.cmd_port, func, data);
	}

	for (i = 0; i < SKW_NR_LMAC; i++) {
		if (skw->hw.lmac[i].dport < 0)
			continue;

		skw_dbg("data port: %d func:%p data:%p\n", skw->hw.lmac[i].dport, func, data);
		ret |= skw_register_rx_cb(skw, skw->hw.lmac[i].dport, func, data);
	}

	return ret;
}

int skw_rx_init(struct skw_core *skw)
{
	int ret;

	skw_list_init(&skw->rx_todo_list);
	spin_lock_init(&skw->rx_lock);
	skw_wake_lock_init(skw, 0, "skw_rx_wlock");

	ret = skw_register_rx_callback(skw, skw_rx_cb, skw);
	if (ret < 0) {
		skw_err("register rx callback failed, ret: %d\n", ret);
		return ret;
	}

	ret = __skw_rx_init(skw);
	if (ret < 0)
		skw_register_rx_callback(skw, NULL, NULL);

	rx_reorder_flag = true;
	skw_debugfs_file(skw->dentry, "rx_reorder", 0666, &skw_rx_reorder_fops, NULL);

	return 0;
}

int skw_rx_deinit(struct skw_core *skw)
{
	skw_register_rx_callback(skw, NULL, NULL);

	__skw_rx_deinit(skw);
	skw_rx_todo(&skw->rx_todo_list);

	skw_wake_lock_deinit(skw);

	return 0;
}
