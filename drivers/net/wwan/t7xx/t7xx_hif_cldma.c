// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, MediaTek Inc.
 * Copyright (c) 2021, Intel Corporation.
 *
 * Authors:
 *  Amir Hanania <amir.hanania@intel.com>
 *  Haijun Liu <haijun.liu@mediatek.com>
 *  Moises Veleta <moises.veleta@intel.com>
 *  Ricardo Martinez<ricardo.martinez@linux.intel.com>
 *  Sreehari Kancharla <sreehari.kancharla@intel.com>
 *
 * Contributors:
 *  Andy Shevchenko <andriy.shevchenko@linux.intel.com>
 *  Chiranjeevi Rapolu <chiranjeevi.rapolu@intel.com>
 *  Eliot Lee <eliot.lee@intel.com>
 */

#include <linux/bits.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direction.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/iopoll.h>
#include <linux/irqreturn.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "t7xx_cldma.h"
#include "t7xx_common.h"
#include "t7xx_hif_cldma.h"
#include "t7xx_mhccif.h"
#include "t7xx_modem_ops.h"
#include "t7xx_pci.h"
#include "t7xx_pcie_mac.h"
#include "t7xx_reg.h"
#include "t7xx_state_monitor.h"

#define MAX_TX_BUDGET			16
#define MAX_RX_BUDGET			16

#define CHECK_Q_STOP_TIMEOUT_US		1000000
#define CHECK_Q_STOP_STEP_US		10000

static enum cldma_queue_type rxq_type[CLDMA_RXQ_NUM];
static enum cldma_queue_type txq_type[CLDMA_TXQ_NUM];
static int rxq_buff_size[CLDMA_RXQ_NUM];
static int txq_buff_size[CLDMA_TXQ_NUM];

static void md_cd_queue_struct_reset(struct cldma_queue *queue, struct cldma_ctrl *md_ctrl,
				     enum mtk_txrx tx_rx, unsigned char index)
{
	queue->dir = tx_rx;
	queue->index = index;
	queue->hif_id = md_ctrl->hif_id;
	queue->md_ctrl = md_ctrl;
	queue->tr_ring = NULL;
	queue->tr_done = NULL;
	queue->tx_xmit = NULL;
}

static void md_cd_queue_struct_init(struct cldma_queue *queue, struct cldma_ctrl *md_ctrl,
				    enum mtk_txrx tx_rx, unsigned char index)
{
	md_cd_queue_struct_reset(queue, md_ctrl, tx_rx, index);
	init_waitqueue_head(&queue->req_wq);
	spin_lock_init(&queue->ring_lock);
}

static void t7xx_cldma_tgpd_set_data_ptr(struct cldma_tgpd *tgpd, dma_addr_t data_ptr)
{
	tgpd->data_buff_bd_ptr_h = cpu_to_le32(upper_32_bits(data_ptr));
	tgpd->data_buff_bd_ptr_l = cpu_to_le32(lower_32_bits(data_ptr));
}

static void t7xx_cldma_tgpd_set_next_ptr(struct cldma_tgpd *tgpd, dma_addr_t next_ptr)
{
	tgpd->next_gpd_ptr_h = cpu_to_le32(upper_32_bits(next_ptr));
	tgpd->next_gpd_ptr_l = cpu_to_le32(lower_32_bits(next_ptr));
}

static void t7xx_cldma_rgpd_set_data_ptr(struct cldma_rgpd *rgpd, dma_addr_t data_ptr)
{
	rgpd->data_buff_bd_ptr_h = cpu_to_le32(upper_32_bits(data_ptr));
	rgpd->data_buff_bd_ptr_l = cpu_to_le32(lower_32_bits(data_ptr));
}

static void t7xx_cldma_rgpd_set_next_ptr(struct cldma_rgpd *rgpd, dma_addr_t next_ptr)
{
	rgpd->next_gpd_ptr_h = cpu_to_le32(upper_32_bits(next_ptr));
	rgpd->next_gpd_ptr_l = cpu_to_le32(lower_32_bits(next_ptr));
}

static struct cldma_request *t7xx_cldma_ring_step_forward(struct cldma_ring *ring,
							  struct cldma_request *req)
{
	if (req->entry.next == &ring->gpd_ring)
		return list_first_entry(&ring->gpd_ring, struct cldma_request, entry);

	return list_next_entry(req, entry);
}

static struct cldma_request *t7xx_cldma_ring_step_backward(struct cldma_ring *ring,
							   struct cldma_request *req)
{
	if (req->entry.prev == &ring->gpd_ring)
		return list_last_entry(&ring->gpd_ring, struct cldma_request, entry);

	return list_prev_entry(req, entry);
}

static int t7xx_cldma_alloc_and_map_skb(struct cldma_ctrl *md_ctrl, struct cldma_request *req,
					size_t size)
{
	req->skb = __dev_alloc_skb(size, GFP_KERNEL);
	if (!req->skb)
		return -ENOMEM;

	req->mapped_buff = dma_map_single(md_ctrl->dev, req->skb->data,
					  t7xx_skb_data_size(req->skb), DMA_FROM_DEVICE);
	if (dma_mapping_error(md_ctrl->dev, req->mapped_buff)) {
		dev_err(md_ctrl->dev, "DMA mapping failed\n");
		dev_kfree_skb_any(req->skb);
		req->skb = NULL;
		req->mapped_buff = 0;
		return -ENOMEM;
	}

	return 0;
}

static int t7xx_cldma_gpd_rx_from_queue(struct cldma_queue *queue, int budget, bool *over_budget)
{
	struct cldma_ctrl *md_ctrl = queue->md_ctrl;
	unsigned char hwo_polling_count = 0;
	struct t7xx_cldma_hw *hw_info;
	bool rx_not_done = true;
	int count = 0;

	hw_info = &md_ctrl->hw_info;

	do {
		struct cldma_request *req;
		struct cldma_rgpd *rgpd;
		struct sk_buff *skb;
		int ret;

		req = queue->tr_done;
		if (!req)
			return -ENODATA;

		rgpd = req->gpd;
		if ((rgpd->gpd_flags & GPD_FLAGS_HWO) || !req->skb) {
			dma_addr_t gpd_addr;

			if (!pci_device_is_present(to_pci_dev(md_ctrl->dev))) {
				dev_err(md_ctrl->dev, "PCIe Link disconnected\n");
				return -ENODEV;
			}

			gpd_addr = ioread64(hw_info->ap_pdn_base + REG_CLDMA_DL_CURRENT_ADDRL_0 +
					    queue->index * sizeof(u64));
			if (req->gpd_addr == gpd_addr || hwo_polling_count++ >= 100)
				return 0;

			udelay(1);
			continue;
		}

		hwo_polling_count = 0;
		skb = req->skb;

		if (req->mapped_buff) {
			dma_unmap_single(md_ctrl->dev, req->mapped_buff,
					 t7xx_skb_data_size(skb), DMA_FROM_DEVICE);
			req->mapped_buff = 0;
		}

		skb->len = 0;
		skb_reset_tail_pointer(skb);
		skb_put(skb, le16_to_cpu(rgpd->data_buff_len));

		ret = md_ctrl->recv_skb(queue, skb);
		if (ret < 0)
			return ret;

		req->skb = NULL;
		t7xx_cldma_rgpd_set_data_ptr(rgpd, 0);
		queue->tr_done = t7xx_cldma_ring_step_forward(queue->tr_ring, req);
		req = queue->rx_refill;

		ret = t7xx_cldma_alloc_and_map_skb(md_ctrl, req, queue->tr_ring->pkt_size);
		if (ret)
			return ret;

		rgpd = req->gpd;
		t7xx_cldma_rgpd_set_data_ptr(rgpd, req->mapped_buff);
		rgpd->data_buff_len = 0;
		rgpd->gpd_flags = GPD_FLAGS_IOC | GPD_FLAGS_HWO;
		queue->rx_refill = t7xx_cldma_ring_step_forward(queue->tr_ring, req);
		rx_not_done = ++count < budget || !need_resched();
	} while (rx_not_done);

	*over_budget = true;
	return 0;
}

