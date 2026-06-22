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
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/semaphore.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/skbuff.h>
#include "skw_sdio.h"
#include "skw_sdio_log.h"
#include <linux/workqueue.h>
#define MODEM_ASSERT_TIMEOUT_VALUE  2*HZ
#define MAX_SG_COUNT	100
#define SDIO_BUFFER_SIZE	(16*1024)
#define MAX_FIRMWARE_SIZE 256
#define PORT_STATE_IDLE	0
#define PORT_STATE_OPEN	1
#define PORT_STATE_CLSE	2
#define PORT_STATE_ASST	3

#define CRC_16_L_SEED   0x80
#define CRC_16_L_POLYNOMIAL  0x8000
#define CRC_16_POLYNOMIAL  0x1021

int recovery_debug_status;
int cp_detect_sleep_mode;
#define IS_LOG_PORT(portno)  ((skw_cp_ver == SKW_SDIO_V10)?(portno==1):(portno==SDIO2_BSP_LOG_PORT))
struct sdio_port {
	struct platform_device *pdev;
	struct scatterlist *sg_rx;
	int	 sg_index;
	int	 total;
	int	sent_packet;
	unsigned int type;
	unsigned int channel;
	rx_submit_fn rx_submit;
	void *rx_data;
	int	state;
	char *read_buffer;
	int rx_rp;
	int rx_wp;
	char *write_buffer;
	int  length;
	struct completion rx_done;
	struct completion tx_done;
	struct mutex rx_mutex;
	int	rx_packet;
	int	rx_count;
	int 	tx_flow_ctrl;
	int	 rx_flow_ctrl;
	u16	next_seqno;
};

/***********************************************************/
char firmware_version[128];
char assert_context[1024];
int  assert_context_size=0;
static int assert_info_print;
static u32 last_sent_wifi_cmd[3];
static u64 last_sent_time;
static u64 port_dmamask = DMA_BIT_MASK(32);
struct sdio_port sdio_ports[SDIO2_MAX_CH_NUM];
static char cp_fifo_status;
static BLOCKING_NOTIFIER_HEAD(modem_notifier_list);
#if KERNEL_VERSION(4,4,0) <= LINUX_VERSION_CODE
static DEFINE_PER_CPU(struct page_frag_cache, skw_sdio_alloc_cache);
#endif
unsigned int crc_16_l_calc(char *buf_ptr,unsigned int len);
static int skw_sdio_rx_port_follow_ctl(int portno, int rx_fctl);
//add the crc api the same as cp crc_16 api
extern void kernel_restart(char *cmd);

char skw_cp_ver = SKW_SDIO_V10;
int max_ch_num = MAX_CH_NUM;

/********************************************************
 * skw_sdio_update img crc checksum
 * For update the CP IMG
 *Author: JUNWEI JIANG
 *Date:2022-08-11
 * *****************************************************/

void skw_get_port_statistic(char *buffer, int size)
{
		int ret = 0;
		int i;

		if(!buffer)
			return;

		for(i=0; i<SDIO2_MAX_CH_NUM; i++)
		{
			if(ret >= size)
				break;

			if(sdio_ports[i].state)
				ret += sprintf(&buffer[ret], "port%d: rx %d %d, tx %d %d\n",
						i, sdio_ports[i].rx_count, sdio_ports[i].rx_packet,
						sdio_ports[i].total, sdio_ports[i].sent_packet);

		}
}

unsigned int crc_16_l_calc(char *buf_ptr,unsigned int len)
{
	unsigned int i;
	unsigned short crc=0;

	while(len--!=0)
	{
		for(i= CRC_16_L_SEED;i!=0;i=i>>1)
		{
			if((crc &CRC_16_L_POLYNOMIAL)!=0)
			{
				crc= crc<<1;
				crc= crc ^ CRC_16_POLYNOMIAL;
			}else{
				crc = crc <<1;
			}

			if((*buf_ptr &i)!=0)
			{
				crc = crc ^ CRC_16_POLYNOMIAL;
			}
		}
		buf_ptr++;
	}
	return (crc);
}

static int skw_sdio_rx_port_follow_ctl(int portno, int rx_fctl)
{
	char ftl_val = 0;
	int ret = 0;

	skw_sdio_info(" portno:%d, rx_fctl:%d \n", portno, rx_fctl);

	if((portno < 0) || (portno > max_ch_num))
		return -1;

	if(portno < 8){
		ret = skw_sdio_readb(SKW_SDIO_RX_CHANNEL_FTL0, &ftl_val);
		if(ret)
			return -1;

		if(rx_fctl)
			ftl_val = ftl_val | (1 << portno);
		else
			ftl_val = ftl_val & (~(1 << portno));
		ret = skw_sdio_writeb(SKW_SDIO_RX_CHANNEL_FTL0, ftl_val);
	}
	else{
		portno = portno - 8;
		ret = skw_sdio_readb(SKW_SDIO_RX_CHANNEL_FTL1, &ftl_val);
		if(ret)
			return -1;

		if(rx_fctl)
			ftl_val = ftl_val | (1 << portno);
		else
			ftl_val = ftl_val & (~(1 << portno));
		ret = skw_sdio_writeb(SKW_SDIO_RX_CHANNEL_FTL1, ftl_val);
	}
	return ret;
}

void modem_register_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_register(&modem_notifier_list, nb);
}
void modem_unregister_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&modem_notifier_list, nb);
}
void modem_notify_event(int event)
{
	blocking_notifier_call_chain(&modem_notifier_list, event, NULL);
}

void skw_sdio_exception_work(struct work_struct *work)
{
	int i=0;
	int port_num=0;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	skw_sdio_info(" entern...\n");
	mutex_lock(&skw_sdio->except_mutex);
	if(skw_sdio->cp_state!=1)
	{
		mutex_unlock(&skw_sdio->except_mutex);
		return;
	}
	skw_sdio->cp_state = DEVICE_BLOCKED_EVENT;
	mutex_unlock(&skw_sdio->except_mutex);
	if(!skw_sdio->host_state)
		modem_notify_event(DEVICE_BLOCKED_EVENT);

	if(skw_cp_ver == SKW_SDIO_V20)
		port_num =11;
	else
		port_num = 5;

	for (i=0; i<port_num; i++)
	{
		if(!sdio_ports[i].state || sdio_ports[i].state==PORT_STATE_CLSE)
			continue;

		sdio_ports[i].state = PORT_STATE_ASST;
		complete(&(sdio_ports[i].rx_done));

		if(i!=1)
			complete(&(sdio_ports[i].tx_done));
		if(i==1)
			sdio_ports[i].next_seqno= 1;

	}
	skw_sdio->service_state_map=0;
	skw_recovery_mode();
}

#if KERNEL_VERSION(4,14,0) <= LINUX_VERSION_CODE
static void *skw_sdio_alloc_frag(unsigned int fragsz, gfp_t gfp_mask)
{
	struct page_frag_cache *nc;
	unsigned long flags;
	void *data;

	local_irq_save(flags);
	nc = this_cpu_ptr(&skw_sdio_alloc_cache);
	data = page_frag_alloc(nc, fragsz, gfp_mask);
	local_irq_restore(flags);
	return data;
}
#elif KERNEL_VERSION(4,4,0) > LINUX_VERSION_CODE
static void *skw_sdio_alloc_frag(unsigned int fragsz, gfp_t gfp_mask)
{
	void *data;
	data = netdev_alloc_frag(fragsz);
	return data;
}
static void page_frag_free(void *data)
{
 	put_page(virt_to_head_page(data));
}

#else
static void *skw_sdio_alloc_frag(unsigned int fragsz, gfp_t gfp_mask)
{
	struct page_frag_cache *nc;
	unsigned long flags;
	void *data;
	
	local_irq_save(flags);
	nc = this_cpu_ptr(&skw_sdio_alloc_cache);
	data = __alloc_page_frag(nc, fragsz, gfp_mask);
	local_irq_restore(flags);
	return data;
}
#if KERNEL_VERSION(4,4,94) <= LINUX_VERSION_CODE
	#define page_frag_free __free_page_frag
#endif
#endif

static void skw_sdio_rx_down(struct skw_sdio_data_t * skw_sdio)
{
	wait_for_completion_interruptible(&skw_sdio->rx_completed);
}
void skw_sdio_rx_up(struct skw_sdio_data_t * skw_sdio)
{
	skw_reinit_completion(skw_sdio->rx_completed);
	complete(&skw_sdio->rx_completed);
}
void skw_sdio_dispatch_packets(struct skw_sdio_data_t * skw_sdio)
{
	int i;
	struct sdio_port *port;

	for(i=0; i<max_ch_num; i++) {
		port = &sdio_ports[i];
		if(!port->state)
			continue;
		if(port->rx_rp!=port->rx_wp)
			skw_sdio_dbg("port[%d] sg_index=%d (%d,%d)\n", i,
				port->sg_index, port->rx_rp, port->rx_wp);
		if(port->rx_submit && port->sg_index) {
			port->rx_submit(port->channel, port->sg_rx, port->sg_index, port->rx_data);
			skw_sdio_dbg("port[%d] sg_index=%d (%d,%d)\n", i,
				port->sg_index, port->rx_rp, port->rx_wp);
			port->sg_index = 0;
		}
	}
}
static void skw_sdio_sdma_set_nsize(unsigned int size)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int count;

	if (size == 0) {
		skw_sdio->next_size = MAX_PAC_SIZE;
		return;
	}

	count = (size >> 10) + 9;
	size = SKW_SDIO_ALIGN_BLK(size + (count<<3));
	skw_sdio->next_size = (size>SDIO_BUFFER_SIZE) ? SDIO_BUFFER_SIZE:size;
}

static void skw_sdio_adma_set_packet_num(unsigned int num)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	if (num == 0) 
		num = 1;

	if (num >= MAX_SG_COUNT)
		skw_sdio->remain_packet = MAX_SG_COUNT;
	else 
		skw_sdio->remain_packet = num;
}

/************************************************************************
 *Decription:release debug recovery auto test
 *Author:junwei.jiang
 *Date:2023-05-30
 *Modfiy:
 *
 ********************************************************************* */
