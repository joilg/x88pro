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

#ifndef __SKW_CFG80211_H__
#define __SKW_CFG80211_H__

#include <linux/ieee80211.h>
#include "skw_msg.h"
#include "skw_iface.h"

#define SKW_CONNECT_TIMEOUT               msecs_to_jiffies(4000)
#define SKW_STEP_TIMEOUT                  msecs_to_jiffies(500)
#define SKW_MAX_AUTH_ASSOC_RETRY_NUM      3
#define SKW_MAX_SCAN_SSID                 4
#define SKW_SCAN_TIMEOUT                  8000
#define SKW_CQM_SCAN_TIMEOUT              4
#define SKW_MAX_STA_AUTH_ASSOC_RETRY      3
#define SKW_EXTENDED_CAPA_LEN             11

/* hostap mac acl mode */
#define SKW_MAX_ACL_ENTRIES               16

#define SKW_WOW_DISCONNECT                BIT(0)
#define SKW_WOW_MAGIC_PKT                 BIT(1)
#define SKW_WOW_GTK_REKEY_FAIL            BIT(2)
#define SKW_WOW_EAP_IDENTITY_REQ          BIT(3)
#define SKW_WOW_FOUR_WAY_HANDSHAKE        BIT(4)
#define SKW_WOW_RFKILL_RELEASE            BIT(5)
#define SKW_WOW_BLACKLIST_FILTER          BIT(31)
#define SKW_WOW_ANY_PKT                   0xff

enum SKW_IP_VERSION {
	SKW_IP_IPV4 = 0,
	SKW_IP_IPV6,
};

struct skw_bss_priv {
	u8 bssid_index;
	u8 max_bssid_indicator;
	u16 resv;
};

struct skw_suspend_t {
	u8 wow_enable;
	u8 reserved;
	/* reference SKW_WOW_*,
	 * set wow_flags to 0 if wakeup any
	 */
	u16 wow_flags;
};

#define SKW_SUITE(oui, id)                  (((oui) << 8) | (id))
#define SKW_CIPHER_SUITE_WEP40              SKW_SUITE(0x000FAC, 1)
#define SKW_CIPHER_SUITE_TKIP               SKW_SUITE(0x000FAC, 2)
#define SKW_CIPHER_SUITE_CCMP               SKW_SUITE(0x000FAC, 4)
#define SKW_CIPHER_SUITE_WEP104             SKW_SUITE(0x000FAC, 5)
#define SKW_CIPHER_SUITE_AES_CMAC           SKW_SUITE(0x000FAC, 6)
#define SKW_CIPHER_SUITE_GCMP               SKW_SUITE(0x000FAC, 8)
#define SKW_CIPHER_SUITE_GCMP_256           SKW_SUITE(0x000FAC, 9)
#define SKW_CIPHER_SUITE_CCMP_256           SKW_SUITE(0x000FAC, 10)
#define SKW_CIPHER_SUITE_BIP_GMAC_128       SKW_SUITE(0x000FAC, 11)
#define SKW_CIPHER_SUITE_BIP_GMAC_256       SKW_SUITE(0x000FAC, 12)
#define SKW_CIPHER_SUITE_BIP_CMAC_256       SKW_SUITE(0x000FAC, 13)

#define SKW_CIPHER_SUITE_SMS4               SKW_SUITE(0x001472, 1)

enum SKW_CIPHER_TYPE {
	SKW_CIPHER_TYPE_INVALID = 0,
	SKW_CIPHER_TYPE_WEP40 = 1,
	SKW_CIPHER_TYPE_WEP104 = 2,
	SKW_CIPHER_TYPE_TKIP = 3,
	SKW_CIPHER_TYPE_SMS4 = 4,
	SKW_CIPHER_TYPE_CCMP = 8,
	SKW_CIPHER_TYPE_CCMP_256 = 9,
	SKW_CIPHER_TYPE_GCMP = 10,
	SKW_CIPHER_TYPE_GCMP_256 = 11,
	SKW_CIPHER_TYPE_AES_CMAC = 12, /* BIP_CMAC_128 */
	SKW_CIPHER_TYPE_BIP_CMAC_256 = 13,
	SKW_CIPHER_TYPE_BIP_GMAC_128 = 14,
	SKW_CIPHER_TYPE_BIP_GMAC_256 = 15,
};

