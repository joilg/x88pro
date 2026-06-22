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

#include <net/cfg80211.h>
#include <net/genetlink.h>

#include "skw_vendor.h"
#include "skw_cfg80211.h"
#include "skw_core.h"
#include "skw_iface.h"
#include "skw_util.h"
#include "skw_regd.h"
#include "version.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
#if 0
static int skw_vendor_dbg_reset_logging(struct wiphy *wiphy,
	struct wireless_dev *wdev, const void  *data, int len)
{
	int ret = SKW_OK;

	skw_dbg("Enter\n");

	return ret;
}

static int skw_vendor_set_p2p_rand_mac(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void *data, int len)
{
	int type;
	//struct skw_iface *iface = netdev_priv(wdev->netdev);
	u8 mac_addr[6] = {0};

	skw_dbg("set skw mac addr\n");
	type = nla_type(data);

	if (type == SKW_ATTR_DRIVER_RAND_MAC) {
		memcpy(mac_addr, nla_data(data), 6);
		skw_dbg("mac:%pM\n", mac_addr);
	}

	return 0;
}

static int skw_vendor_set_rand_mac_oui(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void *data, int len)
{
	char *oui = nla_data(data);
	struct skw_iface *iface = SKW_WDEV_TO_IFACE(wdev);

	if (!oui || (nla_len(data) != DOT11_OUI_LEN))
		return -EINVAL;

	skw_dbg("%02x:%02x:%02x\n", oui[0], oui[1], oui[2]);

	memcpy(iface->rand_mac_oui, oui, DOT11_OUI_LEN);

	return 0;
}
#endif

static int skw_vendor_cmd_reply(struct wiphy *wiphy, const void *data, int len)
{
	struct sk_buff *skb;

	/* Alloc the SKB for vendor_event */
	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, len);
	if (unlikely(!skb)) {
		skw_err("skb alloc failed");
		return -ENOMEM;
	}

	/* Push the data to the skb */
	nla_put_nohdr(skb, len, data);

	return cfg80211_vendor_cmd_reply(skb);
}

static int skw_vendor_start_logging(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void  *data, int len)
{
	return 0;
}

static int skw_vendor_set_hal_started(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void  *data, int len)
{
	return 0;
}

static int skw_vendor_set_hal_stop(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void  *data, int len)
{
	return 0;
}

static int skw_vendor_set_hal_pid(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void  *data, int len)
{
	return 0;
}

static int skw_vendor_get_wake_reason_stats(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void *data, int len)
{
	return 0;
}

static int skw_vendor_get_apf_capabilities(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void *data, int len)
{
	struct sk_buff *skb;

#define SKW_ATTR_APF_VERSION      0
#define SKW_ATTR_APF_MAX_LEN      1

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, NLMSG_DEFAULT_SIZE);
	if (!skb)
		return -ENOMEM;

	if (nla_put_u32(skb, SKW_ATTR_APF_VERSION, 4) ||
	    nla_put_u32(skb, SKW_ATTR_APF_MAX_LEN, 1024))
		return -ENOMEM;

	return cfg80211_vendor_cmd_reply(skb);
}

static int skw_vendor_get_ring_buffer_data(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void  *data, int len)
{
	return 0;
}

static int skw_vendor_get_firmware_dump(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void  *data, int len)
{
	return -3; /* WIFI_ERROR_NOT_SUPPORTED */
}

static int skw_vendor_select_tx_power_scenario(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void *data, int len)
{
	return -3; /* WIFI_ERROR_NOT_SUPPORTED */
}

static int skw_vendor_set_latency_mode(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void *data, int len)
{
	return -3; /* WIFI_ERROR_NOT_SUPPORTED */
}