int skw_sdio_recovery_debug(int disable)
{
	recovery_debug_status = disable;
	skw_sdio_info("the recovery status =%d\n", recovery_debug_status);
	return 0;
}

int skw_sdio_recovery_debug_status(void)
{
	skw_sdio_info("the recovery val =%d\n", recovery_debug_status);
	return recovery_debug_status;
}

static int skw_sdio_handle_packet(struct skw_sdio_data_t *skw_sdio,
		struct scatterlist *sg, struct skw_packet_header *header, int portno)
{
	struct sdio_port  *port;
	int buf_size, i;
	char *addr;
	u32 *data;
	if (portno >= max_ch_num)
		return -EINVAL;
	port = &sdio_ports[portno];
	port->rx_packet++;
	port->rx_count += header->len;
	if(portno == LOOPCHECK_PORT) {
		char *cmd = (char *)(header+4);
		cmd[header->len - 12] = 0;
		skw_sdio_info("LOOPCHECK channel received: %s\n", (char *)cmd);
		if (header->len==19 && !strncmp(cmd, "BTREADY", 7)) {
			skw_sdio->service_state_map |= 2;
			//kernel_restart(0);
			skw_sdio->device_active = 1;
			complete(&skw_sdio->download_done);
		}else if(header->len==18 && !strncmp(cmd, "BTEXIT", 6)){
			complete(&skw_sdio->download_done);
		}else if(header->len==21 && !strncmp(cmd, "WIFIREADY", 9)){
			skw_sdio->service_state_map |= 1;
			//kernel_restart(0);
			skw_sdio->device_active = 1;
			complete(&skw_sdio->download_done);
		} else if (header->len==21 && !strncmp((char *)cmd, "BSPASSERT", 9)){
			if(!skw_sdio->cp_state)
				schedule_delayed_work(&skw_sdio->skw_except_work , msecs_to_jiffies(8000));

			mutex_lock(&skw_sdio->except_mutex);
			if(skw_sdio->cp_state==DEVICE_BLOCKED_EVENT){
				if(skw_sdio->adma_rx_enable)
					page_frag_free(header);

				mutex_unlock(&skw_sdio->except_mutex);
				return 0;
			}
			skw_sdio->cp_state=1;/*cp except set value*/
			mutex_unlock(&skw_sdio->except_mutex);
			skw_sdio->service_state_map = 0;
			memset(assert_context, 0, 1024);
			assert_context_size = 0;
			modem_notify_event(DEVICE_ASSERT_EVENT);
			skw_sdio_err(" bsp assert !!!\n");
		}else if (header->len==20 && !strncmp(cmd, "DUMPDONE",8)){
			mutex_lock(&skw_sdio->except_mutex);
			if(skw_sdio->cp_state==DEVICE_BLOCKED_EVENT){
				if(skw_sdio->adma_rx_enable)
					page_frag_free(header);

				mutex_unlock(&skw_sdio->except_mutex);
				return 0;
			}
			skw_sdio->cp_state=DEVICE_DUMPDONE_EVENT;/*cp except set value 2*/
			mutex_unlock(&skw_sdio->except_mutex);
			cancel_delayed_work_sync(&skw_sdio->skw_except_work);
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
			modem_notify_event(DEVICE_DUMPDONE_EVENT);
#else
			if(!strncmp((char *)skw_sdio->chip_id,"SV6160",6) && !recovery_debug_status){
				modem_notify_event(DEVICE_DUMPDONE_EVENT);
			}
#endif
			skw_sdio_err("The CP DUMPDONE OK : \n %d::%s\n",assert_context_size, assert_context);
			for (i=0; i<5; i++) {
				if(!sdio_ports[i].state || sdio_ports[i].state==PORT_STATE_CLSE)
					continue;

				sdio_ports[i].state = PORT_STATE_ASST;
				complete(&(sdio_ports[i].rx_done));
				if(i!=1)
					complete(&(sdio_ports[i].tx_done));
				if(i==1)
					sdio_ports[i].next_seqno= 1;
			}
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
			skw_recovery_mode();//recoverymode open api
#else
			if(!strncmp((char *)skw_sdio->chip_id,"SV6160",6) &&skw_sdio->cp_state !=DEVICE_BLOCKED_EVENT){
				skw_recovery_mode();//recoverymode open api
			}
#endif
		}else if (!strncmp("trunk_W", cmd, 7)) {
			memset(firmware_version, 0 , sizeof(firmware_version));
			strncpy(firmware_version, cmd, strlen(cmd));
			cmd = strstr(firmware_version, "slp=");
			if (cmd)
				cp_detect_sleep_mode = cmd[4] - 0x30;
			else
				cp_detect_sleep_mode = 4;
			if(!skw_sdio->cp_state)
				complete(&skw_sdio->download_done);
			if(skw_sdio->cp_state){
				assert_info_print = 0;
				if(sdio_ports[0].state == PORT_STATE_ASST)
					sdio_ports[0].state = PORT_STATE_OPEN;
				modem_notify_event(DEVICE_BSPREADY_EVENT);
			}
			skw_sdio->host_state =0;
			skw_sdio->cp_state = 0;
			wake_up(&skw_sdio->wq);
		} else if (!strncmp(cmd, "BSPREADY",8)) {
			loopcheck_send_data("RDVERSION", 9);
		}
		skw_sdio_dbg("Line:%d the port=%d \n", __LINE__, port->channel);
		if(skw_sdio->adma_rx_enable)
			page_frag_free(header);
		return 0;
	}
	if(!port->state) {
		if(skw_sdio->adma_rx_enable){
			if (!IS_LOG_PORT(portno))
				skw_sdio_info("port%d discard data for wrong state\n", portno);
			page_frag_free(header);
			return 0;
		}
	}
	if (port->sg_rx && port->rx_data){
		if (port->sg_index >= MAX_SG_COUNT){
			skw_sdio_err(" rx sg_buffer is overflow!\n");
		}else{
			sg_set_buf(&port->sg_rx[port->sg_index++], header, header->len+4);
		}
	}else {
		int packet=0, total=0;
		mutex_lock(&port->rx_mutex);
		buf_size = (port->length + port->rx_wp - port->rx_rp)%port->length;
		buf_size = port->length - 1 - buf_size;
		addr = (char *)(header+1);
		data = (u32 *) addr;
		if(((data[2] & 0xffff) != port->next_seqno) &&
			(header->len > 12) && !IS_LOG_PORT(portno)) {
			skw_sdio_err("portno:%d, packet lost recv seqno=%d expected %d\n", port->channel,
					data[2] & 0xffff, port->next_seqno);
			if(skw_sdio->adma_rx_enable)
				page_frag_free(header);
			mutex_unlock(&port->rx_mutex);
			return 0;
		}
		if(header->len > 12) {
			port->next_seqno++;
			addr += 12;
			header->len -= 12;
			total = data[1] >> 8;
			packet = data[2] & 0xFFFF;
		} else if (header->len == 12) {
			header->len = 0;
			port->tx_flow_ctrl--;
			complete(&port->tx_done);
			skw_port_log(portno,"%s link msg: 0x%x 0x%x port%d: %d \n", __func__,
					data[0], data[1], portno, port->tx_flow_ctrl);
		}
		if(skw_sdio->cp_state){
			if(header->len!=245 || buf_size < 2048) {
				if(assert_info_print++ < 28 && strncmp((const char *)addr, "+LOG", 4)) {
					if (assert_context_size + header->len < sizeof(assert_context)) {
						memcpy(assert_context + assert_context_size, addr, header->len);
						assert_context_size += header->len;
					}
				}
			}
			if(buf_size <2048)
				msleep(10);
		}
		if (port->rx_submit && !port->sg_rx) {
			if (header->len && port->pdev)
				port->rx_submit(portno, port->rx_data, header->len, addr);
		} else if (buf_size < header->len) {
			skw_port_log(portno,"%s port%d overflow:buf_size %d-%d, packet size %d (w,r)=(%d, %d)\n",
					__func__, portno, buf_size, port->length, header->len,
					port->rx_wp,  port->rx_rp);
		} else if(port->state && header->len) {
			if(port->length - port->rx_wp > header->len){
				memcpy(&port->read_buffer[port->rx_wp], addr, header->len);
				port->rx_wp += header->len;
			} else {
				memcpy(&port->read_buffer[port->rx_wp], addr, port->length - port->rx_wp);
				memcpy(&port->read_buffer[0], &addr[port->length - port->rx_wp],
						header->len - port->length + port->rx_wp);
				port->rx_wp = header->len - port->length + port->rx_wp;
			}

			if(!port->rx_flow_ctrl && buf_size-header->len < (port->length/3)) {
				port->rx_flow_ctrl = 1;
				skw_sdio_rx_port_follow_ctl(portno, port->rx_flow_ctrl);
			}
			mutex_unlock(&port->rx_mutex);
			complete(&port->rx_done);
			if(skw_sdio->adma_rx_enable)
				page_frag_free(header);
			return 0;
		}
		mutex_unlock(&port->rx_mutex);
		if(skw_sdio->adma_rx_enable)
			page_frag_free(header);
	}
	return 0;
}

static int skw_sdio2_handle_packet(struct skw_sdio_data_t *skw_sdio,
		struct scatterlist *sg, struct skw_packet2_header *header, int portno)
{
	struct sdio_port  *port;
	int buf_size, i;
	char *addr;
	u32 *data;

