/* Copyright (c) 2008-2010, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/slab.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/diagchar.h>
#include <linux/sched.h>
#ifdef CONFIG_DIAG_OVER_USB
#include <mach/usbdiag.h>
#endif
#include <asm/current.h>
#include "diagchar_hdlc.h"
#include "diagfwd.h"
#include "diagmem.h"
#include "diagchar.h"
#include <linux/timer.h>
#include "../../usb/gadget/f_diag.h"
#if defined(CONFIG_MACH_MECHA)
#include "../../../arch/arm/mach-msm/7x30-lte/sdio_diag.h"
#endif
#if defined(CONFIG_ARCH_MSM8X60_LTE)
#include "diagfwd_sdio.h"
static	unsigned char *buf_9k = NULL;
#endif
MODULE_DESCRIPTION("Diag Char Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");

/*#define DIAG_DEBUG*/
struct diagchar_dev *driver;
struct diagchar_priv {
	int pid;
};
/* The following variables can be specified by module options */
 /* for copy buffer */
static unsigned int itemsize = 2048; /*Size of item in the mempool */
static unsigned int poolsize = 10; /*Number of items in the mempool */
/* for hdlc buffer */
static unsigned int itemsize_hdlc = 8192; /*Size of item in the mempool */
static unsigned int poolsize_hdlc = 8;  /*Number of items in the mempool */
/* for write structure buffer */
static unsigned int itemsize_write_struct = 20; /*Size of item in the mempool */
static unsigned int poolsize_write_struct = 8; /* Num of items in the mempool */
/* This is the max number of user-space clients supported at initialization*/
static unsigned int max_clients = 15;
static unsigned int threshold_client_limit = 30;
/* This is the maximum number of pkt registrations supported at initialization*/
unsigned int diag_max_registration = 25;
unsigned int diag_threshold_registration = 100;

/* Timer variables */
static struct timer_list drain_timer;
static int timer_in_progress;
static int mdm_diag;
void *buf_hdlc;
module_param(itemsize, uint, 0);
module_param(poolsize, uint, 0);
module_param(max_clients, uint, 0);

static char diag_trace[1024];
static ssize_t diag_dump(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	uint16_t i, p;
	memset(diag_trace, 0x00, sizeof(diag_trace));
	DIAG_INFO("%s: diag_debug_buf_idx=%d\n", __func__, diag_debug_buf_idx);
	for (i = 0, p = 0; i < 1023 ; i++)	{
		if (i == diag_debug_buf_idx)
			p += sprintf(diag_trace+p, "%c",  '*');
		else
			p += sprintf(diag_trace+p, "%c",  diag_debug_buf[i]);
	}
	print_hex_dump(KERN_INFO, "diag_debug_buf:", DUMP_PREFIX_ADDRESS, 16, 1, diag_debug_buf, 1026, 1);

	return sprintf(buf, "%s\n", diag_trace);

}
static DEVICE_ATTR(diag_trace, 0444, diag_dump, NULL);

/* delayed_rsp_id 0 represents no delay in the response. Any other number
    means that the diag packet has a delayed response. */
static uint16_t delayed_rsp_id = 1;
#define DIAGPKT_MAX_DELAYED_RSP 0xFFFF
/* This macro gets the next delayed respose id. Once it reaches
 DIAGPKT_MAX_DELAYED_RSP, it stays at DIAGPKT_MAX_DELAYED_RSP */

#define DIAGPKT_NEXT_DELAYED_RSP_ID(x) \
((x < DIAGPKT_MAX_DELAYED_RSP) ? x++ : DIAGPKT_MAX_DELAYED_RSP)

#define COPY_USER_SPACE_OR_EXIT(buf, data, length)		\
do {									\
	if ((count < ret+length) || (copy_to_user(buf,			\
					(void *)&data, length))) {	\
		ret = -EFAULT;						\
		goto exit;						\
	}								\
	ret += length;							\
} while (0)

static void drain_timer_func(unsigned long data)
{
	queue_work(driver->diag_wq , &(driver->diag_drain_work));
}

void diag_drain_work_fn(struct work_struct *work)
{
	int err = 0;
	timer_in_progress = 0;

	mutex_lock(&driver->diagchar_mutex);
	if (buf_hdlc) {
		err = diag_device_write(buf_hdlc, APPS_DATA, NULL);
		if (err) {
			/*Free the buffer right away if write failed */
			diagmem_free(driver, buf_hdlc, POOL_TYPE_HDLC);
			diagmem_free(driver, (unsigned char *)driver->
				 write_ptr_svc, POOL_TYPE_WRITE_STRUCT);
		}
		buf_hdlc = NULL;
#ifdef DIAG_DEBUG
		printk(KERN_INFO "\n Number of bytes written "
				 "from timer is %d ", driver->used);
#endif
		driver->used = 0;
	}
	mutex_unlock(&driver->diagchar_mutex);
}

void diag_read_smd_work_fn(struct work_struct *work)
{
	__diag_smd_send_req();
}

void diag_read_smd_qdsp_work_fn(struct work_struct *work)
{
	__diag_smd_qdsp_send_req();
}

void diag_add_client(int i, struct file *file)
{
	struct diagchar_priv *diagpriv_data;

	driver->client_map[i].pid = current->tgid;
	diagpriv_data = kmalloc(sizeof(struct diagchar_priv),
							GFP_KERNEL);
	if (diagpriv_data)
		diagpriv_data->pid = current->tgid;
	file->private_data = diagpriv_data;
	strncpy(driver->client_map[i].name, current->comm, 20);
	driver->client_map[i].name[19] = '\0';
}

static int diagchar_open(struct inode *inode, struct file *file)
{
	int i = 0;
	void *temp;
	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
	current->comm, current->parent->comm, current->tgid);
	if (driver) {
		mutex_lock(&driver->diagchar_mutex);

		for (i = 0; i < driver->num_clients; i++)
			if (driver->client_map[i].pid == 0)
				break;

		if (i < driver->num_clients) {
			diag_add_client(i, file);
		} else {
			if (i < threshold_client_limit) {
				driver->num_clients++;
				temp = krealloc(driver->client_map
					, (driver->num_clients) * sizeof(struct
						 diag_client_map), GFP_KERNEL);
				if (!temp)
					goto fail;
				else
					driver->client_map = temp;
				temp = krealloc(driver->data_ready
					, (driver->num_clients) * sizeof(int),
							GFP_KERNEL);
				if (!temp)
					goto fail;
				else
					driver->data_ready = temp;
				diag_add_client(i, file);
			} else {
				mutex_unlock(&driver->diagchar_mutex);
				pr_info("Max client limit for DIAG reached\n");
				pr_info("Cannot open handle %s"
					   " %d", current->comm, current->tgid);
				for (i = 0; i < driver->num_clients; i++)
					pr_info("%d) %s PID=%d", i, driver->
						client_map[i].name,
					 driver->client_map[i].pid);
				return -ENOMEM;
			}
		}
		driver->data_ready[i] |= MSG_MASKS_TYPE;
		driver->data_ready[i] |= EVENT_MASKS_TYPE;
		driver->data_ready[i] |= LOG_MASKS_TYPE;

		if (driver->ref_count == 0)
			diagmem_init(driver);
		driver->ref_count++;
		mutex_unlock(&driver->diagchar_mutex);
		return 0;
	}
	return -ENOMEM;