enum SKW_MIB_ID {
	SKW_MIB_RTS_THRESHOLD = 1,
	SKW_MIB_FRAG_THRESHOLD,
	SKW_MIB_COVERAGE_CLASS,
	SKW_MIB_RETRY_SHORT,
	SKW_MIB_RETRY_LONG,
	SKW_MIB_DYN_ACK,
	SKW_MIB_TXQ_LIMIT,
	SKW_MIB_TXQ_MEMORY_LIMIT,
	SKW_MIB_TXQ_QUANTUM,
	SKW_MIB_DOT11_OMI,
	SKW_MIB_DOT11_MODE_B,
	SKW_MIB_DOT11_MODE_G,
	SKW_MIB_DOT11_MODE_A,
	SKW_MIB_DOT11_MODE_HT,
	SKW_MIB_DOT11_MODE_VHT,
	SKW_MIB_DOT11_MODE_HE,
	SKW_MIB_DOT11_CBW_20M,
	SKW_MIB_DOT11_CBW_40M_ABOVE,
	SKW_MIB_DOT11_CBW_40M_BELOW,
	SKW_MIB_DOT11_CBW_80M,
	SKW_MIB_DOT11_CBW_160M,
	SKW_MIB_DOT11_CBW_80P80M,
	SKW_MIB_SET_BAND_2G,
	SKW_MIB_SET_BAND_5G,

	SKW_MIB_LAST
};

enum SKW_CHAN_BW_INFO {
	SKW_CHAN_WIDTH_20,
	SKW_CHAN_WIDTH_40,
	SKW_CHAN_WIDTH_80,
	SKW_CHAN_WIDTH_80P80,
	SKW_CHAN_WIDTH_160,

	SKW_CHAN_WIDTH_MAX,
};

enum SKW_MONITOR_MODE {
	SKW_MONITOR_CLOSE,
	SKW_MONITOR_COMMON,
	SKW_MONITOR_MAC_CAP,
	SKW_MONITOR_PHY_CAP,
	SKW_MONITOR_MAX,
};

#define SKW_BW_2GHZ_20M             BIT(0)
#define SKW_BW_2GHZ_40M             BIT(1)
#define SKW_BW_5GHZ_20M             BIT(2)
#define SKW_BW_5GHZ_40M             BIT(3)
#define SKW_BW_5GHZ_80M             BIT(4)
#define SKW_BW_5GHZ_160M            BIT(5)
#define SKW_BW_5GHZ_8080M           BIT(6)

enum SKW_CMD_DISCONNECT_TYPE_E {
	SKW_DISCONNECT_ONLY = 0,
	SKW_DISCONNECT_SEND_DISASSOC = 1,
	SKW_DISCONNECT_SEND_DEAUTH = 2,
};

#define SKW_SCAN_FLAG_RND_MAC         BIT(0)
#define SKW_SCAN_FLAG_ACS             BIT(1)
#define SKW_SCAN_FLAG_PASSIVE         BIT(15)

struct skw_scan_param {
	u16 flags;  /* reference SKW_SCAN_FLAG_ */
	u8 rand_mac[6];
	u32 nr_chan;
	u32 chan_offset;
	u32 n_ssid;
	u32 ssid_offset;
	u32 ie_len;
	u32 ie_offset;
	u8 ie[];
} __packed;

struct skw_sched_match_sets {
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u16 ssid_len;
	u8 bssid[ETH_ALEN];
	s32 rssi_thold;
} __packed;

struct skw_sched_scan_param {
	u32 req_id;
	u32 flags;
	s32 min_rssi_thold;
	u32 delay;
	u8 mac_addr[ETH_ALEN];
	u8 mac_addr_mask[ETH_ALEN];
	u8 relative_rssi_set;
	s8 relative_rssi;
	u8 scan_width;

	u8 n_ssids;
	u32 n_ssids_len;
	u32 n_ssid_offset;

	u32 ie_len;
	u32 ie_offset;

	u8 n_channels;
	u32 channels_len;
	u32 channels_offset;

	u8 n_match_sets;
	u32 match_sets_len;
	u32 match_sets_offset;

	u8 n_scan_plans;
	u32 scan_plans_len;
	u32 scan_plans_offset;
	u8 data[0];
} __packed;

struct skw_center_chn {
	u32 bw;
	u16 center_chn1;
	u16 center_chn2;
};

struct skw_he_cap_elem {
	u8 mac_cap_info[6];
	u8 phy_cap_info[11];
	u16 rx_mcs_map;
	u16 tx_mcs_map;
	u32 ppe;
} __packed;

