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

#include <linux/string.h>
#include <linux/ctype.h>
#include <net/iw_handler.h>
#include <linux/udp.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <net/cfg80211-wext.h>

#include "skw_core.h"
#include "skw_cfg80211.h"
#include "skw_iface.h"
#include "skw_iw.h"
#include "skw_log.h"

static int skw_iw_commit(struct net_device *dev, struct iw_request_info *info,
			 union iwreq_data *wrqu, char *extra)
{
	skw_dbg("traced\n");

	return 0;
}

static int skw_iw_get_name(struct net_device *dev, struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	skw_dbg("traced\n");

	return 0;
}

static int skw_iw_set_freq(struct net_device *dev, struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	skw_dbg("traced\n");

	return 0;
}

static int skw_iw_get_freq(struct net_device *dev, struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	skw_dbg("traced\n");

	return 0;
}

static int skw_iw_set_mode(struct net_device *dev, struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	skw_dbg("traced\n");

	return 0;
}

static int skw_iw_get_mode(struct net_device *dev, struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	skw_dbg("traced\n");

	return 0;
}

static struct iw_statistics *skw_get_wireless_stats(struct net_device *dev)
{
	skw_dbg("traced\n");

	return NULL;
}

static const iw_handler skw_iw_standard_handlers[] = {
	IW_HANDLER(SIOCSIWCOMMIT, (iw_handler)skw_iw_commit),
	IW_HANDLER(SIOCGIWNAME, (iw_handler)skw_iw_get_name),
	IW_HANDLER(SIOCSIWFREQ, (iw_handler)skw_iw_set_freq),
	IW_HANDLER(SIOCGIWFREQ, (iw_handler)skw_iw_get_freq),
	IW_HANDLER(SIOCSIWMODE,	(iw_handler)skw_iw_set_mode),
	IW_HANDLER(SIOCGIWMODE,	(iw_handler)skw_iw_get_mode),
#ifdef CONFIG_CFG80211_WEXT_EXPORT
	IW_HANDLER(SIOCGIWRANGE, (iw_handler)cfg80211_wext_giwrange),
	IW_HANDLER(SIOCSIWSCAN,	(iw_handler)cfg80211_wext_siwscan),
	IW_HANDLER(SIOCGIWSCAN,	(iw_handler)cfg80211_wext_giwscan),
#endif
};

#ifdef CONFIG_WEXT_PRIV

#define SKW_SET_LEN_64                  64
#define SKW_SET_LEN_128                 128
#define SKW_SET_LEN_256                 256
#define SKW_SET_LEN_512                 512
#define SKW_GET_LEN_512                 512
#define SKW_SET_LEN_1024                1024
#define SKW_GET_LEN_1024                1024
#define SKW_KEEP_BUF_SIZE               1024

/* max to 16 commands */
#define SKW_IW_PRIV_SET                (SIOCIWFIRSTPRIV + 1)
#define SKW_IW_PRIV_GET                (SIOCIWFIRSTPRIV + 3)
#define SKW_IW_PRIV_AT                 (SIOCIWFIRSTPRIV + 5)
#define SKW_IW_PRIV_80211MODE          (SIOCIWFIRSTPRIV + 6)
#define SKW_IW_PRIV_GET_80211MODE      (SIOCIWFIRSTPRIV + 7)
#define SKW_IW_PRIV_KEEP_ALIVE         (SIOCIWFIRSTPRIV + 8)
#define SKW_IW_PRIV_WOW_FILTER         (SIOCIWFIRSTPRIV + 9)

#define SKW_IW_PRIV_LAST               SIOCIWLASTPRIV

static struct skw_keep_active_setup kp_set = {0,};
static u8 skw_wow_flted[256];

static int skw_keep_alive_add_checksum(u8 *buff, u32 len)
{
	u8 *ptr = buff;
	struct iphdr *ip;
	struct udphdr *udp;
	__sum16 sum, sum1;
	u32 udp_len;

	ptr += sizeof(struct ethhdr);
	ip = (struct iphdr *)ptr;
	ip->check = 0;
	ip->check = cpu_to_le16(ip_compute_csum(ip, 20));

	ptr += sizeof(struct iphdr);
	udp = (struct udphdr *)ptr;
	udp->check = 0;

	udp_len = len - sizeof(struct ethhdr)
		 - sizeof(struct iphdr);
	sum1 = csum_partial(ptr,
					udp_len, 0);
	sum = csum_tcpudp_magic(ip->saddr, ip->daddr,
				udp_len, IPPROTO_UDP, sum1);
	udp->check = cpu_to_le16(sum);

	skw_dbg("chsum %x %x\n", ip->check, sum);
	return 0;
}