fail:
	mutex_unlock(&driver->diagchar_mutex);
	driver->num_clients--;
	pr_alert("diag: Insufficient memory for new client");
	return -ENOMEM;
}

static int diagchar_close(struct inode *inode, struct file *file)
{
	int i = 0;
	struct diagchar_priv *diagpriv_data = file->private_data;

	if (!(file->private_data)) {
		pr_alert("diag: Invalid file pointer");
		return -ENOMEM;
	}

	if (!driver)
		return -ENOMEM;

#ifdef CONFIG_DIAG_OVER_USB
	/* If the SD logging process exits, change logging to USB mode */
	if (driver->logging_process_id == current->tgid) {
		driver->logging_mode = USB_MODE;
		diagfwd_connect();
	}
#endif /* DIAG over USB */
	/* Delete the pkt response table entry for the exiting process */
	for (i = 0; i < diag_max_registration; i++) {
		if (driver->table[i].process_id == current->tgid)
			driver->table[i].process_id = 0;
	}

	mutex_lock(&driver->diagchar_mutex);
	driver->ref_count--;
			/* On Client exit, try to destroy all 3 pools */
			diagmem_exit(driver, POOL_TYPE_COPY);
			diagmem_exit(driver, POOL_TYPE_HDLC);
			diagmem_exit(driver, POOL_TYPE_WRITE_STRUCT);
	for (i = 0; i < driver->num_clients; i++) {
		if (NULL != diagpriv_data && diagpriv_data->pid ==
			 driver->client_map[i].pid) {
			driver->client_map[i].pid = 0;
			kfree(diagpriv_data);
			diagpriv_data = NULL;
			break;
		}
	}
	mutex_unlock(&driver->diagchar_mutex);
	return 0;
}

static int diagchar_ioctl(struct inode *inode, struct file *filp,
			   unsigned int iocmd, unsigned long ioarg)
{
	int i, j, count_entries = 0, temp;
	int success = -1;
	void *temp_buf;

	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
	current->comm, current->parent->comm, current->tgid);

	if (iocmd == DIAG_IOCTL_COMMAND_REG) {
		struct bindpkt_params_per_process *pkt_params =
			 (struct bindpkt_params_per_process *) ioarg;

		for (i = 0; i < diag_max_registration; i++) {
			if (driver->table[i].process_id == 0) {
				success = 1;
				driver->table[i].cmd_code =
					 pkt_params->params->cmd_code;
				driver->table[i].subsys_id =
					 pkt_params->params->subsys_id;
				driver->table[i].cmd_code_lo =
					 pkt_params->params->cmd_code_lo;
				driver->table[i].cmd_code_hi =
					pkt_params->params->cmd_code_hi;
				driver->table[i].process_id = current->tgid;
				count_entries++;
				if (pkt_params->count > count_entries)
					pkt_params->params++;
				else
					return success;
			}
			}
		if (i < diag_threshold_registration) {
			/* Increase table size by amount required */
			diag_max_registration += pkt_params->count -
							 count_entries;
			/* Make sure size doesnt go beyond threshold */
			if (diag_max_registration > diag_threshold_registration)
				diag_max_registration =
						 diag_threshold_registration;
			temp_buf = krealloc(driver->table,
					 diag_max_registration*sizeof(struct
					 diag_master_table), GFP_KERNEL);
			BUG_ON(!temp_buf);
			driver->table = temp_buf;

			for (j = i; j < diag_max_registration; j++) {
				success = 1;
				driver->table[j].cmd_code = pkt_params->
							params->cmd_code;
				driver->table[j].subsys_id = pkt_params->
							params->subsys_id;
				driver->table[j].cmd_code_lo = pkt_params->
							params->cmd_code_lo;
				driver->table[j].cmd_code_hi = pkt_params->
							params->cmd_code_hi;
				driver->table[j].process_id = current->tgid;
				count_entries++;
				if (pkt_params->count > count_entries)
					pkt_params->params++;
				else
					return success;
		}
		} else
			pr_err("Max size reached, Pkt Registration failed for"
						" Process %d", current->tgid);

		success = 0;
	} else if (iocmd == DIAG_IOCTL_GET_DELAYED_RSP_ID) {
		struct diagpkt_delay_params *delay_params =
					(struct diagpkt_delay_params *) ioarg;

		if ((delay_params->rsp_ptr) &&
		 (delay_params->size == sizeof(delayed_rsp_id)) &&
				 (delay_params->num_bytes_ptr)) {
			*((uint16_t *)delay_params->rsp_ptr) =
				DIAGPKT_NEXT_DELAYED_RSP_ID(delayed_rsp_id);
			*(delay_params->num_bytes_ptr) = sizeof(delayed_rsp_id);
			success = 0;
		}
	} else if (iocmd == DIAG_IOCTL_LSM_DEINIT) {
		for (i = 0; i < driver->num_clients; i++)
			if (driver->client_map[i].pid == current->tgid)
				break;
		if (i == -1)
			return -EINVAL;
		driver->data_ready[i] |= DEINIT_TYPE;
		wake_up_interruptible(&driver->wait_q);
		success = 1;
	} else if (iocmd == DIAG_IOCTL_SWITCH_LOGGING) {
		mutex_lock(&driver->diagchar_mutex);
		temp = driver->logging_mode;
		driver->logging_mode = (int)ioarg;
		driver->logging_process_id = current->tgid;
		mutex_unlock(&driver->diagchar_mutex);
		if (temp == MEMORY_DEVICE_MODE && driver->logging_mode
							== NO_LOGGING_MODE) {
			driver->in_busy_1 = 1;
			driver->in_busy_2 = 1;
			driver->in_busy_qdsp_1 = 1;
			driver->in_busy_qdsp_2 = 1;
		} else if (temp == NO_LOGGING_MODE && driver->logging_mode
							== MEMORY_DEVICE_MODE) {
			driver->in_busy_1 = 0;
			driver->in_busy_2 = 0;
			driver->in_busy_qdsp_1 = 0;
			driver->in_busy_qdsp_2 = 0;
			/* Poll SMD channels to check for data*/
			if (driver->ch)
				queue_work(driver->diag_wq,
					&(driver->diag_read_smd_work));
			if (driver->chqdsp)
				queue_work(driver->diag_wq,
					&(driver->diag_read_smd_qdsp_work));
		}
#ifdef CONFIG_DIAG_OVER_USB
		else if (temp == USB_MODE && driver->logging_mode
							 == NO_LOGGING_MODE)
			diagfwd_disconnect();
		else if (temp == NO_LOGGING_MODE && driver->logging_mode
								== USB_MODE)
			diagfwd_connect();
		else if (temp == USB_MODE && driver->logging_mode
							== MEMORY_DEVICE_MODE) {
			if (driver->enable_sd_log)
				driver->enable_sd_log(1);
			DIAG_INFO("diag: USB disconnected\n");
			diagfwd_disconnect();
			DIAG_INFO("sdlogging enable\n");
#if 0 /* defined(CONFIG_ARCH_MSM8X60_LTE) */
			if (mdm_diag) {
				driver->in_busy_sdio = 0;
				buf_9k = kzalloc(USB_MAX_OUT_BUF, GFP_KERNEL);
				if (driver->sdio_ch)
					queue_work(driver->diag_sdio_wq, &(driver->diag_read_sdio_work));
			} else
#endif
			{
				driver->in_busy_1 = 0;
				driver->in_busy_2 = 0;
				driver->in_busy_qdsp_2 = 0;
				driver->in_busy_qdsp_2 = 0;
				/* Poll SMD channels to check for data*/
				if (driver->ch)
					queue_work(driver->diag_wq,
						 &(driver->diag_read_smd_work));
				if (driver->chqdsp)
					queue_work(driver->diag_wq,
						&(driver->diag_read_smd_qdsp_work));
			}
		} else if (temp == MEMORY_DEVICE_MODE && driver->logging_mode
								== USB_MODE) {
			DIAG_INFO("sdlogging disable\n");
			if (driver->enable_sd_log)
				driver->enable_sd_log(0);
			diagfwd_connect();

				}
#endif /* DIAG over USB */
		success = 1;
	}

	return success;
}

