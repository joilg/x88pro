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
#ifndef __SKW_LOG_H__
#define __SKW_LOG_H__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

/****************************************************************
 *Description:the skwsdio log define and the skwsdio data debug,
 *Func: skwsdio_log, skwsdio_err, skwsdio_data_pr;
 *Calls:
 *Call By:
 *Input: skwsdio log debug informations
 *Output:
 *Return：
 *Others:
 *Author：JUNWEI.JIANG
 *Date:2022-07-18
 * **************************************************************/
#define skwlog_log(fmt, args...) \
    pr_info("[SKWLOG]:" fmt, ## args)

#define skwlog_err(fmt, args...) \
    pr_err("[SKWLOG_ERR]:" fmt, ## args)


int skw_modem_log_init(struct sv6160_platform_data *p_data, struct file *fp, void *ucom);
void skw_modem_log_set_assert_status(uint32_t cp_assert);
void skw_modem_log_start_rec(void);
void skw_modem_log_stop_rec(void);
void skw_modem_dumpmodem_start_rec(void);
void skw_modem_dumpmodem_stop_rec(void);

void skw_modem_log_exit(void);

#endif
