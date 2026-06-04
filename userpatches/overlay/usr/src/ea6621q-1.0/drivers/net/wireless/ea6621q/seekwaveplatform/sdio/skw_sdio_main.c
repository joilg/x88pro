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

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pm_runtime.h>
#include <linux/mmc/card.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include "skw_sdio_log.h"
#include "skw_sdio_debugfs.h"
#include "skw_sdio.h"
int bind_device=0;

module_param(bind_device, int, S_IRUGO);
#ifndef MMC_CAP2_SDIO_IRQ_NOTHREAD
#define MMC_CAP2_SDIO_IRQ_NOTHREAD (1 << 17)
#endif

#define skw_sdio_transfer_enter() mutex_lock(&skw_sdio->transfer_mutex)
#define skw_sdio_transfer_exit() mutex_unlock(&skw_sdio->transfer_mutex)

int g_irq_init = 0;
static int cp_log_status = 0;
irqreturn_t skw_gpio_irq_handler(int irq, void *dev_id); //interrupt
//int (*skw_dloader)(unsigned int subsys);
int skw_get_chipid(unsigned int address, void *buf,unsigned int len);
int check_chipid(void);
int skw_sdio_cp_reset(void);
int skw_sdio_cp_service_ops(int service_ops);
int skw_sdio_cpdebug_boot(void);
struct skw_sdio_data_t *g_skw_sdio_data;
static struct sdio_driver skw_sdio_driver;

static int skw_WIFI_service_start(void);
static int skw_WIFI_service_stop(void);
static int skw_BT_service_start(void);
static int skw_BT_service_stop(void);
extern int sdio_reset_comm(struct mmc_card *card);
extern void kernel_restart(char *cmd);
extern void skw_sdio_exception_work(struct work_struct *work);

extern char skw_cp_ver;
extern int max_ch_num;
extern char assert_context[];
extern int  assert_context_size;
extern int  cp_detect_sleep_mode;

struct skw_sdio_data_t *skw_sdio_get_data(void)
{
	return g_skw_sdio_data;
}

void skw_sdio_unlock_rx_ws(struct skw_sdio_data_t *skw_sdio)
{

	if (!atomic_read(&skw_sdio->rx_wakelocked))
		return;
	atomic_set(&skw_sdio->rx_wakelocked, 0);
#ifdef CONFIG_WAKELOCK
	__pm_relax(&skw_sdio->rx_wl.ws);
#else
	__pm_relax(skw_sdio->rx_ws);
#endif
}
static void skw_sdio_lock_rx_ws(struct skw_sdio_data_t *skw_sdio)
{
	if (atomic_read(&skw_sdio->rx_wakelocked))
		return;
	atomic_set(&skw_sdio->rx_wakelocked, 1);
#ifdef CONFIG_WAKELOCK
	__pm_stay_awake(&skw_sdio->rx_wl.ws);
#else
	__pm_stay_awake(skw_sdio->rx_ws);
#endif
}
static void skw_sdio_wakeup_source_init(struct skw_sdio_data_t *skw_sdio)
{
	if(skw_sdio) {
#ifdef CONFIG_WAKELOCK
	wake_lock_init(&skw_sdio->rx_wl, WAKE_LOCK_SUSPEND,"skw_sdio_r_wakelock");
#else
	skw_sdio->rx_ws = skw_wakeup_source_register(NULL, "skw_sdio_r_wakelock");
#endif
	}
}
static void skw_sdio_wakeup_source_destroy(struct skw_sdio_data_t *skw_sdio)
{
	if(skw_sdio) {
#ifdef CONFIG_WAKELOCK
	wake_lock_destroy(&skw_sdio->rx_wl);
#else
	wakeup_source_unregister(skw_sdio->rx_ws);
#endif
	}
}

void skw_resume_check(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	unsigned int timeout;

	timeout = 0;
	while((!atomic_read(&skw_sdio->resume_flag)) && (timeout++ < 20000))
		usleep_range(1500, 2000);
}

static void skw_sdio_abort(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func0 = skw_sdio->sdio_func[FUNC_0];
	unsigned char value;
	int ret;

	sdio_claim_host(func0);

	value = sdio_readb(func0, SDIO_VER_CCCR, &ret);

	sdio_writeb(func0, SDIO_ABORT_TRANS, SKW_SDIO_CCCR_ABORT, &ret);

	value = sdio_readb(func0, SDIO_VER_CCCR, &ret);
	skw_sdio_err("SDIO Abort, SDIO_VER_CCCR:0x%x\n", value);

	sdio_release_host(func0);
}

int skw_sdio_sdma_write(unsigned char *src, unsigned int len)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_1];
	int blksize = func->cur_blksize;
	int ret = 0;

	if (!src || len%4) {
		skw_sdio_err("%s invalid para %p, %d\n", __func__, src, len);
		return -1;
	}

	len = (len + blksize -1)/blksize*blksize;

	skw_resume_check();
	skw_sdio_transfer_enter();
	sdio_claim_host(func);
	ret = sdio_writesb(func, SKW_SDIO_PK_MODE_ADDR, src, len);
	if (ret < 0)
		skw_sdio_err("%s  ret = %d\n", __func__, ret);
	sdio_release_host(func);
	if (ret) 
		skw_sdio_abort();
	skw_sdio_transfer_exit();

	return ret;
}

int skw_sdio_sdma_read(unsigned char *src, unsigned int len)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_1];
	int ret = 0;

	skw_resume_check();
	skw_sdio_transfer_enter();
	sdio_claim_host(func);
	ret = sdio_readsb(func, src, SKW_SDIO_PK_MODE_ADDR, len);
	sdio_release_host(func);
	if (ret != 0)
		skw_sdio_abort();
	skw_sdio_transfer_exit();
	return ret;
}