static int skw_keep_active_rule_save(struct skw_core *skw,
	 struct skw_keep_active_rule *kp, u8 idx, u8 en, u32 flags)
{
	int ret;

	if (!skw || idx >= SKW_KEEPACTIVE_RULE_MAX) {
		ret = -EFAULT;
		return ret;
	}

	if (kp) {
		if (kp_set.rule[idx])
			SKW_KFREE(kp_set.rule[idx]);

		kp_set.rule[idx] = SKW_ALLOC(kp->payload_len
			 + sizeof(*kp), GFP_KERNEL);
		memcpy(kp_set.rule[idx], kp, kp->payload_len + sizeof(*kp));
		skw_keep_alive_add_checksum(kp_set.rule[idx]->data[0].payload,
				kp_set.rule[idx]->payload_len
				- sizeof(struct skw_keep_active_rule_data));
		kp_set.rule[idx]->data[0].is_chksumed = 0;
		kp_set.flags[idx] = flags;
	}

	if (en)
		kp_set.en_bitmap |= BIT(idx);
	else
		kp_set.en_bitmap &= ~BIT(idx);

	skw_dbg("enable bitmap 0x%x\n", kp_set.en_bitmap);
	skw_hex_dump("kpsave", &kp_set, sizeof(kp_set), false);

	return 0;
}

static int skw_keep_active_disable_cmd(struct net_device *ndev,
	 u16 next_cmd, int next_rules)
{
	struct skw_spd_action_param spd;
	int ret = 0;

	if (!next_rules)
		spd.sub_cmd = ACTION_DIS_ALL_KEEPALIVE;
	else if (next_cmd == ACTION_EN_ALWAYS_KEEPALIVE)
		spd.sub_cmd = ACTION_DIS_KEEPALIVE;
	else
		spd.sub_cmd = ACTION_DIS_ALWAYS_KEEPALIVE;

	spd.len = 0;

	skw_hex_dump("dpdis:", &spd, sizeof(spd), true);
	ret = skw_send_msg(ndev->ieee80211_ptr->wiphy, ndev,
			 SKW_CMD_SET_SPD_ACTION, &spd, sizeof(spd), NULL, 0);
	if (ret)
		skw_err("failed, ret: %d\n", ret);

	return ret;
}

static int skw_keep_active_cmd(struct net_device *ndev, struct skw_core *skw,
		 u8 en, u32 flags)
{
	int ret = 0;
	u32 idx_map, idx, rules = 0;
	int total, fixed, len = 0, offset = 0;
	struct skw_spd_action_param *spd = NULL;
	struct skw_keep_active_param *kp_param = NULL;

	fixed = sizeof(struct skw_spd_action_param) +
		 sizeof(struct skw_keep_active_param);
	total = fixed + SKW_KEEPACTIVE_LENGTH_MAX;

	spd = SKW_ALLOC(total, GFP_KERNEL);
	if (!spd) {
		skw_err("malloc failed, size: %d\n", total);
		return -ENOMEM;
	}

	kp_param = (struct skw_keep_active_param *)((u8 *)spd
			+ sizeof(*spd));
	offset = fixed;
	idx_map = kp_set.en_bitmap;

	while (idx_map) {
		idx = ffs(idx_map) - 1;

		if (!kp_set.rule[idx]) {
			skw_err("rule exception\n");
			break;
		}

		if (offset + sizeof(struct skw_keep_active_rule)
			 + kp_set.rule[idx]->payload_len > total)
			break;

		memcpy((u8 *)spd + offset, kp_set.rule[idx],
				 sizeof(struct skw_keep_active_rule)
				 + kp_set.rule[idx]->payload_len);

		offset += sizeof(struct skw_keep_active_rule)
			+ kp_set.rule[idx]->payload_len;

		if (kp_set.flags[idx] & SKW_KEEPALIVE_ALWAYS_FLAG)
			spd->sub_cmd = ACTION_EN_ALWAYS_KEEPALIVE;
		else
			spd->sub_cmd = ACTION_EN_KEEPALIVE;

		if (++rules > SKW_KEEPACTIVE_RULE_MAX)
			break;

		SKW_CLEAR(idx_map, BIT(idx));
	}

	kp_param->rule_num = rules;
	spd->len = offset - sizeof(struct skw_spd_action_param);
	len = offset;

	if (en) {
		if (flags & SKW_KEEPALIVE_ALWAYS_FLAG)
			spd->sub_cmd = ACTION_EN_ALWAYS_KEEPALIVE;
		else
			spd->sub_cmd = ACTION_EN_KEEPALIVE;
	}

	ret = skw_keep_active_disable_cmd(ndev, spd->sub_cmd, rules);

	skw_dbg("len:%d rule num:%d\n", len, rules);
	if (rules) {
		skw_hex_dump("actv:", spd, len, true);
		ret = skw_send_msg(ndev->ieee80211_ptr->wiphy, ndev,
				SKW_CMD_SET_SPD_ACTION, spd, len, NULL, 0);
		if (ret)
			skw_err("failed, ret: %d\n", ret);
	}

	SKW_KFREE(spd);
	return ret;
}

