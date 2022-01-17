/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2021, MediaTek Inc.
 * Copyright (c) 2021, Intel Corporation.
 *
 * Authors:
 *  Amir Hanania <amir.hanania@intel.com>
 *  Haijun Liu <haijun.liu@mediatek.com>
 *  Moises Veleta <moises.veleta@intel.com>
 *  Ricardo Martinez<ricardo.martinez@linux.intel.com>
 *
 * Contributors:
 *  Chiranjeevi Rapolu <chiranjeevi.rapolu@intel.com>
 *  Eliot Lee <eliot.lee@intel.com>
 *  Sreehari Kancharla <sreehari.kancharla@intel.com>
 */

#ifndef __T7XX_DPMA_TX_H__
#define __T7XX_DPMA_TX_H__

#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/wait.h>

#include "t7xx_common.h"
#include "t7xx_pci.h"

#define DPMAIF_RXQ_NUM		2
#define DPMAIF_TXQ_NUM		5

enum dpmaif_rdwr {
	DPMAIF_READ,
	DPMAIF_WRITE,
};

struct dpmaif_isr_en_mask {
	unsigned int		ap_ul_l2intr_en_msk;
	unsigned int		ap_dl_l2intr_en_msk;
	unsigned int		ap_udl_ip_busy_en_msk;
	unsigned int		ap_dl_l2intr_err_en_msk;
};

struct dpmaif_ul {
	bool			que_started;
	unsigned char		reserve[3];
	dma_addr_t		drb_base;
	unsigned int		drb_size_cnt;
};

struct dpmaif_dl {
	bool			que_started;
	unsigned char		reserve[3];
	dma_addr_t		pit_base;
	unsigned int		pit_size_cnt;
	dma_addr_t		bat_base;
	unsigned int		bat_size_cnt;
	dma_addr_t		frg_base;
	unsigned int		frg_size_cnt;
	unsigned int		pit_seq;
};

struct dpmaif_dl_hwq {
	unsigned int		bat_remain_size;
	unsigned int		bat_pkt_bufsz;
	unsigned int		frg_pkt_bufsz;
	unsigned int		bat_rsv_length;
	unsigned int		pkt_bid_max_cnt;
	unsigned int		pkt_alignment;
	unsigned int		mtu_size;
	unsigned int		chk_pit_num;
	unsigned int		chk_bat_num;
	unsigned int		chk_frg_num;
};

/* Structure of DL BAT */
struct dpmaif_cur_rx_skb_info {
	bool			msg_pit_received;
	struct sk_buff		*cur_skb;
	unsigned int		cur_chn_idx;
	unsigned int		check_sum;
	unsigned int		pit_dp;
	unsigned int		pkt_type;
	int			err_payload;
};

struct dpmaif_bat {
	unsigned int		p_buffer_addr;
	unsigned int		buffer_addr_ext;
};

struct dpmaif_bat_skb {
	struct sk_buff		*skb;
	dma_addr_t		data_bus_addr;
	unsigned int		data_len;
};

struct dpmaif_bat_page {
	struct page		*page;
	dma_addr_t		data_bus_addr;
	unsigned int		offset;
	unsigned int		data_len;
};

enum bat_type {
	BAT_TYPE_NORMAL = 0,
	BAT_TYPE_FRAG = 1,
};

struct dpmaif_bat_request {
	void			*bat_base;
	dma_addr_t		bat_bus_addr;
	unsigned int		bat_size_cnt;
	unsigned short		bat_wr_idx;
	unsigned short		bat_release_rd_idx;
	void			*bat_skb;
	unsigned int		skb_pkt_cnt;
	unsigned int		pkt_buf_sz;
	unsigned char		*bat_mask;
	atomic_t		refcnt;
	spinlock_t		mask_lock; /* Protects BAT mask */
	enum bat_type		type;
};

struct dpmaif_rx_queue {
	unsigned char		index;
	bool			que_started;
	unsigned short		budget;

	void			*pit_base;
	dma_addr_t		pit_bus_addr;
	unsigned int		pit_size_cnt;

	unsigned short		pit_rd_idx;
	unsigned short		pit_wr_idx;
	unsigned short		pit_release_rd_idx;

	struct dpmaif_bat_request *bat_req;
	struct dpmaif_bat_request *bat_frag;

	wait_queue_head_t	rx_wq;
	struct task_struct	*rx_thread;
	struct sk_buff_head	skb_list;
	unsigned int		skb_list_max_len;

	struct workqueue_struct	*worker;
	struct work_struct	dpmaif_rxq_work;

	atomic_t		rx_processing;