	if (portno >= max_ch_num)
		return -EINVAL;
	port = &sdio_ports[portno];
	port->rx_packet++;
	port->rx_count += header->len;
	if(portno == SDIO2_LOOPCHECK_PORT) {
		char *cmd = (char *)(header+4);
		cmd[header->len - 12] = 0;
		skw_sdio_info("LOOPCHECK channel received: %s\n", (char *)cmd);
		if (header->len==19 && !strncmp(cmd, "BTREADY", 7)) {
			skw_sdio->service_state_map |= 2;
			//kernel_restart(0);
			skw_sdio->device_active = 1;
			complete(&skw_sdio->download_done);
		}else if(header->len==21 && !strncmp(cmd, "WIFIREADY", 9)){
			skw_sdio->service_state_map |= 1;
			//kernel_restart(0);
			skw_sdio->device_active = 1;
			complete(&skw_sdio->download_done);
		} else if (header->len==21 && !strncmp((char *)cmd, "BSPASSERT", 9)) {
			if(!skw_sdio->cp_state)
				schedule_delayed_work(&skw_sdio->skw_except_work , msecs_to_jiffies(6000));

			mutex_lock(&skw_sdio->except_mutex);
			if(skw_sdio->cp_state==DEVICE_BLOCKED_EVENT){
				if(skw_sdio->adma_rx_enable)
					page_frag_free(header);

				mutex_unlock(&skw_sdio->except_mutex);
				return 0;
			}
			skw_sdio->cp_state=1;/*cp except set value*/
			mutex_unlock(&skw_sdio->except_mutex);
			skw_sdio->service_state_map = 0;
			memset(assert_context, 0, 1024);
			assert_context_size = 0;
			modem_notify_event(DEVICE_ASSERT_EVENT);
			skw_sdio_err(" bsp assert !!!\n");
		}else if (header->len==20 && !strncmp(cmd, "DUMPDONE",8)){
			mutex_lock(&skw_sdio->except_mutex);
			if(skw_sdio->cp_state==DEVICE_BLOCKED_EVENT){
				if(skw_sdio->adma_rx_enable)
					page_frag_free(header);

				mutex_unlock(&skw_sdio->except_mutex);
				return 0;
			}
			skw_sdio->cp_state=DEVICE_DUMPDONE_EVENT;/*cp except set value 2*/
			mutex_unlock(&skw_sdio->except_mutex);
			cancel_delayed_work_sync(&skw_sdio->skw_except_work);
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
			modem_notify_event(DEVICE_DUMPDONE_EVENT);
#endif
			skw_sdio_err("The CP DUMPDONE OK : \n %d::%s\n",assert_context_size, assert_context);
			for (i=0; i<5; i++) {
				if(!sdio_ports[i].state || sdio_ports[i].state==PORT_STATE_CLSE)
					continue;

				sdio_ports[i].state = PORT_STATE_ASST;
				complete(&(sdio_ports[i].rx_done));
				if(i!=1)
					complete(&(sdio_ports[i].tx_done));
				if(i==1)
					sdio_ports[i].next_seqno= 1;
			}
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
			skw_recovery_mode();//recoverymode open api
#endif

		}else if (!strncmp("trunk_W", cmd, 7)) {
			if(skw_sdio->cp_state){
				if(sdio_ports[0].state == PORT_STATE_ASST)
					sdio_ports[0].state = PORT_STATE_OPEN;
				modem_notify_event(DEVICE_BSPREADY_EVENT);
			}

			skw_sdio->cp_state = 0;
			assert_info_print = 0;
			memset(firmware_version, 0 , sizeof(firmware_version));
			strncpy(firmware_version, cmd, strlen(cmd));
			skw_sdio_info("firmware version: %s:%s \n",cmd, firmware_version);
		} else if (!strncmp(cmd, "BSPREADY",8)) {
			loopcheck_send_data("RDVERSION", 9);
		}
		skw_sdio_info("Line:%d the port=%d \n", __LINE__, port->channel);
		if(skw_sdio->adma_rx_enable)
			page_frag_free(header);
		return 0;
	}
	if(!port->state) {
		if(skw_sdio->adma_rx_enable){
			if (!IS_LOG_PORT(portno))
				skw_sdio_info("port%d discard data for wrong state\n", portno);
			page_frag_free(header);
			return 0;
		}
	}
	if (port->sg_rx){
		if (port->sg_index >= MAX_SG_COUNT){
			skw_sdio_err(" rx sg_buffer is overflow!\n");
		}else{
			sg_set_buf(&port->sg_rx[port->sg_index++], header, header->len+4);
		}
	}else {
		int packet=0, total=0;
		mutex_lock(&port->rx_mutex);
		buf_size = (port->length + port->rx_wp - port->rx_rp)%port->length;
		buf_size = port->length - 1 - buf_size;
		addr = (char *)(header+1);
		data = (u32 *) addr;
		if(((data[2] & 0xffff) != port->next_seqno) && header->len > 12) {
			skw_sdio_err("portno:%d, packet lost recv seqno=%d expected %d\n", port->channel,
					data[2] & 0xffff, port->next_seqno);
			if(skw_sdio->adma_rx_enable)
				page_frag_free(header);
			mutex_unlock(&port->rx_mutex);
			return 0;
		}
		if(header->len > 12) {
			port->next_seqno++;
			addr += 12;
			header->len -= 12;
			total = data[1] >> 8;
			packet = data[2] & 0xFFFF;
		} else if (header->len == 12) {
			header->len = 0;
			port->tx_flow_ctrl--;
			skw_port_log(portno,"%s link msg: 0x%x 0x%x 0x%x: %d\n", __func__,
					data[0], data[1], data[2], port->tx_flow_ctrl);
			complete(&port->tx_done);
		}
		if(skw_sdio->cp_state){
			if(header->len!=245 || buf_size < 2048)
			skw_sdio_info("%s(%d.%d) (%d,%d) len=%d : 0x%x\n",__func__,
					   portno, port->next_seqno, port->rx_wp,  port->rx_rp, header->len, data[3]);
			if(buf_size <2048)
				msleep(10);
		}
		if (buf_size < header->len) {
			skw_port_log(portno,"%s port%d overflow:buf_size %d-%d, packet size %d (w,r)=(%d, %d)\n",
					__func__, portno, buf_size, port->length, header->len,
					port->rx_wp,  port->rx_rp);
		} else if(port->state && header->len) {
			if(port->length - port->rx_wp > header->len){
				memcpy(&port->read_buffer[port->rx_wp], addr, header->len);
				port->rx_wp += header->len;
			} else {
				memcpy(&port->read_buffer[port->rx_wp], addr, port->length - port->rx_wp);
				memcpy(&port->read_buffer[0], &addr[port->length - port->rx_wp],
						header->len - port->length + port->rx_wp);
				port->rx_wp = header->len - port->length + port->rx_wp;
			}

			if(!port->rx_flow_ctrl && buf_size-header->len < (port->length/3)) {
				port->rx_flow_ctrl = 1;
				skw_sdio_rx_port_follow_ctl(portno, port->rx_flow_ctrl);
			}
			mutex_unlock(&port->rx_mutex);
			complete(&port->rx_done);
			if(skw_sdio->adma_rx_enable)
				page_frag_free(header);
			return 0;
		}
		mutex_unlock(&port->rx_mutex);
		if(skw_sdio->adma_rx_enable)
			page_frag_free(header);
	}
	return 0;
}
int send_modem_assert_command(void)
{
	int ret =0;
	u32 *cmd = last_sent_wifi_cmd;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	if(skw_sdio->cp_state)
		return ret;

	skw_sdio->cp_state=1;/*cp except set value*/
	ret =skw_sdio_writeb(SKW_AP2CP_IRQ_REG, 0x10);
	if(ret !=0){
		skw_sdio->host_state =1;
		skw_sdio_err("the sdio host controller exception err= %d !!\n",ret);
	}
	skw_sdio_err("%s ret=%d cmd: 0x%x 0x%x 0x%x :%d-%d\n", __func__,
			 ret, cmd[0], cmd[1], cmd[2], (u32)last_sent_time, (u32)jiffies);
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
	schedule_delayed_work(&skw_sdio->skw_except_work , msecs_to_jiffies(2000));
#else
	schedule_delayed_work(&skw_sdio->skw_except_work , msecs_to_jiffies(8000));
#endif
	return ret;
}

/* for adma */
static int skw_sdio_adma_parser(struct skw_sdio_data_t *skw_sdio, struct scatterlist *sgs,
				int packet_count)
{
	struct skw_packet_header *header = NULL;
	unsigned int i;
	int channel = 0;
	unsigned int parse_len = 0;
	uint32_t *data;
	struct sdio_port *port;

	port = &sdio_ports[0];
	for (i = 0; i < packet_count; i++) {
		header = (struct skw_packet_header *)sg_virt(sgs + i);
		data = (uint32_t *)header;
		if (atomic_read(&skw_sdio->suspending))
			skw_sdio_info("ch:%d len:%d 0x%x 0x%x\n", header->channel, header->len, data[2], data[3]);
		skw_port_log(header->channel, "%s[%d]:ch:%d len:0x%0x 0x%08X 0x%08X : 0x%08X 0x%08x 0x%08X\n", __func__,
			i,	header->channel, header->len, data[2], data[3], data[5], data[6], data[7]);
		channel = header->channel;

		if (!header->eof && (channel < max_ch_num) && header->len) {
			parse_len += header->len;
			data = (uint32_t *)(header+1);
			if ((channel >= max_ch_num) || (header->len >
				(MAX_PAC_SIZE - sizeof(struct skw_packet_header))) ||
				(header->len == 0)) {
				skw_sdio_err("%s invalid header[%d]len[%d]: 0x%x 0x%x\n",
						__func__,  header->channel, header->len, data[0], data[1]);
				page_frag_free(header);
				continue;
			}
			skw_sdio->rx_packer_cnt++;
			skw_sdio_handle_packet(skw_sdio, sgs+i, header, channel);
		} else {
#if 0
			skw_sdio_err("%s[%d]:ch:%d len:0x%0x 0x%08X 0x%08X : 0x%08X 0x%08x 0x%08X\n", __func__,
			i,	header->channel, header->len, data[2], data[3], data[5], data[6], data[7]);
			print_hex_dump(KERN_ERR, "PACKET ERR:", 0, 16, 1,
					header, 1792, 1);
			skw_sdio_err("%s PUB HAEAD ERROR: packet[%d/%d] channel=%d,size=%d eof=%d!!!",
					__func__, i, packet_count, channel, header->len, header->eof);
#endif
			page_frag_free(header);
			continue;
		}
	}
	atomic_set(&skw_sdio->suspending, 0);
	return 0;
}