static int t7xx_cldma_gpd_rx_collect(struct cldma_queue *queue, int budget)
{
	struct cldma_ctrl *md_ctrl = queue->md_ctrl;
	bool rx_not_done, over_budget = false;
	struct t7xx_cldma_hw *hw_info;
	unsigned int l2_rx_int;
	unsigned long flags;
	int ret;

	hw_info = &md_ctrl->hw_info;

	do {
		rx_not_done = false;

		ret = t7xx_cldma_gpd_rx_from_queue(queue, budget, &over_budget);
		if (ret == -ENODATA)
			return 0;
		else if (ret)
			return ret;

		spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
		if (md_ctrl->rxq_active & BIT(queue->index)) {
			if (!t7xx_cldma_hw_queue_status(hw_info, queue->index, MTK_RX))
				t7xx_cldma_hw_resume_queue(hw_info, queue->index, MTK_RX);

			l2_rx_int = t7xx_cldma_hw_int_status(hw_info, BIT(queue->index), MTK_RX);
			if (l2_rx_int) {
				t7xx_cldma_hw_rx_done(hw_info, l2_rx_int);

				if (over_budget) {
					spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
					return -EAGAIN;
				}

				rx_not_done = true;
			}
		}

		spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
	} while (rx_not_done);

	return 0;
}

static void t7xx_cldma_rx_done(struct work_struct *work)
{
	struct cldma_queue *queue = container_of(work, struct cldma_queue, cldma_work);
	struct cldma_ctrl *md_ctrl = queue->md_ctrl;
	int value;

	value = t7xx_cldma_gpd_rx_collect(queue, queue->budget);
	if (value && md_ctrl->rxq_active & BIT(queue->index)) {
		queue_work(queue->worker, &queue->cldma_work);
		return;
	}

	t7xx_cldma_clear_ip_busy(&md_ctrl->hw_info);
	t7xx_cldma_hw_irq_en_txrx(&md_ctrl->hw_info, queue->index, MTK_RX);
	t7xx_cldma_hw_irq_en_eq(&md_ctrl->hw_info, queue->index, MTK_RX);
	pm_runtime_mark_last_busy(md_ctrl->dev);
	pm_runtime_put_autosuspend(md_ctrl->dev);
}

static int t7xx_cldma_gpd_tx_collect(struct cldma_queue *queue)
{
	struct cldma_ctrl *md_ctrl = queue->md_ctrl;
	unsigned int dma_len, count = 0;
	struct cldma_request *req;
	struct sk_buff *skb_free;
	struct cldma_tgpd *tgpd;
	unsigned long flags;
	dma_addr_t dma_free;

	while (!kthread_should_stop()) {
		spin_lock_irqsave(&queue->ring_lock, flags);
		req = queue->tr_done;
		if (!req) {
			spin_unlock_irqrestore(&queue->ring_lock, flags);
			break;
		}

		tgpd = req->gpd;
		if ((tgpd->gpd_flags & GPD_FLAGS_HWO) || !req->skb) {
			spin_unlock_irqrestore(&queue->ring_lock, flags);
			break;
		}

		queue->budget++;
		dma_free = req->mapped_buff;
		dma_len = le16_to_cpu(tgpd->data_buff_len);
		skb_free = req->skb;
		req->skb = NULL;
		queue->tr_done = t7xx_cldma_ring_step_forward(queue->tr_ring, req);
		spin_unlock_irqrestore(&queue->ring_lock, flags);
		count++;
		dma_unmap_single(md_ctrl->dev, dma_free, dma_len, DMA_TO_DEVICE);
		dev_kfree_skb_any(skb_free);
	}

	if (count)
		wake_up_nr(&queue->req_wq, count);

	return count;
}

static void t7xx_cldma_txq_empty_hndl(struct cldma_queue *queue)
{
	struct cldma_ctrl *md_ctrl = queue->md_ctrl;
	struct cldma_request *req;
	struct cldma_tgpd *tgpd;
	dma_addr_t ul_curr_addr;
	unsigned long flags;
	bool pending_gpd;

	if (!(md_ctrl->txq_active & BIT(queue->index)))
		return;

	spin_lock_irqsave(&queue->ring_lock, flags);
	req = t7xx_cldma_ring_step_backward(queue->tr_ring, queue->tx_xmit);
	tgpd = req->gpd;
	pending_gpd = (tgpd->gpd_flags & GPD_FLAGS_HWO) && req->skb;
	spin_unlock_irqrestore(&queue->ring_lock, flags);

	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	if (pending_gpd) {
		struct t7xx_cldma_hw *hw_info = &md_ctrl->hw_info;

		/* Check current processing TGPD, 64-bit address is in a table by Q index */
		ul_curr_addr = ioread64(hw_info->ap_pdn_base + REG_CLDMA_UL_CURRENT_ADDRL_0 +
					queue->index * sizeof(u64));
		if (req->gpd_addr != ul_curr_addr) {
			/* struct t7xx_fsm_ctl *ctl = queue->md->fsm_ctl; */

			/* spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags); */
			dev_err(md_ctrl->dev, "CLDMA%d queue %d is not empty\n",
				md_ctrl->hif_id, queue->index);
			/* t7xx_fsm_append_cmd(ctl, FSM_CMD_RECOVER, 0);
			 * return;
			 */
		} else {
			t7xx_cldma_hw_resume_queue(hw_info, queue->index, MTK_TX);
		}
	}

	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
}

static void t7xx_cldma_tx_done(struct work_struct *work)
{
	struct cldma_queue *queue = container_of(work, struct cldma_queue, cldma_work);
	struct cldma_ctrl *md_ctrl = queue->md_ctrl;
	struct t7xx_cldma_hw *hw_info;
	unsigned int l2_tx_int;
	unsigned long flags;

	hw_info = &md_ctrl->hw_info;
	t7xx_cldma_gpd_tx_collect(queue);
	l2_tx_int = t7xx_cldma_hw_int_status(hw_info, BIT(queue->index) | EQ_STA_BIT(queue->index),
					     MTK_TX);
	if (l2_tx_int & EQ_STA_BIT(queue->index)) {
		t7xx_cldma_hw_tx_done(hw_info, EQ_STA_BIT(queue->index));
		t7xx_cldma_txq_empty_hndl(queue);
	}

	if (l2_tx_int & BIT(queue->index)) {
		t7xx_cldma_hw_tx_done(hw_info, BIT(queue->index));
		queue_work(queue->worker, &queue->cldma_work);
		return;
	}

	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	if (md_ctrl->txq_active & BIT(queue->index)) {
		t7xx_cldma_clear_ip_busy(hw_info);
		t7xx_cldma_hw_irq_en_eq(hw_info, queue->index, MTK_TX);
		t7xx_cldma_hw_irq_en_txrx(hw_info, queue->index, MTK_TX);
	}

	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
	pm_runtime_mark_last_busy(md_ctrl->dev);
	pm_runtime_put_autosuspend(md_ctrl->dev);
}

static void t7xx_cldma_ring_free(struct cldma_ctrl *md_ctrl,
				 struct cldma_ring *ring, enum dma_data_direction tx_rx)
{
	struct cldma_request *req_cur, *req_next;