struct skw_he_oper_param {
	u16 default_pe_dur:3;
	u16 twt_req:1;
	u16 txop_dur_rts_thr:10;
	u16 vht_opt_info_pre:1;
	u16 co_hosted_bss:1;
	u8 er_su_disable:1;
	u8 opt_info_6g_pre:1;
	u8 reserved:6;
} __packed;

struct skw_he_oper_elem {
	struct skw_he_oper_param he_param;
	u8 bss_color:6;
	u8 partial_bss_color:1;
	u8 bss_color_disabled:1;
	u8 basic_mcs_nss[2];
	//u8 vht_opt_info[3];
	//u8 max_cohosted_bssid_ind[1];
	//u8 opt_info_6g[5];
} __packed;

struct skw_join_param {
	u8 chan_num;
	u8 center_chn1;
	u8 center_chn2;
	u8 bandwidth;
	u16 beacon_interval;
	u16 capability;
	u8 bssid_index;
	u8 max_bssid_indicator;
	u8 bssid[6];
	u16 roaming:1;
	u16 reserved:15;
	u16 bss_ie_offset;
	u32 bss_ie_len;
	u8 bss_ie[];
} __packed;

struct skw_join_resp {
	u8 peer_idx;
} __packed;

struct skw_auth_param {
	u16 auth_algorithm;
	u16 key_data_offset;
	u16 key_data_len;
	u16 auth_data_offset;
	u16 auth_data_len;
	u16 auth_ie_offset;
	u16 auth_ie_len;
	u8  data[];
} __packed;

struct skw_assoc_req_param {
	struct ieee80211_ht_cap ht_capa;
	struct ieee80211_vht_cap  vht_capa;
	u8 bssid[6];
	u8 pre_bssid[6];
	u16 req_ie_offset;
	u16 req_ie_len;
	u8  req_ie[];
} __packed;

struct skw_disconnect_param {
	u8 type;
	u8 local_state_change;
	u16 reason_code;
	u16 ie_offset;
	u16 ie_len;
	u8 ie[];
} __packed;

struct skw_ibss_params {
	/*
	 * 0: join ibss
	 * 1: create ibss
	 */
	u8 type;
	u8 chan;
	u8 bw;
	u8 center_chan1;
	u8 center_chan2;

	u8 ssid_len;
	u8 ssid[32];

	u8 bssid[ETH_ALEN];
	u16 atim_win;
	u16 beacon_int;
} __packed;

enum SKW_KEY_TYPE {
	SKW_KEY_TYPE_PTK = 0,
	SKW_KEY_TYPE_GTK = 1,
	SKW_KEY_TYPE_IGTK = 2,
	SKW_KEY_TYPE_BIGTK = 3,
};

struct skw_key_params {
	u8 mac_addr[ETH_ALEN];
	u8 key_type;
	u8 cipher_type;
	u8 pn[6];
	u8 key_id;
	u8 key_len;
	u8 key[WLAN_MAX_KEY_LEN];
} __packed;

struct skw_startap_param {
	int beacon_int;
	u8 dtim_period;
	u8 flags; /* reference SKW_AP_FLAGS_* */
	u16 chan;
	u8 chan_width;
	u8 center_chn1;
	u8 center_chn2;
	u8 ssid_len;
	u8 ssid[32];

	u16 beacon_head_offset;
	u16 beacon_head_len;
	u16 beacon_tail_offset;
	u16 beacon_tail_len;
	u16 beacon_ies_offset;
	u16 beacon_ies_len;

	u16 probe_rsp_ies_offset;
	u16 probe_rsp_ies_len;
	u16 assoc_rsp_ies_offset;
	u16 assoc_rsp_ies_len;
	u8 ies[0];
} __packed;

struct skw_startap_resp {
};

//TBD: put skw_beacon_param into skw_startp_param
struct skw_beacon_params {
	u16 beacon_head_offset;
	u16 beacon_head_len;
	u16 beacon_tail_offset;
	u16 beacon_tail_len;
	u16 beacon_ies_offset;
	u16 beacon_ies_len;

	u16 probe_rsp_ies_offset;
	u16 probe_rsp_ies_len;
	u16 assoc_rsp_ies_offset;
	u16 assoc_rsp_ies_len;
	u16 probe_rsp_offset;
	u16 probe_rsp_len;
	u8 ies[0];
} __packed;

