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
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "skw_usb_log.h"
#include "skw_usb.h"
#include "skw_usb_debugfs.h"

static unsigned long skw_usb_dbg_level;

extern char firmware_version[];
unsigned long skw_usb_log_level(void)
{
	return skw_usb_dbg_level;
}

static void skw_usb_set_log_level(int level)
{
	unsigned long dbg_level;

	dbg_level = skw_usb_log_level() & 0xffff0000;
	dbg_level |= ((level << 1) - 1);

	xchg(&skw_usb_dbg_level, dbg_level);
}

static void skw_usb_enable_func_log(int func, bool enable)
{
	unsigned long dbg_level = skw_usb_log_level();

	if (enable)
		dbg_level |= func;
	else
		dbg_level &= (~func);

	xchg(&skw_usb_dbg_level, dbg_level);
}

static int skw_usb_log_show(struct seq_file *seq, void *data)
{
#define SKW_USB_LOG_STATUS(s) (level & (s) ? "enable" : "disable")

	int i;
	u32 level = skw_usb_log_level();
	u8 *log_name[] = {"NONE", "ERROR", "WARNNING", "INFO", "DEBUG"};

	for (i = 0; i < 5; i++) {
		if (!(level & BIT(i)))
			break;
	}

	seq_printf(seq, "\nlog   level: %s\n", log_name[i]);

	seq_puts(seq, "\n");
	seq_printf(seq, "port0 log: %s\n", SKW_USB_LOG_STATUS(SKW_USB_PORT0));
	seq_printf(seq, "port1 log: %s\n", SKW_USB_LOG_STATUS(SKW_USB_PORT1));
	seq_printf(seq, "port2 log: %s\n", SKW_USB_LOG_STATUS(SKW_USB_PORT2));
	seq_printf(seq, "port3 log: %s\n", SKW_USB_LOG_STATUS(SKW_USB_PORT3));
	seq_printf(seq, "port4 log: %s\n", SKW_USB_LOG_STATUS(SKW_USB_PORT4));
	seq_printf(seq, "port5 log: %s\n", SKW_USB_LOG_STATUS(SKW_USB_PORT5));
	seq_printf(seq, "port6 log: %s\n", SKW_USB_LOG_STATUS(SKW_USB_PORT6));
	seq_printf(seq, "port7 log: %s\n", SKW_USB_LOG_STATUS(SKW_USB_PORT7));
	seq_printf(seq, "savelog  : %s\n", SKW_USB_LOG_STATUS(SKW_USB_SAVELOG));
	seq_printf(seq, "dump  log: %s\n", SKW_USB_LOG_STATUS(SKW_USB_DUMP));

	return 0;
}

static int skw_usb_log_open(struct inode *inode, struct file *file)
{
	return single_open(file, &skw_usb_log_show, inode->i_private);
}

static int skw_usb_log_control(const char *cmd, bool enable)
{
	if (!strcmp("dump", cmd))
		skw_usb_enable_func_log(SKW_USB_DUMP, enable);
	else if (!strcmp("port0", cmd))
		skw_usb_enable_func_log(SKW_USB_PORT0, enable);
	else if (!strcmp("port1", cmd))
		skw_usb_enable_func_log(SKW_USB_PORT1, enable);
	else if (!strcmp("port2", cmd))
		skw_usb_enable_func_log(SKW_USB_PORT2, enable);
	else if (!strcmp("port3", cmd))
		skw_usb_enable_func_log(SKW_USB_PORT3, enable);
	else if (!strcmp("port4", cmd))
		skw_usb_enable_func_log(SKW_USB_PORT4, enable);
	else if (!strcmp("port5", cmd))
		skw_usb_enable_func_log(SKW_USB_PORT5, enable);
	else if (!strcmp("port6", cmd))
		skw_usb_enable_func_log(SKW_USB_PORT6, enable);
	else if (!strcmp("port7", cmd))
		skw_usb_enable_func_log(SKW_USB_PORT7, enable);
	else if (!strcmp("savelog", cmd))
		skw_usb_enable_func_log(SKW_USB_SAVELOG, enable);
	else if (!strcmp("debug", cmd))
		skw_usb_set_log_level(SKW_USB_DEBUG);
	else if (!strcmp("info", cmd))
		skw_usb_set_log_level(SKW_USB_INFO);
	else if (!strcmp("warn", cmd))
		skw_usb_set_log_level(SKW_USB_WARNING);
	else if (!strcmp("error", cmd))
		skw_usb_set_log_level(SKW_USB_ERROR);
	else
		return -EINVAL;

	return 0;
}

static ssize_t skw_usb_log_write(struct file *fp, const char __user *buffer,
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
			skw_usb_log_control(cmd, enable);
			idx = 0;
			break;

		default:
			cmd[idx++] = c;
			idx %= 32;

			break;
		}

		buffer++;
	}

	return len;
}