static int diagchar_read(struct file *file, char __user *buf, size_t count,
			  loff_t *ppos)
{
	int index = -1, i = 0, ret = 0;
	int num_data = 0, data_type;

#ifdef SDQXDM_DEBUG
	struct timeval t;
#endif

/*
	printk(KERN_INFO "%s:%s(parent:%s): tgid=%d\n", __func__,
	current->comm, current->parent->comm, current->tgid);
*/
	for (i = 0; i < driver->num_clients; i++)
		if (driver->client_map[i].pid == current->tgid)
			index = i;

	if (index == -1) {
		DIAG_ERR("\n Client PID not found in table");
		return -EINVAL;
	}

	wait_event_interruptible(driver->wait_q,
				  driver->data_ready[index]);
	mutex_lock(&driver->diagchar_mutex);

	if ((driver->data_ready[index] & MEMORY_DEVICE_LOG_TYPE) && (driver->
					logging_mode == MEMORY_DEVICE_MODE)) {

		/*Copy the type of data being passed*/
		data_type = driver->data_ready[index] & MEMORY_DEVICE_LOG_TYPE;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);
		/* place holder for number of data field */
		ret += 4;

		for (i = 0; i < driver->poolsize_write_struct; i++) {
			if (driver->buf_tbl[i].length > 0) {
#ifdef DIAG_DEBUG
				printk(KERN_INFO "\n WRITING the buf address "
				       "and length is %x , %d\n", (unsigned int)
					(driver->buf_tbl[i].buf),
					driver->buf_tbl[i].length);
#endif
				num_data++;
				/* Copy the length of data being passed */
				if (copy_to_user(buf+ret, (void *)&(driver->
						buf_tbl[i].length), 4)) {
						num_data--;
						goto drop;
				}
				ret += 4;

				/* Copy the actual data being passed */
				if (copy_to_user(buf+ret, (void *)driver->
				buf_tbl[i].buf, driver->buf_tbl[i].length)) {
					ret -= 4;
					num_data--;
					goto drop;
				}
				ret += driver->buf_tbl[i].length;
drop:
#ifdef DIAG_DEBUG
				printk(KERN_INFO "\n DEQUEUE buf address and"
				       " length is %x,%d\n", (unsigned int)
				       (driver->buf_tbl[i].buf), driver->
				       buf_tbl[i].length);
#endif
				diagmem_free(driver, (unsigned char *)
				(driver->buf_tbl[i].buf), POOL_TYPE_HDLC);
				driver->buf_tbl[i].length = 0;
				driver->buf_tbl[i].buf = 0;
			}
		}

		/* copy modem data */
		if (driver->in_busy_1 == 1) {
			num_data++;
			/*Copy the length of data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					 (driver->write_ptr_1->length), 4);
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					*(driver->buf_in_1),
					 driver->write_ptr_1->length);
			driver->in_busy_1 = 0;
		}
		if (driver->in_busy_2 == 1) {
			num_data++;
			/*Copy the length of data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					 (driver->write_ptr_2->length), 4);
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					 *(driver->buf_in_2),
					 driver->write_ptr_2->length);
			driver->in_busy_2 = 0;
		}

		/* copy q6 data */
		if (driver->in_busy_qdsp_1 == 1) {
			num_data++;
			/*Copy the length of data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
				 (driver->write_ptr_qdsp_1->length), 4);
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
							buf_in_qdsp_1),
					 driver->write_ptr_qdsp_1->length);
			driver->in_busy_qdsp_1 = 0;
		}
		if (driver->in_busy_qdsp_2 == 1) {
			num_data++;
			/*Copy the length of data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
				 (driver->write_ptr_qdsp_2->length), 4);
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
				buf_in_qdsp_2), driver->
					write_ptr_qdsp_2->length);
			driver->in_busy_qdsp_2 = 0;
		}
#if 0 /* def CONFIG_ARCH_MSM8X60_LTE */
		if (driver->in_busy_sdio == 1) {
			num_data++;
			/*Copy the length of data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
				 (driver->write_ptr_mdm->length), 4);
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
				buf_in_sdio), driver->
					write_ptr_mdm->length);
			driver->in_busy_sdio = 0;
		}
#endif
		/* copy number of data fields */
		COPY_USER_SPACE_OR_EXIT(buf+4, num_data, 4);
		ret -= 4;

		driver->data_ready[index] ^= MEMORY_DEVICE_LOG_TYPE;
#if 0 /* def CONFIG_ARCH_MSM8X60_LTE */
		if (mdm_diag) {
			if (driver->sdio_ch)
				queue_work(driver->diag_sdio_wq, &(driver->diag_read_sdio_work));
		}
#endif
		if (driver->ch)
			queue_work(driver->diag_wq,
					 &(driver->diag_read_smd_work));
		if (driver->chqdsp)
			queue_work(driver->diag_wq,
					 &(driver->diag_read_smd_qdsp_work));
		APPEND_DEBUG('n');
		goto exit;
	} else if (driver->data_ready[index] & MEMORY_DEVICE_LOG_TYPE) {
		/* In case, the thread wakes up and the logging mode is
		not memory device any more, the condition needs to be cleared */
		driver->data_ready[index] ^= MEMORY_DEVICE_LOG_TYPE;
	} else if (driver->data_ready[index] & USERMODE_DIAGFWD) {
		data_type = USERMODE_DIAGFWD;
		driver->data_ready[index] ^= USERMODE_DIAGFWD;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);

#ifdef SDQXDM_DEBUG
		do_gettimeofday(&t);

		if (driver->in_busy_1 && t.tv_sec > driver->write_ptr_1->second + 2)
			pr_info("[diag-dbg] late pkt now: %ld.%04ld pkt: %d\n", t.tv_sec, t.tv_usec/1000, driver->write_ptr_1->second);
		if (driver->in_busy_2 && t.tv_sec > driver->write_ptr_2->second + 2)
			pr_info("[diag-dbg] late pkt now: %ld.%04ld pkt: %d\n", t.tv_sec, t.tv_usec/1000, driver->write_ptr_2->second);
#endif
		/* copy modem data */
		if (driver->in_busy_1 == 1) {
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					*(driver->buf_in_1),
					 driver->write_ptr_1->length);
			driver->in_busy_1 = 0;
		}
		if (driver->in_busy_2 == 1) {
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					 *(driver->buf_in_2),
					 driver->write_ptr_2->length);
			driver->in_busy_2 = 0;
		}

		/* copy q6 data */
		if (driver->in_busy_qdsp_1 == 1) {
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
							buf_in_qdsp_1),
					 driver->write_ptr_qdsp_1->length);
			driver->in_busy_qdsp_1 = 0;
		}
		if (driver->in_busy_qdsp_2 == 1) {
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
				buf_in_qdsp_2), driver->
					write_ptr_qdsp_2->length);
			driver->in_busy_qdsp_2 = 0;
		}
