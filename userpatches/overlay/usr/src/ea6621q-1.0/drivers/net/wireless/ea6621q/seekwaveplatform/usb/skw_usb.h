/****************************************************************************
 * Copyright (C) 2020-2030 Seekwave Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"),
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
**************************************************************************/
#ifndef WCN_USB_H
#define WCN_USB_H

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/bitops.h>
#include <linux/kthread.h>
#include <linux/notifier.h>
#include "../skwutil/skw_boot.h"

#define skwusb_log(fmt, args...) \
	pr_info("[SKW_USB]:" fmt, ## args)

#define skwusb_err(fmt, args...) \
	pr_err("[SKW_USB_ERR]:" fmt, ## args)

#define skwusb_data_pr(level, prefix_str, prefix_type, rowsize,\
		groupsize, buf, len, asscii)\
		do{if(loglevel) \
			print_hex_dump(level, prefix_str, prefix_type, rowsize,\
					groupsize, buf, len, asscii);\
		}while(0)


#define USB_RX_TASK_PRIO 90
#define SKW_CHIP_ID_LENGTH			16  //SV6160 chip id lenght

int skw_usb_recovery_debug(int disable);
int skw_usb_recovery_debug_status(void);
#endif
