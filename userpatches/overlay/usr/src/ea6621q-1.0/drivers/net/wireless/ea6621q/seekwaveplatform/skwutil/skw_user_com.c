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
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/compat.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/scatterlist.h>
 #include <linux/notifier.h>
#include <linux/platform_device.h>
#include "skw_boot.h"
#include "skw_log_to_file.h"
#define UCOM_PORTNO_MAX		13
#define UCOM_DEV_PM_OPS NULL
int cp_exception_sts=0;
extern int skw_reset_bootloader_cp(void);
static int	skw_major = 0;
static struct class *skw_com_class = NULL;
struct ucom_dev	{
	atomic_t open;
	spinlock_t lock;
	int 	rx_busy;
	int	tx_busy;
	int	devno;
	int	portno;
	struct sv6160_platform_data *pdata;
	wait_queue_head_t wq;
	wait_queue_head_t dumpq;
	struct cdev cdev;
	char	*rx_buf;
	char	*tx_buf;
	struct notifier_block notifier;
};
static struct ucom_dev *ucoms[UCOM_PORTNO_MAX];

static int user_boot_open(struct inode *ip, struct file *fp)
{
	struct cdev *char_dev;
	int ret = -EIO, i;
	struct ucom_dev *ucom=NULL;
	int count=100;
	while(cp_exception_sts &&count--)
		msleep(10);

	char_dev = ip->i_cdev;
	for(i=0; i< UCOM_PORTNO_MAX; i++) {
		if(ucoms[i] && (ucoms[i]->devno == char_dev->dev)) {
			ucom = ucoms[i];
			ret = 0;
			break;
		}
	}

	if(ucom) {
		if(atomic_read(&ucom->open))
			return -EBUSY;
		atomic_inc(&ucom->open);
		fp->private_data = ucom;
		if(!cp_exception_sts)
			ucom->pdata->open_port(ucom->portno, NULL, NULL);

		printk("Open user_boot device: 0x%x task %d\n", char_dev->dev, (int)current->pid);
	}
	return ret;
}

static int user_boot_release(struct inode *ip, struct file *fp)
{
	struct ucom_dev *ucom = fp->private_data;
	int count=100;
	while(cp_exception_sts &&count--)
		msleep(10);

	if(ucom){
		if(!cp_exception_sts)
			ucom->pdata->close_port(ucom->portno);

		atomic_dec(&ucom->open);
	}
	return 0;
}

static int ucom_open(struct inode *ip, struct file *fp)
{
	struct cdev *char_dev;
	int ret = -EIO, i;
	struct ucom_dev *ucom=NULL;

	char_dev = ip->i_cdev;
	for(i=0; i< UCOM_PORTNO_MAX; i++) {
		if(ucoms[i] && (ucoms[i]->devno == char_dev->dev)) {
			ucom = ucoms[i];
			ret = 0;
			break;
		}
	}

	if(ucom) {
		if(atomic_read(&ucom->open) > 1){
			printk("%s ,%d\n", __func__, __LINE__);
			return -EBUSY;
		}
		atomic_inc(&ucom->open);
		init_waitqueue_head(&ucom->wq);
		spin_lock_init(&ucom->lock);
		ucom->pdata->open_port(ucom->portno, NULL, NULL);
		fp->private_data = ucom;
		printk("%s: ucom[%d] %s(0x%x)\n", __func__, i, ucom->pdata->port_name, ucom->portno);
	}
	return ret;
}

static int ucom_release(struct inode *ip, struct file *fp)
{
	struct ucom_dev *ucom = fp->private_data;

	fp->private_data = NULL;
	if(ucom) {
		atomic_dec(&ucom->open);
		wake_up(&ucom->wq);
		printk("%s: ucom%p %s(0x%x)\n", __func__, ucom, ucom->pdata->port_name, ucom->devno);
	}
	return 0;
}