void *skw_get_bus_dev(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	if ((!skw_sdio) || (!skw_sdio->sdio_dev_host))
		return NULL;
	return &skw_sdio->sdio_func[FUNC_1]->dev;
}
EXPORT_SYMBOL_GPL(skw_get_bus_dev);
static int skw_sdio_start_transfer(struct scatterlist *sgs, int sg_count,
	int total, struct sdio_func *sdio_func, uint fix_inc, bool dir, uint addr)
{
	struct mmc_request mmc_req;
	struct mmc_command mmc_cmd;
	struct mmc_data mmc_dat;
	struct mmc_host *host = sdio_func->card->host;
	bool fifo = (fix_inc == SKW_SDIO_DATA_FIX);
	uint fn_num = sdio_func->num;
	uint blk_num, blk_size, max_blk_count, max_req_size;
	int err_ret = 0;
	u64 write_time[2];


	blk_size = SKW_SDIO_BLK_SIZE;
	max_blk_count = min_t(unsigned int, host->max_blk_count, (uint)MAX_IO_RW_BLK);
	max_req_size = min_t(unsigned int,   max_blk_count*blk_size, host->max_req_size);

	memset(&mmc_req, 0, sizeof(struct mmc_request));
	memset(&mmc_cmd, 0, sizeof(struct mmc_command));
	memset(&mmc_dat, 0, sizeof(struct mmc_data));

	if (total % blk_size != 0) {
		skw_sdio_err("total %d not aligned to blk size\n", total);
		return -1;
	}

	blk_num = total / blk_size;
	mmc_dat.sg = sgs;
	mmc_dat.sg_len = sg_count;
	mmc_dat.blksz = blk_size;
	mmc_dat.blocks = blk_num;
	mmc_dat.flags = dir ? MMC_DATA_WRITE : MMC_DATA_READ;
	mmc_cmd.opcode = 53; /* SD_IO_RW_EXTENDED */
	mmc_cmd.arg = dir ? 1<<31 : 0;
	mmc_cmd.arg |= (fn_num & 0x7) << 28;
	mmc_cmd.arg |= 1<<27;
	mmc_cmd.arg |= fifo ? 0 : 1<<26;
	mmc_cmd.arg |= (addr & 0x1FFFF) << 9;
	mmc_cmd.arg |= blk_num & 0x1FF;
	mmc_cmd.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_ADTC;
	mmc_req.cmd = &mmc_cmd;
	mmc_req.data = &mmc_dat;
	if (!fifo)
		addr += total;
	write_time[0] = jiffies;
	sdio_claim_host(sdio_func);
	mmc_set_data_timeout(&mmc_dat, sdio_func->card);
	write_time[1] = jiffies;
	mmc_wait_for_req(host, &mmc_req);
	skw_sdio_dbg("total:%d sg_count:%d cmd_arg 0x%x, 0x%x 0x%x 0x%x\n", total, sg_count, mmc_cmd.arg,
			(u32)write_time[0], (u32)write_time[1], (u32)jiffies);
	sdio_release_host(sdio_func);

	err_ret = mmc_cmd.error ? mmc_cmd.error : mmc_dat.error;
	if (err_ret != 0) {
		skw_sdio_err("%s:CMD53 %s failed error=%d\n",__func__,
				  dir ? "write" : "read", err_ret);
	}
	return err_ret;
}

int skw_sdio_adma_write(int portno, struct scatterlist *sgs, int sg_count, int total)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int ret = 0;

	skw_resume_check();
	skw_sdio_transfer_enter();
	if(skw_sdio->resume_com==0)
		skw_sdio->resume_com = 1; 
	ret = skw_sdio_start_transfer(sgs, sg_count, SKW_SDIO_ALIGN_BLK(total),
				  skw_sdio->sdio_func[FUNC_1], SKW_SDIO_DATA_FIX,
				  SKW_SDIO_WRITE, SKW_SDIO_PK_MODE_ADDR);
	if (ret) {
		skw_sdio_abort();
	} else {
		if (skw_sdio->device_active==0 && skw_sdio->irq_type)
			skw_sdio->device_active = gpio_get_value(skw_sdio->gpio_in);
	}
	skw_sdio_transfer_exit();

	return ret;
}

int skw_sdio_adma_read(struct skw_sdio_data_t *skw_sdio, struct scatterlist *sgs, int sg_count, int total)
{
	int ret = 0;

	skw_resume_check();
	skw_sdio_transfer_enter();
	ret = skw_sdio_start_transfer(sgs, sg_count, total,
				  skw_sdio->sdio_func[FUNC_1], SKW_SDIO_DATA_FIX,
				  SKW_SDIO_READ, SKW_SDIO_PK_MODE_ADDR);
	if (ret)
		skw_sdio_abort();
	skw_sdio_transfer_exit();
	return ret;
}

static int skw_sdio_dt_set_address(unsigned int address)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_0];
	unsigned char value ;
	int err = 0;
	int i;

	sdio_claim_host(func);
	for (i = 0; i < 4; i++) {
		value = (address >> (8 * i)) & 0xFF;
		sdio_writeb(func, value, SKW_SDIO_FBR_REG+i, &err);
		if (err != 0)
			break;
	}
	sdio_release_host(func);

	return err;
}


int skw_sdio_writel(unsigned int address, void *data)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_1];
	int ret = 0;

	skw_resume_check();
	skw_sdio_transfer_enter();

	ret = skw_sdio_dt_set_address(address);
	if (ret != 0) {
		skw_sdio_transfer_exit();
		return ret;
	}

	sdio_claim_host(func);
	sdio_writel(func, *(unsigned int *)data, SKW_SDIO_DT_MODE_ADDR, &ret);
	sdio_release_host(func);
	skw_sdio_transfer_exit();

	if (ret) {
		skw_sdio_err("%s fail ret:%d, addr=0x%x\n", __func__,
				ret, address);
		skw_sdio_abort();
	}

	return ret;
}

int skw_sdio_readl(unsigned int address, void *data)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_1];
	int ret = 0;


	skw_resume_check();
	skw_sdio_transfer_enter();
	ret = skw_sdio_dt_set_address(address);
	if (ret != 0) {
		skw_sdio_transfer_exit();
		return ret;
	}

	sdio_claim_host(func);

	*(unsigned int *)data = sdio_readl(func, SKW_SDIO_DT_MODE_ADDR, &ret);

	sdio_release_host(func);
	skw_sdio_transfer_exit();
	if (ret) {
		skw_sdio_err("%s fail ret:%d, addr=0x%x\n", __func__, ret, address);
		skw_sdio_abort();
	}

	return ret;
}
/*
 *command = 0: service_start else service stop
 *service = 0: WIFI_service else BT service.
 */
int send_modem_service_command(u16 service, u16 command)
{
	u16 cmd;
	int ret = 0;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	if(command)
		skw_sdio->service_state_map&= ~(1<<service);
		//command = 1;
	cmd = (service<<1)|command;
	cmd = 1 << cmd;
	if (cmd>>8) {
		skw_sdio_err("service command error 0x%x!", cmd);
			return -EINVAL;
	}
	if(skw_sdio->cp_state)
		return -EINVAL;

	ret = skw_sdio_writeb(SKW_AP2CP_IRQ_REG, cmd & 0xff);
	skw_sdio_info("ret = %d cmd %x\n", service, command);
	return ret;
}

static unsigned int max_bytes(struct sdio_func *func)
{
	unsigned int mval = func->card->host->max_blk_size;

	if (func->card->quirks & MMC_QUIRK_BLKSZ_FOR_BYTE_MODE)
		mval = min(mval, func->cur_blksize);
	else
		mval = min(mval, func->max_blksize);

	if (func->card->quirks & MMC_QUIRK_BROKEN_BYTE_MODE_512)
		return min(mval, 511u);

	/* maximum size for byte mode */
	return min(mval, 512u);
}