static int skw_sdio2_adma_parser(struct skw_sdio_data_t *skw_sdio, struct scatterlist *sgs,
				int packet_count)
{
	struct skw_packet2_header *header = NULL;
	unsigned int i;
	int channel = 0;
	unsigned int parse_len = 0;
	uint32_t *data;
	struct sdio_port *port;

	port = &sdio_ports[0];
	for (i = 0; i < packet_count; i++) {
		header = (struct skw_packet2_header *)sg_virt(sgs + i);
		data = (uint32_t *)header;
		if (atomic_read(&skw_sdio->suspending))
			skw_sdio_info("ch:%d len:%d 0x%x 0x%x\n", header->channel, header->len, data[2], data[3]);
		skw_port_log(header->channel, "%s[%d]:ch:%d len:0x%0x 0x%08X 0x%08X : 0x%08X 0x%08x 0x%08X\n", __func__,
			i,	header->channel, header->len, data[2], data[3], data[5], data[6], data[7]);
		channel = header->channel;

		if (!header->eof && (channel < max_ch_num) && header->len) {
			parse_len += header->len;
			data = (uint32_t *)(header+1);
			if ((channel >= max_ch_num) || (header->len >
				(MAX_PAC_SIZE - sizeof(struct skw_packet2_header))) ||
				(header->len == 0)) {
				skw_sdio_err("%s invalid header[%d]len[%d]: 0x%x 0x%x\n",
						__func__,  header->channel, header->len, data[0], data[1]);
				page_frag_free(header);
				continue;
			}
			skw_sdio->rx_packer_cnt++;
			skw_sdio2_handle_packet(skw_sdio, sgs+i, header, channel);
		} else {
			skw_sdio_err("%s[%d]:ch:%d len:0x%0x 0x%08X 0x%08X : 0x%08X 0x%08x 0x%08X\n", __func__,
			i,	header->channel, header->len, data[2], data[3], data[5], data[6], data[7]);
			page_frag_free(header);
			continue;
		}
	}
	atomic_set(&skw_sdio->suspending, 0);
	return 0;
}
/* for normal dma */
static int skw_sdio_sdma_parser(char *data_buf, int total)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct skw_packet_header *header = NULL;
	int channel;
	uint32_t *data;
	unsigned char *p = NULL;
	unsigned int parse_len = 0;
	int current_len=0;
#if 0
	print_hex_dump(KERN_ERR, "skw_rx_buf:", 0, 16, 1,
			data_buf, total, 1);
#endif
	header = (struct skw_packet_header *)data_buf;
	for (parse_len = 0; parse_len < total;) {
		if (header->eof != 0)
			break;
		p = (unsigned char *)header;
		data = (uint32_t *)header;
		if (atomic_read(&skw_sdio->suspending))
			skw_sdio_info("ch:%d len:%d 0x%x 0x%x\n", header->channel, header->len, data[2], data[3]);
		skw_port_log(header->channel, "%s:ch:%d len:0x%0x 0x%08X 0x%08X : 0x%08X 0x%08x 0x%08X\n", __func__,
				header->channel, header->len, data[1], data[2], data[3], data[4], data[5]);
		channel = header->channel;
		current_len = header->len;
		parse_len += current_len;
		if ((channel >= max_ch_num) || (current_len == 0) ||
			(current_len > (MAX_PAC_SIZE - sizeof(struct skw_packet_header)))) {
			skw_sdio_err("%s skip [%d]len[%d]\n",__func__, header->channel, current_len);
			break;
		}
		skw_sdio->rx_packer_cnt++;
		skw_sdio_handle_packet(skw_sdio, NULL, header, channel);
		skw_port_log(header->channel, "the -header->len----%d\n", current_len);
		/* pointer to next packet header*/
		p += sizeof(struct skw_packet_header) + SKW_SDIO_ALIGN_4BYTE(current_len);
		header = (struct skw_packet_header *)p;
	}
	atomic_set(&skw_sdio->suspending, 0);
	return 0;
}
static int skw_sdio2_sdma_parser(char *data_buf, int total)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct skw_packet2_header *header = NULL;
	int channel;
	uint32_t *data;
	unsigned char *p = NULL;
	unsigned int parse_len = 0;
	int current_len=0;
#if 0
	print_hex_dump(KERN_ERR, "skw_rx_buf:", 0, 16, 1,
			data_buf, total, 1);
#endif
	header = (struct skw_packet2_header *)data_buf;
	for (parse_len = 0; parse_len < total;) {
		if (header->eof != 0)
			break;
		p = (unsigned char *)header;
		data = (uint32_t *)header;
		if (atomic_read(&skw_sdio->suspending))
			skw_sdio_info("ch:%d len:%d 0x%x 0x%x\n", header->channel, header->len, data[2], data[3]);
		skw_port_log(header->channel, "ch:%d len:0x%0x 0x%08X 0x%08X : 0x%08X 0x%08x 0x%08X\n",
				header->channel, header->len, data[1], data[2], data[3], data[4], data[5]);
		channel = header->channel;
		current_len = header->len;
		parse_len += current_len;
		if ((channel >= max_ch_num) || (current_len == 0) ||
			(current_len > (MAX_PAC_SIZE - sizeof(struct skw_packet2_header)))) {
			skw_sdio_err("%s skip [%d]len[%d]\n",__func__, header->channel, current_len);
			break;
		}
		skw_sdio->rx_packer_cnt++;
		skw_sdio2_handle_packet(skw_sdio, NULL, header, channel);
		skw_port_log(header->channel, "the -header->len----%d\n", current_len);
		/* pointer to next packet header*/
		p += sizeof(struct skw_packet2_header) + SKW_SDIO_ALIGN_4BYTE(current_len);
		header = (struct skw_packet2_header *)p;
	}
	atomic_set(&skw_sdio->suspending, 0);
	return 0;
}
struct scatterlist *skw_sdio_prepare_adma_buffer(struct skw_sdio_data_t *skw_sdio, int *sg_count, int *nsize_offset)
{
	struct scatterlist *sgs;
	void   *buffer;
	int	i, j, data_size;
	int	alloc_size = PAGE_SIZE;

	sgs = kzalloc((*sg_count) * sizeof(struct scatterlist), GFP_KERNEL);

	if(sgs == NULL)
		return NULL;

	for(i = 0; i < (*sg_count) - 1; i++) {
		buffer = skw_sdio_alloc_frag(alloc_size, GFP_ATOMIC);
		if(buffer)
			sg_set_buf(&sgs[i], buffer, MAX_PAC_SIZE);
		else{
			*sg_count = i+1;
			break;
		}
	}

	if(i <= 0)
		goto err;

	sg_mark_end(&sgs[*sg_count - 1]);
	data_size = MAX_PAC_SIZE * ((*sg_count)-1);
	data_size = data_size%SKW_SDIO_NSIZE_BUF_SIZE;
	*nsize_offset = SKW_SDIO_NSIZE_BUF_SIZE - data_size;
	if(*nsize_offset < 8)
		*nsize_offset = SKW_SDIO_NSIZE_BUF_SIZE + *nsize_offset;
	*nsize_offset = *nsize_offset + SKW_SDIO_NSIZE_BUF_SIZE;
	sg_set_buf(sgs + i, skw_sdio->next_size_buf, *nsize_offset);
	return sgs;
err:
	skw_sdio_err("%s failed\n", __func__);
	for(j=0; j<i; j++)
		page_frag_free(sg_virt(sgs + j));
	kfree(sgs);
	return NULL;

}