static ssize_t ucom_read(struct file *fp, char __user *buf, size_t count, loff_t *pos)
{
	struct ucom_dev *ucom = fp->private_data;
	ssize_t r = count;
	int ret;
	unsigned long flags;
	uint32_t *data;

	spin_lock_irqsave(&ucom->lock, flags);
	if(ucom->rx_busy) {
		spin_unlock_irqrestore(&ucom->lock, flags);
		return -EAGAIN;
	}
	ucom->rx_busy = 1;
	if(count > ucom->pdata->max_buffer_size)
		count = ucom->pdata->max_buffer_size;
	spin_unlock_irqrestore(&ucom->lock, flags);
	ret = ucom->pdata->hw_sdma_rx(ucom->portno, ucom->rx_buf, count);
	ucom->rx_busy = 0;
	data = (uint32_t *)ucom->rx_buf;

	if(ret > 0) {
		r = ret;
		if(ret > count)
			ret = copy_to_user(buf, ucom->rx_buf, count);
		else
			ret = copy_to_user(buf, ucom->rx_buf, ret);
		if(ret > 0)
			return -EFAULT;
	} else	r = ret;
	pr_debug("%s %s ret = %d\n", __func__,current->comm, (int)r);
	return r;
}

static ssize_t ucom_write(struct file *fp, const char __user *buf, size_t count, loff_t *pos)
{
	struct ucom_dev *ucom = fp->private_data;
	int ret = 0;
	ssize_t r = count;
	ssize_t size;
	unsigned long flags;

	spin_lock_irqsave(&ucom->lock, flags);
	if(ucom->tx_busy) {
		spin_unlock_irqrestore(&ucom->lock, flags);
		printk("%s error 0\n", __func__);
		return -EAGAIN;
	}
	ucom->tx_busy = 1;
	spin_unlock_irqrestore(&ucom->lock, flags);
	while(count){
		if(count > ucom->pdata->max_buffer_size)
			size =  ucom->pdata->max_buffer_size;
		else
			size = count;
		if(copy_from_user(ucom->tx_buf, buf, size))
			return -EFAULT;

		if(!strncmp(ucom->pdata->port_name, "LOG", 3)){
			if(!strncmp(ucom->tx_buf, "START", 5)){
				skwboot_log("%s START log to file \n", __func__);
				skw_modem_log_init(ucom->pdata, NULL, (void *)ucom);
			}
			else if(!strncmp(ucom->tx_buf, "STOP", 4)){
				skwboot_log("%s STOP log to file \n", __func__);
				skw_modem_log_exit();
			}
			else
				skwboot_log("%s LOG write string:%s \n", __func__, ucom->tx_buf);
			ucom->tx_busy = 0;
			return r;
		}
		if (ucom->pdata->port_name && !strncmp(ucom->pdata->port_name, "ATC", 3)) {
			 ucom->tx_buf[size++] = 0x0D;
			 ucom->tx_buf[size++] = 0x0A;
			 count += 2;
		}
		ret = ucom->pdata->hw_sdma_tx(ucom->portno, ucom->tx_buf, size);

		if(ret < 0){
			printk("the close the ucom tx_busy=0");
			ucom->tx_busy=0;
			return ret;
		}
		count -= ret;
		buf += ret;
	}
	ucom->tx_busy = 0;
	pr_debug("%s %s ret = %d\n", __func__,current->comm,(int)r);
	return r;
}

static long ucom_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct ucom_dev *ucom = fp->private_data;

	if(atomic_read(&ucom->open)) {
		printk("%s ucom_%p rx_busy=%d\n", __func__, ucom, ucom->rx_busy);
		if (ucom->pdata)
			ucom->pdata->close_port(ucom->portno);
	}
	return 0;
}
#ifdef CONFIG_COMPAT
static long ucom_compat_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	return ucom_ioctl(fp, cmd, (unsigned long)compat_ptr(arg));
}
#endif
static ssize_t boot_read(struct file *fp, char __user *buf, size_t count, loff_t *pos)
{
	u32 status;
	struct ucom_dev *ucom = fp->private_data;
	int ret = 0;

	ret = ucom->pdata->hw_sdma_rx(ucom->portno, (char *)&status, 4);
	if (ret > 0)
		ret = copy_to_user(buf, &status, ret);
	return ret;
}


static ssize_t boot_write(struct file *fp, const char __user *buf, size_t count, loff_t *pos)
{
	struct ucom_dev *ucom = fp->private_data;
	int ret = 0;

	ret = ucom->pdata->hw_sdma_tx(ucom->portno, "WAKE", 4);
	if(ret > 0)
		return count;
	return ret;
}
static long boot_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	return 0;
}



static const struct file_operations skw_ucom_ops = {
	.owner	= THIS_MODULE,
	.open	= ucom_open,
	.read	= ucom_read,
	.write	= ucom_write,
	.unlocked_ioctl = ucom_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ucom_compat_ioctl,
#endif
	.release= ucom_release,
};
static const struct file_operations skw_user_boot_ops = {
	.owner	= THIS_MODULE,
	.open	= user_boot_open,
	.read	= boot_read,
	.write	= boot_write,
	.unlocked_ioctl = boot_ioctl,
	.release= user_boot_release,
};

