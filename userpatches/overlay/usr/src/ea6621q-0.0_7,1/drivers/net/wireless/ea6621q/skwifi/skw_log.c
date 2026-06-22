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

#include <linux/version.h>
#include <linux/uaccess.h>
#include <linux/ctype.h>

#include "skw_compat.h"
#include "skw_log.h"
#include "skw_dentry.h"

#define SKW_LL_MASK 0xffff

#if defined(CONFIG_SKW_LOG_ERROR)
#define SKW_LOG_LEVEL SKW_ERROR
#elif defined(CONFIG_SKW_LOG_WARN)
#define SKW_LOG_LEVEL SKW_WARN
#elif defined(CONFIG_SKW_LOG_INFO)
#define SKW_LOG_LEVEL SKW_INFO
#elif defined(CONFIG_SKW_LOG_DEBUG)
#define SKW_LOG_LEVEL SKW_DEBUG
#elif defined(CONFIG_SKW_LOG_DETAIL)
#define SKW_LOG_LEVEL SKW_DETAIL
#else
#define SKW_LOG_LEVEL SKW_INFO
#endif

static unsigned long skw_dbg_level;

unsigned long skw_log_level(void)
{
	return skw_dbg_level;
}

static void skw_set_log_level(int level)
{
	unsigned long dbg_level;

	dbg_level = skw_log_level() & (~SKW_LL_MASK);
	dbg_level |= ((level << 1) - 1);

	xchg(&skw_dbg_level, dbg_level);
}

static void skw_enable_func_log(int func, bool enable)
{
	unsigned long dbg_level = skw_log_level();

	if (enable)
		dbg_level |= func;
	else
		dbg_level &= (~func);

	xchg(&skw_dbg_level, dbg_level);
}

static int skw_log_show(struct seq_file *seq, void *data)
{
	int i;
	u32 level = skw_log_level();
	u8 *log_name[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "DETAIL"};

	i = ffs((level & SKW_LL_MASK) + 1) - 1;

	seq_puts(seq, "\n");
	seq_printf(seq, "Log Level: %s    [ERROR|WARN|INFO|DEBUG|DETAIL]\n", log_name[i]);

#define SKW_LOG_STATUS(s) (level & (s) ? "enable" : "disable")
	seq_puts(seq, "\n");
	seq_printf(seq, "command log: %s\n", SKW_LOG_STATUS(SKW_CMD));
	seq_printf(seq, "event   log: %s\n", SKW_LOG_STATUS(SKW_EVENT));
	seq_printf(seq, "dump    log: %s\n", SKW_LOG_STATUS(SKW_DUMP));
	seq_printf(seq, "scan    log: %s\n", SKW_LOG_STATUS(SKW_SCAN));
	seq_printf(seq, "timer   log: %s\n", SKW_LOG_STATUS(SKW_TIMER));
	seq_printf(seq, "state   log: %s\n", SKW_LOG_STATUS(SKW_STATE));
	seq_printf(seq, "work    log: %s\n", SKW_LOG_STATUS(SKW_WORK));
#undef SKW_LOG_STATUS

	return 0;
}

static int skw_log_open(struct inode *inode, struct file *file)
{
	// return single_open(file, &skw_log_show, inode->i_private);
	return single_open(file, &skw_log_show, skw_pde_data(inode));
}

static int skw_log_control(const char *cmd, bool enable)
{
	if (!strcmp("command", cmd))
		skw_enable_func_log(SKW_CMD, enable);
	else if (!strcmp("event", cmd))
		skw_enable_func_log(SKW_EVENT, enable);
	else if (!strcmp("dump", cmd))
		skw_enable_func_log(SKW_DUMP, enable);
	else if (!strcmp("scan", cmd))
		skw_enable_func_log(SKW_SCAN, enable);
	else if (!strcmp("timer", cmd))
		skw_enable_func_log(SKW_TIMER, enable);
	else if (!strcmp("state", cmd))
		skw_enable_func_log(SKW_STATE, enable);
	else if (!strcmp("work", cmd))
		skw_enable_func_log(SKW_WORK, enable);
	else if (!strcmp("detail", cmd))
		skw_set_log_level(SKW_DETAIL);
	else if (!strcmp("debug", cmd))
		skw_set_log_level(SKW_DEBUG);
	else if (!strcmp("info", cmd))
		skw_set_log_level(SKW_INFO);
	else if (!strcmp("warn", cmd))
		skw_set_log_level(SKW_WARN);
	else if (!strcmp("error", cmd))
		skw_set_log_level(SKW_ERROR);
	else
		return -EINVAL;

	return 0;
}

static ssize_t skw_log_write(struct file *fp, const char __user *buffer,
				size_t len, loff_t *offset)
{
	int i, idx;
	char cmd[32];
	bool enable = false;

	for (idx = 0, i = 0; i < len; i++) {
		char c;

		if (get_user(c, buffer))
			return -EFAULT;

		switch (c) {
		case ' ':
			break;

		case ':':
			cmd[idx] = 0;
			if (!strcmp("enable", cmd))
				enable = true;
			else
				enable = false;

			idx = 0;
			break;

		case '|':
		case '\0':
		case '\n':
			cmd[idx] = 0;
			skw_log_control(cmd, enable);
			idx = 0;
			break;

		default:
			cmd[idx++] = tolower(c);
			idx %= 32;

			break;
		}

		buffer++;
	}

	return len;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops skw_log_fops = {
	.proc_open = skw_log_open,
	.proc_read = seq_read,
	.proc_release = single_release,
	.proc_write = skw_log_write,
};
#else
static const struct file_operations skw_log_fops = {
	.owner = THIS_MODULE,
	.open = skw_log_open,
	.read = seq_read,
	.release = single_release,
	.write = skw_log_write,
};
#endif

void skw_log_level_init(void)
{
	skw_set_log_level(SKW_LOG_LEVEL);

	skw_enable_func_log(SKW_CMD, false);
	skw_enable_func_log(SKW_EVENT, false);
	skw_enable_func_log(SKW_DUMP, false);
	skw_enable_func_log(SKW_SCAN, false);
	skw_enable_func_log(SKW_TIMER, false);
	skw_enable_func_log(SKW_STATE, true);
	skw_enable_func_log(SKW_WORK, false);

	skw_procfs_file(NULL, "log_level", 0666, &skw_log_fops, NULL);
}