//iwpriv wlan0 keep_alive idx=0,en=1,period=1000,flags=0/1,
//pkt=7c:7a:3c:81:e5:72:00:0b
static int skw_keep_active_set(struct net_device *dev, u8 *param, int len)
{
	int result_len = 0;
	u8 *ch, *result_val;
	char *hex = NULL;
	u8 idx, en = 0, get_pkt = 0;
	u32 flags = 0;
	u8 keep_alive[SKW_KEEPACTIVE_LENGTH_MAX];
	struct skw_keep_active_rule *kp =
		(struct skw_keep_active_rule *)keep_alive;
	int pos = 0, ret = 0;
	struct skw_iface *iface = netdev_priv(dev);
	struct skw_core *skw = iface->skw;

	memset(kp, 0, sizeof(*kp));

	hex = param;
	hex = strstr(hex, "idx=");
	if (hex) {
		ch = strsep(&hex, "=");
		if ((ch == NULL) || (strlen(ch) == 0)) {
			skw_err("idx param\n");
			ret = -EFAULT;
			goto error;
		}

		ch = strsep(&hex, ",");
		if ((ch == NULL) || (strlen(ch) == 0)) {
			skw_err("idx param\n");
			ret = -ERANGE;
			goto error;
		}

		ret = kstrtou8(ch, 0, &idx);
		if (ret) {
			skw_err("idx param\n");
			ret = -EINVAL;
			goto error;
		}
	} else {
		skw_err("idx not found\n");
		ret = -EFAULT;
		goto error;
	}

	if (!hex) {
		ret = -EBADF;
		goto error;
	}

	hex = strstr(hex, "en=");
	if (hex) {
		ch = strsep(&hex, "=");
		if ((ch == NULL) || (strlen(ch) == 0)) {
			skw_err("en param\n");
			ret = -EFAULT;
			goto error;
		}

		ch = strsep(&hex, ",");
		if ((ch == NULL) || (strlen(ch) == 0)) {
			skw_err("en param\n");
			ret = -ERANGE;
			goto error;
		}

		ret = kstrtou8(ch, 0, &en);
		if (ret) {
			skw_err("en param\n");
			ret = -EINVAL;
			goto error;
		}
	} else {
		skw_err("en not found\n");
		ret = -EFAULT;
		goto error;
	}

	if (!hex)
		goto done;

	hex = strstr(hex, "period=");
	if (hex) {
		ch = strsep(&hex, "=");
		if ((ch == NULL) || (strlen(ch) == 0)) {
			skw_err("period param\n");
			ret = -EFAULT;
			goto error;
		}

		ch = strsep(&hex, ",");
		if ((ch == NULL) || (strlen(ch) == 0)) {
			skw_err("period param\n");
			ret = -ERANGE;
			goto error;
		}

		ret = kstrtou32(ch, 0, &kp->keep_interval);
		if (ret) {
			skw_err("period param\n");
			ret = -EINVAL;
			goto error;
		}

	}

	if (!hex)
		goto done;

	hex = strstr(hex, "flags=");
	if (hex) {
		ch = strsep(&hex, "=");
		if ((ch == NULL) || (strlen(ch) == 0)) {
			skw_err("flags param\n");
			ret = -EFAULT;
			goto error;
		}

		ch = strsep(&hex, ",");
		if ((ch == NULL) || (strlen(ch) == 0)) {
			skw_err("flags param\n");
			ret = -ERANGE;
			goto error;
		}

		ret = kstrtou32(ch, 0, &flags);
		if (ret) {
			skw_err("flags param\n");
			ret = -EINVAL;
			goto error;
		}
	}

	if (!hex)
		goto done;

	hex = strstr(hex, "pkt=");
	if (hex) {
		ch = strsep(&hex, "=");
		if ((ch == NULL) || (strlen(ch) == 0)) {
			skw_err("pkt param\n");
			ret = -EFAULT;
			goto error;
		}

		result_val = kp->data[0].payload;
		while (1) {
			u8 temp = 0;
			char *cp = strchr(hex, ':');

			if (cp) {
				*cp = 0;
				cp++;
			}

			ret = kstrtou8(hex, 16, &temp);
			if (ret) {
				skw_err("pkt param\n");
				ret = -EINVAL;
				goto error;
			}

			if (temp < 0 || temp > 255) {
				skw_err("pkt param\n");
				ret = -ERANGE;
				goto error;
			}

			result_val[pos] = temp;
			result_len++;
			pos++;

			if (!cp)
				break;

			if (result_len + sizeof(*kp) >=
				SKW_KEEPACTIVE_LENGTH_MAX)
				break;

			hex = cp;
		}
		get_pkt = 1;
	}

	kp->payload_len = result_len + sizeof(struct skw_keep_active_rule_data);

done:
	skw_dbg("idx:%d en:%d pr:%d pkt:%d len:%d\n", idx, en,
		 kp->keep_interval, get_pkt, result_len);
	skw_hex_dump("kp", kp, sizeof(*kp) + kp->payload_len, false);

	if (!(kp->keep_interval && get_pkt))
		kp = NULL;

	ret = skw_keep_active_rule_save(skw, kp, idx, en, flags);
	if (ret) {
		skw_err("save rule\n");
		goto error;
	}

	ret = skw_keep_active_cmd(dev, skw, en, flags);
	if (ret) {
		skw_err("send rule\n");
		goto error;
	}

	return 0;

error:
	skw_err("error:%d\n", ret);
	return ret;
}