int skw_sdio_dt_write(unsigned int address,	void *buf, unsigned int len)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_1];
	unsigned int remainder = len;
	unsigned int trans_len;
	int ret = 0;

	skw_resume_check();
	skw_sdio_transfer_enter();

	ret = skw_sdio_dt_set_address(address);
	if (ret != 0) {
		skw_sdio_err("%s set address error!!!", __func__);
		skw_sdio_transfer_exit();
		return ret;
	}

	if(skw_sdio->resume_com==0)
		skw_sdio->resume_com = 1;
	sdio_claim_host(func);
	while (remainder > 0) {
		if (remainder >= func->cur_blksize)
			trans_len = func->cur_blksize;
		else
			trans_len = min(remainder, max_bytes(func));
		ret = sdio_memcpy_toio(func, SKW_SDIO_DT_MODE_ADDR, buf, trans_len);
		if (ret) {
			skw_sdio_err("%s sdio_memcpy_toio failed!!!", __func__);
			break;
		}
		remainder -= trans_len;
		buf += trans_len;
	}
	sdio_release_host(func);
	skw_sdio_transfer_exit();
	if (ret) {
		skw_sdio_err("dt write fail ret:%d, address=0x%x\n", ret, address);
		skw_sdio_abort();
	}

	return ret;
}

int skw_sdio_dt_read(unsigned int address, void *buf, unsigned int len)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_1];
	unsigned int remainder = len;
	unsigned int trans_len;
	int ret = 0;

	ret = skw_sdio_dt_set_address(address);
	if (ret != 0) {
		skw_sdio_err("set address error ret=%d !!!", ret);
		return ret;
	}
	if(skw_sdio->resume_com==0)
		skw_sdio->resume_com = 1; 
	skw_sdio_transfer_enter();
	sdio_claim_host(func);
	while (remainder > 0) {
		if (remainder >= func->cur_blksize)
			trans_len = func->cur_blksize;
		else
			trans_len = min(remainder, max_bytes(func));
		ret = sdio_memcpy_fromio(func, buf, SKW_SDIO_DT_MODE_ADDR, trans_len);
		if (ret) {
			skw_sdio_err("sdio_memcpy_fromio: %p 0x%x ret=%d\n", buf, *(uint32_t *)buf, ret);
			break;
		}
		remainder -= trans_len;
		buf += trans_len;
	}
	sdio_release_host(func);
	skw_sdio_transfer_exit();
	if (ret) {
		skw_sdio_err("dt read fail ret:%d, address=0x%x\n", ret, address);
		skw_sdio_abort();
	}

	return ret;
}

int skw_sdio_readb(unsigned int address, unsigned char *value)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_0];
	unsigned char reg = 0;
	int err = 0;

	sdio_claim_host(func);
	reg = sdio_readb(func, address, &err);
	if (value)
		*value = reg;
	sdio_release_host(func);
	return err;
}

int skw_sdio_writeb(unsigned int address, unsigned char value)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_0];
	int err = 0;

	try_to_wakeup_modem(8);
	sdio_claim_host(func);
	sdio_writeb(func, value, address, &err);
	sdio_release_host(func);

	return err;
}

int skw_sdio_host_irq_init(unsigned int irq_gpio_num)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int ret = 0;

	skw_sdio->irq_type = SKW_SDIO_EXTERNAL_IRQ;
	skw_sdio->device_active = gpio_get_value(skw_sdio->gpio_in);
	skw_sdio->irq_num = gpio_to_irq(skw_sdio->gpio_in);
	skw_sdio->irq_trigger_type = IRQF_TRIGGER_RISING;
	skw_sdio_info("gpio_In:%d,gpio_out:%d irq %d\n",
			   	skw_sdio->gpio_in, skw_sdio->gpio_out, skw_sdio->irq_num);
	if (skw_sdio->irq_num) {
		ret = request_irq(skw_sdio->irq_num, skw_gpio_irq_handler,
				skw_sdio->irq_trigger_type | IRQF_ONESHOT, "skw-gpio-irq", NULL);
		if (ret != 0) {
			free_irq(skw_sdio->irq_num, NULL);
			skw_sdio_err("%s request gpio irq fail ret=%d\n", __func__, ret);
			return -1;
		} else {
			skw_sdio_dbg("gpio request_irq=%d  GPIO value %d!\n",
					skw_sdio->irq_num, skw_sdio->device_active);
		}
	}
	enable_irq_wake(skw_sdio->irq_num);
	skw_sdio_rx_up(skw_sdio);
	return ret;
}

static int skw_sdio_get_dev_func(struct sdio_func *func)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	if (func->num >= MAX_FUNC_NUM) {
		skw_sdio_err("func num err!!! func num is %d!!!",
			func->num);
		return -1;
	}
	skw_sdio_dbg("func num is %d.", func->num);

	if (func->num == 1) {
		skw_sdio->sdio_func[FUNC_0] = kmemdup(func, sizeof(*func),
							 GFP_KERNEL);
		skw_sdio->sdio_func[FUNC_0]->num = 0;
		skw_sdio->sdio_func[FUNC_0]->max_blksize = SKW_SDIO_BLK_SIZE;
	}

	skw_sdio->sdio_func[FUNC_1] = func;

	return 0;
}

void skw_sdio_inband_irq_handler(struct sdio_func *func)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func0 = skw_sdio->sdio_func[FUNC_0];
	int ret;

	if (!SKW_CARD_ONLINE(skw_sdio)) {
		skw_sdio_err("%s  card offline\n", __func__);
		return;
	}

	skw_resume_check();

	/* send cmd to clear cp int status */
	sdio_claim_host(func0);
	try_to_wakeup_modem(8);
	sdio_f0_readb(func0, SDIO_CCCR_INTx, &ret);
	sdio_release_host(func0);
	if (ret < 0)
		skw_sdio_err("%s error %d\n", __func__, ret);
	skw_sdio_lock_rx_ws(skw_sdio);
	skw_sdio_rx_up(skw_sdio);
}

#ifdef CONFIG_PM_SLEEP
static int skw_sdio_suspend(struct device *dev)
{
	struct sdio_func *func = container_of(dev, struct sdio_func, dev);
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int  ret = 0;

	skw_sdio_dbg("[%s]enter\n", __func__);

	atomic_set(&skw_sdio->resume_flag, 0);

	if (SKW_CARD_ONLINE(skw_sdio))
		func->card->host->pm_flags |= MMC_PM_KEEP_POWER;

	func = skw_sdio->sdio_func[FUNC_1];
	send_host_suspend_indication(skw_sdio);
	if ((skw_sdio->irq_type == SKW_SDIO_INBAND_IRQ) && skw_sdio->resume_com) {
		sdio_claim_host(func);
		try_to_wakeup_modem(8);
		msleep(1);
		ret = sdio_release_irq(func);
		sdio_release_host(func);
		skw_sdio_dbg("%s sdio_release_irq ret = %d\n", __func__, ret);
	} 
	atomic_set(&skw_sdio->suspending, 1);
	skw_sdio->resume_com = 0;
	return ret;
}