static const struct file_operations skw_usb_log_fops = {
	.owner = THIS_MODULE,
	.open = skw_usb_log_open,
	.read = seq_read,
	.release = single_release,
	.write = skw_usb_log_write,
};

static int skw_version_show(struct seq_file *seq, void *data)
{
	seq_printf(seq, "firmware info:\n %s\n", firmware_version );
	return 0;
}

static int skw_version_open(struct inode *inode, struct file *file)
{
	return single_open(file, &skw_version_show, inode->i_private);
}


static const struct file_operations skw_version_fops = {
	.owner = THIS_MODULE,
	.open = skw_version_open,
	.read = seq_read,
	.release = single_release,
};


static int skw_cp_log_show(struct seq_file *seq, void *data)
{
	if (!skw_usb_cp_log_status())
		seq_printf(seq, "Enabled");
	else
		seq_printf(seq, "Disabled");
	return 0;
}
static int skw_cp_log_open(struct inode *inode, struct file *file)
{
	return single_open(file, &skw_cp_log_show, inode->i_private);
}


static ssize_t skw_cp_log_write(struct file *fp, const char __user *buffer,
		size_t len, loff_t *offset)
{
	char cmd[16]={0};

	if (len >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, buffer, len))
		return -EFAULT;
	if (!strncmp("enable", cmd, 6))
		skw_usb_cp_log(0);
	else if (!strncmp("disable", cmd, 7))
		skw_usb_cp_log(1);

	return len;
}

static const struct file_operations skw_cp_log_fops = {
	.owner = THIS_MODULE,
	.open = skw_cp_log_open,
	.read = seq_read,
	.release = single_release,
	.write = skw_cp_log_write,
};

static int skw_port_statistic_show(struct seq_file *seq, void *data)
{
	char *statistic = kzalloc(2048, GFP_KERNEL);

	skw_get_port_statistic(statistic, 2048);
	seq_printf(seq, "Statistic:\n%s", statistic );
	kfree(statistic);
	return 0;
}

static int skw_port_statistic_open(struct inode *inode, struct file *file)
{
	return single_open(file, &skw_port_statistic_show, inode->i_private);
}

static const struct file_operations skw_port_statistic_fops = {
	.owner = THIS_MODULE,
	.open = skw_port_statistic_open,
	.read = seq_read,
	.release = single_release,
};

static int skwusb_recovery_debug_show(struct seq_file *seq, void *data)
{
	if (skw_usb_recovery_debug_status())
		seq_printf(seq, "Disabled");
	else
		seq_printf(seq, "Enabled");

	return 0;
}
static int skwusb_recovery_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, &skwusb_recovery_debug_show, inode->i_private);
}

static ssize_t skwusb_recovery_debug_write(struct file *fp, const char __user *buffer,
				size_t len, loff_t *offset)
{
	char cmd[16]={0};

	if (len >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, buffer, len))
		return -EFAULT;
	if (!strncmp("disable", cmd, 7))
		skw_usb_recovery_debug(1);
	else if (!strncmp("enable", cmd, 6))
		skw_usb_recovery_debug(0);

	return len;
}

static const struct file_operations skwusb_recovery_debug_fops = {
	.owner = THIS_MODULE,
	.open = skwusb_recovery_debug_open,
	.read = seq_read,
	.release = single_release,
	.write = skwusb_recovery_debug_write,
};

void skw_usb_log_level_init(void)
{
	skw_usb_set_log_level(SKW_USB_INFO);

	skw_usb_enable_func_log(SKW_USB_DUMP, false);
	skw_usb_enable_func_log(SKW_USB_PORT0, false);
	skw_usb_enable_func_log(SKW_USB_PORT1, false);
	skw_usb_enable_func_log(SKW_USB_PORT2, false);
	skw_usb_enable_func_log(SKW_USB_PORT3, false);
	skw_usb_enable_func_log(SKW_USB_PORT4, false);
	skw_usb_enable_func_log(SKW_USB_PORT5, false);
	skw_usb_enable_func_log(SKW_USB_PORT6, false);
	skw_usb_enable_func_log(SKW_USB_SAVELOG, false);
	skw_usb_enable_func_log(SKW_USB_PORT7, false);
	skw_usb_add_debugfs("log_level", 0666, NULL, &skw_usb_log_fops);
	skw_usb_add_debugfs("Version", 0664, NULL, &skw_version_fops);
	skw_usb_add_debugfs("CPLog", 0666, NULL, &skw_cp_log_fops);
	skw_usb_add_debugfs("Statistic", 0666, NULL, &skw_port_statistic_fops);
	skw_usb_add_debugfs("recovery", 0666, NULL, &skwusb_recovery_debug_fops);
}