static int skw_vendor_get_feature_set(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void *data, int len)
{
	u32 feature_set = 0;

	/* Hardcoding these values for now, need to get
	 * these values from FW, will change in a later check-in
	 */
	feature_set |= WIFI_FEATURE_INFRA;
	feature_set |= WIFI_FEATURE_INFRA_5G;
	feature_set |= WIFI_FEATURE_P2P;
	feature_set |= WIFI_FEATURE_SOFT_AP;
	feature_set |= WIFI_FEATURE_AP_STA;
	//feature_set |= WIFI_FEATURE_TDLS;
	//feature_set |= WIFI_FEATURE_TDLS_OFFCHANNEL;
	//feature_set |= WIFI_FEATURE_NAN;
	//feature_set |= WIFI_FEATURE_HOTSPOT;
	//feature_set |= WIFI_FEATURE_LINK_LAYER_STATS; //TBC
	//feature_set |= WIFI_FEATURE_RSSI_MONITOR; //TBC with roaming
	//feature_set |= WIFI_FEATURE_MKEEP_ALIVE; //TBC compare with QUALCOM
	//feature_set |= WIFI_FEATURE_CONFIG_NDO; //TBC
	//feature_set |= WIFI_FEATURE_SCAN_RAND;
	//feature_set |= WIFI_FEATURE_RAND_MAC;
	//feature_set |= WIFI_FEATURE_P2P_RAND_MAC ;
	//feature_set |= WIFI_FEATURE_CONTROL_ROAMING;

	skw_dbg("feature: 0x%x\n", feature_set);

	return skw_vendor_cmd_reply(wiphy, &feature_set, sizeof(u32));
}

static int skw_vendor_set_country(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void *data, int data_len)
{
	char *country = nla_data(data);

	if (!country)
		return -EINVAL;

	skw_dbg("country: %c%c\n", country[0], country[1]);

	return skw_set_regdom(wiphy, country);
}

static int skw_vendor_get_ver(struct wiphy *wiphy, struct wireless_dev *wdev,
			const void *data, int len)
{
	char version[256] = {0};
	struct skw_core *skw = wiphy_priv(wiphy);

	switch (nla_type(data)) {
	case SKW_ATTR_VER_DRIVER:
		strncpy(version, SKW_VERSION, sizeof(version));
		break;

	case SKW_ATTR_VER_FIRMWARE:
		snprintf(version, sizeof(version), "%s-%s",
			 skw->fw.plat_ver, skw->fw.wifi_ver);
		break;

	default:
		skw_err("invalid nla type\n");
		strcpy(version, "invalid");
		break;
	}

	return skw_vendor_cmd_reply(wiphy, version, sizeof(version));
}

static int skw_vendor_get_usable_chans(struct wiphy *wiphy,
			struct wireless_dev *wdev, const void *data, int len)
{
	int i, nr, max;
	struct sk_buff *skb;
	enum nl80211_band band;
	struct skw_usable_chan *chans;
	struct skw_usable_chan_info *info = (struct skw_usable_chan_info *)data;

	skw_dbg("band_mask: 0x%x\n", info->band_mask);

	max = ieee80211_get_num_supported_channels(wiphy);

	chans = SKW_ALLOC(max * sizeof(*chans), GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, NLMSG_DEFAULT_SIZE);
	if (!skb) {
		SKW_KFREE(chans);
		return -ENOMEM;
	}

	for (nr = 0, band = 0; band < NUM_NL80211_BANDS; band++) {
		if (!(info->band_mask & BIT(to_skw_band(band))) ||
		    !wiphy->bands[band])
			continue;

		for (i = 0; i < wiphy->bands[band]->n_channels; i++) {
			struct ieee80211_channel *chan;

			chan = &wiphy->bands[band]->channels[i];

			if (chan->flags & IEEE80211_CHAN_DISABLED)
				continue;

			chans[nr].center_freq = chan->center_freq;
			chans[nr].band_width = SKW_CHAN_WIDTH_20;
			chans[nr].iface_mode_mask = BIT(SKW_STA_MODE) |
						    BIT(SKW_AP_MODE) |
						    BIT(SKW_GC_MODE) |
						    BIT(SKW_GO_MODE);
			nr++;
		}
	}

	if (nla_put_nohdr(skb, nr * sizeof(*chans), chans)) {
		SKW_KFREE(chans);
		kfree_skb(skb);
		return -ENOMEM;
	}

	SKW_KFREE(chans);
	return cfg80211_vendor_cmd_reply(skb);
}

