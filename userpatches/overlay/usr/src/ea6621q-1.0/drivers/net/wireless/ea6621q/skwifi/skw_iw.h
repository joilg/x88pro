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

#ifndef __SKW_IW_H__
#define __SKW_IW_H__

#define SKW_MAX_TLV_BUFF_LEN          1024

#define SKW_KEEPACTIVE_RULE_MAX            4
#define SKW_KEEPACTIVE_LENGTH_MAX            540
#define SKW_KEEPALIVE_ALWAYS_FLAG          BIT(0)

struct skw_keep_active_rule_data {
	u16 is_chksumed;
	u8 payload[0];
} __packed;
struct skw_keep_active_rule {
	u32 keep_interval;
	u8 payload_len;
	struct skw_keep_active_rule_data data[0];
} __packed;

struct skw_keep_active_setup {
	u32 en_bitmap;
	u32 flags[SKW_KEEPACTIVE_RULE_MAX];
	struct skw_keep_active_rule *rule[SKW_KEEPACTIVE_RULE_MAX];
} __packed;

struct skw_keep_active_param {
	u8 rule_num;
	struct skw_keep_active_rule rules[0];
} __packed;

typedef int (*skw_at_handler)(struct skw_core *skw, void *param,
			char *args, char *resp, int resp_len);

struct skw_at_cmd {
	char *name;
	skw_at_handler handler;
	char *help_info;
};

typedef int (*skw_iwpriv_handler)(struct skw_iface *iface, void *param,
			char *args, char *resp, int resp_len);

struct skw_iwpriv_cmd {
	char *name;
	skw_iwpriv_handler handler;
	char *help_info;
};

const void *skw_iw_handlers(void);
#endif