struct skw_del_sta_param {
	u8 mac[6];
	u16 reason_code;
	u8 tx_frame;
} __packed;

enum skw_rate_info_bw {
	SKW_RATE_INFO_BW_20,
	SKW_RATE_INFO_BW_40,
	SKW_RATE_INFO_BW_80,
	SKW_RATE_INFO_BW_HE_RU = 15,
};

enum skw_rate_info_flags {
	SKW_RATE_INFO_FLAGS_LEGACY,
	SKW_RATE_INFO_FLAGS_HT,
	SKW_RATE_INFO_FLAGS_VHT,
	SKW_RATE_INFO_FLAGS_HE,
};

struct skw_get_sta_resp {
	struct skw_rate tx_rate;
	s8 signal;
	u8 noise;
	u8 tx_psr;
	u32 tx_failed;
	u16 filter_cnt[35];
	u16 filter_drop_offload_cnt[35];
} __packed;

struct skw_roc_param {
	u8 enable;
	u16 channel_num;
	u16 channel_type;
	u32 duration;
	u64 cookie;
} __packed;

struct skw_mgmt_tx_param {
	u32 wait;
	u64 cookie;
	u8 channel;
	u8 dont_wait_for_ack;
	u16 mgmt_frame_len;
	struct ieee80211_mgmt mgmt[0];
} __packed;

struct skw_mgmt_register_param {
	u16 frame_type;
	u8 reg;
	u8 resv[5];
	u64 timestamp;
} __packed;

struct skw_station_params {
	u8 mac[ETH_ALEN];
	u16 resv;

	u64 timestamp;
};

#define SKW_CQM_DEFAUT_RSSI_THOLD	(-70)
#define SKW_CQM_DEFAUT_RSSI_HYST	(40)

struct skw_set_cqm_rssi_param {
	s32 rssi_thold;
	u8 rssi_hyst;
} __packed;

enum SKW_SCAN_TYPE {
	SKW_SCAN_IDLE,
	SKW_SCAN_NORMAL,
	SKW_SCAN_SCHED,
	SKW_SCAN_BG,
	SKW_SCAN_AUTO,
	SKW_SCAN_ROAM,
#ifdef RRM_SCAN_SUPPORT
	SKW_SCAN_RRM,
#endif
};

enum SKW_CQM_STATUS {
	CQM_STATUS_RSSI_LOW = 1,
	CQM_STATUS_RSSI_HIGH = 2,
	CQM_STATUS_BEACON_LOSS = 3,
	CQM_STATUS_TDLS_LOSS = 4,
};

enum SKW_MPDU_DESC_GI {
	DESC_GI_04 = 0,
	DESC_GI_08 = 1,
	DESC_GI_16 = 2,
	DESC_GI_32 = 3,
};

/* define same as cp get station rate bw */
#define SKW_DESC_BW_USED_RU     15

/* define same as cp get station tx gi*/
enum SKW_HE_GI {
	SKW_HE_GI_3_2 = 0,
	SKW_HE_GI_1_6 = 1,
	SKW_HE_GI_0_8 = 2,
};

/* define same as cp get station tx gi*/
enum SKW_HTVHT_GI {
	SKW_HTVHT_GI_0_8 = 0,
	SKW_HTVHT_GI_0_4 = 1,
};

struct skw_cqm_info {
	u8 cqm_status;
	s16 cqm_rssi;
	u8 bssid[ETH_ALEN];
	u8 chan;
} __packed;

struct skw_del_sta {
	u8 reason_code;
	u8 mac[ETH_ALEN];
} __packed;

struct skw_mic_failure {
	u8 is_mcbc;
	u8 key_id;
	u8 mac[ETH_ALEN];
} __packed;

struct skw_tdls_oper {
	u16 oper; /* reference enum nl80211_tdls_operation */
	u8 peer_addr[ETH_ALEN];
};

struct skw_ts_info {
	u8 tsid;
	u8 up;
	u8 peer[ETH_ALEN];
	__le16 admitted_time;
} __packed;

struct skw_tdls_chan_switch {
	u8 addr[6];
	u8 chn_switch_enable;  /* 0: disable, 1: enable */
	u8 oper_class;
	u16 chn;
	u8 band;               /* enum nl80211_band */
	u8 chan_width;         /* enum skw_chan_width */
};

struct skw_setip_param {
	u8 ip_type;
	union {
		__be32 ipv4;
		u8 ipv6[16];
	};
} __packed;