//iwpriv wlan0 wow_filter enable,pattern=6+7c:7a:3c:81:e5:72#20+0b#20+!ee:66
//iwpriv wlan0 wow_filter disable
int skw_wow_filter_set(struct net_device *ndev, u8 *param, int len, char *resp)
{
	u8 *ch, *result_val;
	char *hex, *ptr;
	struct skw_spd_action_param *spd = NULL;
	struct skw_wow_input_param *wow_param = NULL;
	struct skw_wow_rule *rule = NULL;
	int pos = 0, ret = 0, rule_idx = 0, offset, total, result_len = 0;
	struct skw_pkt_pattern *ptn;
	u8 data[256];
	u32 temp, resp_len = 0, i;
	char help[] = "Usage:[list]|[disable]|[enable,pattern=6+10#23+!11,pattern=45+31:31]";

	memcpy(data, param, len);
	hex = data;
	ptr = hex;

	if (!strcmp(hex, "list")) {
		if (len != sizeof("list")) {
			resp_len = sprintf(resp, "ERROR: %s\n %s\n",
				"list cmd", help);
			return -EFAULT;
		}
		resp_len = sprintf(resp, "List: %s\n", skw_wow_flted);
		resp_len += sprintf(resp + resp_len, "%s\n", "OK");
		return ret;
	}

	if (!strcmp(hex, "disable")) {
		if (len != sizeof("disable")) {
			resp_len = sprintf(resp, "ERROR: %s\n %s\n",
				 "dis cmd", help);
			return -EFAULT;
		}
		ret = skw_wow_disable(ndev->ieee80211_ptr->wiphy);
		if (!ret) {
			memset(skw_wow_flted, 0, sizeof(skw_wow_flted));
			memcpy(skw_wow_flted, param, len);
		}

		resp_len = sprintf(resp, "%s\n", "OK");
		return ret;
	}

	ret = strncmp(hex, "enable", strlen("enable"));
	if (ret) {
		resp_len = sprintf(resp, "ERROR: %s\n %s\n",
			 "en cmd", help);
		return -EFAULT;
	}

	hex += strlen("enable");
	if (hex >= ptr + len - 1) {
		resp_len = sprintf(resp, "ERROR: %s\n %s\n",
			 "en cmd", help);
		return -EFAULT;
	}

	total = sizeof(struct skw_spd_action_param) +
		sizeof(struct skw_wow_input_param) +
		sizeof(struct skw_wow_rule) * SKW_MAX_WOW_RULE_NUM;

	spd = SKW_ALLOC(total, GFP_KERNEL);
	if (!spd) {
		skw_err("malloc failed, size: %d\n", total);
		resp_len = sprintf(resp, "ERROR: %s\n", "malloc failed");
		return -ENOMEM;
	}

	wow_param = (struct skw_wow_input_param *)((u8 *)spd
		+ sizeof(*spd));
	spd->sub_cmd = ACTION_EN_WOW;
	wow_param->wow_flags |= SKW_WOW_BLACKLIST_FILTER;

	while (hex < ptr + len - 1) {
		rule = &wow_param->rules[rule_idx];
		result_len = 0;

		ret = strncmp(hex, ",pattern=", strlen(",pattern="));
		if (!ret) {
			hex += strlen(",pattern=");
			result_val = rule->rule;

			while (hex < ptr + len - 1) {
				ret = sscanf(hex, "%d+%02x",
					&offset, &temp);
				if (ret != 2) {
					ret = sscanf(hex, "%d+!%02x",
						&offset, &temp);
					if (ret != 2) {
						resp_len = sprintf(resp,
							"ERROR: %s\n",
							"match char + +!");
						ret = -EINVAL;
						goto err;
					}
				}

				if (offset > ETH_DATA_LEN) {
					resp_len = sprintf(resp,
						"ERROR: offset:%d over limit\n",
						offset);
					ret = -EINVAL;
					goto err;
				}

				ptn = (struct skw_pkt_pattern *)result_val;
				result_val += sizeof(*ptn);
				result_len += sizeof(*ptn);

				if (result_len >= sizeof(rule->rule)) {
					resp_len = sprintf(resp,
						"ERROR: %s\n",
						"ptn over limit\n");
					break;
				}

				ptn->op = PAT_TYPE_ETH;
				ptn->offset = offset;

				ch = strsep(&hex, "+");
				if ((ch == NULL) || (strlen(ch) == 0)) {
					resp_len = sprintf(resp,
						"ERROR: %s\n",
						"match char +\n");
					ret = -EINVAL;
					goto err;
				}

				if (hex[0] == '!') {
					ptn->type_offset = PAT_OP_TYPE_DIFF;
					ch = strsep(&hex, "!");
				}

				pos = 0;
				while (hex < ptr + len - 1) {
					char *cp;

					if (isxdigit(hex[0]) &&
						isxdigit(hex[1]) &&
						(sscanf(hex, "%2x", &temp)
							== 1)) {
					} else {
						resp_len = sprintf(resp,
							"ERROR: match char %c%c end\n",
							hex[0], hex[1]);
						ret = -EINVAL;
						goto err;
					}

					result_val[pos] = temp;
					result_len++;
					pos++;

					if (result_len >= sizeof(rule->rule)) {
						resp_len = sprintf(resp,
							"ERROR: %s\n",
							"size over limit\n");
						break;
					}

					if (hex[2] == ',' || hex[2] == '#')
						break;
					else if (hex[2] == '\0') {
						hex += 2;
						break;
					} else  if (hex[2] != ':') {
						resp_len = sprintf(resp,
							"ERROR: char data %c\n",
							hex[2]);
						ret = -EINVAL;
						goto err;
					}

					cp = strchr(hex, ':');
					if (cp) {
						*cp = 0;
						cp++;
					}

					hex = cp;
				}
				result_val += pos;
				ptn->len = pos;

				if (hex[2] == ',') {
					hex += 2;
					break;
				} else if (hex[2] == '#')
					ch = strsep(&hex, "#");
			}
		} else {
			resp_len = sprintf(resp, "ERROR: %s\n",
				"match char pattern=\n");
			ret = -EINVAL;
			goto err;
		}

		rule->len = result_len;
		rule_idx++;
		skw_hex_dump("rule", rule, sizeof(*rule), false);

		if (rule_idx > SKW_MAX_WOW_RULE_NUM)
			break;
	}

	if (!rule_idx) {
		resp_len = sprintf(resp, "ERROR: %s\n", "no rule\n");
		ret = -EINVAL;
		goto err;
	}

	for (i = 0; i < rule_idx; i++)
		if (!wow_param->rules[i].len) {
			resp_len = sprintf(resp, "ERROR: %s\n", "rule len 0\n");
			ret = -EINVAL;
			goto err;
		}

	wow_param->rule_num = rule_idx;
	spd->len = sizeof(struct skw_wow_input_param) +
		sizeof(struct skw_wow_rule) * rule_idx;

	skw_dbg("len:%d %d\n", spd->len, total);
	skw_hex_dump("wow", spd, total, true);

	ret = skw_msg_xmit(ndev->ieee80211_ptr->wiphy, 0,
		 SKW_CMD_SET_SPD_ACTION, spd, total, NULL, 0);
	if (ret)
		skw_err("failed, ret: %d\n", ret);
	else {
		memset(skw_wow_flted, 0, sizeof(skw_wow_flted));
		memcpy(skw_wow_flted, param, len);
	}

err:
	if (ret)
		resp_len += sprintf(resp + resp_len, " %s\n", help);
	else
		resp_len = sprintf(resp, "%s\n", "OK");

	SKW_KFREE(spd);
	return ret;
}