static int skw_sdio_resume(struct device *dev)
{
	struct sdio_func *func = container_of(dev, struct sdio_func, dev);
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int ret = 0;

	skw_sdio_dbg("[%s]enter\n", __func__);
#if defined(SKW_BOOT_DEBUG)
	skw_dloader(2);
#endif

	if (SKW_CARD_ONLINE(skw_sdio))
		func->card->host->pm_flags &= ~MMC_PM_KEEP_POWER;

	func = skw_sdio->sdio_func[FUNC_1];
	send_host_resume_indication(skw_sdio);
	if (!func->irq_handler && (skw_sdio->irq_type == SKW_SDIO_INBAND_IRQ)) {
		sdio_claim_host(func);
		ret=sdio_claim_irq(func, skw_sdio_inband_irq_handler);
		sdio_release_host(func);
		if(ret < 0)
			skw_sdio_err("%s sdio_claim_irq ret = %d\n", __func__, ret);
	}
	atomic_set(&skw_sdio->resume_flag, 1);
	return ret;
}
#endif
irqreturn_t skw_gpio_irq_handler(int irq, void *dev_id) //interrupt
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int	value = gpio_get_value(skw_sdio->gpio_in);

	if (skw_sdio->power_off)
		return IRQ_HANDLED;
	if (!SKW_CARD_ONLINE(skw_sdio)) {
		skw_sdio_err("%s card offline\n", __func__);
		return IRQ_HANDLED;
	}
	skw_sdio_lock_rx_ws(skw_sdio);
	if (value && (skw_sdio->irq_type == SKW_SDIO_EXTERNAL_IRQ))
			skw_sdio_rx_up(skw_sdio);
	host_gpio_in_routine(value);

	return IRQ_HANDLED;
}

static int skw_check_cp_ready(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	skw_reinit_completion(skw_sdio->download_done);
	if (wait_for_completion_timeout(&skw_sdio->download_done,
		msecs_to_jiffies(1000)) == 0) {
		 skw_sdio_err("check CP-ready time out\n");
		 return -ETIME;
	}
	skw_sdio_dbg("CHECK CP PASS!\n");
	return 0;
}

static int skw_sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct mmc_host *host = func->card->host;
	int ret;

	skw_sdio_log(SKW_SDIO_INFO,"%s: func->class=%x, vendor=0x%04x, device=0x%04x, "
		 "func_num=0x%04x, clock=%d blksize=0x%x max_blkcnt %d\n", __func__,
		 func->class, func->vendor, func->device, func->num, host->ios.clock,
		 func->cur_blksize,
			 func->card->host->max_blk_count);

	ret = skw_sdio_get_dev_func(func);
	if (ret < 0) {
		skw_sdio_err("get func err\n");
		return ret;
	}

	skw_sdio->sdio_dev_host = skw_sdio->sdio_func[FUNC_1]->card->host;
	if (skw_sdio->sdio_dev_host == NULL) {
		skw_sdio_err("get host failed!!!");
		return -1;
	}

	if (!skw_sdio->pwrseq) {
		struct sdio_func *func1 = skw_sdio->sdio_func[FUNC_1];
		/* Enable Function 1 */
		sdio_claim_host(func1);
		ret = sdio_enable_func(func1);

		skw_sdio_info("sdio_enable_func ret=%d type %d\n", ret, skw_sdio->irq_type);
		if(!ret) {
			sdio_set_block_size(func1, SKW_SDIO_BLK_SIZE);
			func1->max_blksize = SKW_SDIO_BLK_SIZE;
			if (skw_sdio->irq_type == SKW_SDIO_INBAND_IRQ)
				if(sdio_claim_irq(func1,skw_sdio_inband_irq_handler))
					skw_sdio_err("sdio_claim_irq failed\n");
			sdio_release_host(func1);
		} else {
			sdio_release_host(func1);
			skw_sdio_err("enable func1 err!!! ret is %d\n", ret);
			return ret;
		}
		skw_sdio->resume_com = 1;
		skw_sdio_info("enable func1 done\n");
	} else
		pm_runtime_put_noidle(&func->dev);
	if (!SKW_CARD_ONLINE(skw_sdio))
		atomic_sub(SKW_SDIO_CARD_OFFLINE, &skw_sdio->online);

	complete(&skw_sdio->scan_done);

	check_chipid();
	/* the card is nonremovable */
	skw_sdio->sdio_dev_host->caps |= MMC_CAP_NONREMOVABLE;
	if (bind_device == 1) {
		ret = skw_sdio_writeb(SKW_SDIO_PLD_DMA_TYPE,ADMA);
		skw_sdio->adma_rx_enable = 1;
		if(ret !=0){
			skw_sdio_err("the dma type write fail ret:%d\n",ret);
			return -1;
		}
		skw_sdio_info("line%d,adma type \n",  __LINE__);
		send_modem_service_command(WIFI_SERVICE, SERVICE_START);
	}else if (bind_device ==2){
		ret = skw_sdio_writeb(SKW_SDIO_PLD_DMA_TYPE,SDMA);
		skw_sdio->adma_rx_enable = 0;
		if(ret !=0){
			skw_sdio_err("the dma type write fail: %d\n",ret);
			return -1;
		}
		send_modem_service_command(WIFI_SERVICE, SERVICE_START);
		skw_sdio_info("the skw_sdio sdma write the pass\n");
	}
	skw_sdio_bind_platform_driver(skw_sdio->sdio_func[FUNC_1]);

#ifdef CONFIG_BT_SEEKWAVE
	skw_sdio_bind_btseekwave_driver(skw_sdio->sdio_func[FUNC_1]);
#endif
	skw_sdio->service_state_map = 0;
	skw_sdio->power_off = 0;
	skw_sdio->host_active = 1;
	return 0;
}

static void skw_sdio_remove(struct sdio_func *func)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	skw_sdio_info("Enter\n");

	complete(&skw_sdio->remove_done);

	if (skw_sdio->irq_type == SKW_SDIO_INBAND_IRQ) {
		sdio_claim_host(skw_sdio->sdio_func[FUNC_1]);
		sdio_release_irq(skw_sdio->sdio_func[FUNC_1]);
		sdio_release_host(skw_sdio->sdio_func[FUNC_1]);
	} else	if (skw_sdio->irq_num)
		free_irq(skw_sdio->irq_num, NULL);

	skw_sdio->host_active = 0;
	skw_sdio_unbind_platform_driver(skw_sdio->sdio_func[FUNC_1]);
	skw_sdio_unbind_WIFI_driver(skw_sdio->sdio_func[FUNC_1]);
	skw_sdio_unbind_BT_driver(skw_sdio->sdio_func[FUNC_1]);
	kfree(skw_sdio->sdio_func[FUNC_0]);
}