#if 0 /* def CONFIG_ARCH_MSM8X60_LTE */
		if (driver->in_busy_sdio == 1) {
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
				buf_in_sdio), driver->
					write_ptr_mdm->length);
			driver->in_busy_sdio = 0;
		}
		if (mdm_diag) {
			if (driver->sdio_ch)
				queue_work(driver->diag_sdio_wq, &(driver->diag_read_sdio_work));
		}
#endif
		if (driver->ch)
			queue_work(driver->diag_wq,
					 &(driver->diag_read_smd_work));
		if (driver->chqdsp)
			queue_work(driver->diag_wq,
					 &(driver->diag_read_smd_qdsp_work));
#ifdef DIAG_DEBUG
		pr_info("%s() return %d byte\n", __func__, ret);
#endif

		goto exit;
	}

	if (driver->data_ready[index] & DEINIT_TYPE) {
		/*Copy the type of data being passed*/
		data_type = driver->data_ready[index] & DEINIT_TYPE;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);
		driver->data_ready[index] ^= DEINIT_TYPE;
		goto exit;
	}

	if (driver->data_ready[index] & MSG_MASKS_TYPE) {
		/*Copy the type of data being passed*/
		data_type = driver->data_ready[index] & MSG_MASKS_TYPE;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);
		COPY_USER_SPACE_OR_EXIT(buf+4, *(driver->msg_masks),
							 MSG_MASK_SIZE);
		driver->data_ready[index] ^= MSG_MASKS_TYPE;
		goto exit;
	}

	if (driver->data_ready[index] & EVENT_MASKS_TYPE) {
		/*Copy the type of data being passed*/
		data_type = driver->data_ready[index] & EVENT_MASKS_TYPE;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);
		COPY_USER_SPACE_OR_EXIT(buf+4, *(driver->event_masks),
							 EVENT_MASK_SIZE);
		driver->data_ready[index] ^= EVENT_MASKS_TYPE;
		goto exit;
	}

	if (driver->data_ready[index] & LOG_MASKS_TYPE) {
		/*Copy the type of data being passed*/
		data_type = driver->data_ready[index] & LOG_MASKS_TYPE;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);
		COPY_USER_SPACE_OR_EXIT(buf+4, *(driver->log_masks),
							 LOG_MASK_SIZE);
		driver->data_ready[index] ^= LOG_MASKS_TYPE;
		goto exit;
	}

	if (driver->data_ready[index] & PKT_TYPE) {
		/*Copy the type of data being passed*/
		data_type = driver->data_ready[index] & PKT_TYPE;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);
		COPY_USER_SPACE_OR_EXIT(buf+4, *(driver->pkt_buf),
							 driver->pkt_length);
		driver->data_ready[index] ^= PKT_TYPE;
		goto exit;
	}

exit:
	if (ret)
		wake_lock_timeout(&driver->wake_lock, HZ / 2);

	mutex_unlock(&driver->diagchar_mutex);
	return ret;
}

static int diagchar_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	int err, ret = 0, pkt_type;
#ifdef DIAG_DEBUG
	int length = 0, i;
#endif
	struct diag_send_desc_type send = { NULL, NULL, DIAG_STATE_START, 0 };
	struct diag_hdlc_dest_type enc = { NULL, NULL, 0 };
	void *buf_copy = NULL;
	int payload_size;
#ifdef CONFIG_DIAG_OVER_USB
	if (((driver->logging_mode == USB_MODE) && (!driver->usb_connected)) ||
				(driver->logging_mode == NO_LOGGING_MODE)) {
		/*Drop the diag payload */
		return -EIO;
	}
#endif /* DIAG over USB */
	/* Get the packet type F3/log/event/Pkt response */
	err = copy_from_user((&pkt_type), buf, 4);
	/*First 4 bytes indicate the type of payload - ignore these */
	payload_size = count - 4;

	if (pkt_type == MEMORY_DEVICE_LOG_TYPE) {
		if (!mask_request_validate((unsigned char *)buf)) {
			DIAG_ERR("mask request Invalid ..cannot send to modem \n");
			return -EFAULT;
		}
		buf = buf + 4;
#ifdef DIAG_DEBUG
		printk(KERN_INFO "\n I got the masks: %d\n", payload_size);
		for (i = 0; i < payload_size; i++)
			printk(KERN_DEBUG "\t %x", *(((unsigned char *)buf)+i));
#endif
		diag_process_hdlc((void *)buf, payload_size);
		return 0;
	} else if(pkt_type == USERMODE_DIAGFWD) {
#ifdef DIAG_DEBUG
		pr_info("%s#%d recv %d bytes\n", __func__, __LINE__, payload_size);
#endif
		buf += 4;
		diag_process_hdlc((void *)buf, payload_size);
		return count;
	}
	buf_copy = diagmem_alloc(driver, payload_size, POOL_TYPE_COPY);
	if (!buf_copy) {
		driver->dropped_count++;
		return -ENOMEM;
	}

	err = copy_from_user(buf_copy, buf + 4, payload_size);
	if (err) {
		DIAG_ERR("diagchar : copy_from_user failed\n");
		ret = -EFAULT;
		goto fail_free_copy;
	}