	struct dpmaif_ctrl	*dpmaif_ctrl;
	unsigned int		expect_pit_seq;
	unsigned int		pit_remain_release_cnt;
	struct dpmaif_cur_rx_skb_info rx_data_info;
};

struct dpmaif_tx_queue {
	unsigned char		index;
	bool			que_started;
	atomic_t		tx_budget;
	void			*drb_base;
	dma_addr_t		drb_bus_addr;
	unsigned int		drb_size_cnt;
	unsigned short		drb_wr_idx;
	unsigned short		drb_rd_idx;
	unsigned short		drb_release_rd_idx;
	unsigned short		last_ch_id;
	void			*drb_skb_base;
	wait_queue_head_t	req_wq;
	struct workqueue_struct	*worker;
	struct work_struct	dpmaif_tx_work;
	spinlock_t		tx_lock; /* Protects txq DRB */
	atomic_t		tx_processing;

	struct dpmaif_ctrl	*dpmaif_ctrl;
	spinlock_t		tx_skb_lock; /* Protects TX thread skb list */
	struct list_head	tx_skb_queue;
	unsigned int		tx_submit_skb_cnt;
	unsigned int		tx_list_max_len;
	unsigned int		tx_skb_stat;
	bool			drb_lack;
};

struct dpmaif_skb_info {
	struct list_head	entry;
	struct sk_buff		*skb;
	unsigned int		data_len;
	dma_addr_t		data_bus_addr;
};

struct dpmaif_isr_para {
	struct dpmaif_ctrl	*dpmaif_ctrl;
	unsigned char		pcie_int;
	unsigned char		dlq_id;
};

enum dpmaif_state {
	DPMAIF_STATE_MIN,
	DPMAIF_STATE_PWROFF,
	DPMAIF_STATE_PWRON,
	DPMAIF_STATE_EXCEPTION,
	DPMAIF_STATE_MAX
};

struct dpmaif_hw_info {
	void __iomem			*pcie_base;
	struct dpmaif_dl		dl_que[DPMAIF_RXQ_NUM];
	struct dpmaif_ul		ul_que[DPMAIF_TXQ_NUM];
	struct dpmaif_dl_hwq		dl_que_hw[DPMAIF_RXQ_NUM];
	struct dpmaif_isr_en_mask	isr_en_mask;
};

enum dpmaif_txq_state {
	DMPAIF_TXQ_STATE_IRQ,
	DMPAIF_TXQ_STATE_FULL,
};

struct dpmaif_callbacks {
	void (*state_notify)(struct t7xx_pci_dev *t7xx_dev,
			     enum dpmaif_txq_state state, int txqt);
	void (*recv_skb)(struct t7xx_pci_dev *t7xx_dev, struct sk_buff *skb);
};

struct dpmaif_ctrl {
	struct device			*dev;
	struct t7xx_pci_dev		*t7xx_dev;
	struct md_pm_entity		dpmaif_pm_entity;
	enum dpmaif_state		state;
	bool				dpmaif_sw_init_done;
	struct dpmaif_hw_info		hif_hw_info;
	struct dpmaif_tx_queue		txq[DPMAIF_TXQ_NUM];
	struct dpmaif_rx_queue		rxq[DPMAIF_RXQ_NUM];

	unsigned char			rxq_int_mapping[DPMAIF_RXQ_NUM];
	struct dpmaif_isr_para		isr_para[DPMAIF_RXQ_NUM];

	struct dpmaif_bat_request	bat_req;
	struct dpmaif_bat_request	bat_frag;
	struct workqueue_struct		*bat_release_wq;
	struct work_struct		bat_release_work;

	wait_queue_head_t		tx_wq;
	struct task_struct		*tx_thread;
	unsigned char			txq_select_times;

	struct dpmaif_callbacks		*callbacks;
};

struct dpmaif_ctrl *t7xx_dpmaif_hif_init(struct t7xx_pci_dev *t7xx_dev,
					 struct dpmaif_callbacks *callbacks);
void t7xx_dpmaif_hif_exit(struct dpmaif_ctrl *dpmaif_ctrl);
int t7xx_dpmaif_md_state_callback(struct dpmaif_ctrl *dpmaif_ctrl, unsigned char state);
unsigned int t7xx_ring_buf_get_next_wrdx(unsigned int buf_len, unsigned int buf_idx);
unsigned int t7xx_ring_buf_rd_wr_count(unsigned int total_cnt, unsigned int rd_idx,
				       unsigned int wrt_idx, enum dpmaif_rdwr);

#endif /* __T7XX_DPMA_TX_H__ */
