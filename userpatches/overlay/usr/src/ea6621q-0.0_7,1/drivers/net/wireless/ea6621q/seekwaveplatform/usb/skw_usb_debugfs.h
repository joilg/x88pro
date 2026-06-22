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
#ifndef __SKW_USB_DEBUGFS_H__
#define __SKW_USB_DEBUGFS_H__

#include <linux/fs.h>
#include <linux/debugfs.h>

static inline int skw_usb_default_open(struct inode *node, struct file *fp)
{
	fp->private_data = node->i_private;
	return 0;
}

static inline void  skw_usb_remove_debugfs(struct dentry *dentry)
{
	debugfs_remove(dentry);
}

struct dentry *skw_usb_add_debugfs(const char *name, umode_t mode, void *data,
			       const struct file_operations *fops);

int skw_usb_debugfs_init(void);
void skw_usb_debugfs_deinit(void);

#endif