	list_for_each_entry_safe(req_cur, req_next, &ring->gpd_ring, entry) {
		if (req_cur->mapped_buff && req_cur->skb) {
			dma_unmap_single(md_ctrl->dev, req_cur->mapped_buff,
					 t7xx_skb_data_size(req_cur->skb), tx_rx);
			req_cur->mapped_buff = 0;
		}

		dev_kfree_skb_any(req_cur->skb);

		if (req_cur->gpd)
			dma_pool_free(md_ctrl->gpd_dmapool, req_cur->gpd, req_cur->gpd_addr);

		list_del(&req_cur->entry);
		kfree_sensitive(req_cur);
	}
}

static struct cldma_request *t7xx_alloc_rx_request(struct cldma_ctrl *md_ctrl, size_t pkt_size)
{
	struct cldma_request *item;
	int val;

	item = kzalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return NULL;

	item->gpd = dma_pool_zalloc(md_ctrl->gpd_dmapool, GFP_KERNEL, &item->gpd_addr);
	if (!item->gpd)
		goto err_free_req;

	val = t7xx_cldma_alloc_and_map_skb(md_ctrl, item, pkt_size);
	if (val)
		goto err_free_pool;

	return item;

err_free_pool:
	dma_pool_free(md_ctrl->gpd_dmapool, item->gpd, item->gpd_addr);

err_free_req:
	kfree(item);

	return NULL;
}

static int t7xx_cldma_rx_ring_init(struct cldma_ctrl *md_ctrl, struct cldma_ring *ring)
{
	struct cldma_request *item, *first_item = NULL;
	struct cldma_rgpd *prev_gpd, *gpd = NULL;
	int i;

	for (i = 0; i < ring->length; i++) {
		item = t7xx_alloc_rx_request(md_ctrl, ring->pkt_size);
		if (!item) {
			t7xx_cldma_ring_free(md_ctrl, ring, DMA_FROM_DEVICE);
			return -ENOMEM;
		}

		gpd = item->gpd;
		t7xx_cldma_rgpd_set_data_ptr(gpd, item->mapped_buff);
		gpd->data_allow_len = cpu_to_le16(ring->pkt_size);
		gpd->gpd_flags = GPD_FLAGS_IOC | GPD_FLAGS_HWO;

		if (i)
			t7xx_cldma_rgpd_set_next_ptr(prev_gpd, item->gpd_addr);
		else
			first_item = item;

		INIT_LIST_HEAD(&item->entry);
		list_add_tail(&item->entry, &ring->gpd_ring);
		prev_gpd = gpd;
	}

	if (first_item)
		t7xx_cldma_rgpd_set_next_ptr(gpd, first_item->gpd_addr);

	return 0;
}

static struct cldma_request *t7xx_alloc_tx_request(struct cldma_ctrl *md_ctrl)
{
	struct cldma_request *item;

	item = kzalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return NULL;

	item->gpd = dma_pool_zalloc(md_ctrl->gpd_dmapool, GFP_KERNEL, &item->gpd_addr);
	if (!item->gpd) {
		kfree(item);
		return NULL;
	}

	return item;
}

static int t7xx_cldma_tx_ring_init(struct cldma_ctrl *md_ctrl, struct cldma_ring *ring)
{
	struct cldma_request *item, *first_item = NULL;
	struct cldma_tgpd *tgpd, *prev_gpd;
	int i;

	for (i = 0; i < ring->length; i++) {
		item = t7xx_alloc_tx_request(md_ctrl);
		if (!item) {
			t7xx_cldma_ring_free(md_ctrl, ring, DMA_TO_DEVICE);
			return -ENOMEM;
		}

		tgpd = item->gpd;
		tgpd->gpd_flags = GPD_FLAGS_IOC;

		if (!first_item)
			first_item = item;
		else
			t7xx_cldma_tgpd_set_next_ptr(prev_gpd, item->gpd_addr);

		INIT_LIST_HEAD(&item->entry);
		list_add_tail(&item->entry, &ring->gpd_ring);
		prev_gpd = tgpd;
	}

	if (first_item)
		t7xx_cldma_tgpd_set_next_ptr(tgpd, first_item->gpd_addr);

	return 0;
}

/**
 * t7xx_cldma_queue_reset() - Reset CLDMA request pointers to their initial values.
 * @queue: Pointer to the queue structure.
 *
 */
static void t7xx_cldma_queue_reset(struct cldma_queue *queue)
{
	struct cldma_request *req;

	req = list_first_entry(&queue->tr_ring->gpd_ring, struct cldma_request, entry);
	queue->tr_done = req;
	queue->budget = queue->tr_ring->length;

	if (queue->dir == MTK_TX)
		queue->tx_xmit = req;
	else
		queue->rx_refill = req;
}

static void t7xx_cldma_rx_queue_init(struct cldma_queue *queue)
{
	struct cldma_ctrl *md_ctrl = queue->md_ctrl;

	queue->dir = MTK_RX;
	queue->tr_ring = &md_ctrl->rx_ring[queue->index];
	queue->q_type = rxq_type[queue->index];
	t7xx_cldma_queue_reset(queue);
}

static void t7xx_cldma_tx_queue_init(struct cldma_queue *queue)
{
	struct cldma_ctrl *md_ctrl = queue->md_ctrl;

	queue->dir = MTK_TX;
	queue->tr_ring = &md_ctrl->tx_ring[queue->index];
	queue->q_type = txq_type[queue->index];
	t7xx_cldma_queue_reset(queue);
}

static void t7xx_cldma_enable_irq(struct cldma_ctrl *md_ctrl)
{
	t7xx_pcie_mac_set_int(md_ctrl->t7xx_dev, md_ctrl->hw_info.phy_interrupt_id);
}

static void t7xx_cldma_disable_irq(struct cldma_ctrl *md_ctrl)
{
	t7xx_pcie_mac_clear_int(md_ctrl->t7xx_dev, md_ctrl->hw_info.phy_interrupt_id);
}