#define SKW_CONN_FLAG_ASSOCED            BIT(0)
#define SKW_CONN_FLAG_KEY_VALID          BIT(1)
#define SKW_CONN_FLAG_USE_MFP            BIT(2)
#define SKW_CONN_FLAG_AUTH_AUTO          BIT(3)
#define SKW_CONN_FLAG_SAE_AUTH           BIT(4)

struct skw_connect_param {
	struct mutex lock;

	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u16 ssid_len;
	u8 bssid[ETH_ALEN];

	u8 key[32];
	u8 key_len, key_idx;

	u8 prev_bssid[ETH_ALEN];

	enum SKW_STATES state;
	enum nl80211_auth_type auth_type;

	u32 flags; /* reference SKW_CONN_FLAG_ */

	u8 *assoc_ie;
	size_t assoc_ie_len;

	struct ieee80211_ht_cap ht_capa, ht_capa_mask;
	struct ieee80211_vht_cap vht_capa, vht_capa_mask;

	struct ieee80211_channel *channel;

	struct cfg80211_crypto_settings crypto;
};

enum SKW_BAND {
	SKW_BAND_2GHZ,
	SKW_BAND_5GHZ,
	SKW_BAND_6GHZ,
	SKW_BAND_60GHZ,

	SKW_BAND_INVALD,
};

static inline enum SKW_BAND to_skw_band(enum nl80211_band band)
{
	enum SKW_BAND new_band;

	switch (band) {
	case NL80211_BAND_2GHZ:
		new_band = SKW_BAND_2GHZ;
		break;

	case NL80211_BAND_5GHZ:
		new_band = SKW_BAND_5GHZ;
		break;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	case NL80211_BAND_6GHZ:
		new_band = SKW_BAND_6GHZ;
		break;
#endif

	default:
		new_band = SKW_BAND_INVALD;
		break;
	}

	return new_band;
}

static inline struct skw_bss_priv *skw_bss_priv(struct cfg80211_bss *bss)
{
	return (struct skw_bss_priv *)bss->priv;
}

static inline int skw_to_freq(u16 chan)
{
	enum nl80211_band band;

	band = chan > 14 ? NL80211_BAND_5GHZ : NL80211_BAND_2GHZ;
	return ieee80211_channel_to_frequency(chan, band);
}

static inline void skw_join_resp_handler(struct skw_core *skw,
					 struct skw_iface *iface,
					 struct skw_join_resp *resp)
{
	SKW_BUG_ON(skw_lmac_bind_iface(iface->skw, iface, 0));
	iface->default_multicast = resp->peer_idx;
}

static inline void skw_startap_resp_handler(struct skw_core *skw,
					    struct skw_iface *iface,
					    struct skw_startap_resp *resp)
{
	SKW_BUG_ON(skw_lmac_bind_iface(iface->skw, iface, 0));
	iface->default_multicast = iface->id;
}

static inline u8 skw_desc_nss_to_nss_num(u8 cp_nss_idx)
{
	/* convert cp nss index to nss number */
	return cp_nss_idx + 1;
}

static inline enum SKW_HE_GI skw_desc_he_gi_to_skw_gi(enum SKW_MPDU_DESC_GI desc_gi)
{
	enum SKW_HE_GI gi = 0;

	/* convert cp desc gi to SKW_HE_GI */
	switch (desc_gi) {
	case DESC_GI_04:
	case DESC_GI_08:
		gi = SKW_HE_GI_0_8;
		break;
	case DESC_GI_16:
		gi = SKW_HE_GI_1_6;
		break;
	case DESC_GI_32:
		gi = SKW_HE_GI_3_2;
		break;
	default:
		SKW_BUG_ON(1);
	}

	return gi;
}

static inline enum SKW_HTVHT_GI skw_desc_htvht_gi_to_skw_gi(enum SKW_MPDU_DESC_GI desc_gi)
{
	enum SKW_HTVHT_GI gi = 0;

	/* convert cp desc gi to SKW_HTVHT_GI */
	switch (desc_gi) {
	case DESC_GI_04:
		gi = SKW_HTVHT_GI_0_4;
		break;
	case DESC_GI_08:
		gi = SKW_HTVHT_GI_0_8;
		break;
	default:
		SKW_BUG_ON(1);
	}

	return gi;
}

static inline u8 skw_desc_gi_to_skw_gi(enum SKW_MPDU_DESC_GI desc_gi,
		enum SKW_RX_MPDU_DESC_PPDUMODE ppdu_mode)
{
	u8 gi = 0;