static int skw_vendor_get_channel_list(struct wiphy *wiphy,
		struct wireless_dev *wdev, const void *data, int len)
{
#define SKW_ATTR_CHANNELS_COUNT     36
#define SKW_ATTR_CHANNELS_LIST      37
	int channels[32], size;
	int i, band, nr_channels;
	struct sk_buff *skb;

	band = nla_get_u32(data);
	if (band > NL80211_BAND_5GHZ) {
		skw_err("invalid band: %d\n", band);
		return -EINVAL;
	}

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, NLMSG_DEFAULT_SIZE);
	if (!skb)
		return -ENOMEM;

	nr_channels = wiphy->bands[band]->n_channels;
	size = nr_channels * sizeof(int);

	for (i = 0; i < nr_channels; i++)
		channels[i] = wiphy->bands[band]->channels[i].hw_value;

	if (nla_put_u32(skb, SKW_ATTR_CHANNELS_COUNT, nr_channels) ||
	    nla_put(skb, SKW_ATTR_CHANNELS_LIST, size, channels)) {
		kfree_skb(skb);

		return -ENOMEM;
	}

	return cfg80211_vendor_cmd_reply(skb);
}

static int skw_vendor_get_ring_buffers_status(struct wiphy *wiphy,
			struct wireless_dev *wdev, const void  *data, int len)
{
#define SKW_ATTR_RING_BUFFERS_STATUS    13
#define SKW_ATTR_RING_BUFFERS_COUNT     14
	struct sk_buff *skb;
	struct skw_ring_buff_status status = {
		.name = "skw_drv",
		.flags = 0,
		.ring_id = 0,
		.ring_buffer_byte_size = 1024,
		.verbose_level = 0,
		.written_bytes = 0,
		.read_bytes = 0,
		.written_records = 0,
	};

	skb = cfg80211_vendor_cmd_alloc_reply_skb(wiphy, NLMSG_DEFAULT_SIZE);
	if (!skb)
		return -ENOMEM;

	if (nla_put_u32(skb, SKW_ATTR_RING_BUFFERS_COUNT, 1) ||
	    nla_put(skb, SKW_ATTR_RING_BUFFERS_STATUS, sizeof(status), &status))
		return -ENOMEM;

	return cfg80211_vendor_cmd_reply(skb);
}

static int skw_vendor_get_logger_feature(struct wiphy *wiphy,
			struct wireless_dev *wdev, const void  *data, int len)
{
	u32 features = 0;

	skw_dbg("features: 0x%x\n", features);

	return skw_vendor_cmd_reply(wiphy, &features, sizeof(features));
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
#if 0
const struct nla_policy skw_drv_attr_policy[SKW_ATTR_DRIVER_MAX] = {
	[SKW_ATTR_DRIVER_RAND_MAC] = { .type = NLA_BINARY, .len = 6 },
};
#endif

const struct nla_policy skw_set_hal_policy[SKW_ATTR_SET_HAL_MAX] = {
	[0] = {.strict_start_type = 0},
	[SKW_ATTR_SET_HAL_DEINIT] = {.type = NLA_UNSPEC},
	[SKW_ATTR_SET_HAL_INIT] = {.type = NLA_NUL_STRING},
	[SKW_ATTR_SET_HAL_PID] = {.type = NLA_U32},
};

const struct nla_policy skw_get_version_policy[SKW_ATTR_VER_MAX] = {
	[SKW_ATTR_VER_DRIVER] = {.type = NLA_U32},
	[SKW_ATTR_VER_FIRMWARE] = {.type = NLA_U32},
};

const struct nla_policy skw_dbg_policy[SKW_ATTR_DEBUG_MAX] = {
	[SKW_ATTR_RING_ID] = {.type = NLA_U32},
	[SKW_ATTR_RING_NAME] = {.type = NLA_NUL_STRING},
	[SKW_ATTR_RING_FLAGS] = {.type = NLA_U32},
	[SKW_ATTR_LOG_LEVEL] = {.type = NLA_U32},
	[SKW_ATTR_LOG_TIME_INTVAL] = {.type = NLA_U32},
	[SKW_ATTR_LOG_MIN_DATA_SIZE] = {.type = NLA_U32},
	[SKW_ATTR_FW_DUMP_LEN] = {.type = NLA_U32},
	[SKW_ATTR_FW_DUMP_DATA] = {.type = NLA_U64},
	[SKW_ATTR_FW_ERR_CODE] = {.type = NLA_U32},
	[SKW_ATTR_RING_DATA] = {.type = NLA_BINARY},
	[SKW_ATTR_RING_STATUS] = {.type = NLA_BINARY},
	[SKW_ATTR_RING_NUM] = {.type = NLA_U32},
};

#endif

#define SKW_VENDOR_DEFAULT_FLAGS (WIPHY_VENDOR_CMD_NEED_WDEV |      \
				  WIPHY_VENDOR_CMD_NEED_NETDEV)