static void t7xx_cldma_irq_work_cb(struct cldma_ctrl *md_ctrl)
{
	u32 l2_tx_int_msk, l2_rx_int_msk, l2_tx_int, l2_rx_int, val;
	struct t7xx_cldma_hw *hw_info = &md_ctrl->hw_info;
	int i;

	/* L2 raw interrupt status */
	l2_tx_int = ioread32(hw_info->ap_pdn_base + REG_CLDMA_L2TISAR0);
	l2_rx_int = ioread32(hw_info->ap_pdn_base + REG_CLDMA_L2RISAR0);
	l2_tx_int_msk = ioread32(hw_info->ap_pdn_base + REG_CLDMA_L2TIMR0);
	l2_rx_int_msk = ioread32(hw_info->ap_ao_base + REG_CLDMA_L2RIMR0);
	l2_tx_int &= ~l2_tx_int_msk;
	l2_rx_int &= ~l2_rx_int_msk;

	if (l2_tx_int) {
		if (l2_tx_int & (TQ_ERR_INT_BITMASK | TQ_ACTIVE_START_ERR_INT_BITMASK)) {
			/* Read and clear L3 TX interrupt status */
			val = ioread32(hw_info->ap_pdn_base + REG_CLDMA_L3TISAR0);
			iowrite32(val, hw_info->ap_pdn_base + REG_CLDMA_L3TISAR0);
			val = ioread32(hw_info->ap_pdn_base + REG_CLDMA_L3TISAR1);
			iowrite32(val, hw_info->ap_pdn_base + REG_CLDMA_L3TISAR1);
		}

		t7xx_cldma_hw_tx_done(hw_info, l2_tx_int);
		if (l2_tx_int & (TXRX_STATUS_BITMASK | EMPTY_STATUS_BITMASK)) {
			for_each_set_bit(i, (unsigned long *)&l2_tx_int, L2_INT_BIT_COUNT) {
				if (i < CLDMA_TXQ_NUM) {
					pm_runtime_get(md_ctrl->dev);
					t7xx_cldma_hw_irq_dis_eq(hw_info, i, MTK_TX);
					t7xx_cldma_hw_irq_dis_txrx(hw_info, i, MTK_TX);
					queue_work(md_ctrl->txq[i].worker,
						   &md_ctrl->txq[i].cldma_work);
				} else {
					t7xx_cldma_txq_empty_hndl(&md_ctrl->txq[i - CLDMA_TXQ_NUM]);
				}
			}
		}
	}

	if (l2_rx_int) {
		if (l2_rx_int & (RQ_ERR_INT_BITMASK | RQ_ACTIVE_START_ERR_INT_BITMASK)) {
			/* Read and clear L3 RX interrupt status */
			val = ioread32(hw_info->ap_pdn_base + REG_CLDMA_L3RISAR0);
			iowrite32(val, hw_info->ap_pdn_base + REG_CLDMA_L3RISAR0);
			val = ioread32(hw_info->ap_pdn_base + REG_CLDMA_L3RISAR1);
			iowrite32(val, hw_info->ap_pdn_base + REG_CLDMA_L3RISAR1);
		}

		t7xx_cldma_hw_rx_done(hw_info, l2_rx_int);
		if (l2_rx_int & (TXRX_STATUS_BITMASK | EMPTY_STATUS_BITMASK)) {
			l2_rx_int |=  l2_rx_int >> CLDMA_RXQ_NUM;
			for_each_set_bit(i, (unsigned long *)&l2_rx_int, CLDMA_RXQ_NUM) {
				pm_runtime_get(md_ctrl->dev);
				t7xx_cldma_hw_irq_dis_eq(hw_info, i, MTK_RX);
				t7xx_cldma_hw_irq_dis_txrx(hw_info, i, MTK_RX);
				queue_work(md_ctrl->rxq[i].worker, &md_ctrl->rxq[i].cldma_work);
			}
		}
	}
}

static bool t7xx_cldma_queues_active(struct t7xx_cldma_hw *hw_info)
{
	unsigned int tx_active;
	unsigned int rx_active;

	tx_active = t7xx_cldma_hw_queue_status(hw_info, CLDMA_ALL_Q, MTK_TX);
	rx_active = t7xx_cldma_hw_queue_status(hw_info, CLDMA_ALL_Q, MTK_RX);
	if (tx_active == CLDMA_INVALID_STATUS || rx_active == CLDMA_INVALID_STATUS)
		return false;

	return tx_active || rx_active;
}

/**
 * t7xx_cldma_stop() - Stop CLDMA.
 * @md_ctrl: CLDMA context structure.
 *
 * Stop TX and RX queues. Disable L1 and L2 interrupts.
 * Clear status registers.
 *
 * Return:
 * * 0		- Success.
 * * -ERROR	- Error code from polling cldma_queues_active.
 */
int t7xx_cldma_stop(struct cldma_ctrl *md_ctrl)
{
	struct t7xx_cldma_hw *hw_info = &md_ctrl->hw_info;
	bool active;
	int i, ret;

	md_ctrl->rxq_active = 0;
	t7xx_cldma_hw_stop_queue(hw_info, CLDMA_ALL_Q, MTK_RX);
	md_ctrl->txq_active = 0;
	t7xx_cldma_hw_stop_queue(hw_info, CLDMA_ALL_Q, MTK_TX);
	md_ctrl->txq_started = 0;
	t7xx_cldma_disable_irq(md_ctrl);
	t7xx_cldma_hw_stop(hw_info, MTK_RX);
	t7xx_cldma_hw_stop(hw_info, MTK_TX);
	t7xx_cldma_hw_tx_done(hw_info, CLDMA_L2TISAR0_ALL_INT_MASK);
	t7xx_cldma_hw_rx_done(hw_info, CLDMA_L2RISAR0_ALL_INT_MASK);

	if (md_ctrl->is_late_init) {
		for (i = 0; i < CLDMA_TXQ_NUM; i++)
			flush_work(&md_ctrl->txq[i].cldma_work);

		for (i = 0; i < CLDMA_RXQ_NUM; i++)
			flush_work(&md_ctrl->rxq[i].cldma_work);
	}

	ret = read_poll_timeout(t7xx_cldma_queues_active, active, !active, CHECK_Q_STOP_STEP_US,
				CHECK_Q_STOP_TIMEOUT_US, true, hw_info);
	if (ret)
		dev_err(md_ctrl->dev, "Could not stop CLDMA%d queues", md_ctrl->hif_id);

	return ret;
}

static void t7xx_cldma_late_release(struct cldma_ctrl *md_ctrl)
{
	int i;

	if (!md_ctrl->is_late_init)
		return;

	for (i = 0; i < CLDMA_TXQ_NUM; i++)
		t7xx_cldma_ring_free(md_ctrl, &md_ctrl->tx_ring[i], DMA_TO_DEVICE);

	for (i = 0; i < CLDMA_RXQ_NUM; i++)
		t7xx_cldma_ring_free(md_ctrl, &md_ctrl->rx_ring[i], DMA_FROM_DEVICE);

	dma_pool_destroy(md_ctrl->gpd_dmapool);
	md_ctrl->gpd_dmapool = NULL;
	md_ctrl->is_late_init = false;
}

void t7xx_cldma_reset(struct cldma_ctrl *md_ctrl)
{
	struct t7xx_modem *md = md_ctrl->t7xx_dev->md;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	md_ctrl->txq_active = 0;
	md_ctrl->rxq_active = 0;
	t7xx_cldma_disable_irq(md_ctrl);
	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
	for (i = 0; i < CLDMA_TXQ_NUM; i++) {
		md_ctrl->txq[i].md = md;
		cancel_work_sync(&md_ctrl->txq[i].cldma_work);
		spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
		md_cd_queue_struct_reset(&md_ctrl->txq[i], md_ctrl, MTK_TX, i);
		spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
	}

	for (i = 0; i < CLDMA_RXQ_NUM; i++) {
		md_ctrl->rxq[i].md = md;
		cancel_work_sync(&md_ctrl->rxq[i].cldma_work);
		spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
		md_cd_queue_struct_reset(&md_ctrl->rxq[i], md_ctrl, MTK_RX, i);
		spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
	}

	t7xx_cldma_late_release(md_ctrl);
}

/**
 * t7xx_cldma_start() - Start CLDMA.
 * @md_ctrl: CLDMA context structure.
 *
 * Set TX/RX start address.
 * Start all RX queues and enable L2 interrupt.
 */
void t7xx_cldma_start(struct cldma_ctrl *md_ctrl)
{
	unsigned long flags;

	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	if (md_ctrl->is_late_init) {
		struct t7xx_cldma_hw *hw_info = &md_ctrl->hw_info;
		int i;

		t7xx_cldma_enable_irq(md_ctrl);

		for (i = 0; i < CLDMA_TXQ_NUM; i++) {
			if (md_ctrl->txq[i].tr_done)
				t7xx_cldma_hw_set_start_addr(hw_info, i,
							     md_ctrl->txq[i].tr_done->gpd_addr,
							     MTK_TX);
		}

		for (i = 0; i < CLDMA_RXQ_NUM; i++) {
			if (md_ctrl->rxq[i].tr_done)
				t7xx_cldma_hw_set_start_addr(hw_info, i,
							     md_ctrl->rxq[i].tr_done->gpd_addr,
							     MTK_RX);
		}

		/* Enable L2 interrupt */
		t7xx_cldma_hw_start_queue(hw_info, CLDMA_ALL_Q, MTK_RX);
		t7xx_cldma_hw_start(hw_info);
		md_ctrl->txq_started = 0;
		md_ctrl->txq_active |= TXRX_STATUS_BITMASK;
		md_ctrl->rxq_active |= TXRX_STATUS_BITMASK;
	}

	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
}