static int skw_iwpriv_keep_alive(struct net_device *dev,
			struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	char *param;
	char help[] = "ERROR useage:[idx=0,en=0/1,period=100,flags=0/1,pkt=7c:11]";
	int ret = 0;

	WARN_ON(SKW_KEEP_BUF_SIZE < wrqu->data.length);

	param = SKW_ALLOC(SKW_KEEP_BUF_SIZE, GFP_KERNEL);
	if (!param) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(param, wrqu->data.pointer, sizeof(param))) {
		skw_err("copy failed, length: %d\n",
			wrqu->data.length);

		ret = -EFAULT;
		goto free;
	}

	skw_dbg("cmd: 0x%x, (len: %d)\n",
		info->cmd, wrqu->data.length);
	skw_hex_dump("param:", param, sizeof(param), false);

	ret = skw_keep_active_set(dev, param, sizeof(param));
	if (ret)
		memcpy(extra, help, sizeof(help));
	else
		memcpy(extra, "OK", sizeof("OK"));

	wrqu->data.length = SKW_GET_LEN_512;

	skw_dbg("resp: %s\n", extra);

free:
	SKW_KFREE(param);

out:
	return ret;
}

static int skw_iwpriv_wow_filter(struct net_device *dev,
			struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	char param[256];
	int ret;

	WARN_ON(sizeof(param) < wrqu->data.length);

	if (copy_from_user(param, wrqu->data.pointer, sizeof(param))) {
		skw_err("copy failed, length: %d\n",
			wrqu->data.length);

		return -EFAULT;
	}

	param[255] = '\0';

	skw_dbg("cmd: 0x%x, (len: %d)\n",
		info->cmd, wrqu->data.length);
	skw_hex_dump("flt", param, sizeof(param), false);

	ret = skw_wow_filter_set(dev, param,
		min_t(int, sizeof(param), (int)wrqu->data.length),
		extra);

	wrqu->data.length = SKW_GET_LEN_512;

	skw_dbg("resp: %s\n", extra);
	return 0;
}