static int skw_ucom_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sv6160_platform_data *pdata = dev->platform_data;
	struct ucom_dev	*ucom;
	int ret = 0;

	if(skw_com_class == NULL) {
		skw_com_class = class_create(THIS_MODULE, "btcom");
		if(IS_ERR(skw_com_class)) {
			ret =  PTR_ERR(skw_com_class);
			skw_com_class = NULL;
			return ret;
		}
	}
	if (pdata) {
		ucom = kzalloc(sizeof(struct ucom_dev), GFP_KERNEL);
		if(!ucom)
			return -ENOMEM;
		if (strncmp(pdata->port_name, "BTBOOT", 6)) {
			ucom->rx_buf = kzalloc(pdata->max_buffer_size, GFP_KERNEL);
			if(ucom->rx_buf) {
				ucom->tx_buf = kzalloc(pdata->max_buffer_size, GFP_KERNEL);
				if(!ucom->tx_buf) {
					kfree(ucom->rx_buf);
					kfree(ucom);
					return -ENOMEM;
				}
			}else{
				kfree(ucom);
				return -ENOMEM;
			}
			ret =__register_chrdev(skw_major, pdata->data_port+1, 1, pdata->port_name, &skw_ucom_ops);
		} else {
			pdata->data_port = UCOM_PORTNO_MAX - 1;
			ret =__register_chrdev(skw_major, UCOM_PORTNO_MAX, 1,
					pdata->port_name, &skw_user_boot_ops);
		}
		if(ret < 0) {
			kfree(ucom->rx_buf);
			kfree(ucom->tx_buf);
			kfree(ucom);
			return ret;
		}
		if(skw_major == 0)
			skw_major = ret;
		ucom->devno = MKDEV(skw_major, pdata->data_port+1);
		printk("register char device:%s %d:%d\n",
				pdata->port_name, skw_major, pdata->data_port);
		ucom->pdata = pdata;
		ucom->portno = pdata->data_port;
		atomic_set(&ucom->open, 0);
		platform_set_drvdata(pdev, ucom);
		ucoms[ucom->portno] = ucom;
		device_create(skw_com_class, NULL, ucom->devno, NULL, "%s", pdata->port_name);
		return 0;
	}
	return -EINVAL;
}

static int skw_ucom_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sv6160_platform_data *pdata = dev->platform_data;
	struct ucom_dev *ucom;
	int ret, i;
	int devno;

	ucom = platform_get_drvdata(pdev);
	if(!strncmp(ucom->pdata->port_name, "LOG", 3)){
#ifndef CONFIG_SEEKWAVE_PLD_RELEASE
		skw_modem_log_exit();
#endif
	}
	if(ucom) {
		ret = wait_event_interruptible_timeout(ucom->wq,
				(!atomic_read(&ucom->open)),
				msecs_to_jiffies(10000));
		ucom->pdata = NULL;
		devno = ucom->devno;
		device_destroy(skw_com_class, devno);
		ucoms[ucom->portno] = NULL;
		kfree(ucom->rx_buf);
		ucom->rx_buf = NULL;
		kfree(ucom->tx_buf);
		ucom->tx_buf = NULL;
		kfree(ucom);
		__unregister_chrdev(MAJOR(devno), MINOR(devno), 1,  pdata->port_name);
		platform_set_drvdata(pdev, NULL);
	}
	for(i=0; i<UCOM_PORTNO_MAX; i++) {
		if(ucoms[i])
			break;
	}
	if(i>=UCOM_PORTNO_MAX) {
		class_destroy(skw_com_class);
		skw_com_class = NULL;
	}
	return 0;
}

static struct platform_driver skw_ucom_driver = {
	.driver = {
		.name	= (char*)"skw_ucom",
		.bus	= &platform_bus_type,
		.pm	= UCOM_DEV_PM_OPS,
	},
	.probe = skw_ucom_probe,
	.remove = skw_ucom_remove,
};

int skw_ucom_init(void)
{

	platform_driver_register(&skw_ucom_driver);
	return 0;
}

void skw_ucom_exit(void)
{
	cp_exception_sts=0;
	platform_driver_unregister(&skw_ucom_driver);
}