#ifdef DIAG_DEBUG
	printk(KERN_DEBUG "data is -->\n");
	for (i = 0; i < payload_size; i++)
		printk(KERN_DEBUG "\t %x \t", *(((unsigned char *)buf_copy)+i));
#endif
	send.state = DIAG_STATE_START;
	send.pkt = buf_copy;
	send.last = (void *)(buf_copy + payload_size - 1);
	send.terminate = 1;
#ifdef DIAG_DEBUG
	printk(KERN_INFO "\n Already used bytes in buffer %d, and"
	" incoming payload size is %d\n", driver->used, payload_size);
	printk(KERN_DEBUG "hdlc encoded data is -->\n");
	for (i = 0; i < payload_size + 8; i++) {
		printk(KERN_DEBUG "\t %x \t", *(((unsigned char *)buf_hdlc)+i));
		if (*(((unsigned char *)buf_hdlc)+i) != 0x7e)
			length++;
	}
#endif
	mutex_lock(&driver->diagchar_mutex);
	if (!buf_hdlc)
		buf_hdlc = diagmem_alloc(driver, HDLC_OUT_BUF_SIZE,
						 POOL_TYPE_HDLC);
	if (!buf_hdlc) {
		ret = -ENOMEM;
		goto fail_free_hdlc;
	}
	if (HDLC_OUT_BUF_SIZE - driver->used <= (2*payload_size) + 3) {
		err = diag_device_write(buf_hdlc, APPS_DATA, NULL);
		if (err) {
			/*Free the buffer right away if write failed */
			diagmem_free(driver, buf_hdlc, POOL_TYPE_HDLC);
			diagmem_free(driver, (unsigned char *)driver->
				 write_ptr_svc, POOL_TYPE_WRITE_STRUCT);
			ret = -EIO;
			goto fail_free_hdlc;
		}
		buf_hdlc = NULL;
#ifdef DIAG_DEBUG
		printk(KERN_INFO "\n size written is %d\n", driver->used);
#endif
		driver->used = 0;
		buf_hdlc = diagmem_alloc(driver, HDLC_OUT_BUF_SIZE,
							 POOL_TYPE_HDLC);
		if (!buf_hdlc) {
			ret = -ENOMEM;
			goto fail_free_hdlc;
		}
	}

	enc.dest = buf_hdlc + driver->used;
	enc.dest_last = (void *)(buf_hdlc + driver->used + 2*payload_size + 3);
	diag_hdlc_encode(&send, &enc);

	/* This is to check if after HDLC encoding, we are still within the
	 limits of aggregation buffer. If not, we write out the current buffer
	and start aggregation in a newly allocated buffer */
	if ((unsigned int) enc.dest >=
		 (unsigned int)(buf_hdlc + HDLC_OUT_BUF_SIZE)) {
		err = diag_device_write(buf_hdlc, APPS_DATA, NULL);
		if (err) {
			/*Free the buffer right away if write failed */
			diagmem_free(driver, buf_hdlc, POOL_TYPE_HDLC);
			diagmem_free(driver, (unsigned char *)driver->
				 write_ptr_svc, POOL_TYPE_WRITE_STRUCT);
			ret = -EIO;
			goto fail_free_hdlc;
		}
		buf_hdlc = NULL;
#ifdef DIAG_DEBUG
		printk(KERN_INFO "\n size written is %d\n", driver->used);
#endif
		driver->used = 0;
		buf_hdlc = diagmem_alloc(driver, HDLC_OUT_BUF_SIZE,
							 POOL_TYPE_HDLC);
		if (!buf_hdlc) {
			ret = -ENOMEM;
			goto fail_free_hdlc;
		}
		enc.dest = buf_hdlc + driver->used;
		enc.dest_last = (void *)(buf_hdlc + driver->used +
							 (2*payload_size) + 3);
		diag_hdlc_encode(&send, &enc);
	}

	driver->used = (uint32_t) enc.dest - (uint32_t) buf_hdlc;
	if (pkt_type == DATA_TYPE_RESPONSE) {
		err = diag_device_write(buf_hdlc, APPS_DATA, NULL);
		if (err) {
			/*Free the buffer right away if write failed */
			diagmem_free(driver, buf_hdlc, POOL_TYPE_HDLC);
			diagmem_free(driver, (unsigned char *)driver->
				 write_ptr_svc, POOL_TYPE_WRITE_STRUCT);
			ret = -EIO;
			goto fail_free_hdlc;
		}
		buf_hdlc = NULL;
#ifdef DIAG_DEBUG
		printk(KERN_INFO "\n size written is %d\n", driver->used);
#endif
		driver->used = 0;
	}

	mutex_unlock(&driver->diagchar_mutex);
	diagmem_free(driver, buf_copy, POOL_TYPE_COPY);
	if (!timer_in_progress)	{
		timer_in_progress = 1;
		ret = mod_timer(&drain_timer, jiffies + msecs_to_jiffies(500));
	}
	return 0;

fail_free_hdlc:
	buf_hdlc = NULL;
	driver->used = 0;
	diagmem_free(driver, buf_copy, POOL_TYPE_COPY);
	mutex_unlock(&driver->diagchar_mutex);
	return ret;

fail_free_copy:
	diagmem_free(driver, buf_copy, POOL_TYPE_COPY);
	return ret;
}

int mask_request_validate(unsigned char mask_buf[])
{
	uint8_t packet_id;
	uint8_t subsys_id;
	uint16_t ss_cmd;

	packet_id = mask_buf[4];

	if (packet_id == 0x4B) {
		subsys_id = mask_buf[5];
		ss_cmd = *(uint16_t *)(mask_buf + 6);
		/* Packets with SSID which are allowed */
		switch (subsys_id) {
		case 0x04: /* DIAG_SUBSYS_WCDMA */
			if ((ss_cmd == 0) || (ss_cmd == 0xF))
			return 1;
			break;
		case 0x08: /* DIAG_SUBSYS_GSM */
			if ((ss_cmd == 0) || (ss_cmd == 0x1))
				return 1;
			break;
		case 0x09: /* DIAG_SUBSYS_UMTS */
		case 0x0F: /* DIAG_SUBSYS_CM */
			if (ss_cmd == 0)
				return 1;
			break;
		case 0x0C: /* DIAG_SUBSYS_OS */
			if ((ss_cmd == 2) || (ss_cmd == 0x100))
				return 1; /* MPU and APU */
			break;
		case 0x12: /* DIAG_SUBSYS_DIAG_SERV */
			if ((ss_cmd == 0) || (ss_cmd == 0x6) || (ss_cmd == 0x7))
				return 1;
			break;
		case 0x13: /* DIAG_SUBSYS_FS */
			if ((ss_cmd == 0) || (ss_cmd == 0x1))
				return 1;
			break;
		default:
				return 0;
			break;
		}
	} else {
		switch (packet_id) {
		case 0x00:    /* Version Number */
		case 0x0C:    /* CDMA status packet */
		case 0x1C:    /* Diag Version */
		case 0x1D:    /* Time Stamp */
		case 0x60:    /* Event Report Control */
		case 0x63:    /* Status snapshot */
		case 0x73:    /* Logging Configuration */
		case 0x7C:    /* Extended build ID */
		case 0x7D:    /* Extended Message configuration */
		case 0x81:    /* Event get mask */
		case 0x82:    /* Set the event mask */
			return 1;
			break;
		default:
				return 0;
			break;
		}
		}
		return 0;
}