static int skw_send_at_cmd(struct skw_core *skw, char *cmd, int cmd_len,
			char *buf, int buf_len)
{
	int ret, len, resp_len;
	char *command, *resp;

	len = round_up(cmd_len, skw->hw.align);

	resp_len = round_up(buf_len, skw->hw_pdata->align_value);

	command = SKW_ALLOC(len, GFP_KERNEL);
	if (!command) {
		ret = -ENOMEM;
		goto out;
	}

	resp = SKW_ALLOC(resp_len, GFP_KERNEL);
	if (!resp) {
		ret = -ENOMEM;
		goto fail_alloc_resp;
	}

	ret = skw_uart_open(skw);
	if (ret < 0)
		goto failed;

	memcpy(command, cmd, cmd_len);
	ret = skw_uart_write(skw, command, len);
	if (ret < 0)
		goto failed;

	ret = skw_uart_read(skw, resp, resp_len);
	if (ret < 0)
		goto failed;

	memcpy(buf, resp, buf_len);
	ret = 0;

failed:
	SKW_KFREE(resp);
fail_alloc_resp:
	SKW_KFREE(command);
out:
	if (ret < 0)
		skw_err("failed: ret: %d\n", ret);

	return ret;
}

static int skw_iwpriv_mode(struct net_device *dev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	int i;
	char param[32] = {0};
	struct skw_iface *iface = (struct skw_iface *)netdev_priv(dev);

	struct skw_iw_wireless_mode {
		char *name;
		enum skw_wireless_mode mode;
	} modes[] = {
		{"11B", SKW_WIRELESS_11B},
		{"11G", SKW_WIRELESS_11G},
		{"11A", SKW_WIRELESS_11A},
		{"11N", SKW_WIRELESS_11N},
		{"11AC", SKW_WIRELESS_11AC},
		{"11AX", SKW_WIRELESS_11AX},
		{"11G_ONLY", SKW_WIRELESS_11G_ONLY},
		{"11N_ONLY", SKW_WIRELESS_11N_ONLY},

		/*keep last*/
		{NULL, 0}
	};

	WARN_ON(sizeof(param) < wrqu->data.length);

	if (copy_from_user(param, wrqu->data.pointer, sizeof(param))) {
		skw_err("copy failed, length: %d\n",
			wrqu->data.length);

		return -EFAULT;
	}

	skw_dbg("cmd: 0x%x, %s(len: %d)\n",
		info->cmd, param, wrqu->data.length);

	for (i = 0; modes[i].name; i++) {
		if (!strcmp(modes[i].name, param)) {
			iface->extend.wireless_mode = modes[i].mode;
			return 0;
		}
	}

	return -EINVAL;
}

static int skw_iwpriv_get_mode(struct net_device *dev,
			struct iw_request_info *info,
			union iwreq_data *wrqu, char *extra)
{
	skw_dbg("traced\n");
	return 0;
}

