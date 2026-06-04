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
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>
#include <linux/notifier.h>
#include <linux/semaphore.h>
#include <linux/pm_runtime.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/wait.h>
#include <linux/gpio.h>
#include "skw_usb.h"
#include "skw_usb_log.h"
#include "skw_usb_debugfs.h"
#define MAX_BUFFER_SIZE 23*1024
#define MAX_MSG_SIZE	MAX_BUFFER_SIZE

#define VENDOR_MSG_MODEM_ASSERT 0xA5
#define VENDOR_MSG_SERVICE_CTRL 0xA6
#define VENDOR_MSG_PACKET_COUNT 0xA7
#define VENDOR_MSG_LOG_SWITCH   0xA8
#define VENDOR_MSG_MODEM_RESET  0xA9
#define	WIFI_SERVICE	0
#define BT_SERVICE	  1

#define SERVICE_START	0
#define SERVICE_STOP	1

#define MODEM_OFF		0
#define MODEM_ON		1
#define MODEM_HALT		2

#define WIFI_PORT_SHARE_FLAG	0x4000

#define MAX_USB_PORT MAX_PORT_COUNT
#define MAX_PACKET_COUNT 20
struct delayed_work skw_except_work;
static struct platform_device *wifi_data_pdev;
static u64 port_dmamask = DMA_BIT_MASK(32);
static u32 service_state_map = 0;
static int cp_log_status = 0;
static char *firmware_data;
static int   firmware_size;
static int   firmware_addr;
struct seekwave_device *usb_boot_data;
struct completion download_done;
static BLOCKING_NOTIFIER_HEAD(modem_notifier_list);
int chip_en_gpio;
int modem_status;
int recovery_debug_status;
char *skw_chipid;
static u32 last_sent_wifi_cmd[3];
static u32 last_recv_wifi_evt[3];
static u32 last_recv_wifi_ack[3];
static u64 last_sent_time, last_ack_time;
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */

static const struct usb_device_id skw_usb_io_id_table[] = {
	{ USB_DEVICE(0x0483, 0x5720) },
	{ USB_DEVICE(0x0483, 0x5721) },
	{ USB_DEVICE(0x3607, 0x6316) },
	{ USB_DEVICE(0x3607, 0x6160) },
	{}	/* Terminating entry */
};
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static struct recovery_data{
	struct mutex except_mutex;
	int cp_state;
} g_recovery_data;

#define SKW_USB_GET_RECOVERY_DATA() &g_recovery_data

struct usb_port_struct {
	struct work_struct work;
	struct platform_device *pdev;
	int	portno;
	struct usb_interface *interface;
	struct usb_device *udev;
	struct urb *read_urb;
	struct usb_endpoint_descriptor *epin;
	struct urb *write_urb;
	struct usb_endpoint_descriptor *epout;
	char *read_buffer;
	char   *write_buffer;
	int	   buffer_size;
	struct usb_anchor read_submitted;
	struct usb_anchor write_submitted;
	struct task_struct *thread;
	rx_submit_fn rx_submit;
	adma_callback adma_tx_callback;
	sdma_callback sdma_tx_callback;
	void *rx_data;
	void *tx_data;
	int	state;
	int  ep_mps;
	int  max_packet_count;
	struct semaphore sem;
	int	is_dloader;
	int	sent_packet_count;
	int	req_tx_packet;
	wait_queue_head_t   rx_wait;
	wait_queue_head_t	tx_wait;
	struct list_head rx_urb_list;
	struct list_head tx_urb_list;
	struct list_head rx_done_urb_list;
	struct list_head suspend_urb_list;
	spinlock_t rx_urb_lock;
	spinlock_t tx_urb_lock;
	int	tx_urb_count;
	int	 rx_packet_count;
	int 	suspend;
} *usb_ports[MAX_USB_PORT];

int modem_assert(void);
static int skw_recovery_mode(void);
static struct usb_port_struct *log_port;
extern void kernel_restart(char *cmd);
static int bulkin_read_timeout(int portno, char *buffer, int size, int *actual, int timeout);
static int bulkout_write_timeout(int portno, char *buffer, int size, int *actual, int timeout);
static void bulkout_async_complete(struct urb *urb);
static void bulkin_async_complete(struct urb *urb);
static int assert_info_print;
#ifdef CONFIG_WAKELOCK
static	struct wake_lock usb_wakelock;
#else
static	struct wakeup_source *usb_wakelock;
#endif
int    wakelocked;
static int usb_bt_rx_entry(void *para);
char firmware_version[128];
int	bt_audio_port;
struct platform_device *bluetooth_pdev;
static int wifi_port_share;
int    host_wake_gpio;

void skw_get_port_statistic(char *buffer, int size)
{
	int ret = 0;
	int i;

	if(!buffer)
		return;

	for(i=0; i<2; i++) {
		if(ret >= size)
			break;

		ret += sprintf(&buffer[ret],
				"port%d: req_tx %d tx_done %d, rx %d\n",
				i, usb_ports[i]->req_tx_packet,
				usb_ports[i]->sent_packet_count,
				usb_ports[i]->rx_packet_count);
	}
}

#include "usb_boot.c"
void modem_register_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_register(&modem_notifier_list, nb);
}
void modem_unregister_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&modem_notifier_list, nb);
}
static void modem_notify_event(int event)
{
	blocking_notifier_call_chain(&modem_notifier_list, event, NULL);
}
static void skw_usb_wakeup_source_init(void)
{
#ifdef CONFIG_WAKELOCK
	wake_lock_init(&usb_wakelock, WAKE_LOCK_SUSPEND,"skw_usb_wakelock");
#else
	usb_wakelock =  skw_wakeup_source_register(NULL, "skw_usb_wakelock");
#endif
	wakelocked = 0;
}
static void skw_usb_wakeup_source_destroy(void)
{
#ifdef CONFIG_WAKELOCK
	wake_lock_destroy(&usb_wakelock);
#else
	wakeup_source_unregister(usb_wakelock);
#endif
}
static void skw_usb_wake_lock(void)
{
	if (wakelocked)
		return;
#ifdef CONFIG_WAKELOCK
	__pm_stay_awake(&usb_wakelock.ws);
#else
	__pm_stay_awake(usb_wakelock);
#endif
	wakelocked = 1;
}
static void skw_usb_wake_unlock(void)
{
	if (!wakelocked)
		return;
#ifdef CONFIG_WAKELOCK
	__pm_relax(&usb_wakelock.ws);
#else
	__pm_relax(usb_wakelock);
#endif
	wakelocked = 0;
}

void skw_usb_exception_work(struct work_struct *work)
{
	struct recovery_data *recovery = SKW_USB_GET_RECOVERY_DATA();
	skw_usb_info(" entern...\n");
	mutex_lock(&recovery->except_mutex);
	if(recovery->cp_state!=1)
	{
		skw_usb_info("assert coming\n");
		mutex_unlock(&recovery->except_mutex);
		return;
	}
	skw_usb_wake_lock();
	recovery->cp_state = DEVICE_BLOCKED_EVENT;
	mutex_unlock(&recovery->except_mutex);
	modem_notify_event(DEVICE_BLOCKED_EVENT);
	service_state_map=0;
}

int skw_usb_recovery_debug(int disable)
{
	recovery_debug_status = disable;
	skw_usb_info("the recovery status =%d\n", recovery_debug_status);
	return 0;
}

int skw_usb_recovery_debug_status(void)
{
	skw_usb_info("the recovery val =%d\n", recovery_debug_status);
	return recovery_debug_status;
}

static void usb_setup_service_devices(void)
{
	struct usb_port_struct *bt_port;
	int ret;

	if(usb_ports[1]->pdev){
		if(wifi_data_pdev==NULL) {
			ret = platform_device_add(usb_ports[1]->pdev);
			if(ret) {
				skw_usb_err("the fail to register WIFI device\n");
				platform_device_put(usb_ports[1]->pdev);
			} else {
				wifi_data_pdev = usb_ports[1]->pdev;
				skw_usb_info("add WIFI devices done\n");
			}
		 }
	} else
		 skw_usb_err("NOT suppport WIFI service\n");

	if (bluetooth_pdev) {
		bt_port = usb_ports[bt_audio_port];
		bt_port->pdev = bluetooth_pdev;
		bluetooth_pdev = NULL;
		ret = platform_device_add(bt_port->pdev);
		if(ret) {
			dev_err(&bt_port->udev->dev, "failt to register Bluetooth device\n");
			platform_device_put(bt_port->pdev);
			bt_port->pdev = NULL;
		} else
			skw_usb_info("add Bluetooth devices done\n");
	}
}
static void usb_port_alloc_recv_urbs(struct usb_port_struct *port, struct usb_endpoint_descriptor *epd, int count, int buffer_size)
{
	int i;
	struct urb *urb;

	for(i=0; i<count; i++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if(!urb)
			break;
		if(!buffer_size) {
			urb->transfer_buffer = NULL;
			urb->transfer_buffer_length = 0;
		} else{
			urb->transfer_buffer = kzalloc(buffer_size, GFP_KERNEL);
			if(!urb->transfer_buffer) {
				usb_free_urb(urb);
				break;
			}
			urb->transfer_buffer_length = buffer_size;
		}
		usb_fill_bulk_urb(urb, port->udev,usb_rcvbulkpipe(port->udev, epd->bEndpointAddress),
			urb->transfer_buffer, buffer_size, bulkin_async_complete, NULL);
		list_add_tail(&urb->urb_list, &port->rx_urb_list);
	}
	skw_usb_dbg("%s urb cout %d\n", __func__, i);
}