static const struct file_operations diagcharfops = {
	.owner = THIS_MODULE,
	.read = diagchar_read,
	.write = diagchar_write,
	.ioctl = diagchar_ioctl,
	.open = diagchar_open,
	.release = diagchar_close
};
#if defined(CONFIG_MACH_MECHA) || defined(CONFIG_ARCH_MSM8X60_LTE)

static int diagcharmdm_open(struct inode *inode, struct file *file)
{
	int i = 0;
#if defined(CONFIG_MACH_MECHA)
	if (!sdio_diag_initialized) {
		DIAG_INFO("sdio diag isn't in embedded mode \n");
		return 0;
	}
#endif

	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
	current->comm, current->parent->comm, current->tgid);

	if (driver) {
		mutex_lock(&driver->diagcharmdm_mutex);

		for (i = 0; i < driver->num_mdmclients; i++)
			if (driver->mdmclient_map[i].pid == 0)
				break;

		if (i < driver->num_mdmclients) {
			driver->mdmclient_map[i].pid = current->tgid;
			strncpy(driver->mdmclient_map[i].name, current->comm, 20);
			driver->mdmclient_map[i].name[19] = '\0';
		} else {
			DIAG_INFO("%s:reach max client count\n", __func__);
		}

		driver->mdmdata_ready[i] |= MSG_MASKS_TYPE;
		driver->mdmdata_ready[i] |= EVENT_MASKS_TYPE;
		driver->mdmdata_ready[i] |= LOG_MASKS_TYPE;

#if defined(CONFIG_ARCH_MSM8X60_LTE)
		if (driver->ref_count == 0)
			diagmem_init(driver);
		driver->ref_count++;
#endif
		mutex_unlock(&driver->diagcharmdm_mutex);
		return 0;
	}

	return -ENOMEM;

}

static int diagcharmdm_close(struct inode *inode, struct file *file)
{

	int i = 0;
#if defined(CONFIG_MACH_MECHA)
	if (!sdio_diag_initialized) {
		DIAG_INFO("sdio diag isn't in embedded mode \n");
		return 0;
	}
#endif

	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
	current->comm, current->parent->comm, current->tgid);

	if (driver) {
		mutex_lock(&driver->diagcharmdm_mutex);

#if defined(CONFIG_ARCH_MSM8X60_LTE)
		driver->ref_count--;
				/* On Client exit, try to destroy all 3 pools */
				diagmem_exit(driver, POOL_TYPE_COPY);
				diagmem_exit(driver, POOL_TYPE_HDLC);
				diagmem_exit(driver, POOL_TYPE_WRITE_STRUCT);

#endif

		for (i = 0; i < driver->num_mdmclients; i++)
			if (driver->mdmclient_map[i].pid == current->tgid) {
				driver->mdmclient_map[i].pid = 0;
				break;
			}
		mutex_unlock(&driver->diagcharmdm_mutex);
		return 0;
	}
	return -ENOMEM;


}

static int diagcharmdm_ioctl(struct inode *inode, struct file *filp,
			   unsigned int iocmd, unsigned long ioarg)
{
	int success = -1;

#if defined(CONFIG_MACH_MECHA)
	if (!sdio_diag_initialized) {
		DIAG_INFO("sdio diag isn't in embedded mode \n");
		return 0;
	}
#endif

	if (iocmd == DIAG_IOCTL_SWITCH_LOGGING) {
		mutex_lock(&driver->diagcharmdm_mutex);
		driver->logging_mode = (int)ioarg;
		driver->logging_process_id = current->tgid;
		mutex_unlock(&driver->diagcharmdm_mutex);
		if (driver->logging_mode == MEMORY_DEVICE_MODE) {
			DIAG_INFO("diagcharmdm_ioctl enable\n");
			diagfwd_disconnect();
			driver->qxdm2sd_drop = 0;
#if defined(CONFIG_MACH_MECHA)
			driver->in_busy_mdm_1 = 0;
			driver->in_busy_mdm_2 = 0;
			/* Poll SMD channels to check for data*/
			queue_work(driver->mdm_diag_workqueue, &(driver->diag_read_smd_mdm_work));
#endif
#if defined(CONFIG_ARCH_MSM8X60_LTE)
				driver->in_busy_sdio_1 = 0;
				driver->in_busy_sdio_2 = 0;
				buf_9k = kzalloc(USB_MAX_OUT_BUF, GFP_KERNEL);
				if (driver->sdio_ch)
					queue_work(driver->diag_sdio_wq, &(driver->diag_read_sdio_work));
#endif

		} else if (driver->logging_mode == USB_MODE) {
			DIAG_INFO("diagcharmdm_ioctl disable\n");
			diagfwd_connect();
			driver->qxdm2sd_drop = 1;
#if defined(CONFIG_MACH_MECHA)
			driver->in_busy_mdm_1 = 1;
			driver->in_busy_mdm_2 = 1;
#endif
#if defined(CONFIG_ARCH_MSM8X60_LTE)
			kfree(buf_9k);
#endif

				}
		success = 1;
	}

	return success;


	return 0;

}
static int diagcharmdm_read(struct file *file, char __user *buf, size_t count,
			  loff_t *ppos)
{

	int index = -1, i = 0, ret = 0;
	int num_data = 0, data_type;
#if defined(CONFIG_MACH_MECHA)

	if (!sdio_diag_initialized) {
		DIAG_INFO("sdio diag isn't in embedded mode \n");
		return 0;
	}
#endif
#ifdef DIAG_DEBUG
	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
	current->comm, current->parent->comm, current->tgid);
#endif
	for (i = 0; i < driver->num_mdmclients; i++)
		if (driver->mdmclient_map[i].pid == current->tgid)
			index = i;

	if (index == -1) {
		DIAG_ERR("\n Client PID not found in table");
		return -EINVAL;
	}

	wait_event_interruptible(driver->mdmwait_q,
				  driver->mdmdata_ready[index]);

	mutex_lock(&driver->diagcharmdm_mutex);

	if ((driver->mdmdata_ready[index] & MEMORY_DEVICE_LOG_TYPE) && (driver->
					logging_mode == MEMORY_DEVICE_MODE)) {
		/*Copy the type of data being passed*/
		data_type = driver->data_ready[index] & MEMORY_DEVICE_LOG_TYPE;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);
		/* place holder for number of data field */
		ret += 4;