int skw_sdio_rx_thread(void *p)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int read_len, buf_num;
	int ret = 0;
	unsigned int rx_nsize = 0;
	unsigned int valid_len = 0;
	char *rx_buf;
	struct scatterlist *sgs = NULL;
	char fifo_ind;

	skw_sdio_sdma_set_nsize(0);
	skw_sdio_adma_set_packet_num(1);
	cp_fifo_status = 0;
	while (1) {
		/* Wait the semaphore */
		skw_sdio_rx_down(skw_sdio);
		if (skw_sdio->threads_exit)
			break;
		if (!SKW_CARD_ONLINE(skw_sdio)) {
			skw_sdio_unlock_rx_ws(skw_sdio);
			skw_sdio_err("line %d not have card\n",__LINE__);
			continue;
		}
		skw_resume_check();
		if (skw_sdio->irq_type == SKW_SDIO_EXTERNAL_IRQ) {
			int value = gpio_get_value(skw_sdio->gpio_in);

			if(value == 0) {
				ret = skw_sdio_readb(SKW_SDIO_CP2AP_FIFO_IND, &fifo_ind);
				if(ret){
					skw_sdio_unlock_rx_ws(skw_sdio);
					continue;
				}
			}
			ret = skw_sdio_readb(SKW_SDIO_CP2AP_FIFO_IND, &fifo_ind);
			if(ret) {
				skw_sdio_unlock_rx_ws(skw_sdio);
				continue;
			}
			skw_sdio_dbg("line:%d cp fifo status(%d,%d) ret=%d\n",
					__LINE__,fifo_ind, cp_fifo_status, ret);
			if (!ret && !fifo_ind)
				skw_sdio_dbg("cp fifo ret -- %d \n", ret);
			if(fifo_ind == cp_fifo_status) {
				skw_sdio_unlock_rx_ws(skw_sdio);
				continue;
			}
		}
		cp_fifo_status = fifo_ind;
receive_again:
		if (skw_sdio->adma_rx_enable) {
			int	nsize_offset;
			buf_num = skw_sdio->remain_packet;
			if (buf_num > MAX_PAC_COUNT)
				buf_num = MAX_PAC_COUNT;

			buf_num = buf_num + 1;
			sgs = skw_sdio_prepare_adma_buffer(skw_sdio, &buf_num, &nsize_offset);
			buf_num = buf_num -1;
			if (! sgs) {
				skw_sdio_err("get_tx_buf_list fail\n");
				goto submit_packets;
			}
			if (skw_sdio->power_off) {
				rx_nsize = 0;
				ret = -EIO;
			} else {
				ret = skw_sdio_adma_read(skw_sdio, sgs, buf_num + 1,
					buf_num * MAX_PAC_SIZE+nsize_offset);
			}
			if (ret) {
				skw_sdio_err("%s adma read fail ret:%d\n", __func__, ret);
				if (ret == -ETIMEDOUT && !skw_sdio->power_off) {
					try_to_wakeup_modem(8);
					ret = skw_sdio_adma_read(skw_sdio, sgs, buf_num + 1,
							buf_num * MAX_PAC_SIZE+nsize_offset);
				}
				if (ret) {
					rx_nsize = 0;
					kfree(sgs);
					goto submit_packets;
				}
			}
			rx_nsize =  *((uint32_t *)(skw_sdio->next_size_buf + (nsize_offset - 4)));
			valid_len = *((uint32_t *)(skw_sdio->next_size_buf + (nsize_offset - 8)));
			skw_sdio_dbg("line:%d total:%lld next_pac:%d:, valid len:%d cnt %d\n",
					  __LINE__,skw_sdio->rx_packer_cnt, rx_nsize, valid_len, buf_num);

			if(skw_cp_ver == SKW_SDIO_V10){
				skw_sdio_adma_parser(skw_sdio, sgs, buf_num);
			}
			else{
				skw_sdio2_adma_parser(skw_sdio, sgs, buf_num);
			}
			kfree(sgs);
		} else {
			unsigned int alloc_size;

			read_len = skw_sdio->next_size;
			alloc_size = SKW_SDIO_ALIGN_BLK(read_len);
			rx_buf = kzalloc(alloc_size, GFP_KERNEL);
			if (!rx_buf) 
				goto submit_packets;

			ret = skw_sdio_sdma_read(rx_buf, alloc_size);
#if 0
			print_hex_dump(KERN_ERR, "src_sdma_data:", 0, 16, 1,
					rx_buf, alloc_size, 1);
#endif
			if (ret != 0) {
				rx_nsize = 0;
				goto submit_packets;
			}
			rx_nsize = *((uint32_t *)(rx_buf + (alloc_size- 4)));
			valid_len = *((uint32_t *)(rx_buf + (alloc_size - 8)));

			skw_sdio_dbg("%s the sdma rx thread alloc_size:%d,read_len:%d,rx_nsize:%d,valid_len:%d\n",
					__func__,alloc_size, read_len, rx_nsize, valid_len);
			if(skw_cp_ver == SKW_SDIO_V10){
				skw_sdio_sdma_parser(rx_buf, valid_len);
			}
			else{
				skw_sdio2_sdma_parser(rx_buf, valid_len);
			}
			kfree(rx_buf);
		}
submit_packets:
		skw_sdio_dispatch_packets(skw_sdio);
		if (skw_sdio->adma_rx_enable)
			skw_sdio_adma_set_packet_num(rx_nsize);
		else
			skw_sdio_sdma_set_nsize(rx_nsize);
		if (skw_sdio->power_off)
			rx_nsize = 0;
		if (rx_nsize > 0)
			goto receive_again;

		skw_sdio_unlock_rx_ws(skw_sdio);
	}
	skw_sdio_dbg("%s exit\n", __func__);
	return 0;
}

static int open_sdio_port(int id, void *callback, void *data)
{
	struct sdio_port *port;

	if(id >= max_ch_num)
		return -EINVAL;

	port = &sdio_ports[id];
	if((port->state==PORT_STATE_OPEN) || port->rx_submit)
		return -EBUSY;
	port->rx_submit = callback;
	port->rx_data = data;
	port->rx_wp = port->rx_rp = 0;
	init_completion(&port->rx_done);
	init_completion(&port->tx_done);
	mutex_init(&port->rx_mutex);
	port->state = PORT_STATE_OPEN;
	port->tx_flow_ctrl = 0;
	port->rx_flow_ctrl = 0;
	port->next_seqno = 1; //cp start seqno default no 1
	skw_sdio_info("%s(%d) %s portno = %d\n", current->comm, current->pid, __func__, id);
	return 0;
}
static int close_sdio_port(int id)
{
	struct sdio_port *port;

	if(id >= max_ch_num)
		return -EINVAL;
	port = &sdio_ports[id];
	skw_sdio_info("%s(state=%d) portno = %d\n", current->comm, port->state, id);
	if(!port->state)
		return -ENODEV;
	port->next_seqno = 1; //cp start seqno default no 1
	port->state = PORT_STATE_CLSE;
	port->rx_submit = NULL;
	complete(&port->rx_done);
	return 0;
}

void send_host_suspend_indication(struct skw_sdio_data_t *skw_sdio)
{
	uint32_t value;
	uint32_t timeout = 2000;
	if(skw_sdio->gpio_out>=0 && skw_sdio->resume_com) {
		skw_sdio_dbg("%s enter gpio=0\n", __func__);
		skw_sdio->host_active = 0;
		gpio_set_value(skw_sdio->gpio_out, 0);
		skw_sdio->device_active = 0;
		do {
			value = gpio_get_value(skw_sdio->gpio_in);
			if(value == 0)
				break;
			udelay(10);
		}while(timeout--);
	} else
		skw_sdio_dbg("%s enter\n", __func__);
}

void send_host_resume_indication(struct skw_sdio_data_t *skw_sdio)
{
	if(skw_sdio->gpio_out >= 0) {
		skw_sdio_dbg("%s enter\n", __func__);
		skw_sdio->host_active = 1;
		gpio_set_value(skw_sdio->gpio_out, 1);
		skw_sdio->resume_com = 1;
	}
}

static void send_cp_wakeup_signal(struct skw_sdio_data_t *skw_sdio)
{
	gpio_set_value(skw_sdio->gpio_out, 0);
	udelay(5);
	gpio_set_value(skw_sdio->gpio_out, 1);
}

int try_to_wakeup_modem(int portno)
{
	int ret = 0;
	int val;
	unsigned long flags;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	if(skw_sdio->gpio_out < 0)
		return 0;
	skw_sdio->device_active = gpio_get_value(skw_sdio->gpio_in);

	if(skw_sdio->device_active)
		return 0;
	skw_reinit_completion(skw_sdio->device_wakeup);
	skw_sdio->tx_req_map |= 1<<portno;
	skw_port_log(portno,"%s enter device_active=%d : %d\n", __func__, skw_sdio->device_active, skw_sdio->resume_com);
	if(skw_sdio->device_active == 0) {
		local_irq_save(flags);
		if(skw_sdio->resume_com==0)
			gpio_set_value(skw_sdio->gpio_out, 1);
		else
			send_cp_wakeup_signal(skw_sdio);
		local_irq_restore(flags);
		ret = wait_for_completion_interruptible_timeout(&skw_sdio->device_wakeup, HZ/100);
		if (ret < 0) {
			skw_sdio->tx_req_map &= ~(1<<portno);
			return -ETIMEDOUT;
		}
	}
	val = gpio_get_value(skw_sdio->gpio_in);
	if(!val) {
		local_irq_save(flags);
		send_cp_wakeup_signal(skw_sdio);
		local_irq_restore(flags);
		ret = wait_for_completion_interruptible_timeout(&skw_sdio->device_wakeup, HZ/100);
		if (ret < 0) {
			skw_sdio->tx_req_map &= ~(1<<portno);
			return -ETIMEDOUT;
		}
		val = gpio_get_value(skw_sdio->gpio_in);
	}
	if ( val && !skw_sdio->sdio_func[FUNC_1]->irq_handler &&
		!skw_sdio->resume_com && skw_sdio->irq_type == SKW_SDIO_INBAND_IRQ) {
		sdio_claim_host(skw_sdio->sdio_func[FUNC_1]);
		ret=sdio_claim_irq(skw_sdio->sdio_func[FUNC_1],skw_sdio_inband_irq_handler);
		sdio_release_host(skw_sdio->sdio_func[FUNC_1]);
		skw_port_log(portno,"%s enable SDIO inband IRQ ret=%d\n", __func__, ret);
	}
	return ret;
}
void host_gpio_in_routine(int value)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int  device_active = skw_sdio->device_active;
	skw_sdio->device_active = value;
	skw_sdio_dbg("%s enter %d-%d, host tx=0x%x:%d\n", __func__, device_active,
			skw_sdio->device_active, skw_sdio->tx_req_map, skw_sdio->host_active);
	if(device_active  && !skw_sdio->device_active &&
	   skw_sdio->tx_req_map && skw_sdio->host_active) {
		send_cp_wakeup_signal(skw_sdio);
	}
	if(skw_sdio->device_active && atomic_read(&skw_sdio->resume_flag))
		complete(&skw_sdio->device_wakeup);
	if(skw_sdio->device_active) {
		if(skw_sdio->host_active == 0)
			skw_sdio->host_active = 1;
		gpio_set_value(skw_sdio->gpio_out, 1);
		skw_sdio->resume_com = 1;
	}
}