static void t7xx_cldma_clear_txq(struct cldma_ctrl *md_ctrl, int qnum)
{
	struct cldma_queue *txq = &md_ctrl->txq[qnum];
	struct cldma_request *req;
	struct cldma_tgpd *tgpd;
	unsigned long flags;

	spin_lock_irqsave(&txq->ring_lock, flags);
	t7xx_cldma_queue_reset(txq);
	list_for_each_entry(req, &txq->tr_ring->gpd_ring, entry) {
		tgpd = req->gpd;
		tgpd->gpd_flags &= ~GPD_FLAGS_HWO;
		t7xx_cldma_tgpd_set_data_ptr(tgpd, 0);
		tgpd->data_buff_len = 0;
		dev_kfree_skb_any(req->skb);
		req->skb = NULL;
	}

	spin_unlock_irqrestore(&txq->ring_lock, flags);
}

static int t7xx_cldma_clear_rxq(struct cldma_ctrl *md_ctrl, int qnum)
{
	struct cldma_queue *rxq = &md_ctrl->rxq[qnum];
	struct cldma_request *req;
	struct cldma_rgpd *rgpd;
	unsigned long flags;

	spin_lock_irqsave(&rxq->ring_lock, flags);
	t7xx_cldma_queue_reset(rxq);
	list_for_each_entry(req, &rxq->tr_ring->gpd_ring, entry) {
		rgpd = req->gpd;
		rgpd->gpd_flags = GPD_FLAGS_IOC | GPD_FLAGS_HWO;
		rgpd->data_buff_len = 0;

		if (req->skb) {
			req->skb->len = 0;
			skb_reset_tail_pointer(req->skb);
		}
	}

	spin_unlock_irqrestore(&rxq->ring_lock, flags);
	list_for_each_entry(req, &rxq->tr_ring->gpd_ring, entry) {
		int ret;

		if (req->skb)
			continue;

		ret = t7xx_cldma_alloc_and_map_skb(md_ctrl, req, rxq->tr_ring->pkt_size);
		if (ret)
			return ret;

		t7xx_cldma_rgpd_set_data_ptr(req->gpd, req->mapped_buff);
	}

	return 0;
}

static void t7xx_cldma_clear_all_queue(struct cldma_ctrl *md_ctrl, enum mtk_txrx tx_rx)
{
	int i;

	if (tx_rx == MTK_TX) {
		for (i = 0; i < CLDMA_TXQ_NUM; i++)
			t7xx_cldma_clear_txq(md_ctrl, i);
	} else {
		for (i = 0; i < CLDMA_RXQ_NUM; i++)
			t7xx_cldma_clear_rxq(md_ctrl, i);
	}
}

static void t7xx_cldma_stop_queue(struct cldma_ctrl *md_ctrl, unsigned char qno,
				  enum mtk_txrx tx_rx)
{
	struct t7xx_cldma_hw *hw_info = &md_ctrl->hw_info;
	unsigned long flags;

	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	if (tx_rx == MTK_RX) {
		t7xx_cldma_hw_irq_dis_eq(hw_info, qno, MTK_RX);
		t7xx_cldma_hw_irq_dis_txrx(hw_info, qno, MTK_RX);

		if (qno == CLDMA_ALL_Q)
			md_ctrl->rxq_active &= ~TXRX_STATUS_BITMASK;
		else
			md_ctrl->rxq_active &= ~(TXRX_STATUS_BITMASK & BIT(qno));

		t7xx_cldma_hw_stop_queue(hw_info, qno, MTK_RX);
	} else if (tx_rx == MTK_TX) {
		t7xx_cldma_hw_irq_dis_eq(hw_info, qno, MTK_TX);
		t7xx_cldma_hw_irq_dis_txrx(hw_info, qno, MTK_TX);

		if (qno == CLDMA_ALL_Q)
			md_ctrl->txq_active &= ~TXRX_STATUS_BITMASK;
		else
			md_ctrl->txq_active &= ~(TXRX_STATUS_BITMASK & BIT(qno));

		t7xx_cldma_hw_stop_queue(hw_info, qno, MTK_TX);
	}

	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
}

static int t7xx_cldma_gpd_handle_tx_request(struct cldma_queue *queue, struct cldma_request *tx_req,
					    struct sk_buff *skb)
{
	struct cldma_ctrl *md_ctrl = queue->md_ctrl;
	struct cldma_tgpd *tgpd = tx_req->gpd;
	unsigned long flags;

	/* Update GPD */
	tx_req->mapped_buff = dma_map_single(md_ctrl->dev, skb->data, skb->len, DMA_TO_DEVICE);

	if (dma_mapping_error(md_ctrl->dev, tx_req->mapped_buff)) {
		dev_err(md_ctrl->dev, "DMA mapping failed\n");
		return -ENOMEM;
	}

	t7xx_cldma_tgpd_set_data_ptr(tgpd, tx_req->mapped_buff);
	tgpd->data_buff_len = cpu_to_le16(skb->len);

	/* This lock must cover TGPD setting, as even without a resume operation,
	 * CLDMA can send next HWO=1 if last TGPD just finished.
	 */
	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	if (md_ctrl->txq_active & BIT(queue->index))
		tgpd->gpd_flags |= GPD_FLAGS_HWO;

	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
	tx_req->skb = skb;
	return 0;
}

static void t7xx_cldma_hw_start_send(struct cldma_ctrl *md_ctrl, u8 qno)
{
	struct t7xx_cldma_hw *hw_info = &md_ctrl->hw_info;
	struct cldma_request *req;

	/* Check whether the device was powered off (CLDMA start address is not set) */
	if (!t7xx_cldma_tx_addr_is_set(hw_info, qno)) {
		t7xx_cldma_hw_init(hw_info);
		req = t7xx_cldma_ring_step_backward(md_ctrl->txq[qno].tr_ring,
						    md_ctrl->txq[qno].tx_xmit);
		t7xx_cldma_hw_set_start_addr(hw_info, qno, req->gpd_addr, MTK_TX);
		md_ctrl->txq_started &= ~BIT(qno);
	}

	if (!t7xx_cldma_hw_queue_status(hw_info, qno, MTK_TX)) {
		if (md_ctrl->txq_started & BIT(qno))
			t7xx_cldma_hw_resume_queue(hw_info, qno, MTK_TX);
		else
			t7xx_cldma_hw_start_queue(hw_info, qno, MTK_TX);

		md_ctrl->txq_started |= BIT(qno);
	}
}

int t7xx_cldma_write_room(struct cldma_ctrl *md_ctrl, unsigned char qno)
{
	struct cldma_queue *queue = &md_ctrl->txq[qno];

	if (!queue)
		return -EINVAL;

	if (queue->budget >= MAX_TX_BUDGET)
		return queue->budget;

	return 0;
}

/**
 * t7xx_cldma_set_recv_skb() - Set the callback to handle RX packets.
 * @md_ctrl: CLDMA context structure.
 * @recv_skb: Receiving skb callback.
 */
void t7xx_cldma_set_recv_skb(struct cldma_ctrl *md_ctrl,
			     int (*recv_skb)(struct cldma_queue *queue, struct sk_buff *skb))
{
	md_ctrl->recv_skb = recv_skb;
}