#if defined(CONFIG_MACH_MECHA)
		/* copy modem data */
		if (driver->in_busy_mdm_1 == 1) {
			num_data++;
			/*Copy the length of data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					 (driver->write_ptr_mdm_1->length), 4);

			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					*(driver->buf_in_mdm_1),
					 driver->write_ptr_mdm_1->length);
			driver->in_busy_mdm_1 = 0;
		}
		if (driver->in_busy_mdm_2 == 1) {
			num_data++;
			/*Copy the length of data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					 (driver->write_ptr_mdm_2->length), 4);
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
					 *(driver->buf_in_mdm_2),
					 driver->write_ptr_mdm_2->length);
			driver->in_busy_mdm_2 = 0;
		}
#endif
#ifdef CONFIG_ARCH_MSM8X60_LTE
		if (driver->in_busy_sdio_1 == 1) {

			num_data++;
			/*Copy the length of data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
				 (driver->write_ptr_mdm_1->length), 4);
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
				buf_in_sdio_1), driver->
					write_ptr_mdm_1->length);
			driver->in_busy_sdio_1 = 0;
		}
		if (driver->in_busy_sdio_2 == 1) {

			num_data++;
			/*Copy the length of data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret,
				 (driver->write_ptr_mdm_2->length), 4);
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
				buf_in_sdio_2), driver->
					write_ptr_mdm_2->length);
			driver->in_busy_sdio_2 = 0;
		}
#endif
		/* copy number of data fields */
		COPY_USER_SPACE_OR_EXIT(buf+4, num_data, 4);
		ret -= 4;

		driver->mdmdata_ready[index] ^= MEMORY_DEVICE_LOG_TYPE;
#ifdef CONFIG_MACH_MECHA
		queue_work(driver->mdm_diag_workqueue, &(driver->diag_read_smd_mdm_work));
#endif
#ifdef CONFIG_ARCH_MSM8X60_LTE
			if (driver->sdio_ch)
			queue_work(driver->diag_sdio_wq, &(driver->diag_read_sdio_work));
#endif
		goto exit;
	} else if (driver->mdmdata_ready[index] & MEMORY_DEVICE_LOG_TYPE) {
		/* In case, the thread wakes up and the logging mode is
		not memory device any more, the condition needs to be cleared */
		driver->mdmdata_ready[index] ^= MEMORY_DEVICE_LOG_TYPE;
	}
#ifdef CONFIG_ARCH_MSM8X60_LTE
	else if (driver->mdmdata_ready[index] & USERMODE_DIAGFWD) {
		data_type = USERMODE_DIAGFWD;
		driver->mdmdata_ready[index] ^= USERMODE_DIAGFWD;
		COPY_USER_SPACE_OR_EXIT(buf, data_type, 4);

		if (driver->in_busy_sdio_1 == 1) {
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
				buf_in_sdio_1), driver->
					write_ptr_mdm_1->length);
			driver->in_busy_sdio_1 = 0;
		}
		if (driver->in_busy_sdio_2 == 1) {
			/*Copy the actual data being passed*/
			COPY_USER_SPACE_OR_EXIT(buf+ret, *(driver->
				buf_in_sdio_2), driver->
					write_ptr_mdm_2->length);
			driver->in_busy_sdio_2 = 0;
		}
		if (driver->sdio_ch)
		queue_work(driver->diag_sdio_wq, &(driver->diag_read_sdio_work));
		goto exit;
	}
#endif
	if (driver->mdmdata_ready[index] & DEINIT_TYPE) {

		driver->mdmdata_ready[index] ^= DEINIT_TYPE;
		goto exit;
	}

	if (driver->mdmdata_ready[index] & MSG_MASKS_TYPE) {

		driver->mdmdata_ready[index] ^= MSG_MASKS_TYPE;
		goto exit;
	}

	if (driver->mdmdata_ready[index] & EVENT_MASKS_TYPE) {

		driver->mdmdata_ready[index] ^= EVENT_MASKS_TYPE;
		goto exit;
	}

	if (driver->mdmdata_ready[index] & LOG_MASKS_TYPE) {

		driver->mdmdata_ready[index] ^= LOG_MASKS_TYPE;
		goto exit;
	}

	if (driver->mdmdata_ready[index] & PKT_TYPE) {

		driver->mdmdata_ready[index] ^= PKT_TYPE;
		goto exit;
	}
exit:
	mutex_unlock(&driver->diagcharmdm_mutex);

	return ret;

}

static int diagcharmdm_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{

	int err, pkt_type;
	int payload_size;
#if defined(CONFIG_MACH_MECHA)
	int ret = 0;
	unsigned char *tmp_buf = NULL;
#endif
#ifdef DIAG_DEBUG
	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
	current->comm, current->parent->comm, current->tgid);
#endif
#if defined(CONFIG_MACH_MECHA)
	if (!sdio_diag_initialized) {
		DIAG_INFO("sdio diag isn't in embedded mode \n");
		return 0;
	}

	if (driver->logging_mode == MEMORY_DEVICE_MODE) {

		/* Get the packet type F3/log/event/Pkt response */
		err = copy_from_user((&pkt_type), buf, 4);
		/*First 4 bytes indicate the type of payload - ignore these */
		payload_size = count - 4;

		if (pkt_type == MEMORY_DEVICE_LOG_TYPE) {
			if (!mask_request_validate((unsigned char *)buf)) {
				DIAG_ERR("mask request Invalid ..cannot send to modem \n");
				return -EFAULT;
			}

			buf = buf + 4;
			tmp_buf = kzalloc(IN_BUF_SIZE, GFP_KERNEL);
			memcpy(tmp_buf, buf, payload_size);
			msm_sdio_diag_write((void *)tmp_buf, payload_size);
			tmp_buf = NULL;
			return 0;
		}
	}
	return ret;
#else

#ifdef CONFIG_DIAG_OVER_USB
	if (((driver->logging_mode == USB_MODE) && (!driver->usb_connected)) ||
				(driver->logging_mode == NO_LOGGING_MODE)) {
		/*Drop the diag payload */
		return -EIO;
	}
#endif /* DIAG over USB */

	/* Get the packet type F3/log/event/Pkt response */
	err = copy_from_user((&pkt_type), buf, 4);
	/*First 4 bytes indicate the type of payload - ignore these */
	payload_size = count - 4;
	if (pkt_type == MEMORY_DEVICE_LOG_TYPE) {
			DIAGFWD_INFO("writing mask file\n");
		if (!mask_request_validate((unsigned char *)buf)) {
			DIAG_ERR("mask request Invalid ..cannot send to modem \n");
			return -EFAULT;
		}
		buf = buf + 4;
			if (driver->sdio_ch) {
				memcpy(buf_9k, buf, payload_size);
				sdio_write(driver->sdio_ch, buf_9k, payload_size);
			}
		return count;
	} else if(pkt_type == USERMODE_DIAGFWD) {

		buf += 4;

			if (driver->sdio_ch) {
				memcpy(buf_9k, buf, payload_size);
				sdio_write(driver->sdio_ch, buf_9k,payload_size);
			}

		return count;
	}
	return 0;