static int setup_sdio_packet(void *packet, u8 channel, char *msg, int size)
{
	struct skw_packet_header *header = NULL;
	u32 *data = packet;

	data[0] = 0;
	header = (struct skw_packet_header *)data;
	header->channel = channel;
	header->len = size;
	memcpy(data+1, msg, size);
	data++;
	data[size>>2] = 0;
	header = (struct skw_packet_header *)&data[size>>2];
	header->eof = 1;
	size += 8;
	return size;
}
static int setup_sdio2_packet(void *packet, u8 channel, char *msg, int size)
{
	struct skw_packet2_header *header = NULL;
	u32 *data = packet;

	data[0] = 0;
	header = (struct skw_packet2_header *)data;
	header->channel = channel;
	header->len = size;
	memcpy(data+1, msg, size);
	data++;
	data[size>>2] = 0;
	header = (struct skw_packet2_header *)&data[size>>2];
	header->eof = 1;
	size += 8;
	return size;
}
int loopcheck_send_data(char *buffer, int size)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_port *port;
	int ret, count;
	port = &sdio_ports[LOOPCHECK_PORT];
	count = (size + 3) & 0xFFFFFFFC;
	if(count + 8 < port->length) {
		if(skw_cp_ver == SKW_SDIO_V10){
			count = setup_sdio_packet(port->write_buffer, port->channel, buffer, count);
		}
		else{
			count = setup_sdio2_packet(port->write_buffer, port->channel, buffer, count);
		}
		try_to_wakeup_modem(LOOPCHECK_PORT);
		if(!(ret = skw_sdio_sdma_write(port->write_buffer, count))) {
			port->total += count;
			port->sent_packet++;
			ret = size;
		}
		skw_sdio->tx_req_map &= ~(1<<LOOPCHECK_PORT);
		return ret;
	}
	return -ENOMEM;
}
static int send_data(int portno, char *buffer, int size)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_port *port;
	int ret, count, i;
	if(size==0)
		return 0;
	if(portno >= max_ch_num)
		return -EINVAL;
	port = &sdio_ports[portno];
	if(!port->state || skw_sdio->cp_state)
		return -EIO;

	if(port->state == PORT_STATE_CLSE) {
		port->state = PORT_STATE_IDLE;
		return -EIO;
	}

	count = (size + 3) & 0xFFFFFFFC;
	if(count + 8 < port->length) {
		if(skw_cp_ver == SKW_SDIO_V10){
			count = setup_sdio_packet(port->write_buffer, port->channel, buffer, count);
		}
		else{
			count = setup_sdio2_packet(port->write_buffer, port->channel, buffer, count);
		}
		skw_reinit_completion(port->tx_done);
		try_to_wakeup_modem(portno);

		if(skw_sdio->cp_state)
			return -EIO;

		if(!(ret = skw_sdio_sdma_write(port->write_buffer, count))) {
			port->tx_flow_ctrl++;
			if(sdio_ports[portno].state != PORT_STATE_ASST) {
				ret = wait_for_completion_interruptible_timeout(&port->tx_done, 
						msecs_to_jiffies(100));
				if(!ret && port->tx_flow_ctrl) {
					try_to_wakeup_modem(portno);
					port->tx_flow_ctrl--;
				}
			}
			port->total += count;
			port->sent_packet++;
			ret = size;
		} else {
			skw_sdio_info("%s ret=%d\n", __func__, ret);
		}
		skw_sdio->tx_req_map &= ~(1<<portno);
		return ret;
	} else {
		for(i=0; i<2; i++) {
			try_to_wakeup_modem(portno);
			if(!(ret = skw_sdio_sdma_write(buffer, count))) {
				port->total += count;
				port->sent_packet++;
				ret = size;
				break;
			} else {
				skw_sdio_info("%s ret=%d\n", __func__, ret);
				if(ret == -ETIMEDOUT && !skw_sdio->device_active)
					continue;
			}
		}
		skw_sdio->tx_req_map &= ~(1<<portno);
		return ret;
	}
	return -ENOMEM;
}
static int sdio_read(struct sdio_port *port, char *buffer, int size)
{
	int data_size;
	int	ret = 0;
	int buffer_size;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	skw_sdio_dbg("%s buffer size = %d , (wp, rp) = (%d, %d)\n",
			__func__, size, port->rx_wp, port->rx_rp);
	if(port->state == PORT_STATE_ASST) {
		skw_sdio_err("Line:%d The CP assert  portno =%d error code =%d cp_state=%d !!\n",__LINE__,
				port->channel, ENOTCONN, skw_sdio->cp_state);
		if(skw_sdio->cp_state!=0){
			if(port->channel==1)
					port->state = PORT_STATE_OPEN;
			return -ENOTCONN;
		}
	}
try_again0:
	skw_reinit_completion(port->rx_done);
	if(port->rx_wp == port->rx_rp) {

		if ((port->state == PORT_STATE_CLSE) ||
			( port->channel>1 && !(skw_sdio->service_state_map & (1<<BT_SERVICE)))){
			skw_sdio_err("the portno %d the state %d\n",port->channel, port->state);
			return -EIO;
		}
		ret = wait_for_completion_interruptible(&port->rx_done);
		if(ret)
			return ret;
		if(port->state == PORT_STATE_CLSE) {
			port->state = PORT_STATE_IDLE;
			return -EAGAIN;
		}else if(port->state == PORT_STATE_ASST) {
			skw_sdio_err("The CP assert  portno =%d error code =%d!!!!\n", port->channel, ENOTCONN);
			if(skw_sdio->cp_state!=0){
				if(port->channel==1)
					port->state = PORT_STATE_OPEN;
				return -ENOTCONN;
			}
		}
	}
	mutex_lock(&port->rx_mutex);
	data_size = (port->length + port->rx_wp - port->rx_rp)%port->length;
	if(data_size==0) {
		skw_sdio_info("%s buffer size = %d , (wp, rp) = (%d, %d)\n",
			__func__, size, port->rx_wp, port->rx_rp);
		mutex_unlock(&port->rx_mutex);
		goto try_again0;
	}
	if(size > data_size)
		size = data_size;
	data_size = port->length - port->rx_rp;
	if(size > data_size) {
		memcpy(buffer, &port->read_buffer[port->rx_rp], data_size);
		memcpy(buffer+data_size, &port->read_buffer[0], size - data_size);
		port->rx_rp = size - data_size;
	} else {
		skw_sdio_dbg("size1 = %d , (wp, rp) = (%d, %d) (packet, total)=(%d, %d)\n",
				size, port->rx_wp, port->rx_rp, port->rx_packet, port->rx_count);
		memcpy(buffer, &port->read_buffer[port->rx_rp], size);
		port->rx_rp += size;
	}

	if (port->rx_rp == port->length)
		port->rx_rp = 0;

	if(port->rx_rp == port->rx_wp){
		port->rx_rp = 0;
		port->rx_wp = 0;
	}
	if(port->rx_flow_ctrl) {
		buffer_size = (port->length + port->rx_wp - port->rx_rp)%port->length;
		buffer_size = port->length - 1 - buffer_size;

		if (buffer_size > (port->length*2/3)) {
			port->rx_flow_ctrl = 0;
			skw_sdio_rx_port_follow_ctl(port->channel, port->rx_flow_ctrl);
		}
	}
	mutex_unlock(&port->rx_mutex);
	return size;
}

int recv_data(int portno, char *buffer, int size)
{
	struct sdio_port *port;
	int ret;
	if(size==0)
		return 0;
	if(portno >= max_ch_num)
		return -EINVAL;
	port = &sdio_ports[portno];
	if(!port->state)
		return -EIO;
	if(port->state == PORT_STATE_CLSE) {
		port->state = PORT_STATE_IDLE;
		return -EIO;
	}
	ret = sdio_read(port, buffer, size);
	return ret;
}
int wifi_send_cmd(int portno, struct scatterlist *sg, int sg_num, int total)
{
	struct sdio_port *port;
	int ret, i;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	u32 *data;

	if(total==0)
		return 0;
	if(portno >= max_ch_num)
		return -EINVAL;
	port = &sdio_ports[portno];
	if(!port->state)
		return -EIO;
	data = (u32 *)sg_virt(sg);
	for(i=0; i<2; i++) {
		try_to_wakeup_modem(portno);
		ret = skw_sdio_adma_write(portno, sg, sg_num, total);
		if(!ret)
			break;
		skw_sdio_info("timeout gpioin value=%d \n",gpio_get_value(skw_sdio->gpio_in));
	}
	skw_sdio->tx_req_map &= ~(1<<portno);
	skw_port_log(portno,"%s port%d sg_num=%d total=%d 0x%x 0x%x\n",
			__func__, portno, sg_num, total, data[0], data[1]);
	if(portno == WIFI_CMD_PORT) {
		memcpy(last_sent_wifi_cmd, data, 12);
		last_sent_time = jiffies;
		if (skw_sdio->gpio_in >=0 && !gpio_get_value(skw_sdio->gpio_in)) {
			skw_sdio_info("modem is sleep and wakeup it\n");
			try_to_wakeup_modem(portno);
		}
	}
	port->total += total;
	port->sent_packet += sg_num;
	return ret;
}
static int register_rx_callback(int id, void *func, void *para)
{
	struct sdio_port *port;

	if(id >= max_ch_num)
		return -EINVAL;
	port = &sdio_ports[id];
	if(port->state && func)
		return -EBUSY;
	port->rx_submit = func;
	port->rx_data = para;
	if(func) {
		port->sg_rx = kzalloc(MAX_SG_COUNT * sizeof(struct scatterlist), GFP_KERNEL);
		if(port->sg_rx == NULL)
			return -ENOMEM;
		port->state = PORT_STATE_OPEN;
	} else
		port->state = PORT_STATE_IDLE;
	return 0;
}

static int wifi_get_credit(void)
{
	char val;
	int err;

	err = skw_sdio_readb(SDIOHAL_PD_DL_CP2AP_SIG4, &val);
	if(err)
		return err;
	return val;
}
static int wifi_store_credit_to_cp(unsigned char val)
{
	int err;

	err = skw_sdio_writeb(SKW_SDIO_CREDIT_TO_CP, val);

	return err;
}
struct sv6160_platform_data wifi_pdata = {
	.data_port =  WIFI_DATA_PORT,
	.cmd_port =  WIFI_CMD_PORT,
	.bus_type = SDIO_LINK|TX_ADMA|RX_ADMA|CP_DBG,
	.max_buffer_size = 84*1536,
	.align_value = 256,
	.hw_adma_tx = wifi_send_cmd,
	.hw_sdma_tx = send_data,
	.callback_register = register_rx_callback,
	.modem_assert = send_modem_assert_command,
	.modem_register_notify = modem_register_notify,
	.modem_unregister_notify = modem_unregister_notify,
	.wifi_power_on = skw_sdio_wifi_power_on,
	.at_ops = {
		.port = 0,
		.open = open_sdio_port,
		.close = close_sdio_port,
		.read = recv_data,
		.write = send_data,
	},
	.wifi_get_credit=wifi_get_credit,
	.wifi_store_credit=wifi_store_credit_to_cp,
	.debug_info = assert_context
};
struct sv6160_platform_data ucom_pdata = {
	.data_port = 2,
	.cmd_port  = 3,
	.audio_port = 4,
	.bus_type = SDIO_LINK,
	.max_buffer_size = 0x1000,
	.align_value = 4,
	.hw_sdma_rx = recv_data,
	.hw_sdma_tx = send_data,
	.open_port = open_sdio_port,
	.close_port = close_sdio_port,
	.modem_assert = send_modem_assert_command,
	.modem_register_notify = modem_register_notify,
	.modem_unregister_notify = modem_unregister_notify,
	.skw_dump_mem = skw_sdio_dt_read,
};