/**
 * t7xx_cldma_send_skb() - Send control data to modem.
 * @md_ctrl: CLDMA context structure.
 * @qno: Queue number.
 * @skb: Socket buffer.
 * @blocking: True for blocking operation.
 *
 * Send control packet to modem using a ring buffer.
 * If blocking is set, it will wait for completion.
 *
 * Return:
 * * 0		- Success.
 * * -ENOMEM	- Allocation failure.
 * * -EINVAL	- Invalid queue request.
 * * -EBUSY	- Resource lock failure.
 */
int t7xx_cldma_send_skb(struct cldma_ctrl *md_ctrl, int qno, struct sk_buff *skb, bool blocking)
{
	struct cldma_request *tx_req;
	struct cldma_queue *queue;
	unsigned long flags;
	int ret;

	if (qno >= CLDMA_TXQ_NUM)
		return -EINVAL;

	ret = pm_runtime_resume_and_get(md_ctrl->dev);
	if (ret < 0 && ret != -EACCES)
		return ret;

	t7xx_pci_disable_sleep(md_ctrl->t7xx_dev);
	queue = &md_ctrl->txq[qno];

	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	if (!(md_ctrl->txq_active & BIT(qno))) {
		ret = -EBUSY;
		spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
		goto allow_sleep;
	}

	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);

	do {
		spin_lock_irqsave(&queue->ring_lock, flags);
		tx_req = queue->tx_xmit;
		if (queue->budget > 0 && !tx_req->skb) {
			queue->budget--;
			t7xx_cldma_gpd_handle_tx_request(queue, tx_req, skb);
			queue->tx_xmit = t7xx_cldma_ring_step_forward(queue->tr_ring, tx_req);
			spin_unlock_irqrestore(&queue->ring_lock, flags);

			if (!t7xx_pci_sleep_disable_complete(md_ctrl->t7xx_dev)) {
				ret = -EBUSY;
				break;
			}

			/* Protect the access to the modem for queues operations (resume/start)
			 * which access shared locations by all the queues.
			 * cldma_lock is independent of ring_lock which is per queue.
			 */
			spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
			t7xx_cldma_hw_start_send(md_ctrl, qno);
			spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
			break;
		}

		spin_unlock_irqrestore(&queue->ring_lock, flags);

		if (!t7xx_pci_sleep_disable_complete(md_ctrl->t7xx_dev)) {
			ret = -EBUSY;
			break;
		}

		if (!t7xx_cldma_hw_queue_status(&md_ctrl->hw_info, qno, MTK_TX)) {
			spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
			t7xx_cldma_hw_resume_queue(&md_ctrl->hw_info, qno, MTK_TX);
			spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
		}

		if (!blocking) {
			ret = -EBUSY;
			break;
		}

		ret = wait_event_interruptible_exclusive(queue->req_wq, queue->budget > 0);
	} while (!ret);

allow_sleep:
	t7xx_pci_enable_sleep(md_ctrl->t7xx_dev);
	pm_runtime_mark_last_busy(md_ctrl->dev);
	pm_runtime_put_autosuspend(md_ctrl->dev);
	return ret;
}

int cldma_txq_mtu(unsigned char qno)
{
	if (qno >= CLDMA_TXQ_NUM)
		return -EINVAL;

	return txq_buff_size[qno];
}

static void ccci_cldma_adjust_config(unsigned char cfg_id)
{
	int qno;

	/* set default config */
	for (qno = 0; qno < CLDMA_RXQ_NUM; qno++) {
		rxq_buff_size[qno] = MTK_SKB_4K;
		rxq_type[qno] = CLDMA_SHARED_Q;
	}

	rxq_buff_size[CLDMA_RXQ_NUM - 1] = MTK_SKB_64K;

	for (qno = 0; qno < CLDMA_TXQ_NUM; qno++) {
		txq_buff_size[qno] = MTK_SKB_4K;
		txq_type[qno] = CLDMA_SHARED_Q;
	}

	switch (cfg_id) {
	case HIF_CFG_DEF:
		break;
	case HIF_CFG1:
		rxq_buff_size[7] = MTK_SKB_64K;
		break;
	case HIF_CFG2:
		/*Download Port Configuration*/
		rxq_type[0] = CLDMA_DEDICATED_Q;
		txq_type[0] = CLDMA_DEDICATED_Q;
		txq_buff_size[0] = MTK_SKB_2K;
		rxq_buff_size[0] = MTK_SKB_2K;
		/*Postdump Port Configuration*/
		rxq_type[1] = CLDMA_DEDICATED_Q;
		txq_type[1] = CLDMA_DEDICATED_Q;
		txq_buff_size[1] = MTK_SKB_2K;
		rxq_buff_size[1] = MTK_SKB_2K;
		break;
	default:
		break;
	}
}

static int t7xx_cldma_late_init(struct cldma_ctrl *md_ctrl, unsigned int cfg_id)
{
	char dma_pool_name[32];
	int i, j, ret;

	if (md_ctrl->is_late_init) {
		dev_err(md_ctrl->dev, "CLDMA late init was already done\n");
		return -EALREADY;
	}

	ccci_cldma_adjust_config(cfg_id);
	snprintf(dma_pool_name, sizeof(dma_pool_name), "cldma_req_hif%d", md_ctrl->hif_id);

	md_ctrl->gpd_dmapool = dma_pool_create(dma_pool_name, md_ctrl->dev,
					       sizeof(struct cldma_tgpd), GPD_DMAPOOL_ALIGN, 0);
	if (!md_ctrl->gpd_dmapool) {
		dev_err(md_ctrl->dev, "DMA pool alloc fail\n");
		return -ENOMEM;
	}

	for (i = 0; i < CLDMA_TXQ_NUM; i++) {
		INIT_LIST_HEAD(&md_ctrl->tx_ring[i].gpd_ring);
		md_ctrl->tx_ring[i].length = MAX_TX_BUDGET;

		ret = t7xx_cldma_tx_ring_init(md_ctrl, &md_ctrl->tx_ring[i]);
		if (ret) {
			dev_err(md_ctrl->dev, "control TX ring init fail\n");
			goto err_free_tx_ring;
		}
	}

	for (j = 0; j < CLDMA_RXQ_NUM; j++) {
		INIT_LIST_HEAD(&md_ctrl->rx_ring[j].gpd_ring);
		md_ctrl->rx_ring[j].length = MAX_RX_BUDGET;
		md_ctrl->rx_ring[j].pkt_size = rxq_buff_size[j];

		if (j == CLDMA_RXQ_NUM - 1)
			md_ctrl->rx_ring[j].pkt_size  = MTK_SKB_64K;

		ret = t7xx_cldma_rx_ring_init(md_ctrl, &md_ctrl->rx_ring[j]);
		if (ret) {
			dev_err(md_ctrl->dev, "Control RX ring init fail\n");
			goto err_free_rx_ring;
		}
	}

	for (i = 0; i < CLDMA_TXQ_NUM; i++)
		t7xx_cldma_tx_queue_init(&md_ctrl->txq[i]);

	for (j = 0; j < CLDMA_RXQ_NUM; j++)
		t7xx_cldma_rx_queue_init(&md_ctrl->rxq[j]);

	md_ctrl->is_late_init = true;
	return 0;

err_free_rx_ring:
	while (j--)
		t7xx_cldma_ring_free(md_ctrl, &md_ctrl->rx_ring[j], DMA_FROM_DEVICE);

err_free_tx_ring:
	while (i--)
		t7xx_cldma_ring_free(md_ctrl, &md_ctrl->tx_ring[i], DMA_TO_DEVICE);

	return ret;
}

static void __iomem *pcie_addr_transfer(void __iomem *addr, u32 addr_trs1, u32 phy_addr)
{
	return addr + phy_addr - addr_trs1;
}