static int skw_iwpriv_help(struct skw_iface *iface, void *param, char *args,
			char *resp, int resp_len)
{
	int len = 0;
	struct skw_iwpriv_cmd *cmd = param;

	len = sprintf(resp, " %s:\n", cmd->help_info);
	cmd++;

	while (cmd->handler) {
		len += sprintf(resp + len, "%-4.4s %s\n", "", cmd->help_info);
		cmd++;
	}

	return 0;
}

static int skw_iwpriv_set_bandcfg(struct skw_iface *iface, void *param,
		char *args, char *resp, int resp_len)
{
	u16 res;
	int ret;

	if (args == NULL)
		return -EINVAL;

	ret = kstrtou16(args, 10, &res);
	if (!ret && res < 3) {
		if (res == 0)
			iface->extend.scan_band_filter = 0;
		else if (res == 1)
			iface->extend.scan_band_filter = BIT(NL80211_BAND_2GHZ);
		else if (res == 2)
			iface->extend.scan_band_filter = BIT(NL80211_BAND_5GHZ);

		sprintf(resp, "ok");
	} else
		sprintf(resp, "failed");

	return ret;
}

static int skw_iwpriv_get_bandcfg(struct skw_iface *iface, void *param,
		char *args, char *resp, int resp_len)
{
	if (!iface->extend.scan_band_filter)
		sprintf(resp, "bandcfg=%s", "Auto");
	else if (iface->extend.scan_band_filter & BIT(NL80211_BAND_2GHZ))
		sprintf(resp, "bandcfg=%s", "2G");
	else if (iface->extend.scan_band_filter & BIT(NL80211_BAND_5GHZ))
		sprintf(resp, "bandcfg=%s", "5G");

	return 0;
}

static struct skw_iwpriv_cmd skw_iwpriv_set_cmds[] = {
	/* keep first */
	{"help", skw_iwpriv_help, "usage"},
	{"bandcfg", skw_iwpriv_set_bandcfg, "bandcfg=0/1/2"},

	/*keep last*/
	{NULL, NULL, NULL}
};

static struct skw_iwpriv_cmd skw_iwpriv_get_cmds[] = {
	/* keep first */
	{"help", skw_iwpriv_help, "usage"},

	{"bandcfg", skw_iwpriv_get_bandcfg, "bandcfg"},

	/*keep last*/
	{NULL, NULL, NULL}
};

static struct skw_iwpriv_cmd *skw_iwpriv_cmd_match(struct skw_iwpriv_cmd *cmds,
					const char *key, int key_len)
{
	int i;

	for (i = 0; cmds[i].name; i++) {
		if (!memcmp(cmds[i].name, key, key_len))
			return &cmds[i];
	}

	return NULL;
}

static int skw_iwpriv_set(struct net_device *dev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	int ret = 0;
	int key_len;
	char param[128];
	char *token, *args;
	struct skw_iwpriv_cmd *iwpriv_cmd;
	struct skw_iface *iface = (struct skw_iface *)netdev_priv(dev);

	WARN_ON(sizeof(param) < wrqu->data.length);

	if (copy_from_user(param, wrqu->data.pointer, sizeof(param))) {
		skw_err("copy failed, length: %d\n",
			wrqu->data.length);

		return -EFAULT;
	}

	skw_dbg("cmd: 0x%x, %s(len: %d)\n",
		info->cmd, param, wrqu->data.length);

	token = strchr(param, '=');
	if (!token) {
		key_len = strlen(param);
		args = NULL;
	} else {
		key_len = token - param;
		args = token + 1;
	}

	iwpriv_cmd = skw_iwpriv_cmd_match(skw_iwpriv_set_cmds, param, key_len);
	if (iwpriv_cmd)
		ret = iwpriv_cmd->handler(iface, iwpriv_cmd, args,
				extra, SKW_GET_LEN_512);
	else
		ret = skw_iwpriv_help(iface, skw_iwpriv_set_cmds, NULL,
				extra, SKW_GET_LEN_512);

	if (ret < 0)
		sprintf(extra, " usage: %s\n", iwpriv_cmd->help_info);

	wrqu->data.length = SKW_GET_LEN_512;

	skw_dbg("resp: %s\n", extra);

	return 0;
}

static int skw_iwpriv_get(struct net_device *dev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	int ret;
	char cmd[128];
	struct skw_iwpriv_cmd *priv_cmd;
	struct skw_iface *iface = (struct skw_iface *)netdev_priv(dev);

	if (copy_from_user(cmd, wrqu->data.pointer, sizeof(cmd))) {
		skw_err("copy failed, length: %d\n",
			wrqu->data.length);

		return -EFAULT;
	}

	skw_dbg("cmd: 0x%x, %s(len: %d)\n", info->cmd, cmd, wrqu->data.length);

	priv_cmd = skw_iwpriv_cmd_match(skw_iwpriv_get_cmds, cmd, strlen(cmd));
	if (priv_cmd)
		ret = priv_cmd->handler(iface, priv_cmd, NULL, extra,
				SKW_GET_LEN_512);
	else
		ret = skw_iwpriv_help(iface, skw_iwpriv_get_cmds, NULL,
				extra, SKW_GET_LEN_512);

	wrqu->data.length = SKW_GET_LEN_512;

	skw_dbg("resp: %s\n", extra);

	return ret;
}