int skw_sdio_bind_platform_driver(struct sdio_func *func)
{
	struct platform_device *pdev;
	char	pdev_name[32];
	struct sdio_port *port;
	int ret = 0;

	memset(sdio_ports, 0, sizeof(struct sdio_port)*MAX_CH_NUM);
	sprintf(pdev_name, "skw_ucom");
/*
 *	creaete AT device
 */
	pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
	if(!pdev)
		return -ENOMEM;
	pdev->dev.parent = &func->dev;
	pdev->dev.dma_mask = &port_dmamask;
	pdev->dev.coherent_dma_mask = port_dmamask;
	ucom_pdata.port_name = "ATC";
	if(skw_cp_ver == SKW_SDIO_V10)
		ucom_pdata.data_port = 0;
	else
		ucom_pdata.data_port = SDIO2_BSP_ATC_PORT;
	ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
	if(ret) {
		dev_err(&func->dev, "failed to add platform data \n");
		platform_device_put(pdev);;
		return ret;
	}
	port = &sdio_ports[ucom_pdata.data_port];
	port->state = PORT_STATE_IDLE;
	ret = platform_device_add(pdev);
	if(ret) {
		dev_err(&func->dev, "failt to register platform device\n");
		platform_device_put(pdev);
		return ret;
	}
	port->pdev = pdev;
	port->channel = ucom_pdata.data_port;
	port->length = SDIO_BUFFER_SIZE >> 2;
	port->read_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->read_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s RX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}
	port->write_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->write_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}
/*
 *	creaete log device
 */
	pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
	if(!pdev)
		return -ENOMEM;
	pdev->dev.parent = &func->dev;
	pdev->dev.dma_mask = &port_dmamask;
	pdev->dev.coherent_dma_mask = port_dmamask;
	ucom_pdata.port_name = "LOG";
	if(skw_cp_ver == SKW_SDIO_V10)
		ucom_pdata.data_port = 1;
	else
		ucom_pdata.data_port = SDIO2_BSP_LOG_PORT;
	ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
	if(ret) {
		dev_err(&func->dev, "failed to add %s device \n", ucom_pdata.port_name);
		platform_device_put(pdev);;
		return ret;
	}
	port = &sdio_ports[ucom_pdata.data_port];
	port->state = PORT_STATE_IDLE;
	ret = platform_device_add(pdev);
	if(ret) {
		dev_err(&func->dev, "failt to register platform device\n");
		platform_device_put(pdev);
		return ret;
	}

	port->pdev = pdev;
	port->channel = ucom_pdata.data_port;
	port->length = SDIO_BUFFER_SIZE >> 2;
	port->read_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->read_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s RX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}
	port->write_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->write_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}
/*
 *	creaete LOOPCHECK device
 */
	pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
	if(!pdev)
		return -ENOMEM;
	pdev->dev.parent = &func->dev;
	pdev->dev.dma_mask = &port_dmamask;
	pdev->dev.coherent_dma_mask = port_dmamask;
	ucom_pdata.port_name = "LOOPCHECK";
	if(skw_cp_ver == SKW_SDIO_V10)
		ucom_pdata.data_port = 7;
	else
		ucom_pdata.data_port = SDIO2_LOOPCHECK_PORT;
	ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
	if(ret) {
		dev_err(&func->dev, "failed to add platform data \n");
		platform_device_put(pdev);;
		return ret;
	}
	port = &sdio_ports[ucom_pdata.data_port];
	port->state = PORT_STATE_IDLE;
	ret = platform_device_add(pdev);
	if(ret) {
		dev_err(&func->dev, "failt to register platform device\n");
		platform_device_put(pdev);
		return ret;
	}

	port->pdev = pdev;
	port->channel = ucom_pdata.data_port;
	port->length = SDIO_BUFFER_SIZE >> 2;
	port->read_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->read_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s RX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}
	port->write_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->write_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}
	if(skw_cp_ver == SKW_SDIO_V20){
	/*
	 *	create BSPUPDATE device
	 */
		pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
		if(!pdev)
			return -ENOMEM;
		pdev->dev.parent = &func->dev;
		pdev->dev.dma_mask = &port_dmamask;
		pdev->dev.coherent_dma_mask = port_dmamask;
		ucom_pdata.port_name = "BSPUPDATE";
		ucom_pdata.data_port = SDIO2_BSP_UPDATE_PORT;
		ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
		if(ret) {
			dev_err(&func->dev, "failed to add platform data \n");
			platform_device_put(pdev);;
			return ret;
		}
		port = &sdio_ports[ucom_pdata.data_port];
		port->state = PORT_STATE_IDLE;
		ret = platform_device_add(pdev);
		if(ret) {
			dev_err(&func->dev, "failt to register platform device\n");
			platform_device_put(pdev);
			return ret;
		}

		port->pdev = pdev;
		port->channel = ucom_pdata.data_port;
		port->length = SDIO_BUFFER_SIZE >> 2;
		port->read_buffer = kzalloc(port->length , GFP_KERNEL);
		if(port->read_buffer == NULL) {
			dev_err(&func->dev, "failed to allocate %s RX buffer\n", ucom_pdata.port_name);
			return -ENOMEM;
		}
		port->write_buffer = kzalloc(port->length , GFP_KERNEL);
		if(port->write_buffer == NULL) {
			dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
			return -ENOMEM;
		}
	}
	return ret;
}
int skw_sdio_bind_WIFI_driver(struct sdio_func *func)
{
	struct platform_device *pdev;
	char	pdev_name[32];
	struct sdio_port *port;
	int ret = 0;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	if (sdio_ports[WIFI_DATA_PORT].pdev)
		return 0;
	sprintf(pdev_name, "%s%d", SV6160_WIRELESS, func->num);
	pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
	if(!pdev)
		return -ENOMEM;
	pdev->dev.parent = &func->dev;
	pdev->dev.dma_mask = &port_dmamask;
	pdev->dev.coherent_dma_mask = port_dmamask;
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
	wifi_pdata.bus_type |= CP_RLS;
#else
	if(!strncmp((char *)skw_sdio->chip_id,"SV6160",6)){
		wifi_pdata.bus_type |= CP_RLS;
	}
#endif
	/*support the sdma type bus*/
	if(!skw_sdio->adma_rx_enable){
		if(skw_cp_ver == SKW_SDIO_V10)
			wifi_pdata.bus_type = SDIO_LINK|TX_ADMA|RX_SDMA;
		else
			wifi_pdata.bus_type = SDIO2_LINK|TX_ADMA|RX_SDMA;
	}
	else{
		if(skw_cp_ver == SKW_SDIO_V10)
			wifi_pdata.bus_type = SDIO_LINK|TX_ADMA|RX_ADMA|CP_DBG;
		else
			wifi_pdata.bus_type = SDIO2_LINK|TX_ADMA|RX_ADMA|CP_DBG;
	}
	skw_sdio_info(" wifi_pdata bus_type:0x%x \n", wifi_pdata.bus_type);
	if(skw_cp_ver == SKW_SDIO_V20){
		wifi_pdata.data_port = (SDIO2_WIFI_DATA1_PORT << 4) | SDIO2_WIFI_DATA_PORT;
		wifi_pdata.cmd_port = SDIO2_WIFI_CMD_PORT;
	}
	memcpy(wifi_pdata.chipid, skw_sdio->chip_id, SKW_CHIP_ID_LENGTH);
	ret = platform_device_add_data(pdev, &wifi_pdata, sizeof(wifi_pdata));
	if(ret) {
		dev_err(&func->dev, "failed to add platform data \n");
		platform_device_put(pdev);;
		return ret;
	}
	if(skw_cp_ver == SKW_SDIO_V20){
		port = &sdio_ports[(wifi_pdata.data_port >> 4) & 0x0F];
		port->pdev = pdev;
		port->channel = (wifi_pdata.data_port >> 4) & 0x0F;
		port->rx_wp = 0;
		port->rx_rp = 0;
		port->sg_index = 0;
		port->state = 0;
	}

	port = &sdio_ports[wifi_pdata.data_port & 0x0F];
	port->pdev = pdev;
	port->channel = wifi_pdata.data_port & 0x0F;
	port->rx_wp = 0;
	port->rx_rp = 0;
	port->sg_index = 0;
	port->state = 0;

	port = &sdio_ports[wifi_pdata.cmd_port];
	port->pdev = pdev;
	port->channel = wifi_pdata.cmd_port;
	port->rx_wp = 0;
	port->rx_rp = 0;
	port->sg_index = 0;
	port->state = 0;

	ret = platform_device_add(pdev);
	if(ret) {
		dev_err(&func->dev, "failt to register platform device\n");
		platform_device_put(pdev);
	}
	return ret;
}
int skw_sdio_wifi_status(void)
{
	struct sdio_port *port = &sdio_ports[wifi_pdata.cmd_port];

	if (port->pdev == NULL)
		return 0;
	return 1;
}
int skw_sdio_wifi_power_on(int power_on)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int ret;
	if (power_on) {
		if (skw_sdio->power_off)
			skw_recovery_mode();
		ret = skw_sdio_bind_WIFI_driver(skw_sdio->sdio_func[FUNC_1]);
	} else {
		ret = skw_sdio_unbind_WIFI_driver(skw_sdio->sdio_func[FUNC_1]);
	}
	return ret;
}
#ifdef CONFIG_BT_SEEKWAVE
int skw_sdio_bind_btseekwave_driver(struct sdio_func *func)
{
	struct platform_device *pdev;
	char	pdev_name[32];
	struct sdio_port *port;
	int ret = 0;


	sprintf(pdev_name, "btseekwave");
/*
 *	creaete BT DATA device
 */
	pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
	if(!pdev)
		return -ENOMEM;
	pdev->dev.parent = &func->dev;
	pdev->dev.dma_mask = &port_dmamask;
	pdev->dev.coherent_dma_mask = port_dmamask;
	ucom_pdata.data_port = 2;

	ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
	if(ret) {
		dev_err(&func->dev, "failed to add platform data \n");
		platform_device_put(pdev);;
		return ret;
	}
	port = &sdio_ports[ucom_pdata.data_port];
	port->state = PORT_STATE_IDLE;
	ret = platform_device_add(pdev);
	if(ret) {
		dev_err(&func->dev, "failt to register platform device\n");
		platform_device_put(pdev);
		return ret;
	}
	port->pdev = pdev;
	port->channel = ucom_pdata.data_port;
	port->length = SDIO_BUFFER_SIZE;
	port->write_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->write_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}