static void t7xx_hw_info_init(struct cldma_ctrl *md_ctrl)
{
	struct t7xx_cldma_hw *hw_info;
	u32 phy_ao_base, phy_pd_base;
	struct t7xx_addr_base *pbase;

	hw_info = &md_ctrl->hw_info;
	hw_info->hw_mode = MODE_BIT_64;
	pbase = &md_ctrl->t7xx_dev->base_addr;

	if (md_ctrl->hif_id == ID_CLDMA1) {
		phy_ao_base = CLDMA1_AO_BASE;
		phy_pd_base = CLDMA1_PD_BASE;
		hw_info->phy_interrupt_id = CLDMA1_INT;
	} else {
		phy_ao_base = CLDMA0_AO_BASE;
		phy_pd_base = CLDMA0_PD_BASE;
		hw_info->phy_interrupt_id = CLDMA0_INT;
	}

	hw_info->ap_ao_base = pcie_addr_transfer(pbase->pcie_ext_reg_base,
						 pbase->pcie_dev_reg_trsl_addr, phy_ao_base);
	hw_info->ap_pdn_base = pcie_addr_transfer(pbase->pcie_ext_reg_base,
						  pbase->pcie_dev_reg_trsl_addr, phy_pd_base);
}

static int t7xx_cldma_default_recv_skb(struct cldma_queue *queue, struct sk_buff *skb)
{
	dev_kfree_skb_any(skb);
	return 0;
}

int t7xx_cldma_alloc(enum cldma_id hif_id, struct t7xx_pci_dev *t7xx_dev)
{
	struct device *dev = &t7xx_dev->pdev->dev;
	struct cldma_ctrl *md_ctrl;

	md_ctrl = devm_kzalloc(dev, sizeof(*md_ctrl), GFP_KERNEL);
	if (!md_ctrl)
		return -ENOMEM;

	md_ctrl->t7xx_dev = t7xx_dev;
	md_ctrl->dev = dev;
	md_ctrl->hif_id = hif_id;
	md_ctrl->recv_skb = t7xx_cldma_default_recv_skb;
	t7xx_hw_info_init(md_ctrl);
	t7xx_dev->md->md_ctrl[hif_id] = md_ctrl;
	return 0;
}

/**
 * t7xx_cldma_exception() - CLDMA exception handler.
 * @md_ctrl: CLDMA context structure.
 * @stage: exception stage.
 *
 * Part of the modem exception recovery.
 * Stages are one after the other as describe below:
 * HIF_EX_INIT:		Disable and clear TXQ.
 * HIF_EX_CLEARQ_DONE:	Disable RX, flush TX/RX workqueues and clear RX.
 * HIF_EX_ALLQ_RESET:	HW is back in safe mode for re-initialization and restart.
 */

/*
 * Modem Exception Handshake Flow
 *
 * Modem HW Exception interrupt received
 *           (MD_IRQ_CCIF_EX)
 *                   |
 *         +---------v--------+
 *         |   HIF_EX_INIT    | : Disable and clear TXQ
 *         +------------------+
 *                   |
 *         +---------v--------+
 *         | HIF_EX_INIT_DONE | : Wait for the init to be done
 *         +------------------+
 *                   |
 *         +---------v--------+
 *         |HIF_EX_CLEARQ_DONE| : Disable and clear RXQ
 *         +------------------+ : Flush TX/RX workqueues
 *                   |
 *         +---------v--------+
 *         |HIF_EX_ALLQ_RESET | : Restart HW and CLDMA
 *         +------------------+
 */
void t7xx_cldma_exception(struct cldma_ctrl *md_ctrl, enum hif_ex_stage stage)
{
	switch (stage) {
	case HIF_EX_INIT:
		t7xx_cldma_stop_queue(md_ctrl, CLDMA_ALL_Q, MTK_TX);
		t7xx_cldma_clear_all_queue(md_ctrl, MTK_TX);
		break;

	case HIF_EX_CLEARQ_DONE:
		/* We do not want to get CLDMA IRQ when MD is
		 * resetting CLDMA after it got clearq_ack.
		 */
		t7xx_cldma_stop_queue(md_ctrl, CLDMA_ALL_Q, MTK_RX);
		t7xx_cldma_stop(md_ctrl);

		if (md_ctrl->hif_id == ID_CLDMA1)
			t7xx_cldma_hw_reset(md_ctrl->t7xx_dev->base_addr.infracfg_ao_base);

		t7xx_cldma_clear_all_queue(md_ctrl, MTK_RX);
		break;

	case HIF_EX_ALLQ_RESET:
		t7xx_cldma_hw_init(&md_ctrl->hw_info);
		t7xx_cldma_start(md_ctrl);
		break;

	default:
		break;
	}
}

static void t7xx_cldma_resume_early(struct t7xx_pci_dev *mtk_dev, void *entity_param)
{
	struct cldma_ctrl *md_ctrl = entity_param;
	struct t7xx_cldma_hw *hw_info;
	unsigned long flags;
	int qno_t;

	hw_info = &md_ctrl->hw_info;
	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	t7xx_cldma_hw_restore(hw_info);
	for (qno_t = 0; qno_t < CLDMA_TXQ_NUM; qno_t++) {
		t7xx_cldma_hw_set_start_addr(hw_info, qno_t, md_ctrl->txq[qno_t].tx_xmit->gpd_addr,
					     MTK_TX);
		t7xx_cldma_hw_set_start_addr(hw_info, qno_t, md_ctrl->rxq[qno_t].tr_done->gpd_addr,
					     MTK_RX);
	}

	t7xx_cldma_enable_irq(md_ctrl);
	t7xx_cldma_hw_start_queue(hw_info, CLDMA_ALL_Q, MTK_RX);
	md_ctrl->rxq_active |= TXRX_STATUS_BITMASK;
	t7xx_cldma_hw_irq_en_eq(hw_info, CLDMA_ALL_Q, MTK_RX);
	t7xx_cldma_hw_irq_en_txrx(hw_info, CLDMA_ALL_Q, MTK_RX);
	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
}

static int t7xx_cldma_resume(struct t7xx_pci_dev *t7xx_dev, void *entity_param)
{
	struct cldma_ctrl *md_ctrl = entity_param;
	unsigned long flags;

	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	md_ctrl->txq_active |= TXRX_STATUS_BITMASK;
	t7xx_cldma_hw_irq_en_txrx(&md_ctrl->hw_info, CLDMA_ALL_Q, MTK_TX);
	t7xx_cldma_hw_irq_en_eq(&md_ctrl->hw_info, CLDMA_ALL_Q, MTK_TX);
	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);

	if (md_ctrl->hif_id == ID_CLDMA1)
		t7xx_mhccif_mask_clr(t7xx_dev, D2H_SW_INT_MASK);

	return 0;
}

static void t7xx_cldma_suspend_late(struct t7xx_pci_dev *t7xx_dev, void *entity_param)
{
	struct cldma_ctrl *md_ctrl = entity_param;
	struct t7xx_cldma_hw *hw_info;
	unsigned long flags;

	hw_info = &md_ctrl->hw_info;
	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	t7xx_cldma_hw_irq_dis_eq(hw_info, CLDMA_ALL_Q, MTK_RX);
	t7xx_cldma_hw_irq_dis_txrx(hw_info, CLDMA_ALL_Q, MTK_RX);
	md_ctrl->rxq_active &= ~TXRX_STATUS_BITMASK;
	t7xx_cldma_hw_stop_queue(hw_info, CLDMA_ALL_Q, MTK_RX);
	t7xx_cldma_clear_ip_busy(hw_info);
	t7xx_cldma_disable_irq(md_ctrl);
	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
}