void skw_sdio_launch_thread(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	init_completion(&skw_sdio->rx_completed);
	skw_sdio_wakeup_source_init(skw_sdio);
	skw_sdio->rx_thread =
		kthread_create(skw_sdio_rx_thread, NULL, "skw_sdio_rx_thread");
	if (skw_sdio->rx_thread) {
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
		sched_set_fifo_low(skw_sdio->rx_thread);
#else
		struct sched_param param;
		param.sched_priority = 1;
		sched_setscheduler(skw_sdio->rx_thread, SCHED_FIFO, &param);
#endif
		kthread_bind(skw_sdio->rx_thread, cpumask_first(cpu_online_mask));
		set_user_nice(skw_sdio->rx_thread, SKW_MIN_NICE);
		wake_up_process(skw_sdio->rx_thread);
	} else
		skw_sdio_err("creat skw_sdio_rx_thread fail\n");
}

void skw_sdio_stop_thread(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	if (skw_sdio->rx_thread) {
		skw_sdio->threads_exit = 1;
		skw_sdio_rx_up(skw_sdio);
		kthread_stop(skw_sdio->rx_thread);
		skw_sdio->rx_thread = NULL;
		skw_sdio_wakeup_source_destroy(skw_sdio);
	}
	skw_sdio_info("done\n");
}

static const struct dev_pm_ops skw_sdio_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(skw_sdio_suspend, skw_sdio_resume)
};

static const struct sdio_device_id skw_sdio_ids[] = {
	//{ .compatible = "seekwave-sdio", },
	{SDIO_DEVICE(0, 0)},
	{SDIO_DEVICE(0xABCD, 0x1234)},
	{SDIO_DEVICE(0x3607, 0x6160)},
	{},
};

static struct sdio_driver skw_sdio_driver = {
	.probe = skw_sdio_probe,
	.remove = skw_sdio_remove,
	.name = "skw_sdio",
	.id_table = skw_sdio_ids,
	.drv = {
		.pm = &skw_sdio_pm_ops,
	},
};

void skw_sdio_remove_card(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	init_completion(&skw_sdio->remove_done);
	sdio_unregister_driver(&skw_sdio_driver);
	skw_sdio_info(" sdio_unregister_driver\n");
	if (wait_for_completion_timeout(&skw_sdio->remove_done,
					msecs_to_jiffies(5000)) == 0)
		skw_sdio_err("remove card time out\n");
	else
		skw_sdio_info("remove card end\n");

}

int skw_sdio_scan_card(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int ret = 0;

	skw_sdio_info("sdio_scan_card\n");

	if (SKW_CARD_ONLINE(skw_sdio)) {
		skw_sdio_info("card already online!, 0x%x\n", atomic_read(&skw_sdio->online));
		skw_sdio_remove_card();
		msleep(100);
	}

	init_completion(&skw_sdio->scan_done);
	init_completion(&skw_sdio->download_done);
	init_completion(&skw_sdio->device_wakeup);
	init_waitqueue_head(&skw_sdio->wq);
	//skw_sdio->irq_type = SKW_SDIO_EXTERNAL_IRQ;
	skw_sdio->irq_type = SKW_SDIO_INBAND_IRQ;
	ret = sdio_register_driver(&skw_sdio_driver);
	if (ret != 0) {
		skw_sdio_err("sdio_register_driver error :%d\n", ret);
		return ret;
	}
	if (wait_for_completion_timeout(&skw_sdio->scan_done, msecs_to_jiffies(2000)) == 0) {
		skw_sdio_err("wait scan card time out\n");
		return -ENODEV;
	}
	if (!skw_sdio->sdio_dev_host) {
		skw_sdio_err("sdio_dev_host is NULL!\n");
		return -ENODEV;
	}
	skw_sdio_info("scan end!\n");

	return ret;
}

/****************************************************************
 *Description:sleep feature support en api
 *Author:junwei.jiang
 *Date:2023-06-14
 * ************************************************************/
int skw_sdio_slp_feature_en(unsigned int address, unsigned int slp_en)
{
	int ret = 0;
	ret = skw_sdio_writeb(address,slp_en);
	if(ret !=0){
		skw_sdio_err("no-sleep support en write fail, ret=%d\n",ret);
		return -1;
	}
	skw_sdio_info("no-sleep_support_enable:%d\n ",slp_en);
	return 0;
}

/****************************************************************
 *Description:set the dma type SDMA, AMDA
 *Author:junwei.jiang
 *Date:2021-11-23
 * ************************************************************/
int skw_sdio_set_dma_type(unsigned int address, unsigned int dma_type)
{
	int ret = 0;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	if(dma_type == SDMA){
		/*support the sdma so adma_rx_enable set 0*/
		skw_sdio->adma_rx_enable = 0;
	}
	if(!bind_device){
		ret = skw_sdio_writeb(address,dma_type);
		if(ret !=0){
			skw_sdio_err("dma type write fail, ret=%d\n",ret);
			return -1;
		}
	}
	skw_sdio_info("dma_type=%d,adma_rx_enable:%d\n ",dma_type,skw_sdio->adma_rx_enable);
	return 0;
}