	switch (ppdu_mode) {
	case SKW_PPDUMODE_HT_MIXED:
	case SKW_PPDUMODE_VHT_SU:
	case SKW_PPDUMODE_VHT_MU:
		gi = skw_desc_htvht_gi_to_skw_gi(gi);
		break;

	case SKW_PPDUMODE_HE_SU:
	case SKW_PPDUMODE_HE_TB:
	case SKW_PPDUMODE_HE_ER_SU:
	case SKW_PPDUMODE_HE_MU:
		gi = skw_desc_he_gi_to_skw_gi(gi);
		break;

	default:
		gi = desc_gi;
		break;
	};

	return gi;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
static inline enum nl80211_he_gi skw_gi_to_nl80211_info_gi(enum SKW_HE_GI skw_gi)
{
	enum nl80211_he_gi gi = 0;

	/* cp tx get station gi to nl80211_he_gi */
	switch (skw_gi) {
	case SKW_HE_GI_3_2:
		gi = NL80211_RATE_INFO_HE_GI_3_2;
		break;
	case SKW_HE_GI_1_6:
		gi = NL80211_RATE_INFO_HE_GI_1_6;
		break;
	case SKW_HE_GI_0_8:
		gi = NL80211_RATE_INFO_HE_GI_0_8;
		break;
	default:
		SKW_BUG_ON(1);
	}

	return gi;
}
#endif

int to_skw_bw(enum nl80211_chan_width bw);
struct wiphy *skw_alloc_wiphy(int priv_size);
int skw_setup_wiphy(struct wiphy *wiphy, struct skw_chip_info *chip);

int skw_mgmt_tx(struct wiphy *wiphy, struct skw_iface *iface,
		struct ieee80211_channel *chan, u32 wait, u64 *cookie,
		bool dont_wait_ack, const void *frame, int frame_len);

int skw_cmd_del_sta(struct wiphy *wiphy, struct net_device *dev,
		const u8 *mac, u8 type, u16 reason, bool tx_frame);

int skw_delete_station(struct wiphy *wiphy, struct net_device *dev,
			const u8 *mac, u8 subtype, u16 reason);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
int skw_change_station(struct wiphy *wiphy, struct net_device *dev,
		const u8 *mac, struct station_parameters *params);
int skw_add_station(struct wiphy *wiphy, struct net_device *dev,
		    const u8 *mac, struct station_parameters *params);
#else
int skw_change_station(struct wiphy *wiphy, struct net_device *dev,
			u8 *mac, struct station_parameters *params);
int skw_add_station(struct wiphy *wiphy, struct net_device *dev,
		u8 *mac, struct station_parameters *params);
#endif

void skw_scan_done(struct skw_core *skw, struct skw_iface *iface, bool abort);

void skw_set_state(struct skw_sm *sm, enum SKW_STATES state);
int skw_roam_connect(struct skw_iface *iface, const u8 *bssid, u8 chn);

int skw_sta_leave(struct wiphy *wiphy, struct net_device *dev,
		const u8 *bssid, u16 reason, bool tx_frame);

void skw_tx_mlme_mgmt(struct net_device *dev, u16 stype,
		      const u8 *bssid, const u8 *da, u16 reason);

int skw_connect_sae_auth(struct wiphy *wiphy, struct net_device *dev,
			 struct cfg80211_bss *bss);
int skw_connect_auth(struct wiphy *wiphy, struct net_device *dev,
		struct skw_connect_param *conn, struct cfg80211_bss *bss);
int skw_connect_assoc(struct wiphy *wiphy, struct net_device *ndev,
		struct skw_connect_param *conn);
void skw_connected(struct net_device *dev, struct skw_connect_param *conn,
		   const u8 *req_ie, int req_ie_len, const u8 *resp_ie,
		   int resp_ie_len, u16 status, gfp_t gfp);
void skw_disconnected(struct net_device *dev, u16 reason,
		bool local_gen, gfp_t gfp);
int skw_cmd_unjoin(struct wiphy *wiphy, struct net_device *ndev,
		   const u8 *addr, u16 reason, bool tx_frame);
int skw_set_mib(struct wiphy *wiphy, struct net_device *dev);
int skw_wow_disable(struct wiphy *wiphy);
int skw_cmd_monitor(struct wiphy *wiphy, struct cfg80211_chan_def *chandef, u8 mode);
#endif