static int skw_iwpriv_at(struct net_device *dev,
			   struct iw_request_info *info,
			   union iwreq_data *wrqu, char *extra)
{
	int ret;
	char cmd[SKW_SET_LEN_256];
	int len = wrqu->data.length;
	struct skw_core *skw = ((struct skw_iface *)netdev_priv(dev))->skw;

	BUG_ON(sizeof(cmd) < len);

	if (copy_from_user(cmd, wrqu->data.pointer, sizeof(cmd))) {
		skw_err("copy failed, length: %d\n", len);

		return -EFAULT;
	}

	skw_dbg("cmd: %s, len: %d\n", cmd, len);

	if (len + 2 > sizeof(cmd))
		return -EINVAL;

	cmd[len - 1] = 0xd;
	cmd[len + 0] = 0xa;
	cmd[len + 1] = 0x0;

	ret = skw_send_at_cmd(skw, cmd, len + 2, extra, SKW_GET_LEN_512);

	wrqu->data.length = SKW_GET_LEN_512;

	skw_dbg("resp: %s", extra);

	return ret;
}

static struct iw_priv_args skw_iw_priv_args[] = {
	{
		SKW_IW_PRIV_SET,
		IW_PRIV_TYPE_CHAR | SKW_SET_LEN_128,
		IW_PRIV_TYPE_CHAR | SKW_GET_LEN_512,
		"set",
	},
	{
		SKW_IW_PRIV_GET,
		IW_PRIV_TYPE_CHAR | SKW_SET_LEN_128,
		IW_PRIV_TYPE_CHAR | SKW_GET_LEN_512,
		"get",
	},
	{
		SKW_IW_PRIV_AT,
		IW_PRIV_TYPE_CHAR | SKW_SET_LEN_256,
		IW_PRIV_TYPE_CHAR | SKW_GET_LEN_512,
		"at",
	},
	{
		SKW_IW_PRIV_80211MODE,
		IW_PRIV_TYPE_CHAR | SKW_SET_LEN_128,
		IW_PRIV_TYPE_CHAR | SKW_GET_LEN_512,
		"mode",
	},
	{
		SKW_IW_PRIV_GET_80211MODE,
		IW_PRIV_TYPE_CHAR | SKW_SET_LEN_128,
		IW_PRIV_TYPE_CHAR | SKW_GET_LEN_512,
		"get_mode",
	},
	{
		SKW_IW_PRIV_KEEP_ALIVE,
		IW_PRIV_TYPE_CHAR | SKW_SET_LEN_1024,
		IW_PRIV_TYPE_CHAR | SKW_GET_LEN_512,
		"keep_alive",
	},
	{
		SKW_IW_PRIV_WOW_FILTER,
		IW_PRIV_TYPE_CHAR | SKW_SET_LEN_512,
		IW_PRIV_TYPE_CHAR | SKW_GET_LEN_512,
		"wow_filter",
	},
	{0, 0, 0, {0}}
};

static const iw_handler skw_iw_priv_handlers[] = {
	NULL,
	skw_iwpriv_set,
	NULL,
	skw_iwpriv_get,
	NULL,
	skw_iwpriv_at,
	skw_iwpriv_mode,
	skw_iwpriv_get_mode,
	skw_iwpriv_keep_alive,
	skw_iwpriv_wow_filter,
};
#endif

static const struct iw_handler_def skw_iw_ops = {
	.standard = skw_iw_standard_handlers,
	.num_standard = ARRAY_SIZE(skw_iw_standard_handlers),
#ifdef CONFIG_WEXT_PRIV
	.private = skw_iw_priv_handlers,
	.num_private = ARRAY_SIZE(skw_iw_priv_handlers),
	.private_args = skw_iw_priv_args,
	.num_private_args = ARRAY_SIZE(skw_iw_priv_args),
#endif
	.get_wireless_stats = skw_get_wireless_stats,
};

const void *skw_iw_handlers(void)
{
#ifdef CONFIG_WIRELESS_EXT
	return &skw_iw_ops;
#else
	skw_info("CONFIG_WIRELESS_EXT not enabled\n");
	return NULL;
#endif
}