static void usb_port_alloc_xmit_urbs(struct usb_port_struct *port, struct usb_endpoint_descriptor *epd, int count, int buffer_size)
{
	int i;
	struct urb *urb;

	for(i=0; i<count; i++) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if(!urb)
			break;
		if(!buffer_size) {
			urb->transfer_buffer = NULL;
			urb->transfer_buffer_length = 0;
		} else{
			urb->transfer_buffer = kzalloc(buffer_size, GFP_KERNEL);
			if(!urb->transfer_buffer) {
				usb_free_urb(urb);
				break;
			}
			urb->transfer_buffer_length = buffer_size;
		}
		usb_fill_bulk_urb(urb, port->udev,usb_sndbulkpipe(port->udev, epd->bEndpointAddress),
			urb->transfer_buffer, buffer_size, bulkout_async_complete, NULL);
		list_add_tail(&urb->urb_list, &port->tx_urb_list);
	}
	skw_usb_dbg("%s urb cout %d\n", __func__, i);
}

/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
int open_usb_port(int id, void *callback, void *data)
{
	struct usb_port_struct *port;

	if(id >= MAX_USB_PORT)
		return -EINVAL;

	port = usb_ports[id];
	if(port->state==0)
		return -EIO;
	if (port->state==1) {
		init_usb_anchor(&port->read_submitted);
		init_usb_anchor(&port->write_submitted);
	}
	port->state = 2;
	port->rx_submit = callback;
	port->rx_data = data;
	if(callback && data && !port->thread) {
		sema_init(&port->sem, 0);
		port->thread = kthread_create(usb_bt_rx_entry, port, port->interface->cur_altsetting->string);
		if(port->thread)
			wake_up_process(port->thread);
	}
	return 0;
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
int  bulkin_read(struct usb_port_struct *port, void *buffer, int size)
{
	int retval = -1;
	DECLARE_COMPLETION_ONSTACK(done);

	if(port->state==0)
		return -EIO;

	if(port == log_port)
		memset(buffer, 0 , size);

	if(port && port->read_urb) {
		port->read_urb->transfer_buffer = buffer;
		port->read_urb->transfer_buffer_length = size;
		port->read_urb->context = &done;
		usb_anchor_urb(port->read_urb, &port->read_submitted);
		retval = usb_submit_urb(port->read_urb,GFP_KERNEL);
		if(retval==0) {
			retval = wait_for_completion_interruptible(&done);
			if(retval == -ERESTARTSYS)
				usb_kill_urb(port->read_urb);
			else if(port->read_urb->status)
				retval = port->read_urb->status;
			else if(retval==0)
				retval = port->read_urb->actual_length;
			port->read_urb->context = NULL;
		} else {
			if (retval < 0) {
				usb_unanchor_urb(port->read_urb);
				dev_info(&port->pdev->dev, "%s is error= %d!!!\n", __func__, retval);
			}
			port->read_urb->context = NULL;
		}
	}
	if(port == log_port) {
		if(assert_info_print && assert_info_print<28 && retval<100) {
			assert_info_print++;
			if(retval > 4)
				printk("%s", (char *)buffer);
		}
		if(retval == 4)
			assert_info_print = 28;
	}
	return retval;
}
EXPORT_SYMBOL(bulkin_read);
int bulkin_read_async(struct usb_port_struct *port)
{
	int	 retval = -1;
	unsigned long flags;
	struct urb *urb;

	spin_lock_irqsave(&port->rx_urb_lock, flags);
	urb = list_first_entry(&port->rx_urb_list, struct urb, urb_list);
	list_del_init(&urb->urb_list);
	spin_unlock_irqrestore(&port->rx_urb_lock, flags);
	if(urb->context) {
		dev_info(&port->pdev->dev, "%s is busy!!!\n", __func__);
		return -EBUSY;
	}

	if(urb) {
		urb->complete = bulkin_async_complete;
		urb->context = port;
		if (port->suspend) {
			dev_info(&port->pdev->dev, "%s save URB in suspend list!!!\n", __func__);
			list_add_tail(&urb->urb_list, &port->suspend_urb_list);
		} else {
			usb_anchor_urb(urb, &port->read_submitted);
			retval = usb_submit_urb(urb, GFP_KERNEL);
			if (retval < 0) {
				usb_unanchor_urb(urb);
				dev_info(&port->pdev->dev, "%s is failed! %d\n", __func__, retval);
				list_add_tail(&urb->urb_list, &port->suspend_urb_list);
			}
		}
	}
	return retval;
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
int bulkout_write(struct usb_port_struct *port, void *buffer, int size)
{
	int retval = -1;
	DECLARE_COMPLETION_ONSTACK(done);

	if (port && port->write_urb) {
		port->write_urb->transfer_buffer = buffer;
		port->write_urb->transfer_buffer_length = size;
		if(size%port->ep_mps == 0)
			port->write_urb->transfer_flags |= URB_ZERO_PACKET;
		port->write_urb->context = &done;
		usb_anchor_urb(port->write_urb, &port->write_submitted);
		retval = usb_submit_urb(port->write_urb,GFP_KERNEL);
		dev_dbg(&port->udev->dev,"%s wait done %d\n",__func__, retval);
		if(retval==0) {
			retval = wait_for_completion_interruptible_timeout(&done, msecs_to_jiffies(3000));
			if(retval==-ERESTARTSYS || !retval) {
				if(port->write_urb && port->write_urb->context)
					usb_kill_urb(port->write_urb);
			}
			else if (port->write_urb->status)
				retval = port->write_urb->status;
			else
				retval = port->write_urb->actual_length;
			port->write_urb->context = NULL;
		} else {

			if (retval < 0) {
				usb_unanchor_urb(port->write_urb);
				dev_info(&port->pdev->dev, "%s is error!!! %d\n", __func__, retval);
			}
			port->write_urb->context = NULL;
		}
	}
	return retval;
}

int bulkout_write_async(struct usb_port_struct *port, void *buffer, int size)
{
	int retval = -1;
	struct urb *urb;
	unsigned long flags;

	spin_lock_irqsave(&port->tx_urb_lock, flags);
	if(list_empty(&port->tx_urb_list)) {
		spin_unlock_irqrestore(&port->tx_urb_lock, flags);
		retval = wait_event_interruptible(port->tx_wait, (!list_empty(&port->tx_urb_list)));
		spin_lock_irqsave(&port->tx_urb_lock, flags);
	}
	urb = list_first_entry(&port->tx_urb_list, struct urb, urb_list);
	list_del_init(&urb->urb_list);
	port->tx_urb_count++;
	spin_unlock_irqrestore(&port->tx_urb_lock, flags);

	if (port && urb) {
		usb_fill_bulk_urb(urb, port->udev,usb_sndbulkpipe(port->udev, port->epout->bEndpointAddress),
			buffer, size, bulkout_async_complete, port);
		if(size%port->ep_mps == 0)
			urb->transfer_flags |= URB_ZERO_PACKET;
		
		usb_anchor_urb(urb, &port->write_submitted);
		retval = usb_submit_urb(urb,GFP_KERNEL);
		if (retval < 0) {
			usb_unanchor_urb(urb);
			dev_info(&port->pdev->dev, "%s is error!!! %d\n", __func__, retval);
		}
		dev_dbg(&port->udev->dev,"%s %d wait done %d %d\n",__func__, port->portno, retval, port->tx_urb_count);
	}
	return retval;
}

EXPORT_SYMBOL(bulkout_write);

int bulkout_write_sg_async(struct usb_port_struct *port, struct scatterlist *sgs, int sg_num, int total)
{
	struct urb *urb;
	unsigned long flags;
	int retval = -1;

	spin_lock_irqsave(&port->tx_urb_lock, flags);
	if(list_empty(&port->tx_urb_list)) {
		spin_unlock_irqrestore(&port->tx_urb_lock, flags);
		retval = wait_event_interruptible(port->tx_wait, (!list_empty(&port->tx_urb_list)));
		spin_lock_irqsave(&port->tx_urb_lock, flags);
	}
	urb = list_first_entry(&port->tx_urb_list, struct urb, urb_list);
	port->tx_urb_count++;
	list_del_init(&urb->urb_list);
	port->req_tx_packet += sg_num;
	spin_unlock_irqrestore(&port->tx_urb_lock, flags);
	urb->transfer_buffer = NULL;
	urb->transfer_buffer_length = 0;
	usb_fill_bulk_urb(urb, port->udev,usb_sndbulkpipe(port->udev, port->epout->bEndpointAddress),
		NULL, 0, bulkout_async_complete, port);
	urb->sg = sgs;
	urb->num_sgs = sg_num;
	urb->transfer_buffer_length = total;
	if(total%port->ep_mps == 0)
		urb->transfer_flags |= URB_ZERO_PACKET;
	usb_anchor_urb(urb, &port->write_submitted);
	//dev_info(&port->udev->dev,"%s %d submit  %d\n",__func__, port->portno,  port->tx_urb_count);
	return usb_submit_urb(urb,GFP_KERNEL);
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static int bulkout_write_sg(struct usb_port_struct *port, struct scatterlist *sgs, int sg_num, int total)
{
	int	 retval = -1;
	DECLARE_COMPLETION_ONSTACK(done);

	if(!port->write_urb)
		return -ENODEV;
	if(port->write_urb->context) {
		dev_dbg(&port->pdev->dev, "%s is busy!!!\n", __func__);
		return -EBUSY;
	}
	if(port && port->write_urb) {
		port->write_urb->sg = sgs;
		port->write_urb->num_sgs = sg_num;
		port->write_urb->transfer_buffer_length = total;
		if(total%port->ep_mps == 0)
			port->write_urb->transfer_flags |= URB_ZERO_PACKET;
		port->write_urb->context = &done;
		skw_port_log(port->portno,"%s port%d size = %d\n",__func__, port->portno, total);
		port->req_tx_packet += port->write_urb->num_sgs;
		usb_anchor_urb(port->write_urb, &port->write_submitted);
		retval = usb_submit_urb(port->write_urb,GFP_KERNEL);
		if(retval==0) {
			retval = wait_for_completion_interruptible(&done);
			if(retval==0)
				retval = port->write_urb->actual_length;
			port->write_urb->context = NULL;
			port->sent_packet_count += sg_num;

		} else {
			skw_port_log(port->portno, "%s retval = %d\n", __func__, retval);
			usb_unanchor_urb(port->write_urb);
			port->write_urb->context = NULL;
		}
	}
	if(retval > 0)
		return 0;
	return retval;
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static int send_data(int portno, char *buffer, int total)
{
	struct usb_port_struct *port;
	//u32 *data = (u32 *)buffer;

	if(total==0)
		return 0;
	if (modem_status != MODEM_ON)
		return -EIO;
	port = usb_ports[portno];
	if(!port || !port->state)
		return -EIO;
	//if (total % port->epout->wMaxPacketSize == 0)
	//	total++;
	return bulkout_write(port, buffer, total);
}
static int send_data_async(int portno, char *buffer, int total)
{
	struct usb_port_struct *port;

	if(total==0)
		return 0;
	if (modem_status != MODEM_ON)
		return -EIO;
	port = usb_ports[portno];
	if(!port || !port->state)
		return -EIO;
	return bulkout_write_async(port, buffer, total);
}

int recv_data(int portno, char *buffer, int total)
{
	struct usb_port_struct *port;

	if(total==0)
		return 0;

	port = usb_ports[portno];
	if(!port || !port->state)
		return -EIO;
	return bulkin_read(port, buffer, total);
}

int close_usb_port(int portno)
{
	struct usb_port_struct *port;

	port = usb_ports[portno];

	if (port) {
		port->state = 1;
		if(port->write_urb && port->write_urb->context)
			usb_kill_urb(port->write_urb);
		if(port->read_urb && port->read_urb->context)
			usb_kill_urb(port->read_urb);
		if(port->thread && down_timeout(&port->sem, 3000))
			skw_usb_info("port%d rx thread exit\n", portno);
		port->thread = NULL;
	}
	return 0;
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
int wifi_send_cmd(int portno, struct scatterlist *sg, int sg_num, int total)
{
	struct usb_port_struct *port;
	u32 *data;
	int ret;

	if(total==0)
		return 0;
	if (modem_status != MODEM_ON)
		return -EIO;
	if(portno >= MAX_USB_PORT)
		return -EINVAL;
	port = usb_ports[portno];
	if(!port || !port->state)
		return -EIO;
	if (port->suspend)
		skw_usb_info("port%d is suspended\n", portno);
	if (portno == 0) {
		data = (u32 *)sg_virt(sg);
		memcpy(last_sent_wifi_cmd, data, 12);
		last_sent_time = jiffies;
	}
	ret = bulkout_write_sg(port, sg, sg_num, total);
	return ret;
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
int wifi_send_cmd_async(int portno, struct scatterlist *sg, int sg_num, int total)
{
	struct usb_port_struct *port;
	u32 *data;
	int ret;

	if(total==0)
		return 0;
	if (modem_status != MODEM_ON)
		return -EIO;
	if(portno >= MAX_USB_PORT)
		return -EINVAL;
	port = usb_ports[portno];
	if(!port || !port->state)
		return -EIO;

	if (port->suspend)
		skw_usb_info("port%d is suspended\n", portno);
	if (portno == 0) {
		data = (u32 *)sg_virt(sg);
		memcpy(last_sent_wifi_cmd, data, 12);
		last_sent_time = jiffies;
	}
	ret = bulkout_write_sg_async(port, sg, sg_num, total);
	return ret;
}

/************************************************************************
 *Decription: manual assert modem
 *Author:jiayong.yang
 *Date:2021-08-03
 *Modfiy:
 *Notes: this function must not be invoked in IRQ context.
 ************************************************************************/
int modem_assert(void)
{
	struct usb_port_struct *port;
	struct recovery_data *recovery = SKW_USB_GET_RECOVERY_DATA();
	int ret = -1;
	u32 *cmd = last_sent_wifi_cmd;

	if(modem_status == MODEM_HALT){
		skw_usb_info("modem in recovery mode \n");
		return 0;
	}
	skw_usb_wake_lock();
	port = usb_ports[0];
	if(port && port->state) {
		recovery->cp_state =1;
		ret = usb_control_msg(port->udev, usb_sndctrlpipe(port->udev, 0),
				VENDOR_MSG_MODEM_ASSERT, USB_DIR_OUT| USB_TYPE_VENDOR|USB_RECIP_DEVICE,
				0,0,NULL,0,1000);
		skw_usb_err("SND ASSERT CMD ret = %d cmd: 0x%x 0x%x 0x%x :%x-%x:%x\n",
				ret, cmd[0], cmd[1], cmd[2], (u32)last_sent_time, (u32)jiffies,
				(u32)last_ack_time);
		modem_status = MODEM_HALT;
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
		schedule_delayed_work(&skw_except_work , msecs_to_jiffies(2000));
#else
		schedule_delayed_work(&skw_except_work , msecs_to_jiffies(6000));
#endif
	}
	return ret;
}

EXPORT_SYMBOL(modem_assert);
static int send_modem_service_command(u16 service, u16 command)
{
	struct usb_port_struct *port;
	int ret = -1;
	int timeout = 100;

	port = usb_ports[1];
	if(port)
		skw_usb_dbg("%s (%d,%d) state= %d\n", __func__,
			service, command, port->state);
	if(port && port->state) {
		skw_reinit_completion(download_done);
		ret = usb_control_msg(port->udev, usb_sndctrlpipe(port->udev, 0),
				VENDOR_MSG_SERVICE_CTRL, USB_DIR_OUT| USB_TYPE_VENDOR|USB_RECIP_DEVICE,
				service, command, NULL, 0, 1000);
	}
	if(command == SERVICE_START) {
		wait_for_completion_interruptible_timeout(&download_done, msecs_to_jiffies(timeout + 2000*service));
	} else {
		if(service==BT_SERVICE && modem_status==MODEM_ON)
			wait_for_completion_interruptible_timeout(&download_done, msecs_to_jiffies(50));
		service_state_map &= ~(1<<service);
	}
	return ret;
}

static int skw_get_packet_count(u8 portno)
{
	struct usb_port_struct *port;
	int ret = -1;
	u16 *packet_count, size=2;

	port = usb_ports[portno];
	if(port && port->state) {
		ret = usb_control_msg(port->udev, usb_rcvctrlpipe(port->udev, 0),
				VENDOR_MSG_PACKET_COUNT, USB_DIR_IN| USB_TYPE_VENDOR|USB_RECIP_DEVICE,
				portno, 0, port->read_buffer, size, 1000);

		packet_count = (u16 *)port->read_buffer;
		if(ret < 0)
			skw_port_log(portno,"%s (%d,%d) ret = %d\n", __func__, portno, *packet_count, ret);
		if(ret==size)
			port->max_packet_count = *packet_count;
		else
			port->max_packet_count = MAX_PACKET_COUNT;
	}
	return ret;
}


void skw_usb_cp_log(int disable)
{
	struct usb_port_struct *port;
	int ret = -1;

	port = usb_ports[0];
	if(port && port->state) {
		ret = usb_control_msg(port->udev, usb_rcvctrlpipe(port->udev, 0),
				VENDOR_MSG_LOG_SWITCH, USB_DIR_IN| USB_TYPE_VENDOR|USB_RECIP_DEVICE,
				disable, 0, NULL, 0, 1000);
		if(ret==0)
			cp_log_status = disable;
		skw_usb_info("%s (disable=%d) ret = %d\n", __func__, disable, ret);
	}
}
int skw_usb_cp_log_status(void)
{
	return cp_log_status;
}
/************************************************************************
 *Decription:send BT start command to modem.
 *Author:jiayong.yang
 *Date:2021-08-30
 *Modfiy:
 *
 ********************************************************************* */
int skw_BT_service_start(void)
{
	skw_usb_info("Enter modem_status=%d\n", modem_status);
	if (service_state_map & (1<<BT_SERVICE))
		return 0;

#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
	skw_usb_cp_log(1);
#endif

	return send_modem_service_command(BT_SERVICE, SERVICE_START);
}

EXPORT_SYMBOL(skw_BT_service_start);

/************************************************************************
 *Decription:send BT stop command to modem.
 *Author:jiayong.yang
 *Date:2021-08-30
 *Modfiy:
 *
 ********************************************************************* */
int skw_BT_service_stop(void)
{
	skw_usb_info("Enter modem_status=%d\n", modem_status);
	if (service_state_map & (1<<BT_SERVICE)){
		return send_modem_service_command(BT_SERVICE, SERVICE_STOP);
	}
	return 0;
}

EXPORT_SYMBOL(skw_BT_service_stop);
/************************************************************************
 *Decription:send WIFI start command to modem.
 *Author:jiayong.yang
 *Date:2021-08-30
 *Modfiy:
 *
 ********************************************************************* */
int skw_WIFI_service_start(void)
{
	int count=90;
	u16 cmd = SERVICE_START;

	skw_usb_info("Enter STARTWIFI---modem_status=%d, 0x%x\n",
			modem_status, service_state_map);
	if (modem_status == MODEM_HALT) {
		while(!usb_ports[1] && count--)
			msleep(10);
	}
	if (service_state_map & (1<<WIFI_SERVICE))
		return 0;
	if(wifi_port_share)
		cmd |= WIFI_PORT_SHARE_FLAG;

#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
	skw_usb_cp_log(1);
#endif

	return send_modem_service_command(WIFI_SERVICE, cmd);
}
EXPORT_SYMBOL(skw_WIFI_service_start);

/************************************************************************
 *Decription: send WIFI stop command to modem.
 *Author:jiayong.yang
 *Date:2021-08-30
 *Modfiy:
 *
 ********************************************************************* */
int skw_WIFI_service_stop(void)
{
	int count=70;
	skw_usb_info("Enter,STOPWIFI--- modem status %d, 0x%x\n",
			modem_status, service_state_map);
	if (modem_status == MODEM_HALT) {
		service_state_map &= ~(1<<WIFI_SERVICE);
		while(!usb_ports[1] && count--)
			msleep(10);
		return 0;
	}
	if (service_state_map & (1<<WIFI_SERVICE))
		return send_modem_service_command(WIFI_SERVICE, SERVICE_STOP);
	return 0;
}
EXPORT_SYMBOL(skw_WIFI_service_stop);
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-08-30
 *Modfiy:
 *
 ********************************************************************* */
static void bulkin_complete(struct urb *urb)
{
	struct usb_port_struct *port;
	int portno = usb_pipeendpoint(urb->pipe) - 1;

	port = usb_ports[portno];
	if(urb) {
		if(urb->status) {
			skw_usb_info("endpoint%d actual = %d status %d\n",
				usb_pipeendpoint(urb->pipe), urb->actual_length, urb->status);
		}
		if(urb->status == -ENOENT && port  && port->suspend)
			list_add_tail(&urb->urb_list, &port->suspend_urb_list);
		else	if (urb->context)
			complete(urb->context);
	}
}
static void bulkin_async_complete(struct urb *urb)
{
	struct usb_port_struct *port = urb->context;

	if(urb->status) {
		skw_usb_info("endpoint%d actual = %d status %d\n",
			usb_pipeendpoint(urb->pipe), urb->actual_length, urb->status);
	}
	if (port->suspend)
		skw_usb_info("port is suspended urb_stat=%d\n", urb->status);
	if(urb->status == -ENOENT && port->suspend)
		list_add_tail(&urb->urb_list, &port->suspend_urb_list);
	else if (port) {
		urb->context = NULL;
		spin_lock(&port->rx_urb_lock);
		list_add_tail(&urb->urb_list, &port->rx_done_urb_list);
		spin_unlock(&port->rx_urb_lock);
		wake_up_interruptible(&port->rx_wait);
	}
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static void bulkout_complete(struct urb *urb)
{
	if(urb->status)
		skw_usb_info("endpoint%d actual = %d status %d\n",
			usb_pipeendpoint(urb->pipe),  urb->actual_length, urb->status);

	if (urb->context)
		complete(urb->context);
}

static void bulkout_async_complete(struct urb *urb)
{
	struct usb_port_struct *port = urb->context;
	//unsigned long flags;

	if(urb->status) {
		if(urb->sg && port->adma_tx_callback) {
			u32 *data;

			data = (u32 *)sg_virt(urb->sg);
			if((data[0] >>7) > 0x600 && port->portno)
				skw_usb_info("%s invalid packet size: 0x%x\n", __func__, data[0] >>7);
			port->adma_tx_callback(port->portno, urb->sg, urb->num_sgs, port->tx_data, urb->status);
		}
		else if(urb->transfer_buffer && port->sdma_tx_callback)
			port->sdma_tx_callback(port->portno, urb->transfer_buffer, urb->transfer_buffer_length, port->tx_data, urb->status);
		skw_usb_info("port%d endpoint%d actual = %d status %d\n",port->portno,
				usb_pipeendpoint(urb->pipe), urb->actual_length, urb->status);
	} else if(urb->sg && port->adma_tx_callback) {
		port->adma_tx_callback(port->portno, urb->sg, urb->num_sgs, port->tx_data, 0);
		port->sent_packet_count += urb->num_sgs;
	} else if(urb->transfer_buffer && port->sdma_tx_callback)
		port->sdma_tx_callback(port->portno, urb->transfer_buffer, urb->transfer_buffer_length, port->tx_data, 0);
	urb->context = NULL;
	spin_lock(&port->tx_urb_lock);
	list_add_tail(&urb->urb_list, &port->tx_urb_list);
	port->tx_urb_count--;
	if(port->tx_urb_count==0 && port->sent_packet_count!=port->req_tx_packet)
		skw_usb_info("%s port[%d]= %d %d\n", __func__, port->portno, port->sent_packet_count, port->req_tx_packet);
	spin_unlock(&port->tx_urb_lock);
	wake_up_interruptible(&port->tx_wait);
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
int bulkin_read_timeout(int portno, char *buffer, int size, int *actual, int timeout)
{
	struct usb_port_struct *port;
	unsigned int pipe;
	int	ret;

	if(portno >= MAX_USB_PORT || !buffer || !size)
		return -EINVAL;
	port = usb_ports[portno];
	if(!port->state)
		return -EIO;
	if(actual)
		*actual = 0;
	pipe = usb_rcvbulkpipe(port->udev, port->epin->bEndpointAddress);
	ret = usb_bulk_msg(port->udev, pipe, buffer, size, actual,timeout);
	if(ret)
		return ret;
	if(port == log_port) {
		if(assert_info_print && assert_info_print<28 && *actual<100) {
			assert_info_print++;
			if(*actual > 4)
				printk("%s", (char *)buffer);
		}
		if(*actual == 4)
			assert_info_print = 28;
	}
	if(actual)
		return *actual;
	return ret;
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
int bulkout_write_timeout(int portno, char *buffer, int size, int *actual, int timeout)
{
	struct usb_port_struct *port;
	unsigned int pipe;
	int	ret;

	if(portno >= MAX_USB_PORT || !buffer || !size)
		return -EINVAL;
	port = usb_ports[portno];
	
	if(!port->state)
		return -EIO;
	if(actual)
		*actual = 0;
	pipe = usb_sndbulkpipe(port->udev, port->epout->bEndpointAddress);
	ret = usb_bulk_msg(port->udev, pipe, buffer, size, actual,timeout);
	if(ret)
		return ret;
	if(actual)
		return *actual;
	return ret;
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static int register_rx_callback(int id, void *func, void *para);
static int skw_usb_dump_read(unsigned int address, void *buf, unsigned int len);
static int register_tx_callback(int id, void *func, void *para);
static struct sv6160_platform_data wifi_pdata = {
	.data_port = 0,
	.cmd_port = 1,
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
	.bus_type = USB_LINK|TX_ADMA|RX_SDMA|TX_ASYN|CP_RLS,
#else
	.bus_type = USB_LINK|TX_ADMA|RX_SDMA|TX_ASYN|CP_DBG,
#endif
	.max_buffer_size = MAX_BUFFER_SIZE,
	.align_value = 512,
	.hw_adma_tx = wifi_send_cmd,
	.hw_sdma_tx = send_data,
	.hw_adma_tx_async = wifi_send_cmd_async,
	.hw_sdma_tx_async = send_data_async,
	.callback_register = register_rx_callback,
	.modem_assert = modem_assert,
	.modem_register_notify = modem_register_notify,
	.modem_unregister_notify = modem_unregister_notify,
	.at_ops = {
		.port = 2,
		.open = open_usb_port,
		.close = close_usb_port,
		.read = recv_data,
		.write = send_data,
	},
	.tx_callback_register = register_tx_callback,
};


/**********************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 **********************************************************************/
int usb_port_async_entry(void *para)
{
	struct usb_port_struct *port = para;
	struct scatterlist *sgs, *sg;
	struct sched_param param;
	unsigned long flags;
	int   size, read, ret;
	u16	mpc;
	char *buffer;
	struct urb *urb;
	u16  data_flag = 0x8000;

	if(port->portno == 0) {
		param.sched_priority = USB_RX_TASK_PRIO;
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
		sched_set_fifo_low(current);
#else
		sched_setscheduler(current, SCHED_FIFO, &param);
#endif
	}
	if(port->max_packet_count)
		mpc = port->max_packet_count;
	else
		mpc = 2;

	if (!strncmp(skw_chipid, "SV6316", 6) ||
            !strncmp(skw_chipid, "SV6160LITE", 10))
		data_flag = 2;
	sgs = kzalloc((mpc+1)*sizeof(struct scatterlist), GFP_KERNEL);
	if (!sgs)
		return -ENOMEM;
	usb_port_alloc_recv_urbs(port, port->epin, 3, 32*1024);
	usb_port_alloc_xmit_urbs(port, port->epout,3,0);
	skw_usb_info("%s %d running packet %d %s %d...\n", __func__, port->portno, mpc, skw_chipid, data_flag);
	while(!list_empty(&port->rx_urb_list)) {
		ret = bulkin_read_async(port);
	}

	while(port->state){
		int  *data, sg_count, offset;

		sg_init_table(sgs, mpc+1);
		spin_lock_irqsave(&port->rx_urb_lock, flags);
		if(list_empty(&port->rx_done_urb_list)) {
			spin_unlock_irqrestore(&port->rx_urb_lock, flags);
			wait_event_interruptible(port->rx_wait, (!list_empty(&port->rx_done_urb_list)));
			spin_lock_irqsave(&port->rx_urb_lock, flags);
		}
		urb = list_first_entry(&port->rx_done_urb_list, struct urb, urb_list);
		list_del_init(&urb->urb_list);
		list_add_tail(&urb->urb_list, &port->rx_urb_list);
		spin_unlock_irqrestore(&port->rx_urb_lock, flags);

		read = urb->actual_length;
		buffer = urb->transfer_buffer;
		if(urb->status < 0 || !port->state) {
			dev_err(&port->udev->dev, "%s bulkin read status=%d state=%d\n", __func__, urb->status, port->state);
			break;
		}
		if(port->rx_submit) {
			int is_cmd;
			u32 d32;
			data = (int *)buffer;
			d32 = data[0];
			offset = 0;
			sg_count = 0;
			sg = sgs;
			is_cmd = 0;
			while(offset+12 < read) {
				sg_count++;
				if(sg_count > mpc) {
					skw_usb_warn("packet count is overflow %d : %d : %d : %d!!!\n",
							offset, read, sg_count, mpc);
					sg_count--;
					break;
				}
				size = data[2] >> 16;
				size += 3;
				size = size & 0xfffffffc;
				if(data[2] & data_flag) {
					if (sg_count > 1 && !is_cmd)
						size = -1;
					else
						is_cmd = 1;
				}
				if(size + offset > read || size > 2048 || size < 12) {
					skw_usb_warn("Invalid packet size=%d: %d : %d :%d  0x%x:0x%x!!!\n",
						size, offset, read, sg_count, d32, data[2]);
					print_hex_dump(KERN_ERR, "PACKET ERR:", 0, 16, 1,
					      urb->transfer_buffer, urb->actual_length, 1);
					sg_count--;
					break;
				}
				sg_set_buf(sg,  &buffer[offset], size);
				sg++;
				offset  += size;
				if (is_cmd) {
					dev_dbg(&port->udev->dev, "rx_submit:command:0x%x 0x%x 0x%x 0x%x len=%d\n",
							 data[0], data[1], data[2], data[3], read);
					if ((data[2] & 0xff) == 0x10)
						memcpy(last_recv_wifi_ack, &data[2], 12);
					else
						memcpy(last_recv_wifi_evt, &data[2], 12);
					last_ack_time = jiffies;
				}
				data = (int *)&buffer[offset];
			}
			if(sg_count >15)
				dev_info(&port->udev->dev, "rx_submit: port%d packet count %d\n",
					port->portno, sg_count);
			if(is_cmd) {
				port = usb_ports[wifi_pdata.cmd_port];
			} else
				port = usb_ports[wifi_pdata.data_port];
			if (port) {
				if (port->suspend)
					dev_info(&port->udev->dev, "submit command(%d) in suspend status", is_cmd);
				port->rx_submit(port->portno, sgs, sg_count, port->rx_data);
				port->rx_packet_count += sg_count;
			} else {
				pr_err("%s port = NULL!!!\n", __func__);
			}
			if (modem_status != MODEM_ON)
				break;
			port = para;
			ret = bulkin_read_async(port);
		}
	}
	dev_info(&port->udev->dev, "%s-port%d is stopped\n", __func__, port->portno);
	kfree(sgs);

	if(port->write_urb && port->write_urb->context) {
		usb_kill_anchored_urbs(&port->write_submitted);
	}
	if(port->read_urb && port->read_urb->context) {
		usb_kill_anchored_urbs(&port->read_submitted);
	}

	skw_usb_info("%s port%d exit context = %p\n",
			__func__, port->portno, port->write_urb->context);
	if(port->write_urb && port->write_urb->context)
		wait_for_completion_interruptible(port->write_urb->context);

	spin_lock_irqsave(&port->rx_urb_lock, flags);
	while(!list_empty(&port->rx_urb_list)) {
		urb = list_first_entry(&port->rx_urb_list, struct urb, urb_list);
		list_del_init(&urb->urb_list);
		if(urb->transfer_buffer)
			kfree(urb->transfer_buffer);
		skw_usb_info("%s release rx  urb %p\n", __func__, urb);
		usb_free_urb(urb);
	}
	spin_unlock_irqrestore(&port->rx_urb_lock, flags);
	while(!list_empty(&port->tx_urb_list)) {
		urb = list_first_entry(&port->tx_urb_list, struct urb, urb_list);
		list_del_init(&urb->urb_list);
		skw_usb_info("%s release tx urb %p\n", __func__, urb);
		usb_free_urb(urb);
	}
	up(&port->sem);
	return 0;
}
int usb_port_entry(void *para)
{
	struct usb_port_struct *port = para;
	struct scatterlist *sgs, *sg;
	struct sched_param param;
	int   size, read, buf_size;
	u16	mpc;
	char *buffer;
	u16  data_flag = 0x8000;

	if(port->portno == 0) {
		param.sched_priority = USB_RX_TASK_PRIO;
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
		sched_set_fifo_low(current);
#else
		sched_setscheduler(current, SCHED_FIFO, &param);
#endif
	}

	if(port->max_packet_count)
		mpc = port->max_packet_count;
	else
		mpc = 2;

	if (!strncmp(wifi_pdata.chipid, "SV6160LITE", 10))
		data_flag = 2;
	sgs = kzalloc((mpc+1)*sizeof(struct scatterlist), GFP_KERNEL);
	if (!sgs)
		return -ENOMEM;
	buf_size = 1568 * mpc;
	buffer = kzalloc(buf_size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	skw_usb_info("%s%d (MPC %d buffer_size 0x%x )is runninng\n",
			__func__, port->portno, mpc, buf_size);
	while(port->state){
		int  *data, sg_count, offset;

		sg_init_table(sgs, mpc+1);
		read = 0;
read_msg:
		do{
			if(port->state==0)
				break;
			read = bulkin_read(port, buffer, buf_size);

		}while(!read);

		if(read < 0 || !port->state) {
			dev_err(&port->udev->dev, "bulkin read_len=%d state = %d\n", read, port->state);
			break;
		}
		if (modem_status != MODEM_ON)
			break;
		if(port->rx_submit) {
			int is_cmd;
			data = (int *)buffer;
			offset = 0;
			sg_count = 0;
			sg = sgs;
			is_cmd = 0;
			while(offset < read) {
				sg_count++;
				if(sg_count > mpc) {
					skw_usb_warn("packet count is overflow %d : %d : %d : %d!!!\n",
							offset, read, sg_count, mpc);
					sg_count--;
					break;
				}
				size = data[2] >> 16;
				size += 3;
				size = size & 0xfffffffc;
				if(size + offset > read)
					size = read  - offset;
				dev_dbg(&port->udev->dev,"submit len=%d size =%d msg: 0x%x 0x%x 0x%x \n",
					 read, size, data[0], data[1], data[2]);
				if ((data[2] & 0xff) == 0x10)
					memcpy(last_recv_wifi_ack, &data[2], 12);
				else
					memcpy(last_recv_wifi_evt, &data[2], 12);
				last_ack_time = jiffies;
				if(data[2] & data_flag)
					is_cmd = 1;
				sg_set_buf(sg,  &buffer[offset], size);
				sg++;
				offset  += size;
				data = (int *)&buffer[offset];
			}
			if(sg_count >15)
				dev_info(&port->udev->dev, "rx_submit: port%d packet count %d\n",
					port->portno, sg_count);
			if(is_cmd)
				port = usb_ports[wifi_pdata.cmd_port];
			port->rx_submit(port->portno, sgs, sg_count, port->rx_data);
			port->rx_packet_count += sg_count;
			port = para;
		}
		else if(port->state)
			goto read_msg;
	}
	kfree(buffer);
	dev_info(&port->udev->dev, "%s-port%d is stopped\n", __func__, port->portno);
	kfree(sgs);

	if(port->read_urb && port->read_urb->context) {
		usb_kill_anchored_urbs(&port->read_submitted);
	}
	if(port->write_urb && port->write_urb->context) {
		usb_kill_anchored_urbs(&port->write_submitted);
        if(port->write_urb->context)
            wait_for_completion_interruptible(port->write_urb->context);
    }
	up(&port->sem);
	return 0;
}
static void skw_usb_kill_wifi_threads(struct usb_port_struct *p)
{
	int i;
	struct usb_port_struct *port;
	for(i=0; i<3; i++) {
		port = usb_ports[i];
		if(port==NULL)
			break;
		if(port && port->thread) {
			port->state = 0;
			usb_kill_anchored_urbs(&port->write_submitted);
			usb_kill_anchored_urbs(&port->read_submitted);
		}
	}
}

void show_assert_context(void)
{
	int read;

	if(log_port && log_port->state!=2) {
		char *buffer;
		buffer = kzalloc(1024, GFP_KERNEL);
		if (!buffer)
			return;
		open_usb_port(log_port->portno, 0, 0);
		do {
			read = bulkin_read_timeout(log_port->portno, buffer, 1024, &read, 1000);
			if(read == 4 || read < 0) {
				close_usb_port(log_port->portno);
				break;
			}
		}while(assert_info_print<100);
		kfree(buffer);
	}
}

int usb_loopcheck_entry(void *para)
{
	struct usb_port_struct *port = para;
	char *buffer;
	int read, size;
	int count= 0, timeout=100;
	struct recovery_data *recovery = SKW_USB_GET_RECOVERY_DATA();

	size = 512;
	buffer = kzalloc(size, GFP_KERNEL);
	while(port->state && buffer){
		read = 0;
		memset(buffer,0,size);
		do{
			if(port->state==0)
				break;
			read = bulkin_read(port, buffer, 256);
		}while(!read);
		if (port->suspend) {
			dev_err(&port->udev->dev, " is suspended\n");
			msleep(500);
			continue;
		}

		if(read < 0 || !port->state) {
			dev_err(&port->udev->dev, "bulkin read_len=%d\n",read);
			break;
		}
		if(strncmp(buffer, "BSPREADY", read))
			skw_usb_info("recv(%d): %s\n", read, buffer);
		memcpy(buffer+256, "LOOPCHECK", 9);
		if (read==8 && !strncmp(buffer, "BSPREADY", read)) {
			bulkout_write(port, buffer+256, 9);
		} else if (read==9 && !strncmp(buffer, "WIFIREADY", read)) {
			service_state_map |= (1<<WIFI_SERVICE);
			complete(&download_done);
			timeout=1000;
			bulkout_write(port, buffer+256, 9);
		} else if (read==6 && !strncmp(buffer, "BTEXIT", read)) {
			complete(&download_done);
		} else if (read==7 && !strncmp(buffer, "BTREADY", read)) {
			service_state_map |= (1<<BT_SERVICE);
			complete(&download_done);
			bulkout_write(port, buffer+256, 9);
		} else if (read==9 && !strncmp(buffer, "BSPASSERT", read)) {
			skw_usb_err("cmd:0x%x 0x%x 0x%x ack:%x %x:%x event:0x%x:0x%x:0x%x time:0x%x:0x%x:0x%x\n",
			       last_sent_wifi_cmd[0],last_sent_wifi_cmd[1],last_sent_wifi_cmd[2],
			       last_recv_wifi_ack[0],last_recv_wifi_ack[1],last_recv_wifi_ack[2],
			       last_recv_wifi_evt[0],last_recv_wifi_evt[1],last_recv_wifi_evt[2],
			       (u32)jiffies,(u32)last_sent_time, (u32)last_ack_time);
			if(recovery->cp_state==1)
				cancel_delayed_work_sync(&skw_except_work);

			mutex_lock(&recovery->except_mutex);
			if(recovery->cp_state==DEVICE_BLOCKED_EVENT){
				mutex_unlock(&recovery->except_mutex);
				break;
			}
			skw_usb_wake_lock();
			recovery->cp_state = 1;
			mutex_unlock(&recovery->except_mutex);

			assert_info_print = 1;
			memset(buffer, 0, read);
			skw_usb_kill_wifi_threads(port);
			modem_status = MODEM_HALT;
			show_assert_context();
			modem_notify_event(DEVICE_ASSERT_EVENT);
#ifndef CONFIG_SEEKWAVE_PLD_RELEASE
			read = bulkin_read(port, buffer, 256);
			skw_usb_info("bspassert after recv(%d): %s\n", read, buffer);
#endif
			msleep(10);
			modem_notify_event(DEVICE_DUMPDONE_EVENT);
			msleep(10);
			skw_recovery_mode();
			service_state_map =0;

			break;
		} else if (!strncmp("trunk_W", buffer, 7)) {
			recovery->cp_state = 0;
			assert_info_print = 0;
			modem_status = MODEM_ON;
			memset(firmware_version, 0 , sizeof(firmware_version));
			strncpy(firmware_version, buffer, read);
			modem_notify_event(DEVICE_BSPREADY_EVENT);
			count = 0;
			skw_usb_wake_unlock();
			usb_setup_service_devices();
		}
		msleep(timeout);
	}
	dev_info(&port->udev->dev, "%s-port%d is stopped\n", __func__, port->portno);

	if(port->read_urb && port->read_urb->context) { 
		usb_kill_anchored_urbs(&port->read_submitted);
	}

	if(port->write_urb && port->write_urb->context) {
		usb_kill_anchored_urbs(&port->write_submitted);
        if (port->write_urb->context)
            wait_for_completion_interruptible(port->write_urb->context);
	}

	kfree(buffer);
	up(&port->sem);
	return 0;
}

static int usb_bt_rx_entry(void *para)
{
	struct usb_port_struct *port = para;
	char *buffer;
	int read, size;

	size = 2048;
	buffer = kzalloc(size, GFP_KERNEL);
	while(port->state==2 && buffer){
		read = 0;
		memset(buffer,0,size);
		do{
			if(port->state != 2)
				break;
			read = bulkin_read(port, buffer, size);
		}while(!read);

		if(read < 0) {
			dev_err(&port->udev->dev, "bulkin read_len=%d\n",read);
			break;
		}
		if(port->rx_submit)
			port->rx_submit(port->portno, port->rx_data, read, buffer);
	}
	dev_info(&port->udev->dev, "%s-port%d is stopped\n", __func__, port->portno);
	if(port->write_urb && port->write_urb->context) {
		usb_kill_anchored_urbs(&port->write_submitted);
	}
	if(port->read_urb && port->read_urb->context) {
		usb_kill_anchored_urbs(&port->read_submitted);
	}

	if(buffer)
		kfree(buffer);
	up(&port->sem);
	return 0;
}


/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static struct sv6160_platform_data ucom_pdata = {
	.max_buffer_size =0x800,
	.bus_type = USB_LINK,
	.hw_sdma_tx = send_data,
	.hw_sdma_rx = recv_data,
	.open_port = open_usb_port,
	.close_port = close_usb_port,
	.modem_assert = modem_assert,
	.modem_register_notify = modem_register_notify,
	.modem_unregister_notify = modem_unregister_notify,
	.skw_dump_mem = skw_usb_dump_read,
};



static int skw_usb_dump_read(unsigned int address, void *buf, unsigned int len)
{
	return 0;
}
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static int register_rx_callback(int id, void *func, void *para)
{
	if(id >= MAX_USB_PORT)
		return -EINVAL;

	if(usb_ports[id] == NULL)
		return -EIO;
	if(func && !usb_ports[id]->rx_submit) {
		if(id==1)
			skw_WIFI_service_start();
		usb_ports[id]->rx_submit = func;
		usb_ports[id]->rx_data = para;
		return 0;
	} else if(!func && usb_ports[id]->rx_submit) {
		if(id==1)
			skw_WIFI_service_stop();

		usb_ports[id]->rx_submit = func;
		usb_ports[id]->rx_data = para;
		return 0;
	}
	if(wifi_pdata.bus_type & TX_ASYN) {
		if(wifi_pdata.bus_type & TX_SDMA)
			usb_ports[id]->sdma_tx_callback = func;
		else
			usb_ports[id]->adma_tx_callback = func;
	}
	usb_ports[id]->tx_data = para;
	return 0;
}

/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static int register_tx_callback(int id, void *func, void *para)
{
	if(id >= MAX_USB_PORT)
		return -EINVAL;

	if(usb_ports[id] == NULL)
		return -EIO;
	if(wifi_pdata.bus_type & TX_ASYN) {
		if(wifi_pdata.bus_type & TX_SDMA)
			usb_ports[id]->sdma_tx_callback = func;
		else
			usb_ports[id]->adma_tx_callback = func;
	}
	usb_ports[id]->tx_data = para;
	return 0;
}


/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static int skw_usb_io_probe(struct usb_interface *interface,
				const struct usb_device_id *id)
{
	struct usb_port_struct *port;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *epd;
	struct platform_device *pdev;
	struct usb_device *udev = interface_to_usbdev(interface);
	char	pdev_name[32], names[32];
	int	i, ret, dloader=0;

	memset(names, 0 ,sizeof(names));
	iface_desc = interface->cur_altsetting;
	if (iface_desc->string == NULL)
		return -EINVAL;
	sprintf(names, "%s", iface_desc->string);

	if (!strncmp(names, "Boot", 4))
		dloader = 1;

//	if ((udev->descriptor.idProduct != 0x6316) && !dloader
//		&& chip_en_gpio < 0 && modem_status == MODEM_OFF)
//		return -EINVAL;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	if (!strncmp(names, "WIFITCMD", 8))
		wifi_port_share = 1;
	usb_ports[iface_desc->desc.bInterfaceNumber] = port;
	INIT_LIST_HEAD(&port->rx_urb_list);
	INIT_LIST_HEAD(&port->tx_urb_list);
	INIT_LIST_HEAD(&port->rx_done_urb_list);
	INIT_LIST_HEAD(&port->suspend_urb_list);
	spin_lock_init(&port->rx_urb_lock);
	spin_lock_init(&port->tx_urb_lock);
	port->tx_urb_count = 0;
	init_waitqueue_head(&port->rx_wait);
	init_waitqueue_head(&port->tx_wait);
	if(dloader)
		dloader = 1;
	else if(iface_desc->desc.bInterfaceNumber == 1) {
		sprintf(pdev_name, "%s%d", SV6160_WIRELESS,
			iface_desc->desc.bInterfaceNumber);

		pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
		if(!pdev)
			return -ENOMEM;
	} else {
#ifdef CONFIG_BT_SEEKWAVE
		pdev = NULL;
		if (!strncmp(names, "DATA", 4)) {
			ucom_pdata.data_port = 0;
		} else	if (!strncmp(names, "BTDATA", 6))
			ucom_pdata.data_port = iface_desc->desc.bInterfaceNumber;
		else	if (!strncmp(names, "BTCMD", 5))
			ucom_pdata.cmd_port = iface_desc->desc.bInterfaceNumber;
		else	if (!strncmp(names, "BTAUDIO", 7)) {
			ucom_pdata.audio_port = iface_desc->desc.bInterfaceNumber;
			sprintf(pdev_name, "%s", "btseekwave");
			ucom_pdata.port_name = "BTHCI";
			bluetooth_pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
			if (!bluetooth_pdev)
				return -ENOMEM;
			bluetooth_pdev->dev.parent = &udev->dev;
			bluetooth_pdev->dev.dma_mask = &port_dmamask;
			bluetooth_pdev->dev.coherent_dma_mask = port_dmamask;
			bt_audio_port = iface_desc->desc.bInterfaceNumber;
			platform_device_add_data(bluetooth_pdev, &ucom_pdata, sizeof(ucom_pdata));
		} else if (!strncmp(names, "AUDIO", 5)) {
			ucom_pdata.audio_port = 0;
			sprintf(pdev_name, "%s", "btseekwave");
			ucom_pdata.port_name = "BTHCI";
			bluetooth_pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
			if (!bluetooth_pdev)
				return -ENOMEM;
			bluetooth_pdev->dev.parent = &udev->dev;
			bluetooth_pdev->dev.dma_mask = &port_dmamask;
			bluetooth_pdev->dev.coherent_dma_mask = port_dmamask;
			bt_audio_port = iface_desc->desc.bInterfaceNumber;
			platform_device_add_data(bluetooth_pdev, &ucom_pdata, sizeof(ucom_pdata));
		} else
#endif
		if (iface_desc->desc.bInterfaceNumber > 0) {
			sprintf(pdev_name, "%s", "skw_ucom");
			ucom_pdata.port_name = iface_desc->string;
			ucom_pdata.data_port = iface_desc->desc.bInterfaceNumber;
			pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
			if(!pdev)
				return -ENOMEM;
		}
	}
	if(!dloader) {
		if (1==iface_desc->desc.bInterfaceNumber && wifi_data_pdev) {
			pdev = wifi_data_pdev;
			pdev->dev.parent = NULL;
			port->pdev = pdev;
		} else if (iface_desc->desc.bInterfaceNumber && pdev) {
			pdev->dev.parent = &udev->dev;
			pdev->dev.dma_mask = &port_dmamask;
			pdev->dev.coherent_dma_mask = port_dmamask;

			if(iface_desc->desc.bInterfaceNumber == 1) {
				wifi_pdata.align_value = iface_desc->endpoint[0].desc.wMaxPacketSize;
				if(usb_boot_data && usb_boot_data->iram_dl_size >0x70000)
					wifi_pdata.at_ops.port = 5;
				else
					wifi_pdata.at_ops.port = 2;
				if(udev->config->string && !strncmp(udev->config->string, "ECOM", 4)) {
					wifi_pdata.bus_type &= ~TYPE_MASK;
					wifi_pdata.bus_type |= USB2_LINK;
				}
				ret = platform_device_add_data(pdev, &wifi_pdata, sizeof(wifi_pdata));
				modem_status = MODEM_ON;
			} else
				ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));

			if(ret) {
				dev_err(&udev->dev, "failed to add platform data \n");
				platform_device_put(pdev);
				kfree(port);
				return ret;
			}
			if(iface_desc->desc.bInterfaceNumber>1){
				ret = platform_device_add(pdev);
				if(ret) {
					dev_err(&udev->dev, "failt to register platform device\n");
					platform_device_put(pdev);
					kfree(port);
					return ret;
				}
			}
			port->pdev = pdev;
		}
	}
	usb_set_intfdata(interface, port);

	port->interface = usb_get_intf(interface);
	port->udev = usb_get_dev(udev);
	/* register struct wcn_usb_intf */
	dev_info(&port->udev->dev, "intf[%x] is registerred: ep count %d %s\n",
			iface_desc->desc.bInterfaceNumber,
			iface_desc->desc.bNumEndpoints,
			iface_desc->string);
	ret = -ENOMEM;
	for(i=0; i<iface_desc->desc.bNumEndpoints; i++) {

		epd = &iface_desc->endpoint[i].desc;
		port->buffer_size = MAX_BUFFER_SIZE;
		port->ep_mps = epd->wMaxPacketSize;
		if(usb_endpoint_is_bulk_in(epd)) {
			port->epin = epd;
			port->read_urb = usb_alloc_urb(0, GFP_KERNEL);
			if(!port->read_urb)
				goto err0;
			if(iface_desc->desc.bInterfaceNumber > 1) {
				port->read_buffer = NULL;
				port->buffer_size = 0;
			} else {
				port->read_buffer = kzalloc(port->buffer_size , GFP_KERNEL);
				if(!port->read_buffer)
					goto err0;
			}
			usb_fill_bulk_urb(port->read_urb, udev,
				usb_rcvbulkpipe(udev, epd->bEndpointAddress),
				port->read_buffer, port->buffer_size,
				bulkin_complete, port);
			port->read_urb->context = NULL;
			init_usb_anchor(&port->read_submitted);
			dev_dbg(&pdev->dev, "BulkinEP = 0x%x rp=%p\n",
					epd->bEndpointAddress, port->read_buffer);
		} else if(usb_endpoint_is_bulk_out(epd)) {
			port->epout = epd;
			port->write_urb = usb_alloc_urb(0, GFP_KERNEL);
			if(!port->write_urb)
				goto err0;
			if(iface_desc->desc.bInterfaceNumber > 1) {
				port->write_buffer = NULL;
				port->buffer_size = 0;
			} else{
				port->write_buffer = kzalloc(port->buffer_size, GFP_KERNEL);
				if(!port->write_buffer)
					goto err0;
			}
			usb_fill_bulk_urb(port->write_urb, udev,
				usb_sndbulkpipe(udev, epd->bEndpointAddress),
				port->write_buffer, port->buffer_size, bulkout_complete,port);
			port->write_urb->context = NULL;
			init_usb_anchor(&port->write_submitted);
			dev_dbg(&pdev->dev, "BulkoutEP = 0x%x wp =%p context %p\n",
					epd->bEndpointAddress, port->write_buffer, port->write_urb->context);
		}
	}
	if(!dloader) {
		port->portno = iface_desc->desc.bInterfaceNumber;
		port->state = 1;
		if (port->portno<=1) {
			if (!strncmp(names, "WIFIDATA", 8)) {
				skw_get_packet_count(port->portno);
				wifi_pdata.cmd_port = 1 - port->portno;
				wifi_pdata.data_port = port->portno;
				port->thread = kthread_create(usb_port_async_entry, port, iface_desc->string);
			} else {
				wifi_pdata.cmd_port = port->portno;
				wifi_pdata.data_port = 1 - port->portno;
				if(!strncmp(names, "WIFICMD", 7))
					port->thread = kthread_create(usb_port_entry, port, iface_desc->string);
			}
			if(port->thread) {
				sema_init(&port->sem, 0);
				wake_up_process(port->thread);
			} else
				sema_init(&port->sem, 1);
		} else if(!strncmp(names, "LOOP", 4)) {
			sema_init(&port->sem, 0);
			port->thread = kthread_create(usb_loopcheck_entry, port, iface_desc->string);
			if(port->thread)
				wake_up_process(port->thread);
		} else	sema_init(&port->sem, 1);
	} else {
		port->state = 1;
		assert_info_print = 0;
		INIT_WORK(&port->work, dloader_work);
		if(modem_status == MODEM_HALT) {
			dev_info(&udev->dev, "schedule dloader work to recovery modem\n");
			skw_usb_wake_lock();
			schedule_work(&port->work);
		}
		port->is_dloader = 1;
	}
	if (!strncmp(names, "LOG", 3))
		log_port = port;
	return 0;
err0:
	dev_err(&udev->dev, "no memory  to register device\n");
	if(port->write_buffer)
		kfree(port->write_buffer);
	if(port->read_buffer)
		kfree(port->read_buffer);
	if(port->write_urb)
		usb_free_urb(port->write_urb);
	if(port->read_urb)
		usb_free_urb(port->read_urb);
	if(port->pdev)
		platform_device_unregister(port->pdev);
	usb_ports[iface_desc->desc.bInterfaceNumber] = NULL;
	kfree(port);
	return ret;
}

/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static int launch_download_work(char *data, int size,int addr)
{
	int chk_ports =0;
	firmware_size = size;//link to usb_download size
	firmware_data	= data;//link to usb_download dl_data
	firmware_addr = addr;
	do{
		if((usb_ports[0] !=NULL)&&(usb_ports[0]->state)) {
			chk_ports = 1;
			break;
		}
		msleep(10);
	}while(!chk_ports);
	schedule_work(&usb_ports[0]->work);
	return 0;
}

static int skw_recovery_mode(void)
{
	if(chip_en_gpio > 0 && !recovery_debug_status) {
		if (usb_ports[0])
			usb_reset_device(usb_ports[0]->udev);
		gpio_set_value(chip_en_gpio, 0);
		skw_usb_info("set chip enable reset\n");
		msleep(30);
		gpio_set_value(chip_en_gpio, 1);
	} else {
		/*
		 *  call power  API here: power-off  delay-X-ms power-on
		 */
		//power-off() first
		if (usb_ports[0])
			usb_reset_device(usb_ports[0]->udev);
		msleep(30);
		//power-on();
	}
	return 0;
}

/************************************************************************
 *Decription:
 *Author:JUNWEI.JIANG
 *Date:2021-12-20
 *Modfiy:
 *
 ********************************************************************* */
static irqreturn_t skw_gpio_irq_handler(int irq, void *dev_id)
{
	int     value = gpio_get_value(host_wake_gpio);

	printk("gpio value = %d\n", value);
	return IRQ_HANDLED;
}

int skw_boot_loader(struct seekwave_device *boot_data)
{
	int ret = 1;

	usb_boot_data= boot_data;
	skw_usb_info("status:%d , chip_en_gpio=%d host_wake_gpio=%d", modem_status,
			usb_boot_data->chip_en, usb_boot_data->gpio_in);
	chip_en_gpio = usb_boot_data->chip_en;
	if(chip_en_gpio > 0)
		gpio_set_value(chip_en_gpio, 1);
	if (host_wake_gpio < 0 && usb_boot_data->gpio_in>=0) {
		int irq_num;

		host_wake_gpio = usb_boot_data->gpio_in;
		irq_num = gpio_to_irq(host_wake_gpio);
		ret = request_irq(irq_num, skw_gpio_irq_handler,
                                  IRQF_TRIGGER_RISING | IRQF_ONESHOT, "skw-gpio-irq", NULL);
		skw_usb_info("request_gpio_irq ret=%d\n", ret);
		if (ret == 0)
			enable_irq_wake(irq_num);
	}
	if(!boot_data->first_dl_flag ){
		if (usb_ports[0] && !usb_ports[0]->is_dloader) {
			usb_setup_service_devices();
		} else if(boot_data->iram_img_data !=NULL && boot_data->dram_img_data!=NULL){
			skw_usb_info("USB FIRST BOOT... \n");
			ret=launch_download_work(boot_data->iram_img_data,boot_data->iram_dl_size,boot_data->iram_dl_addr);
		}else{
			skw_usb_info("The CPBOOT not download from AP!!!!\n");
		}
	}
	if(boot_data->dl_module==RECOVERY_BOOT){
		skw_recovery_mode();
		return 0;
	}
	if(boot_data->service_ops==SKW_WIFI_START){
		//skw_WIFI_service_start();
		//skw_usb_info("----WIFI-SERVICE-----START!!!\n");
	}else if(boot_data->service_ops== SKW_WIFI_STOP &&
			(service_state_map & (1<<WIFI_SERVICE))){
		skw_WIFI_service_stop();
		//skw_usb_info("----WIFI-SERVICE-----STOP!!!\n");
	}else if(boot_data->service_ops == SKW_BT_START){
		skw_usb_info("----BT-SERVICE-----START!!!\n");
		ret=skw_BT_service_start();
	}else if(boot_data->service_ops==SKW_BT_STOP &&
			(service_state_map & (1<<BT_SERVICE))){
		skw_usb_info("----BT-SERVICE-----STOP!!!\n");
		return 0;
//		ret=skw_BT_service_stop();
	}
	if(ret < 0)
		return -1;
	else
		return 0;
}
EXPORT_SYMBOL_GPL(skw_boot_loader);
void *skw_get_bus_dev(void)
{
	int time_count=0;
	if(modem_status == MODEM_OFF && !usb_ports[0]) {
		skwusb_err("%s power on USB\n", __func__);
		do{
			msleep(10);
			time_count++;
		}while(!usb_ports[0] && time_count < 50);
	}
	if(!usb_ports[0] || !usb_ports[0]->state || !usb_ports[0]->udev){
		skwusb_err("%s the port open device fail !!!\n", __func__);
		return NULL;
	}
	return &usb_ports[0]->udev->dev;
}
EXPORT_SYMBOL_GPL(skw_get_bus_dev);

/************************************************************************
 *Decription:check dev ready for boot
 *Author:junwei.jiang
 *Date:2022-06-07
 *Modfiy:
 *
 ********************************************************************* */
int skw_reset_bus_dev(void)
{
	struct usb_port_struct *port;
	int ret = -1;

	port = usb_ports[0];
	if(!port)
		return ret;
	ret = usb_reset_device(port->udev);
	skw_usb_info("ret = %d\n", ret);
	modem_status = MODEM_HALT;
	return ret;
}
EXPORT_SYMBOL_GPL(skw_reset_bus_dev);

/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static void skw_usb_io_disconnect(struct usb_interface *interface)
{
	int infno = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_port_struct *port;
	unsigned long flags;
	struct urb *urb;

	skw_usb_info("interface[%x] disconnected %d\n", infno, modem_status);
	port = usb_get_intfdata(interface);
	if(!port)
		return;
	log_port = NULL;
	port->state = 0;
	if(!port->is_dloader) {
		if (infno > 1)
			platform_device_unregister(port->pdev);
		if (modem_status==MODEM_ON) {
			if(port->pdev == wifi_data_pdev) {
				if(wifi_data_pdev) {
					platform_device_unregister(port->pdev);
					wifi_data_pdev = NULL;
				}
			}
		}
		if(port->thread && down_timeout(&port->sem, 3000))
			skw_usb_info("start  to unregister interface[%x]\n", infno);
	} else
		flush_work(&port->work);
	if(port->read_urb && !port->read_urb->context) {
		kfree(port->read_urb);
		port->read_urb = NULL;
	} else skw_usb_info("%s memory leak port.r%d!!!!!!!!\n", __func__, infno);
	if(port->write_urb && !port->write_urb->context) {
		kfree(port->write_urb);
		port->write_urb = NULL;
	} else skw_usb_info("%s memory leak port.w%d!!!!!!!!\n", __func__, infno);
	if(port->read_buffer)
		kfree(port->read_buffer);
	if(port->write_buffer)
		kfree(port->write_buffer);
	spin_lock_irqsave(&port->rx_urb_lock, flags);
	while(!list_empty(&port->rx_done_urb_list)) {
		urb = list_first_entry(&port->rx_done_urb_list, struct urb, urb_list);
		list_del_init(&urb->urb_list);
		if(urb->transfer_buffer)
			kfree(urb->transfer_buffer);
		usb_free_urb(urb);
	}
	spin_unlock_irqrestore(&port->rx_urb_lock, flags);

	usb_ports[infno] = NULL;
	/* this lock must give me! */
	usb_set_intfdata(interface, NULL);
	usb_put_dev(port->udev);
	usb_put_intf(interface);
	kfree(port);
}

/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static int skw_usb_io_pre_reset(struct usb_interface *interface)
{
	/* there is a lock to prevent we reset a interface when
	 * urb submit
	 */
	struct usb_port_struct *port;

	port = usb_get_intfdata(interface);

	return 0;
}

/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static int skw_usb_io_post_reset(struct usb_interface *interface)
{
	struct usb_port_struct *port;

	port = usb_get_intfdata(interface);
	return 0;
}
#ifdef CONFIG_PM
static int skw_usb_io_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct usb_port_struct *port;

	port = usb_get_intfdata(interface);

	if( port->write_urb->context) {
		skw_usb_info("port%d  message discard\n", port->portno);
	}
	port->suspend = 1;
	skw_usb_info("port%d recv %s MSG\n", port->portno, PMSG_IS_AUTO(message)? "Auto":"None-auto");

	if(port->portno == 1 || port->read_urb->context)
		usb_kill_anchored_urbs(&port->read_submitted);
	skw_usb_info("done\n");
	return 0;
}
static int skw_usb_io_resume(struct usb_interface *interface)
{
	struct usb_port_struct *port;
	struct urb *urb;

	port = usb_get_intfdata(interface);

	skw_usb_info("port%d enter...\n", port->portno);
	port->suspend = 0;
	while(!list_empty(&port->suspend_urb_list)) {
		urb = list_first_entry(&port->suspend_urb_list, struct urb, urb_list);
		list_del_init(&urb->urb_list);
		if(urb) {
			//if (port->portno == wifi_pdata.data_port)
			//urb->complete = bulkin_async_complete;
			urb->context = port;
			usb_anchor_urb(urb, &port->read_submitted);
			usb_submit_urb(urb, GFP_KERNEL);
		}
	}
	return 0;
}
#endif
/************************************************************************
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
struct usb_driver skw_usb_io_driver = {
	.name = "skw_usb_io",
	.probe = skw_usb_io_probe,
	.disconnect = skw_usb_io_disconnect,
#ifdef CONFIG_PM
        .suspend   = skw_usb_io_suspend,
        .resume    = skw_usb_io_resume,
#endif	
	.pre_reset = skw_usb_io_pre_reset,
	.post_reset = skw_usb_io_post_reset,
	.id_table = skw_usb_io_id_table,
	.supports_autosuspend = 1,
};

/**
 * wcn_usb_io_init() - init wcn_usb_io's memory and register this driver.
 * @void: void.
 */
static int __init skw_usb_io_init(void)
{
	wifi_data_pdev = NULL;
	bluetooth_pdev = NULL;
	log_port = NULL;
	usb_boot_data = NULL;
	wifi_port_share = 0;
	memset(usb_ports, 0, sizeof(usb_ports));
	init_completion(&download_done);
	skw_usb_wakeup_source_init();
	skw_usb_debugfs_init();
	skw_usb_log_level_init();
	chip_en_gpio = -1;
	modem_status = MODEM_OFF;
	skw_chipid = wifi_pdata.chipid;
	mutex_init(&g_recovery_data.except_mutex);
	INIT_DELAYED_WORK(&skw_except_work, skw_usb_exception_work);
	return usb_register(&skw_usb_io_driver);
}

/************************************************************************
 *Copyright(C) 2020-2021: Seekwave tech LTD 		China
 *Decription:
 *Author:jiayong.yang
 *Date:2021-05-27
 *Modfiy:
 *
 ********************************************************************* */
static void __exit skw_usb_io_exit(void)
{
	pr_info("%s enter...", __func__);
	cancel_delayed_work_sync(&skw_except_work);
	mutex_destroy(&g_recovery_data.except_mutex);
	skw_usb_wakeup_source_destroy();
	usb_deregister(&skw_usb_io_driver);
	if(wifi_data_pdev)
		platform_device_put(wifi_data_pdev);
}
module_init(skw_usb_io_init)
module_exit(skw_usb_io_exit)
MODULE_LICENSE("GPL v2");