/****************************************************************
*Description:
*Func:used the ap boot cp interface;
*Output:the dloader the bin to cp
*Return：0:pass; other : fail
*Author：JUNWEI.JIANG
*Date:2021-09-07
****************************************************************/
int skw_sdio_boot_cp(int boot_mode)
{
	int ret =0;
	struct sdio_func *func;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	if (skw_sdio->irq_num == 0) {
		skw_sdio->gpio_in = -1;
		skw_sdio->gpio_out = -1;
	}
	skw_sdio_set_dma_type(skw_sdio->sdio_bootdata->dma_type_addr,
		skw_sdio->sdio_bootdata->dma_type);
	skw_sdio_slp_feature_en(skw_sdio->sdio_bootdata->slp_disable_addr,
		skw_sdio->sdio_bootdata->slp_disable);
	//2:download the boot bin 1CPALL 2, wifi 3,bt
	skw_sdio_info("DOWNLOAD BIN TO CP\n");
	memset(skw_sdio->next_size_buf, 0, 16);
	ret = skw_sdio_dt_write(SKW_BT_CONFIG_ADDR, skw_sdio->next_size_buf, 16);
	if(skw_sdio->sdio_bootdata->dram_dl_size)
		ret = skw_sdio_dt_write(skw_sdio->sdio_bootdata->dram_dl_addr,
				skw_sdio->sdio_bootdata->dram_img_data,skw_sdio->sdio_bootdata->dram_dl_size);
	if(skw_sdio->sdio_bootdata->iram_dl_size)
		ret = skw_sdio_dt_write(skw_sdio->sdio_bootdata->iram_dl_addr,
				skw_sdio->sdio_bootdata->iram_img_data,skw_sdio->sdio_bootdata->iram_dl_size);
	if(ret !=0)
		goto FAIL;
	//first boot need the setup cp first_dl_flag=0 is first
	skw_sdio_info("line:%d write the download done flag\n", __LINE__);
	ret= skw_sdio_writeb(skw_sdio->sdio_bootdata->save_setup_addr,BIT(0));
	if(ret !=0)
		goto FAIL;
	if(!skw_sdio->cp_state) {
		ret = skw_check_cp_ready();
		if ((ret == -ETIME) && (boot_mode==SKW_FIRST_BOOT))
			skw_sdio_rx_up(skw_sdio);
		else if(!ret && boot_mode==SKW_FIRST_BOOT) {
			if ((cp_detect_sleep_mode==2 || cp_detect_sleep_mode==1) &&
				(skw_sdio->irq_type == SKW_SDIO_INBAND_IRQ) &&
				(skw_sdio->sdio_bootdata->gpio_in >=0)) {
				func = skw_sdio->sdio_func[FUNC_1];
				sdio_claim_host(func);
				sdio_release_irq(func);
				sdio_release_host(func);
				skw_sdio->gpio_in = skw_sdio->sdio_bootdata->gpio_in;
				skw_sdio->gpio_out = skw_sdio->sdio_bootdata->gpio_out;
				ret = skw_sdio_host_irq_init(skw_sdio->gpio_in);
			}
		}
	}
	if(ret !=0)
		goto FAIL;

	if(!ret&&boot_mode==SKW_FIRST_BOOT) {
		skw_sdio_bind_WIFI_driver(skw_sdio->sdio_func[FUNC_1]);
#ifndef CONFIG_BT_SEEKWAVE
		skw_sdio_bind_BT_driver(skw_sdio->sdio_func[FUNC_1]);
#endif
	}
	return ret;
FAIL:
	skw_sdio_err("fail ret=%d\n", ret);
	return ret;
}

/************************************************************************
 *Decription:release CP close the CP log
 *Author:junwei.jiang
 *Date:2023-02-16
 *Modfiy:
 *
 ********************************************************************* */
int skw_sdio_cp_log(int disable)
{
	int ret = 0;

	cp_log_status = disable;
	ret= skw_sdio_writeb(SDIOHAL_CPLOG_TO_AP_SWITCH, disable);
	if(ret <0){
		skw_sdio_err("close the log signal send fail ret=%d\n", ret);
		return ret;
	}
	skw_sdio_writeb(SKW_AP2CP_IRQ_REG, 0x20);
	skw_sdio_info("close the cp log :%d\n",disable);
	return 0;
}

int skw_sdio_cp_log_status(void)
{
	return cp_log_status;
}
/************************************************************************
 *Decription:send WIFI start command to modem.
 *Author:junwei.jiang
 *Date:2022-10-27
 *Modfiy:
 *
 ********************************************************************* */
static int skw_WIFI_service_start(void)
{
	int ret;

	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	skw_sdio_info("Enter STARTWIFI cp_state:%d\n",skw_sdio->cp_state);
	if (skw_sdio->service_state_map & (1<<WIFI_SERVICE))
		return 0;

	mutex_lock(&skw_sdio->except_mutex);
	if (skw_sdio->service_state_map==0 && skw_sdio->power_off)
		skw_recovery_mode();
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
	skw_sdio_cp_log(1);
#endif
	ret = send_modem_service_command(WIFI_SERVICE, SERVICE_START);
	if (ret==0)
		ret = skw_check_cp_ready();
	mutex_unlock(&skw_sdio->except_mutex);
	return ret;
}
/************************************************************************
 *Decription: send WIFI stop command to modem.
  *Author:junwei.jiang
 *Date:2022-10-27
 *Modfiy:
 *
 ********************************************************************* */
static int skw_WIFI_service_stop(void)
{
	int ret = 0;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	skw_sdio_info("Enter,STOPWIFI  cp_state:%d",skw_sdio->cp_state);
	mutex_lock(&skw_sdio->except_mutex);
	if (skw_sdio->service_state_map & (1<<WIFI_SERVICE))
		ret = send_modem_service_command(WIFI_SERVICE, SERVICE_STOP);
	if (!skw_sdio->cp_state && ret==0 && skw_sdio->service_state_map==0) {
		if (skw_sdio->chip_en >= 0 && !skw_sdio->power_off) {
			gpio_set_value(skw_sdio->chip_en, 0);
			skw_sdio->power_off = 1;
			skw_sdio_info("power off");
		}
	}
	mutex_unlock(&skw_sdio->except_mutex);
	return ret;
}
/************************************************************************
 *Decription:send BT start command to modem.
 *Author:junwei.jiang
 *Date:2022-10-27
 *Modfiy:
 *
 ********************************************************************* */
static int skw_BT_service_start(void)
{
	int ret;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	skw_sdio_info("Enter cpstate=%d\n",skw_sdio->cp_state);
	if(assert_context_size)
		skw_sdio_info("%s\n", assert_context);
	if (skw_sdio->service_state_map & (1<<BT_SERVICE))
		return 0;

	mutex_lock(&skw_sdio->except_mutex);
	if (skw_sdio->service_state_map==0 && skw_sdio->power_off)
		skw_recovery_mode();
#ifdef CONFIG_SEEKWAVE_PLD_RELEASE
	skw_sdio_cp_log(1);
#endif
	ret =send_modem_service_command(BT_SERVICE, SERVICE_START);
	if (!ret)
		ret = skw_check_cp_ready();
	mutex_unlock(&skw_sdio->except_mutex);
	return ret;
}

/************************************************************************
 *Decription:send BT stop command to modem.
 *Author:junwei.jiang
 *Date:2022-10-27
 *Modfiy:
 *
 ********************************************************************* */
static int skw_BT_service_stop(void)
{
	int ret = 0;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	skw_sdio_info("Enter cpstate=%d\n",skw_sdio->cp_state);

	mutex_lock(&skw_sdio->except_mutex);
	if (skw_sdio->service_state_map & (1<<BT_SERVICE) && !skw_sdio->cp_state) {
		ret = send_modem_service_command(BT_SERVICE, SERVICE_STOP);
		if (!ret)
			skw_check_cp_ready();
	}
	if (!skw_sdio->cp_state && ret==0 && skw_sdio->service_state_map==0) {
		if (skw_sdio->chip_en >= 0 && !skw_sdio->power_off) {
			gpio_set_value(skw_sdio->chip_en, 0);
			skw_sdio->power_off = 1;
			skw_sdio_info("power off");
		}
	}
	mutex_unlock(&skw_sdio->except_mutex);
	return ret;
}