static int t7xx_cldma_suspend(struct t7xx_pci_dev *t7xx_dev, void *entity_param)
{
	struct cldma_ctrl *md_ctrl = entity_param;
	struct t7xx_cldma_hw *hw_info;
	unsigned long flags;

	if (md_ctrl->hif_id == ID_CLDMA1)
		t7xx_mhccif_mask_set(t7xx_dev, D2H_SW_INT_MASK);

	hw_info = &md_ctrl->hw_info;
	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	t7xx_cldma_hw_irq_dis_eq(hw_info, CLDMA_ALL_Q, MTK_TX);
	t7xx_cldma_hw_irq_dis_txrx(hw_info, CLDMA_ALL_Q, MTK_TX);
	md_ctrl->txq_active &= ~TXRX_STATUS_BITMASK;
	t7xx_cldma_hw_stop_queue(hw_info, CLDMA_ALL_Q, MTK_TX);
	md_ctrl->txq_started = 0;
	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
	return 0;
}

static int t7xx_cldma_pm_init(struct cldma_ctrl *md_ctrl)
{
	md_ctrl->pm_entity = kzalloc(sizeof(*md_ctrl->pm_entity), GFP_KERNEL);
	if (!md_ctrl->pm_entity)
		return -ENOMEM;

	md_ctrl->pm_entity->entity_param = md_ctrl;

	if (md_ctrl->hif_id == ID_CLDMA1)
		md_ctrl->pm_entity->id = PM_ENTITY_ID_CTRL1;
	else
		md_ctrl->pm_entity->id = PM_ENTITY_ID_CTRL2;

	md_ctrl->pm_entity->suspend = t7xx_cldma_suspend;
	md_ctrl->pm_entity->suspend_late = t7xx_cldma_suspend_late;
	md_ctrl->pm_entity->resume = t7xx_cldma_resume;
	md_ctrl->pm_entity->resume_early = t7xx_cldma_resume_early;

	return t7xx_pci_pm_entity_register(md_ctrl->t7xx_dev, md_ctrl->pm_entity);
}

static int t7xx_cldma_pm_uninit(struct cldma_ctrl *md_ctrl)
{
	if (!md_ctrl->pm_entity)
		return -EINVAL;

	t7xx_pci_pm_entity_unregister(md_ctrl->t7xx_dev, md_ctrl->pm_entity);
	kfree_sensitive(md_ctrl->pm_entity);
	md_ctrl->pm_entity = NULL;
	return 0;
}

void t7xx_cldma_hif_hw_init(struct cldma_ctrl *md_ctrl)
{
	struct t7xx_cldma_hw *hw_info = &md_ctrl->hw_info;
	unsigned long flags;

	spin_lock_irqsave(&md_ctrl->cldma_lock, flags);
	t7xx_cldma_hw_stop(hw_info, MTK_TX);
	t7xx_cldma_hw_stop(hw_info, MTK_RX);
	t7xx_cldma_hw_rx_done(hw_info, EMPTY_STATUS_BITMASK | TXRX_STATUS_BITMASK);
	t7xx_cldma_hw_tx_done(hw_info, EMPTY_STATUS_BITMASK | TXRX_STATUS_BITMASK);
	t7xx_cldma_hw_init(hw_info);
	spin_unlock_irqrestore(&md_ctrl->cldma_lock, flags);
}

static irqreturn_t t7xx_cldma_isr_handler(int irq, void *data)
{
	struct cldma_ctrl *md_ctrl = data;
	u32 interrupt;

	interrupt = md_ctrl->hw_info.phy_interrupt_id;
	t7xx_pcie_mac_clear_int(md_ctrl->t7xx_dev, interrupt);
	t7xx_cldma_irq_work_cb(md_ctrl);
	t7xx_pcie_mac_clear_int_status(md_ctrl->t7xx_dev, interrupt);
	t7xx_pcie_mac_set_int(md_ctrl->t7xx_dev, interrupt);
	return IRQ_HANDLED;
}

/**
 * t7xx_cldma_init() - Initialize CLDMA.
 * @md: Modem context structure.
 * @md_ctrl: CLDMA context structure.
 *
 * Allocate and initialize device power management entity.
 * Initialize HIF TX/RX queue structure.
 * Register CLDMA callback ISR with PCIe driver.
 *
 * Return:
 * * 0		- Success.
 * * -ERROR	- Error code from failure sub-initializations.
 */
int t7xx_cldma_init(struct t7xx_modem *md, struct cldma_ctrl *md_ctrl)
{
	struct t7xx_cldma_hw *hw_info = &md_ctrl->hw_info;
	int ret, i;

	md_ctrl->txq_active = 0;
	md_ctrl->rxq_active = 0;
	md_ctrl->is_late_init = false;

	ret = t7xx_cldma_pm_init(md_ctrl);
	if (ret)
		return ret;

	spin_lock_init(&md_ctrl->cldma_lock);
	for (i = 0; i < CLDMA_TXQ_NUM; i++) {
		md_cd_queue_struct_init(&md_ctrl->txq[i], md_ctrl, MTK_TX, i);
		md_ctrl->txq[i].md = md;

		md_ctrl->txq[i].worker =
			alloc_workqueue("md_hif%d_tx%d_worker",
					WQ_UNBOUND | WQ_MEM_RECLAIM | (i ? 0 : WQ_HIGHPRI),
					1, md_ctrl->hif_id, i);
		if (!md_ctrl->txq[i].worker)
			return -ENOMEM;

		INIT_WORK(&md_ctrl->txq[i].cldma_work, t7xx_cldma_tx_done);
	}

	for (i = 0; i < CLDMA_RXQ_NUM; i++) {
		md_cd_queue_struct_init(&md_ctrl->rxq[i], md_ctrl, MTK_RX, i);
		md_ctrl->rxq[i].md = md;
		INIT_WORK(&md_ctrl->rxq[i].cldma_work, t7xx_cldma_rx_done);

		md_ctrl->rxq[i].worker = alloc_workqueue("md_hif%d_rx%d_worker",
							 WQ_UNBOUND | WQ_MEM_RECLAIM,
							 1, md_ctrl->hif_id, i);
		if (!md_ctrl->rxq[i].worker)
			return -ENOMEM;
	}

	t7xx_pcie_mac_clear_int(md_ctrl->t7xx_dev, hw_info->phy_interrupt_id);
	md_ctrl->t7xx_dev->intr_handler[hw_info->phy_interrupt_id] = t7xx_cldma_isr_handler;
	md_ctrl->t7xx_dev->intr_thread[hw_info->phy_interrupt_id] = NULL;
	md_ctrl->t7xx_dev->callback_param[hw_info->phy_interrupt_id] = md_ctrl;
	t7xx_pcie_mac_clear_int_status(md_ctrl->t7xx_dev, hw_info->phy_interrupt_id);
	return 0;
}

void t7xx_cldma_switch_cfg(struct cldma_ctrl *md_ctrl, unsigned int cfg_id)
{
	t7xx_cldma_late_release(md_ctrl);
	t7xx_cldma_late_init(md_ctrl, cfg_id);
}

void t7xx_cldma_exit(struct cldma_ctrl *md_ctrl)
{
	int i;

	t7xx_cldma_stop(md_ctrl);
	t7xx_cldma_late_release(md_ctrl);

	for (i = 0; i < CLDMA_TXQ_NUM; i++) {
		if (md_ctrl->txq[i].worker) {
			destroy_workqueue(md_ctrl->txq[i].worker);
			md_ctrl->txq[i].worker = NULL;
		}
	}

	for (i = 0; i < CLDMA_RXQ_NUM; i++) {
		if (md_ctrl->rxq[i].worker) {
			destroy_workqueue(md_ctrl->rxq[i].worker);
			md_ctrl->rxq[i].worker = NULL;
		}
	}

	t7xx_cldma_pm_uninit(md_ctrl);
}