/*
 *	creaete BT COMMAND device
 */
	ucom_pdata.data_port = ucom_pdata.cmd_port;

	port = &sdio_ports[ucom_pdata.data_port];
	port->state = PORT_STATE_IDLE;
	port->pdev = pdev;
	port->channel = ucom_pdata.data_port;
	port->length = SDIO_BUFFER_SIZE;
	port->write_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->write_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}

/*
 *	creaete BT audio device
 */
	ucom_pdata.data_port = ucom_pdata.audio_port;
	port = &sdio_ports[ucom_pdata.data_port];
	port->state = PORT_STATE_IDLE;
	port->pdev = pdev;
	port->channel = ucom_pdata.data_port;
	port->length = SDIO_BUFFER_SIZE;
	port->write_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->write_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}

	return ret;
}
#else
int skw_sdio_bind_BT_driver(struct sdio_func *func)
{
	struct platform_device *pdev;
	char	pdev_name[32];
	struct sdio_port *port;
	int ret = 0;


	sprintf(pdev_name, "skw_ucom");
/*
 *	creaete BT DATA device
 */
	pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
	if(!pdev)
		return -ENOMEM;
	pdev->dev.parent = &func->dev;
	pdev->dev.dma_mask = &port_dmamask;
	pdev->dev.coherent_dma_mask = port_dmamask;
	ucom_pdata.port_name = "BTDATA";
	if(skw_cp_ver == SKW_SDIO_V20)
		ucom_pdata.data_port = 5;
	else
		ucom_pdata.data_port = 2;
	ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
	if(ret) {
		dev_err(&func->dev, "failed to add platform data \n");
		platform_device_put(pdev);;
		return ret;
	}
	port = &sdio_ports[ucom_pdata.data_port];
	port->state = PORT_STATE_IDLE;
	ret = platform_device_add(pdev);
	if(ret) {
		dev_err(&func->dev, "failt to register platform device\n");
		platform_device_put(pdev);
		return ret;
	}
	port->pdev = pdev;
	port->channel = ucom_pdata.data_port;
	port->length = SDIO_BUFFER_SIZE;
	port->read_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->read_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s RX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}
	port->write_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->write_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}

/*
 *	creaete BT COMMAND device
 */
	pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
	if(!pdev)
		return -ENOMEM;
	pdev->dev.parent = &func->dev;
	pdev->dev.dma_mask = &port_dmamask;
	pdev->dev.coherent_dma_mask = port_dmamask;
	ucom_pdata.port_name = "BTCMD";
	if(skw_cp_ver == SKW_SDIO_V20)
		ucom_pdata.data_port = 2;
	else
		ucom_pdata.data_port = ucom_pdata.cmd_port;
	ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
	if(ret) {
		dev_err(&func->dev, "failed to add %s device \n", ucom_pdata.port_name);
		platform_device_put(pdev);;
		return ret;
	}
	port = &sdio_ports[ucom_pdata.data_port];
	port->state = PORT_STATE_IDLE;
	ret = platform_device_add(pdev);
	if(ret) {
		dev_err(&func->dev, "failt to register platform device\n");
		platform_device_put(pdev);
		return ret;
	}

	port->pdev = pdev;
	port->channel = ucom_pdata.data_port;
	port->length = SDIO_BUFFER_SIZE >> 2;
	port->read_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->read_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s RX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}
	port->write_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->write_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}

/*
 *	creaete BT audio device
 */
	pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
	if(!pdev)
		return -ENOMEM;
	pdev->dev.parent = &func->dev;
	pdev->dev.dma_mask = &port_dmamask;
	pdev->dev.coherent_dma_mask = port_dmamask;
	ucom_pdata.port_name = "BTAUDIO";
	if(skw_cp_ver == SKW_SDIO_V20)
		ucom_pdata.data_port = 3;
	else
		ucom_pdata.data_port = ucom_pdata.audio_port;
	ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
	if(ret) {
		dev_err(&func->dev, "failed to add platform data \n");
		platform_device_put(pdev);;
		return ret;
	}
	port = &sdio_ports[ucom_pdata.data_port];
	port->state = PORT_STATE_IDLE;
	ret = platform_device_add(pdev);
	if(ret) {
		dev_err(&func->dev, "failt to register platform device\n");
		platform_device_put(pdev);
		return ret;
	}

	port->pdev = pdev;
	port->channel = ucom_pdata.data_port;
	port->length = SDIO_BUFFER_SIZE >> 2;
	port->read_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->read_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s RX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}
	port->write_buffer = kzalloc(port->length , GFP_KERNEL);
	if(port->write_buffer == NULL) {
		dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
		return -ENOMEM;
	}

	if(skw_cp_ver == SKW_SDIO_V20){
		/*
		*	create BTISOC device
		*/
		pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
		if(!pdev)
			return -ENOMEM;
		pdev->dev.parent = &func->dev;
		pdev->dev.dma_mask = &port_dmamask;
		pdev->dev.coherent_dma_mask = port_dmamask;
		ucom_pdata.port_name = "BTISOC";
		ucom_pdata.data_port = SDIO2_BT_ISOC_PORT;
		ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
		if(ret) {
			dev_err(&func->dev, "failed to add platform data \n");
			platform_device_put(pdev);;
			return ret;
		}
		port = &sdio_ports[ucom_pdata.data_port];
		port->state = PORT_STATE_IDLE;
		ret = platform_device_add(pdev);
		if(ret) {
			dev_err(&func->dev, "failt to register platform device\n");
			platform_device_put(pdev);
			return ret;
		}

		port->pdev = pdev;
		port->channel = ucom_pdata.data_port;
		port->length = SDIO_BUFFER_SIZE >> 2;
		port->read_buffer = kzalloc(port->length , GFP_KERNEL);
		if(port->read_buffer == NULL) {
			dev_err(&func->dev, "failed to allocate %s RX buffer\n", ucom_pdata.port_name);
			return -ENOMEM;
		}
		port->write_buffer = kzalloc(port->length , GFP_KERNEL);
		if(port->write_buffer == NULL) {
			dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
			return -ENOMEM;
		}

		/*
		*	create BTLOG device
		*/
		pdev = platform_device_alloc(pdev_name, PLATFORM_DEVID_AUTO);
		if(!pdev)
			return -ENOMEM;
		pdev->dev.parent = &func->dev;
		pdev->dev.dma_mask = &port_dmamask;
		pdev->dev.coherent_dma_mask = port_dmamask;
		ucom_pdata.port_name = "BTLOG";
		ucom_pdata.data_port = SDIO2_BT_LOG_PORT;
		ret = platform_device_add_data(pdev, &ucom_pdata, sizeof(ucom_pdata));
		if(ret) {
			dev_err(&func->dev, "failed to add platform data \n");
			platform_device_put(pdev);;
			return ret;
		}
		port = &sdio_ports[ucom_pdata.data_port];
		port->state = PORT_STATE_IDLE;
		ret = platform_device_add(pdev);
		if(ret) {
			dev_err(&func->dev, "failt to register platform device\n");
			platform_device_put(pdev);
			return ret;
		}

		port->pdev = pdev;
		port->channel = ucom_pdata.data_port;
		port->length = SDIO_BUFFER_SIZE >> 2;
		port->read_buffer = kzalloc(port->length , GFP_KERNEL);
		if(port->read_buffer == NULL) {
			dev_err(&func->dev, "failed to allocate %s RX buffer\n", ucom_pdata.port_name);
			return -ENOMEM;
		}
		port->write_buffer = kzalloc(port->length , GFP_KERNEL);
		if(port->write_buffer == NULL) {
			dev_err(&func->dev, "failed to allocate %s TX buffer\n", ucom_pdata.port_name);
			return -ENOMEM;
		}
	}
	return ret;
}
#endif
static int skw_sdio_unbind_sdio_port_driver(struct sdio_func *func, int portno )
{
	int i;
	void *pdev;
	struct sdio_port *port;

	for(i=portno; i<max_ch_num;) {
		port = &sdio_ports[i];
		pdev = port->pdev;
		port->pdev = NULL;
		if(port->read_buffer)
			kfree(port->read_buffer);
		if(port->write_buffer)
			kfree(port->write_buffer);
		if(port->sg_rx)
			kfree(port->sg_rx);
		if(pdev)
			platform_device_unregister(pdev);
		port->sg_rx = NULL;
		port->read_buffer = NULL;
		port->write_buffer = NULL;
		port->pdev = NULL;
		port->rx_wp = 0;
		port->rx_rp = 0;
		port->sg_index = 0;
		port->state = 0;
		break;
	}
	return 0;
}

int skw_sdio_unbind_platform_driver(struct sdio_func *func)
{
	int ret;

	ret = skw_sdio_unbind_sdio_port_driver(func, 0);
	ret |= skw_sdio_unbind_sdio_port_driver(func, 1);
	ret |= skw_sdio_unbind_sdio_port_driver(func, 7);
	return ret;
}

int skw_sdio_unbind_WIFI_driver(struct sdio_func *func)
{
	int ret;

	if(skw_cp_ver == SKW_SDIO_V20)
		ret = skw_sdio_unbind_sdio_port_driver(func, SDIO2_WIFI_CMD_PORT);
	else
		ret = skw_sdio_unbind_sdio_port_driver(func, WIFI_DATA_PORT);
	return ret;
}

int skw_sdio_unbind_BT_driver(struct sdio_func *func)
{
	int ret;

	ret = skw_sdio_unbind_sdio_port_driver(func, 2);
	ret |= skw_sdio_unbind_sdio_port_driver(func, 3);
	ret |= skw_sdio_unbind_sdio_port_driver(func, 4);
	return ret;
}