/****************************************************************
*Description:
*Func:used the ap boot cp interface;
*Output:the dloader the bin to cp
*Return：0:pass; other : fail
*Author：JUNWEI.JIANG
*Date:2021-09-07
****************************************************************/
int skw_sdio_cp_service_ops(int service_ops)
{
	int ret =0;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	switch(service_ops)
	{
		case SKW_WIFI_START:
			wait_event_interruptible_timeout(skw_sdio->wq,
				!skw_sdio->cp_state, msecs_to_jiffies(2000));
			ret=skw_WIFI_service_start();
			skw_sdio_dbg("-----WIFI SERIVCE START\n");
		break;
		case SKW_WIFI_STOP:
			ret =skw_WIFI_service_stop();
			skw_sdio_dbg("----WIFI SERVICE---STOP\n");
		break;
		case SKW_BT_START:
		{
			wait_event_interruptible_timeout(skw_sdio->wq,
				!skw_sdio->cp_state, msecs_to_jiffies(2000));
			ret=skw_BT_service_start();
			skw_sdio_dbg("-----BT SERIVCE --START\n");
		}
		break;
		case SKW_BT_STOP:
			ret =skw_BT_service_stop();
			skw_sdio_dbg("-----BT SERVICE --STOP\n");
		break;
		default:
			skw_sdio_err("service not support!\n");
		break;
	}
	return ret;
}
/****************************************************************
*Description:skw_boot_loader
*Func:used the ap boot cp interface;
*Output:the dloader the bin to cp
*Return：0:pass; other : fail
*Author：JUNWEI.JIANG
*Date:2021-09-07
****************************************************************/
int skw_boot_loader(struct seekwave_device *boot_data)
{
	int ret =0;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	skw_sdio->sdio_bootdata= boot_data;

	if (skw_sdio->power_off)
		boot_data->dl_module = RECOVERY_BOOT;
	/*--------CP RESET RESCAN------------*/
	if (boot_data->dl_module== RECOVERY_BOOT) {
		skw_sdio_info("rescan and reset CHIP\n");
		skw_recovery_mode();
	} else {
	/*-------FIRST AP BOOT--------------*/
	if (!skw_sdio->sdio_bootdata->first_dl_flag){
		if (!strncmp((char *)skw_sdio->chip_id,"SV6160",6)){
			boot_data->chip_id = 0x6160;
			skw_sdio_info("boot chip id 0x%x\n", boot_data->chip_id);
		}
		skw_sdio->chip_en = boot_data->chip_en;
		if (skw_sdio->sdio_bootdata->iram_dl_size&&
				skw_sdio->sdio_bootdata->dram_dl_size){
			ret=skw_sdio_boot_cp(SKW_FIRST_BOOT);
		} else
			ret=skw_sdio_cpdebug_boot();
			
		}
	}
	/*------CP SERVICE OPS----------*/
	ret=skw_sdio_cp_service_ops(skw_sdio->sdio_bootdata->service_ops);
	if(ret!=0)
		goto FAIL;
	skw_sdio_info("boot loader ops end!!!\n");
	return 0;
FAIL:
	skw_sdio_err("line:%d  fail ret=%d\n", __LINE__, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(skw_boot_loader);

/****************************************************************
*Description:check dev ready
*Func:used the ap boot cp interface;
*Calls:sdio or usb
*Call By:host dev ready
*Input:NULL
*Output:pass :0 or fail ENODEV
*Others:
*Author：JUNWEI.JIANG
*Date:2022-06-09
****************************************************************/
int skw_reset_bus_dev(void)
{
	return 0;
}
EXPORT_SYMBOL_GPL(skw_reset_bus_dev);


/****************************************************************
*Description:skw_get_chipid
*Func:used the ap boot cp interface;
*Calls:boot data
*Call By:the ap host
*Input:the boot data informations
*Output:the dloader the bin to cp
*Return：0:pass; other : fail
*Others:
*Author：JUNWEI.JIANG
*Date:2021-10-11
****************************************************************/
static int skw_sdio_reset_card(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int ret = 0;

#if (KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE <= KERNEL_VERSION(5,18,19))
	gpio_set_value(skw_sdio->chip_en, 0);
	msleep(2);
	gpio_set_value(skw_sdio->chip_en, 1);
	msleep(5);
	sdio_claim_host(skw_sdio->sdio_func[FUNC_1]);
	ret = mmc_sw_reset(skw_sdio->sdio_dev_host);
	sdio_release_host(skw_sdio->sdio_func[FUNC_1]);	
#elif (KERNEL_VERSION(5,19,0) <=LINUX_VERSION_CODE)
	gpio_set_value(skw_sdio->chip_en, 0);
	msleep(2);
	gpio_set_value(skw_sdio->chip_en, 1);
	msleep(5);
	sdio_claim_host(skw_sdio->sdio_func[FUNC_1]);
	ret = mmc_sw_reset(skw_sdio->sdio_dev_host->card);
	sdio_release_host(skw_sdio->sdio_func[FUNC_1]);
#else
	ret = sdio_reset_comm((skw_sdio->sdio_dev_host->card));
#endif
	return ret;
}

int skw_sdio_cp_reset(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	struct sdio_func *func = skw_sdio->sdio_func[FUNC_1];
	int ret;

	if (skw_sdio->irq_type == SKW_SDIO_INBAND_IRQ) {
		sdio_claim_host(func);
		ret=sdio_release_irq(func);
		sdio_release_host(func);
		if(ret < 0)
			skw_sdio_err("%s sdio_release_irq ret = %d\n", __func__, ret);
	}

	ret = skw_sdio_reset_card();
	if (ret < 0) {
		skw_sdio_info("reset sdio host fail, wait 100ms and try again \n");
		msleep(100);
		ret = skw_sdio_reset_card();
		if (ret < 0) {
			skw_sdio_info("the reset sdio host fail \n");
		} else {
			skw_sdio_info("the reset sdio host pass \n");
		}
	} else {
		skw_sdio_info("the reset sdio host pass \n");
	}
	msleep(5);
	/* Enable Function 1 */
	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	sdio_set_block_size(func, SKW_SDIO_BLK_SIZE);
	func->max_blksize = SKW_SDIO_BLK_SIZE;

	if (skw_sdio->irq_type == SKW_SDIO_INBAND_IRQ) {
		ret=sdio_claim_irq(func, skw_sdio_inband_irq_handler);
		if(ret < 0)
			skw_sdio_err("%s sdio_claim_irq ret = %d\n", __func__, ret);
	}

	sdio_release_host(skw_sdio->sdio_func[FUNC_1]);
	if (ret < 0) {
		skw_sdio_err("enable func1 err!!! ret is %d\n", ret);
		return -1;
	}
	skw_sdio_info("CP RESET OK!\n");
	return 0;
}
/****************************************************************
*Description:skw_sdio_cpdebug_boot
*Func:used the ap boot cp interface;
*Others:
*Author：JUNWEI.JIANG
*Date:2022-07-15
****************************************************************/
int skw_sdio_cpdebug_boot(void)
{
	int ret =0;
	struct sdio_func *func;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	skw_sdio_info("not download CP from AP!!!!\n");
	skw_sdio_set_dma_type(skw_sdio->sdio_bootdata->dma_type_addr,
			skw_sdio->sdio_bootdata->dma_type);
	skw_sdio_slp_feature_en(skw_sdio->sdio_bootdata->slp_disable_addr,
			skw_sdio->sdio_bootdata->slp_disable);

	if(!skw_sdio->cp_state && !skw_sdio->sdio_bootdata->first_dl_flag) {
		if ((cp_detect_sleep_mode==2 || cp_detect_sleep_mode==1) &&
				(skw_sdio->irq_type == SKW_SDIO_INBAND_IRQ) &&
				(skw_sdio->sdio_bootdata->gpio_in >=0)) {
			skw_sdio->irq_type = SKW_SDIO_EXTERNAL_IRQ;
			func = skw_sdio->sdio_func[FUNC_1];
			sdio_claim_host(func);
			sdio_release_irq(func);
			sdio_release_host(func);
			skw_sdio_host_irq_init(skw_sdio->gpio_in);
			//try_to_wakeup_modem(8);
		}
		skw_sdio_bind_WIFI_driver(skw_sdio->sdio_func[FUNC_1]);
#ifndef CONFIG_BT_SEEKWAVE
		skw_sdio_bind_BT_driver(skw_sdio->sdio_func[FUNC_1]);
#endif
	}
	skw_sdio_info(" CP DUEBGBOOT Done!!!\n");
	return ret;
}

/****************************************************************
*Description:skw_recovery_mode
*Func:used the ap boot cp interface;
*Calls:boot data
*Call By:the ap host
*Input:the boot data informations
*Output:reset cp
*Return：0:pass; other : fail
*Others:
*Author：JUNWEI.JIANG
*Date:2022-07-15
****************************************************************/
int skw_recovery_mode(void)
{
	int ret;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	if(!skw_sdio->sdio_bootdata->dram_dl_size || !skw_sdio->sdio_bootdata->iram_dl_size
			|| skw_sdio_recovery_debug_status()){
		skw_sdio_err("CP DEBUG BOOT,AND NO NEED RECOVERY!!! \n");
		return -1;
	}
	ret=skw_sdio_cp_reset();
	if(ret!=0){
		skw_sdio_err("CP RESET fail \n");
		return -1;
	}
	skw_sdio->power_off = 0;
	ret = skw_sdio_boot_cp(RECOVERY_BOOT);
	if(ret!=0){
		skw_sdio_err("CP RESET fail \n");
		return -1;
	}
	skw_sdio_info("Recovery ok\n");
	return ret;
}

int check_chipid(void)
{
	int ret;
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();

	ret = skw_sdio_dt_read(SKW_CHIP_ID0, skw_sdio->chip_id, SKW_CHIP_ID_LENGTH);
	if(!strncmp((char *)skw_sdio->chip_id,"SV6160",6)){
		skw_cp_ver = SKW_SDIO_V10;
		max_ch_num = MAX_CH_NUM;
		skw_sdio_info("Chip id:%s used SDIO10",(char *)skw_sdio->chip_id);
		//print_hex_dump(KERN_ERR, "CHIP ID: ", 0, 16, 1,skw_sdio->chip_id, 32, 1);
	}else{
		skw_cp_ver = SKW_SDIO_V20;
		max_ch_num = SDIO2_MAX_CH_NUM;
		skw_sdio_info("Chip id:%s used SDIO20 ", (char *)skw_sdio->chip_id);
	}

	if(ret<0){
		skw_sdio_err("Get the chip id fail!!\n");
		return ret;
	}
	return 0;
}
static int __init skw_sdio_io_init(void)
{
	struct skw_sdio_data_t *skw_sdio;
	int ret = 0;
	skw_sdio_debugfs_init();
	skw_sdio_log_level_init();

	skw_sdio = kzalloc(sizeof(struct skw_sdio_data_t), GFP_KERNEL);
	if (!skw_sdio) {
		WARN_ON(1);
		return -ENOMEM;
	}

	/* card not ready */
	g_skw_sdio_data = skw_sdio;
	mutex_init(&skw_sdio->transfer_mutex);
	mutex_init(&skw_sdio->except_mutex);
	atomic_set(&skw_sdio->resume_flag, 1);
	skw_sdio->next_size_buf = kzalloc(SKW_BUF_SIZE, GFP_KERNEL);
	if(skw_sdio->next_size_buf == NULL){
		kfree(skw_sdio);
		return -ENOMEM;
	}
	skw_sdio->eof_buf = kzalloc(SKW_BUF_SIZE, GFP_KERNEL);
	atomic_set(&skw_sdio->online, SKW_SDIO_CARD_OFFLINE);
	if(!bind_device){
		skw_sdio->adma_rx_enable = 1;
	}
	INIT_DELAYED_WORK(&skw_sdio->skw_except_work, skw_sdio_exception_work);
	skw_sdio_launch_thread();
	skw_sdio_scan_card();
	skw_sdio_info(" OK\n");
	return ret;
}

static void __exit  skw_sdio_io_exit(void)
{
	struct skw_sdio_data_t *skw_sdio = skw_sdio_get_data();
	int ret = 0;

	skw_sdio_debugfs_deinit();
	skw_sdio_stop_thread();
	if (SKW_CARD_ONLINE(skw_sdio)) {
		skw_sdio_remove_card();
	}
	ret = skw_sdio_reset_card();
	if (ret < 0) {
		skw_sdio_info("reset sdio host fail, wait 100ms and try again \n");
		msleep(100);
		ret = skw_sdio_reset_card();
		if (ret < 0) {
			skw_sdio_info("the reset sdio host fail \n");
		} else {
			skw_sdio_info("the reset sdio host pass \n");
		}
	} else {
		skw_sdio_info("the reset sdio host pass \n");
	}
	cancel_delayed_work_sync(&skw_sdio->skw_except_work);
	mutex_destroy(&skw_sdio->transfer_mutex);
	mutex_destroy(&skw_sdio->except_mutex);
	if (skw_sdio) {
		kfree(skw_sdio->next_size_buf);
		kfree(skw_sdio->eof_buf);
		skw_sdio->sdio_bootdata = NULL;
		skw_sdio->sdio_dev_host = NULL;
		kfree(skw_sdio);
		skw_sdio = NULL;
	}
	skw_sdio_info(" OK\n");
}
late_initcall(skw_sdio_io_init)
module_exit(skw_sdio_io_exit)
MODULE_LICENSE("GPL v2");
