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

#include "skw_sdio_debugfs.h"
#include "skw_sdio_log.h"
#include "skw_sdio.h"

static struct dentry *skw_sdio_root_dir;

static ssize_t skw_sdio_default_read(struct file *fp, char __user *buf, size_t len,
				loff_t *offset)
{
	return 0;
}

static ssize_t skw_sdio_state_write(struct file *fp, const char __user *buffer,
				size_t len, loff_t *offset)
{
	return len;
}

static const struct file_operations skw_sdio_state_fops = {
	.open = skw_sdio_default_open,
	.read = skw_sdio_default_read,
	.write = skw_sdio_state_write,
};

struct dentry *skw_sdio_add_debugfs(const char *name, umode_t mode, void *data,
			       const struct file_operations *fops)
{
	skw_sdio_dbg("%s:name: %s\n",__func__,name);

	return debugfs_create_file(name, mode, skw_sdio_root_dir, data, fops);
}

int skw_sdio_debugfs_init(void)
{
	skw_sdio_root_dir = debugfs_create_dir("skwsdio", NULL);
	if (IS_ERR(skw_sdio_root_dir))
		return PTR_ERR(skw_sdio_root_dir);

	// skw_sdio_add_debugfs("state", 0666, wiphy, &skw_sdio_state_fops);
	// skw_sdio_add_debugfs("log_level", 0444, wiphy, &skw_sdio_log_fops);

	return 0;
}

void skw_sdio_debugfs_deinit(void)
{
	skw_sdio_dbg("%s :traced\n", __func__);

	debugfs_remove_recursive(skw_sdio_root_dir);
}