#endif
}


static const struct file_operations diagcharmdmfops = {
	.owner = THIS_MODULE,
	.read = diagcharmdm_read,
	.write = diagcharmdm_write,
	.ioctl = diagcharmdm_ioctl,
	.open = diagcharmdm_open,
	.release = diagcharmdm_close
};
#endif
static int diagchar_setup_cdev(dev_t devno)
{
	int err;
	dev_t tmp_devno;
	struct device	*diagdev;


	tmp_devno = MKDEV(driver->major, driver->minor_start);

	cdev_init(driver->cdev, &diagcharfops);

	driver->cdev->owner = THIS_MODULE;
	driver->cdev->ops = &diagcharfops;

	err = cdev_add(driver->cdev, tmp_devno, 1);

	if (err) {
		DIAG_ERR("diagchar cdev registration failed !\n\n");
		return -1;
	}

	driver->diagchar_class = class_create(THIS_MODULE, "diag");

	if (IS_ERR(driver->diagchar_class)) {
		DIAG_ERR("Error creating diagchar class.\n");
		return -1;
	}

	diagdev = device_create(driver->diagchar_class, NULL, tmp_devno,
				  (void *)driver, "diag");


	err = 	device_create_file(diagdev, &dev_attr_diag_trace);
	if (err)
		DIAG_INFO("dev_attr_diag_trace registration failed !\n\n");

#if defined(CONFIG_MACH_MECHA) || defined(CONFIG_ARCH_MSM8X60_LTE)
	tmp_devno = MKDEV(driver->major, driver->minor_start+1);

	cdev_init(driver->cdev_mdm, &diagcharmdmfops);

	driver->cdev_mdm->owner = THIS_MODULE;
	driver->cdev_mdm->ops = &diagcharmdmfops;

	err = cdev_add(driver->cdev_mdm, tmp_devno, 1);

	if (err) {
		DIAG_ERR("diagchar cdev mdm registration failed !\n\n");
		return -1;
	}

	device_create(driver->diagchar_class, NULL, tmp_devno,
				  (void *)driver, "diag_mdm");
#endif
	return 0;

}

static int diagchar_cleanup(void)
{
	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
	current->comm, current->parent->comm, current->tgid);
	if (driver) {
		if (driver->cdev) {
			/* TODO - Check if device exists before deleting */
			device_destroy(driver->diagchar_class,
				       MKDEV(driver->major,
					     driver->minor_start));
			cdev_del(driver->cdev);
		}
		if (!IS_ERR(driver->diagchar_class))
			class_destroy(driver->diagchar_class);
		wake_lock_destroy(&driver->wake_lock);
		kfree(driver);
	}
	return 0;
}

static int __init diagchar_init(void)
{
	dev_t dev;
	int error;

	DIAG_INFO("%s:%s(parent:%s): tgid=%d\n", __func__,
	current->comm, current->parent->comm, current->tgid);
	driver = kzalloc(sizeof(struct diagchar_dev) + 5, GFP_KERNEL);

	if (driver) {
		driver->enable_sd_log = (void *)0;
		driver->used = 0;
		timer_in_progress = 0;
		driver->debug_flag = 1;
		setup_timer(&drain_timer, drain_timer_func, 1234);
		driver->itemsize = itemsize;
		driver->poolsize = poolsize;
		driver->itemsize_hdlc = itemsize_hdlc;
		driver->poolsize_hdlc = poolsize_hdlc;
		driver->itemsize_write_struct = itemsize_write_struct;
		driver->poolsize_write_struct = poolsize_write_struct;
		driver->num_clients = max_clients;
		driver->logging_mode = USB_MODE;
		mutex_init(&driver->diagchar_mutex);
		init_waitqueue_head(&driver->wait_q);
		wake_lock_init(&driver->wake_lock, WAKE_LOCK_SUSPEND, "diagchar");

		INIT_WORK(&(driver->diag_drain_work), diag_drain_work_fn);
		INIT_WORK(&(driver->diag_read_smd_work), diag_read_smd_work_fn);
		INIT_WORK(&(driver->diag_read_smd_qdsp_work),
			   diag_read_smd_qdsp_work_fn);

		DIAG_INFO("diagchar initializing ..\n");
#if defined(CONFIG_MACH_MECHA)
		INIT_WORK(&(driver->diag_read_smd_mdm_work),
			   sdio_diag_read_data);
		driver->in_busy_mdm_1 = 0;
		driver->in_busy_mdm_2 = 0;
#endif

#if defined(CONFIG_MACH_MECHA) || defined(CONFIG_ARCH_MSM8X60_LTE)
		driver->num_mdmclients = 1;
		init_waitqueue_head(&driver->mdmwait_q);
		spin_lock_init(&driver->diagchar_lock);
		mutex_init(&driver->diagcharmdm_mutex);

		driver->num = 2;
#else
		driver->num = 1;
#endif
		mdm_diag = diagfwd_init();
		driver->name = ((void *)driver) + sizeof(struct diagchar_dev);
		strlcpy(driver->name, "diag", 4);

		/* Get major number from kernel and initialize */
		error = alloc_chrdev_region(&dev, driver->minor_start,
					    driver->num, driver->name);
		if (!error) {
			driver->major = MAJOR(dev);
			driver->minor_start = MINOR(dev);
		} else {
			DIAG_ERR("Major number not allocated\n");
			goto fail;
		}
		driver->cdev = cdev_alloc();

#if defined(CONFIG_MACH_MECHA) || defined(CONFIG_ARCH_MSM8X60_LTE)
		driver->cdev_mdm = cdev_alloc();
#endif
		error = diagchar_setup_cdev(dev);
		if (error)
			goto fail;
	} else {
		DIAG_ERR("kzalloc failed\n");
		goto fail;
	}

	DIAG_INFO("diagchar initialized\n");
	return 0;

fail:
	diagchar_cleanup();
	diagfwd_exit();
	return -1;

}

static void __exit diagchar_exit(void)
{
	DIAG_INFO("diagchar exiting ..\n");
	/* On Driver exit, send special pool type to
	 ensure no memory leaks */
	diagmem_exit(driver, POOL_TYPE_ALL);
	diagfwd_exit();
#if defined(CONFIG_ARCH_MSM8X60_LTE)
	diagfwd_sdio_exit();
#endif
	diagchar_cleanup();
	DIAG_INFO("done diagchar exit\n");
}

module_init(diagchar_init);
module_exit(diagchar_exit);