#define SKW_CMD_INFO(OUI, CMD)                                      \
	{                                                           \
		.vendor_id = OUI,                                   \
		.subcmd = CMD,                                      \
	}

static struct wiphy_vendor_command skw_vendor_cmds[] = {
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_CHANNEL_LIST),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_channel_list,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_FEATURE_SET),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_feature_set,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_VERSION),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_ver,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
		.policy = skw_get_version_policy,
		.maxattr = SKW_ATTR_VER_MAX,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_RING_BUFFERS_STATUS),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_ring_buffers_status,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_START_LOGGING),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_start_logging,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = skw_dbg_policy,
		.maxattr = SKW_ATTR_DEBUG_MAX,
#endif
	},
#if 0
	{
		.info = SKW_CMD_INFO(OUI_BRCM, SKW_VENDOR_SUBCMD_SET_MAC),
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV,
		.doit = skw_vendor_set_p2p_rand_mac,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
		.policy = skw_drv_attr_policy,
		.maxattr = SKW_ATTR_DRIVER_MAX
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, DEBUG_RESET_LOGGING),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_dbg_reset_logging,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = skw_dbg_policy,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, ANDR_WIFI_RANDOM_MAC_OUI),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_set_rand_mac_oui,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
#endif
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_LOGGER_FEATURE),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_logger_feature,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_SET_COUNTRY),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_set_country,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_SET_HAL_START),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_set_hal_started,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = skw_set_hal_policy,
		.maxattr = SKW_ATTR_SET_HAL_MAX
#endif /* LINUX_VERSION >= 5.3 */
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_SET_HAL_STOP),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_set_hal_stop,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = skw_set_hal_policy,
		.maxattr = SKW_ATTR_SET_HAL_MAX
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_SET_HAL_PID),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_set_hal_pid,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = skw_set_hal_policy,
		.maxattr = SKW_ATTR_SET_HAL_MAX
#endif /* LINUX_VERSION >= 5.3 */
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_FIRMWARE_DUMP),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_firmware_dump,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_RING_BUFFER_DATA),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_ring_buffer_data,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
		.policy = skw_dbg_policy,
		.maxattr = SKW_ATTR_DEBUG_MAX,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_WAKE_REASON_STATS),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_wake_reason_stats,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_APF_CAPABILITIES),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_apf_capabilities,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_SELECT_TX_POWER_SCENARIO),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_select_tx_power_scenario,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_SET_LATENCY_MODE),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_set_latency_mode,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
	{
		.info = SKW_CMD_INFO(OUI_GOOGLE, SKW_VCMD_GET_USABLE_CHANS),
		.flags = SKW_VENDOR_DEFAULT_FLAGS,
		.doit = skw_vendor_get_usable_chans,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
		.policy = VENDOR_CMD_RAW_DATA,
#endif
	},
};

static struct nl80211_vendor_cmd_info skw_vendor_events[] = {
	{
		.vendor_id = 0,
		.subcmd = 0,
	},
};

#endif

void skw_vendor_init(struct wiphy *wiphy)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
	wiphy->vendor_commands = skw_vendor_cmds;
	wiphy->n_vendor_commands = ARRAY_SIZE(skw_vendor_cmds);
	wiphy->vendor_events = skw_vendor_events;
	wiphy->n_vendor_events = ARRAY_SIZE(skw_vendor_events);
#endif
}

void skw_vendor_deinit(struct wiphy *wiphy)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
	wiphy->vendor_commands = NULL;
	wiphy->n_vendor_commands = 0;
#endif
}
