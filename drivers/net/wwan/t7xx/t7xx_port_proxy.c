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
 *
 * Contributors:
 *  Andy Shevchenko <andriy.shevchenko@linux.intel.com>
 *  Chandrashekar Devegowda <chandrashekar.devegowda@intel.com>
 *  Chiranjeevi Rapolu <chiranjeevi.rapolu@intel.com>
 *  Eliot Lee <eliot.lee@intel.com>
 *  Sreehari Kancharla <sreehari.kancharla@intel.com>
 */

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/wwan.h>
#include <net/netlink.h>

#include "t7xx_common.h"
#include "t7xx_hif_cldma.h"
#include "t7xx_modem_ops.h"
#include "t7xx_port.h"
#include "t7xx_port_proxy.h"
#include "t7xx_state_monitor.h"

#define CHECK_RX_SEQ_MASK		GENMASK(14, 0)
#define Q_IDX_CTRL			0
#define Q_IDX_MBIM			2
#define Q_IDX_AT_CMD			5

#define TTY_IPC_MINOR_BASE			100
#define PORT_NOTIFY_PROTOCOL			NETLINK_USERSOCK

#define DEVICE_NAME				"MTK_WWAN_M80"

static struct port_proxy *port_prox;
static struct class *dev_class;

#define for_each_proxy_port(i, p, proxy)	\
	for (i = 0, (p) = &(proxy)->ports_private[i];	\
	     i < (proxy)->port_number;		\
	     i++, (p) = &(proxy)->ports_private[i])

static struct t7xx_port_static t7xx_md_ports[] = {
	{
		.tx_ch = CCCI_SAP_GNSS_TX,
		.rx_ch = CCCI_SAP_GNSS_RX,
		.txq_index = 0,
		.rxq_index = 0,
		.txq_exp_index = 0,
		.rxq_exp_index = 0,
		.path_id = ID_CLDMA0,
		.flags = PORT_F_RX_CHAR_NODE,
		.ops = &wwan_sub_port_ops,
		.minor = 0,
		.name = "ccci_sap_gnss",
		.port_type = WWAN_PORT_AT,
	}, {
		.tx_ch = PORT_CH_UART2_TX,
		.rx_ch = PORT_CH_UART2_RX,
		.txq_index = Q_IDX_AT_CMD,
		.rxq_index = Q_IDX_AT_CMD,
		.txq_exp_index = 0xff,
		.rxq_exp_index = 0xff,
		.path_id = ID_CLDMA1,
		.flags = PORT_F_RX_CHAR_NODE,
		.ops = &wwan_sub_port_ops,
		.name = "AT",
		.port_type = WWAN_PORT_AT,
	}, {
		.tx_ch = PORT_CH_MBIM_TX,
		.rx_ch = PORT_CH_MBIM_RX,
		.txq_index = Q_IDX_MBIM,
		.rxq_index = Q_IDX_MBIM,
		.txq_exp_index = 0,
		.rxq_exp_index = 0,
		.path_id = ID_CLDMA1,
		.flags = PORT_F_RX_CHAR_NODE,
		.ops = &wwan_sub_port_ops,
		.name = "MBIM",
		.port_type = WWAN_PORT_MBIM,
	}, {
		.tx_ch = PORT_CH_MD_LOG_TX,
		.rx_ch = PORT_CH_MD_LOG_RX,
		.txq_index = 7,
		.rxq_index = 7,
		.txq_exp_index = 7,
		.rxq_exp_index = 7,
		.path_id = ID_CLDMA1,
		.flags = PORT_F_RX_CHAR_NODE,
		.ops = &char_port_ops,
		.minor = 2,
		.name = "ttyCMdLog",
		.port_type = WWAN_PORT_AT,
	}, {
		.tx_ch = CCCI_SAP_ADB_TX,
		.rx_ch = CCCI_SAP_ADB_RX,
		.txq_index = 3,
		.rxq_index = 3,
		.txq_exp_index = 0,
		.rxq_exp_index = 0,
		.path_id = ID_CLDMA0,
		.flags = PORT_F_RX_CHAR_NODE,
		.ops = &char_port_ops,
		.minor = 9,
		.name = "ccci_sap_adb",
	}, {
		.tx_ch = PORT_CH_MIPC_TX,
		.rx_ch = PORT_CH_MIPC_RX,
		.txq_index = 2,
		.rxq_index = 2,
		.txq_exp_index = 0,
		.rxq_exp_index = 0,
		.path_id = ID_CLDMA1,
		.flags = PORT_F_RX_CHAR_NODE,
		.ops = &tty_port_ops,
		.minor = 1,
		.name = "ttyCMIPC0",
	}, {
		.tx_ch = PORT_CH_CONTROL_TX,
		.rx_ch = PORT_CH_CONTROL_RX,
		.txq_index = Q_IDX_CTRL,
		.rxq_index = Q_IDX_CTRL,
		.txq_exp_index = 0,
		.rxq_exp_index = 0,
		.path_id = ID_CLDMA1,
		.flags = 0,
		.ops = &ctl_port_ops,
		.name = "t7xx_ctrl",
	}, {
		.tx_ch = CCCI_SAP_CONTROL_TX,
		.rx_ch = CCCI_SAP_CONTROL_RX,
		.txq_index = 0,
		.rxq_index = 0,
		.txq_exp_index = 0,
		.rxq_exp_index = 0,
		.path_id = ID_CLDMA0,
		.flags = 0,
		.ops = &ctl_port_ops,
		.minor = 0xff,
		.name = "ccci_sap_ctrl",
	},
};

static struct t7xx_port_static md_ccci_early_ports[] = {
	{
		.tx_ch = 0xffff,
		.rx_ch = 0xffff,
		.txq_index = 0,
		.rxq_index = 0,
		.txq_exp_index = 0,
		.rxq_exp_index = 0,
		.path_id = ID_CLDMA0,
		.flags = PORT_F_RX_CHAR_NODE | PORT_F_RAW_DATA,
		.ops = &char_port_ops,
		.minor = 1,
		.name = "brom_download",
	}, {
		.tx_ch = 0xffff,
		.rx_ch = 0xffff,
		.txq_index = 1,
		.rxq_index = 1,
		.txq_exp_index = 1,
		.rxq_exp_index = 1,
		.path_id = ID_CLDMA0,
		.flags = PORT_F_RX_CHAR_NODE | PORT_F_RAW_DATA,
		.ops = &char_port_ops,
		.minor = 21,
		.name = "ttyDUMP",
	},
};

static struct t7xx_port *t7xx_proxy_get_port_by_ch(struct port_proxy *port_prox, enum port_ch ch)
{
	struct t7xx_port_static *port_static;
	struct t7xx_port *port;
	int i;

	for_each_proxy_port(i, port, port_prox) {
		port_static = port->port_static;
		if (port_static->rx_ch == ch || port_static->tx_ch == ch)
			return port;
	}

	return NULL;
}

/**
 * port_proxy_recv_skb_from_q() - receive raw data from dedicated queue
 * @queue: CLDMA queue
 * @skb: socket buffer
 *
 * Return: 0 for success or error code for drops
 */
static int port_proxy_recv_skb_from_q(struct cldma_queue *queue, struct sk_buff *skb)
{
	struct t7xx_port *port;
	struct t7xx_port_static *port_static;
	int ret = 0;

	port = port_prox->dedicated_ports[queue->hif_id][queue->index];
	port_static = port->port_static;

	if (skb && port_static->ops->recv_skb)
		ret = port_static->ops->recv_skb(port, skb);

	if (ret < 0 && ret != -ENOBUFS) {
		dev_err(port->dev, "drop on RX ch %d, ret %d\n", port_static->rx_ch, ret);
		dev_kfree_skb_any(skb);
		return -ENETDOWN;
	}

	return ret;
}

/* Sequence numbering to track for lost packets */
void t7xx_port_proxy_set_seq_num(struct t7xx_port *port, struct ccci_header *ccci_h)
{
	if (ccci_h && port) {
		ccci_h->status &= cpu_to_le32(~HDR_FLD_SEQ);
		ccci_h->status |= cpu_to_le32(FIELD_PREP(HDR_FLD_SEQ, port->seq_nums[MTK_TX]));
		ccci_h->status &= cpu_to_le32(~HDR_FLD_AST);
		ccci_h->status |= cpu_to_le32(FIELD_PREP(HDR_FLD_AST, 1));
	}
}

static u16 t7xx_port_check_rx_seq_num(struct t7xx_port *port, struct ccci_header *ccci_h)
{
	u16 seq_num, assert_bit;

	seq_num = FIELD_GET(HDR_FLD_SEQ, le32_to_cpu(ccci_h->status));
	assert_bit = FIELD_GET(HDR_FLD_AST, le32_to_cpu(ccci_h->status));
	if (assert_bit && port->seq_nums[MTK_RX] &&
	    ((seq_num - port->seq_nums[MTK_RX]) & CHECK_RX_SEQ_MASK) != 1) {
		dev_warn_ratelimited(port->dev,
				     "seq num out-of-order %d->%d (header %X, len %X)\n",
				     seq_num, port->seq_nums[MTK_RX],
				     le32_to_cpu(ccci_h->packet_header),
				     le32_to_cpu(ccci_h->packet_len));
	}

	return seq_num;
}

void t7xx_port_proxy_reset(struct port_proxy *port_prox)
{
	struct t7xx_port *port;
	int i;

	for_each_proxy_port(i, port, port_prox) {
		port->seq_nums[MTK_RX] = -1;
		port->seq_nums[MTK_TX] = 0;
	}
}

static int t7xx_port_get_queue_no(struct t7xx_port *port)
{
	struct t7xx_port_static *port_static = port->port_static;
	struct t7xx_fsm_ctl *ctl = port->t7xx_dev->md->fsm_ctl;

	return t7xx_fsm_get_md_state(ctl) == MD_STATE_EXCEPTION ?
		port_static->txq_exp_index : port_static->txq_index;
}

static void t7xx_port_struct_init(struct t7xx_port *port)
{
	INIT_LIST_HEAD(&port->entry);
	INIT_LIST_HEAD(&port->queue_entry);
	skb_queue_head_init(&port->rx_skb_list);
	init_waitqueue_head(&port->rx_wq);
	port->seq_nums[MTK_RX] = -1;
	port->seq_nums[MTK_TX] = 0;
	atomic_set(&port->usage_cnt, 0);
	port->port_proxy = port_prox;
}

static void t7xx_port_adjust_skb(struct t7xx_port *port, struct sk_buff *skb)
{
	struct ccci_header *ccci_h = (struct ccci_header *)skb->data;
	struct t7xx_port_static *port_static = port->port_static;

	if (port->flags & PORT_F_USER_HEADER) {
		if (le32_to_cpu(ccci_h->packet_header) == CCCI_HEADER_NO_DATA) {
			if (skb->len > sizeof(*ccci_h)) {
				dev_err_ratelimited(port->dev,
						    "Recv unexpected data for %s, skb->len=%d\n",
						    port_static->name, skb->len);
				skb_trim(skb, sizeof(*ccci_h));
			}
		}
	} else {
		skb_pull(skb, sizeof(*ccci_h));
	}
}

/**
 * t7xx_port_recv_skb() - receive skb from modem or HIF.
 * @port: port to use.
 * @skb: skb to use.
 *
 * Used to receive native HIF RX data, which has same the RX receive flow.
 *
 * Return:
 * * 0		- Success.
 * * -ENOBUFS	- Not enough queue length.
 */
int t7xx_port_recv_skb(struct t7xx_port *port, struct sk_buff *skb)
{
	unsigned long flags;

	spin_lock_irqsave(&port->rx_wq.lock, flags);
	if (port->rx_skb_list.qlen < port->rx_length_th) {
		struct ccci_header *ccci_h = (struct ccci_header *)skb->data;
		u32 status;

		port->flags &= ~PORT_F_RX_FULLED;
		if (port->flags & PORT_F_RX_ADJUST_HEADER)
			t7xx_port_adjust_skb(port, skb);

		status = FIELD_GET(HDR_FLD_CHN, le32_to_cpu(ccci_h->status));
		if (!(port->flags & PORT_F_RAW_DATA) && status == PORT_CH_STATUS_RX) {
			port->skb_handler(port, skb);
		} else {
			if (port->wwan_port)
				wwan_port_rx(port->wwan_port, skb);
			else
				__skb_queue_tail(&port->rx_skb_list, skb);
		}

		spin_unlock_irqrestore(&port->rx_wq.lock, flags);
		wake_up_all(&port->rx_wq);
		return 0;
	}

	port->flags |= PORT_F_RX_FULLED;
	spin_unlock_irqrestore(&port->rx_wq.lock, flags);
	return -ENOBUFS;
}

/**
 * t7xx_port_kthread_handler() - Kthread handler for specific port.
 * @arg: Port pointer.
 *
 * Receive native HIF RX data, which have same RX receive flow.
 *
 * Return: Always 0 to kthread_run.
 */
int t7xx_port_kthread_handler(void *arg)
{
	while (!kthread_should_stop()) {
		struct t7xx_port *port = arg;
		struct sk_buff *skb;
		unsigned long flags;

		spin_lock_irqsave(&port->rx_wq.lock, flags);
		if (skb_queue_empty(&port->rx_skb_list) &&
		    wait_event_interruptible_locked_irq(port->rx_wq,
							!skb_queue_empty(&port->rx_skb_list) ||
							kthread_should_stop())) {
			spin_unlock_irqrestore(&port->rx_wq.lock, flags);
			continue;
		} else if (kthread_should_stop()) {
			spin_unlock_irqrestore(&port->rx_wq.lock, flags);
			break;
		}

		skb = __skb_dequeue(&port->rx_skb_list);
		spin_unlock_irqrestore(&port->rx_wq.lock, flags);

		if (port->skb_handler)
			port->skb_handler(port, skb);
	}

	return 0;
}

static struct cldma_ctrl *get_md_ctrl(struct t7xx_port *port)
{
	enum cldma_id id = port->port_static->path_id;

	return port->t7xx_dev->md->md_ctrl[id];
}

int t7xx_port_write_room_to_md(struct t7xx_port *port)
{
	struct cldma_ctrl *md_ctrl = get_md_ctrl(port);

	return t7xx_cldma_write_room(md_ctrl, t7xx_port_get_queue_no(port));
}

int t7xx_port_proxy_send_skb(struct t7xx_port *port, struct sk_buff *skb)
{
	struct ccci_header *ccci_h = (struct ccci_header *)(skb->data);
	struct cldma_ctrl *md_ctrl;
	unsigned char tx_qno;
	int ret;

	tx_qno = t7xx_port_get_queue_no(port);
	t7xx_port_proxy_set_seq_num(port, ccci_h);

	md_ctrl = get_md_ctrl(port);
	ret = t7xx_cldma_send_skb(md_ctrl, tx_qno, skb, true);
	if (ret) {
		dev_err(port->dev, "Failed to send skb: %d\n", ret);
		return ret;
	}

	/* Record the port seq_num after the data is sent to HIF.
	 * Only bits 0-14 are used, thus negating overflow.
	 */
	port->seq_nums[MTK_TX]++;

	return 0;
}

int t7xx_port_send_skb_to_md(struct t7xx_port *port, struct sk_buff *skb, bool blocking)
{
	struct t7xx_port_static *port_static = port->port_static;
	struct t7xx_fsm_ctl *ctl = port->t7xx_dev->md->fsm_ctl;
	struct cldma_ctrl *md_ctrl;
	enum md_state md_state;
	unsigned int fsm_state;

	md_state = t7xx_fsm_get_md_state(ctl);

	fsm_state = t7xx_fsm_get_ctl_state(ctl);
	if (fsm_state != FSM_STATE_PRE_START) {
		if (md_state == MD_STATE_WAITING_FOR_HS1 || md_state == MD_STATE_WAITING_FOR_HS2)
			return -ENODEV;

		if (md_state == MD_STATE_EXCEPTION && port_static->tx_ch != PORT_CH_MD_LOG_TX &&
		    port_static->tx_ch != PORT_CH_UART1_TX)
			return -ETXTBSY;

		if (md_state == MD_STATE_STOPPED || md_state == MD_STATE_WAITING_TO_STOP ||
		    md_state == MD_STATE_INVALID)
			return -ENODEV;
	}

	md_ctrl = get_md_ctrl(port);
	return t7xx_cldma_send_skb(md_ctrl, t7xx_port_get_queue_no(port), skb, blocking);
}

static void t7xx_proxy_setup_ch_mapping(struct port_proxy *port_prox)
{
	struct t7xx_port *port;

	int i, j;

	for (i = 0; i < ARRAY_SIZE(port_prox->rx_ch_ports); i++)
		INIT_LIST_HEAD(&port_prox->rx_ch_ports[i]);

	for (j = 0; j < ARRAY_SIZE(port_prox->queue_ports); j++) {
		for (i = 0; i < ARRAY_SIZE(port_prox->queue_ports[j]); i++)
			INIT_LIST_HEAD(&port_prox->queue_ports[j][i]);
	}

	for_each_proxy_port(i, port, port_prox) {
		struct t7xx_port_static *port_static = port->port_static;
		enum cldma_id path_id = port_static->path_id;
		u8 ch_id;

		ch_id = FIELD_GET(PORT_CH_ID_MASK, port_static->rx_ch);
		list_add_tail(&port->entry, &port_prox->rx_ch_ports[ch_id]);
		list_add_tail(&port->queue_entry,
			      &port_prox->queue_ports[path_id][port_static->rxq_index]);
	}
}

void t7xx_port_proxy_send_msg_to_md(struct port_proxy *port_prox, enum port_ch ch,
				    unsigned int msg, unsigned int ex_msg)
{
	struct ctrl_msg_header *ctrl_msg_h;
	struct ccci_header *ccci_h;
	struct t7xx_port *port;
	struct sk_buff *skb;
	int ret;

	port = t7xx_proxy_get_port_by_ch(port_prox, ch);
	if (!port)
		return;

	skb = __dev_alloc_skb(sizeof(*ccci_h), GFP_KERNEL);
	if (!skb)
		return;

	if (ch == PORT_CH_CONTROL_TX) {
		ccci_h = (struct ccci_header *)(skb->data);
		ccci_h->packet_header = cpu_to_le32(CCCI_HEADER_NO_DATA);
		ccci_h->packet_len = cpu_to_le32(sizeof(*ctrl_msg_h) + CCCI_H_LEN);
		ccci_h->status &= cpu_to_le32(~HDR_FLD_CHN);
		ccci_h->status |= cpu_to_le32(FIELD_PREP(HDR_FLD_CHN, ch));
		ccci_h->ex_msg = 0;
		ctrl_msg_h = (struct ctrl_msg_header *)(skb->data + CCCI_H_LEN);
		ctrl_msg_h->data_length = 0;
		ctrl_msg_h->ex_msg = cpu_to_le32(ex_msg);
		ctrl_msg_h->ctrl_msg_id = cpu_to_le32(msg);
		skb_put(skb, CCCI_H_LEN + sizeof(*ctrl_msg_h));
	} else {
		ccci_h = skb_put(skb, sizeof(*ccci_h));
		ccci_h->packet_header = cpu_to_le32(CCCI_HEADER_NO_DATA);
		ccci_h->packet_len = cpu_to_le32(msg);
		ccci_h->status &= cpu_to_le32(~HDR_FLD_CHN);
		ccci_h->status |= cpu_to_le32(FIELD_PREP(HDR_FLD_CHN, ch));
		ccci_h->ex_msg = cpu_to_le32(ex_msg);
	}

	ret = t7xx_port_proxy_send_skb(port, skb);
	if (ret) {
		struct t7xx_port_static *port_static = port->port_static;

		dev_err(port->dev, "port%s send to MD fail\n", port_static->name);
		dev_kfree_skb_any(skb);
	}
}

/**
 * t7xx_port_proxy_dispatch_recv_skb() - Dispatch received skb.
 * @queue: CLDMA queue.
 * @skb: Socket buffer.
 * @drop_skb_on_err: Return value that indicates in case of an error that the skb should be dropped.
 *
 * If recv_skb return with 0 or drop_skb_on_err is true, then it's the port's duty
 * to free the request and the caller should no longer reference the request.
 * If recv_skb returns any other error, caller should free the request.
 *
 * Return:
 ** 0		- Success.
 ** -EINVAL	- Failed to get skb, channel out-of-range, or invalid MD state.
 ** -ENETDOWN	- Network time out.
 */
static int t7xx_port_proxy_dispatch_recv_skb(struct cldma_queue *queue, struct sk_buff *skb,
					     bool *drop_skb_on_err)
{
	struct ccci_header *ccci_h = (struct ccci_header *)skb->data;
	struct port_proxy *port_prox = queue->md->port_prox;
	struct t7xx_fsm_ctl *ctl = queue->md->fsm_ctl;
	struct list_head *port_list;
	struct t7xx_port *port;
	u16 seq_num, channel;
	int ret = 0;
	u8 ch_id;

	channel = FIELD_GET(HDR_FLD_CHN, le32_to_cpu(ccci_h->status));
	ch_id = FIELD_GET(PORT_CH_ID_MASK, channel);

	if (t7xx_fsm_get_md_state(ctl) == MD_STATE_INVALID) {
		*drop_skb_on_err = true;
		return -EINVAL;
	}

	port_list = &port_prox->rx_ch_ports[ch_id];
	list_for_each_entry(port, port_list, entry) {
		struct t7xx_port_static *port_static = port->port_static;

		if (queue->md_ctrl->hif_id != port_static->path_id || channel !=
		    port_static->rx_ch)
			continue;

		/* Multi-cast is not supported, because one port may be freed and can modify
		 * this request before another port can process it.
		 * However we still can use req->state to do some kind of multi-cast if needed.
		 */
		if (port_static->ops->recv_skb) {
			seq_num = t7xx_port_check_rx_seq_num(port, ccci_h);
			ret = port_static->ops->recv_skb(port, skb);
			/* If the packet is stored to RX buffer successfully or dropped,
			 * the sequence number will be updated.
			 */
			if (ret == -ENETDOWN || (ret < 0 && port->flags & PORT_F_RX_ALLOW_DROP)) {
				*drop_skb_on_err = true;
				dev_err_ratelimited(port->dev,
						    "port %s RX full, drop packet\n",
						    port_static->name);
			}

			if (!ret || drop_skb_on_err)
				port->seq_nums[MTK_RX] = seq_num;
		}

		break;
	}

	return ret;
}

static int t7xx_port_proxy_recv_skb(struct cldma_queue *queue, struct sk_buff *skb)
{
	bool drop_skb_on_err = false;
	int ret;

	if (!skb)
		return -EINVAL;

	if (queue->q_type == CLDMA_SHARED_Q) {
		ret = t7xx_port_proxy_dispatch_recv_skb(queue, skb, &drop_skb_on_err);
		if (ret < 0 && drop_skb_on_err) {
			dev_kfree_skb_any(skb);
			return 0;
		}
	} else {
		ret = port_proxy_recv_skb_from_q(queue, skb);
	}

	return ret;
}

/**
 * t7xx_port_proxy_md_status_notify() - Notify all ports of state.
 *@port_prox: The port_proxy pointer.
 *@state: State.
 *
 * Called by t7xx_fsm. Used to dispatch modem status for all ports,
 * which want to know MD state transition.
 */
void t7xx_port_proxy_md_status_notify(struct port_proxy *port_prox, unsigned int state)
{
	struct t7xx_port *port;
	int i;

	for_each_proxy_port(i, port, port_prox) {
		struct t7xx_port_static *port_static = port->port_static;

		if (port_static->ops->md_state_notify)
			port_static->ops->md_state_notify(port, state);
	}
}

static void t7xx_proxy_init_all_ports(struct t7xx_modem *md)
{
	struct port_proxy *port_proxy = md->port_prox;
	struct t7xx_port *port;
	int i;

	for_each_proxy_port(i, port, port_proxy) {
		struct t7xx_port_static *port_static = port->port_static;

		t7xx_port_struct_init(port);

		if (port_static->tx_ch == PORT_CH_CONTROL_TX)
			md->core_md.ctl_port = port;

		if (port_static->tx_ch == CCCI_SAP_CONTROL_TX)
			md->core_sap.ctl_port = port;

		port_static->major = port_prox->major;
		port_static->minor_base = port_prox->minor_base;

		port->t7xx_dev = md->t7xx_dev;
		port->dev = &md->t7xx_dev->pdev->dev;
		spin_lock_init(&port->port_update_lock);
		spin_lock(&port->port_update_lock);
		mutex_init(&port->tx_mutex_lock);

		if (port->flags & PORT_F_CHAR_NODE_SHOW)
			port->chan_enable = true;
		else
			port->chan_enable = false;

		port->chn_crt_stat = false;
		spin_unlock(&port->port_update_lock);

		if (port_static->ops->init)
			port_static->ops->init(port);

		if (port->flags & PORT_F_RAW_DATA) {
			unsigned int index = port_static->rxq_index;
			unsigned int id = port_static->path_id;

			port_proxy->dedicated_ports[id][index] = port;
		}
	}

	t7xx_proxy_setup_ch_mapping(port_proxy);
}

static int port_get_cfg(struct t7xx_port_static **ports, enum port_cfg_id port_cfg_id)
{
	int port_number = 0;

	switch (port_cfg_id) {
	case PORT_CFG0:
		*ports = t7xx_md_ports;
		port_number = ARRAY_SIZE(t7xx_md_ports);
		break;
	case PORT_CFG1:
		*ports = md_ccci_early_ports;
		port_number = ARRAY_SIZE(md_ccci_early_ports);
		break;
	default:
		*ports = NULL;
		port_number = 0;
		break;
	}
	return port_number;
}

void port_switch_cfg(struct t7xx_modem *md, enum port_cfg_id cfg_id)
{
	struct port_proxy *port_proxy = md->port_prox;
	struct device *dev = &md->t7xx_dev->pdev->dev;
	struct t7xx_port_static *port_static;
	struct t7xx_port *ports_private;
	struct t7xx_port *port;
	int i;

	if (port_proxy->current_cfg_id != cfg_id) {
		port_proxy->current_cfg_id = cfg_id;
		for_each_proxy_port(i, port, port_proxy) {
			port_static = port->port_static;
			port_static->ops->uninit(port);
		}

		port_proxy->port_number = port_get_cfg(&port_proxy->ports_shared, cfg_id);

		devm_kfree(dev, port_proxy->ports_private);

		ports_private = devm_kzalloc(dev, sizeof(*ports_private) * port_proxy->port_number,
					     GFP_KERNEL);
		if (!ports_private) {
			dev_err(dev, "no memory for ports !\n");
			return;
		}

		for (i = 0; i < port_proxy->port_number; i++) {
			ports_private[i].port_static = &port_proxy->ports_shared[i];
			ports_private[i].flags = port_proxy->ports_shared[i].flags;
		}

		port_proxy->ports_private = ports_private;
		t7xx_proxy_init_all_ports(md);
	}
}

static struct t7xx_port *proxy_get_port_by_minor(int minor)
{
	struct t7xx_port *port;
	struct t7xx_port_static *port_static;
	int i;

	for_each_proxy_port(i, port, port_prox) {
		port_static = port->port_static;
		if (port_static->minor == minor)
			return port;
	}

	return NULL;
}

struct t7xx_port *port_proxy_get_port(int major, int minor)
{
	if (port_prox && port_prox->major == major)
		return proxy_get_port_by_minor(minor);

	return NULL;
}

struct t7xx_port *port_get_by_minor(int minor)
{
	return proxy_get_port_by_minor(minor);
}

struct t7xx_port *port_get_by_name(char *port_name)
{
	struct t7xx_port *port;
	struct t7xx_port_static *port_static;
	int i;

	if (!port_prox)
		return NULL;

	for_each_proxy_port(i, port, port_prox) {
		port_static = port->port_static;
		if (!strncmp(port_static->name, port_name, strlen(port_static->name)))
			return port;
	}

	return NULL;
}

int port_register_device(const char *name, int major, int minor)
{
	struct device *dev;

	dev = device_create(dev_class, NULL, MKDEV(major, minor), NULL, "%s", name);

	return PTR_ERR_OR_ZERO(dev);
}

void port_unregister_device(int major, int minor)
{
	device_destroy(dev_class, MKDEV(major, minor));
}

static int port_netlink_send_msg(struct t7xx_port *port, int grp, const char *buf, size_t len)
{
	struct port_proxy *pprox;
	struct sk_buff *nl_skb;
	struct nlmsghdr *nlh;

	nl_skb = nlmsg_new(len, GFP_KERNEL);
	if (!nl_skb)
		return -ENOMEM;

	nlh = nlmsg_put(nl_skb, 0, 1, NLMSG_DONE, len, 0);
	if (!nlh) {
		dev_err(port->dev, "could not release netlink\n");
		nlmsg_free(nl_skb);
		return -EFAULT;
	}

	/* Add new netlink message to the skb
	 * after checking if header+payload
	 * can be handled.
	 */
	memcpy(nlmsg_data(nlh), buf, len);

	pprox = port_prox;
	return netlink_broadcast(pprox->netlink_sock, nl_skb, 0, grp, GFP_KERNEL);
}

int port_proxy_broadcast_state(struct t7xx_port *port, int state)
{
	char msg[PORT_NETLINK_MSG_MAX_PAYLOAD];
	struct t7xx_port_static *port_static = port->port_static;

	if (state >= MTK_PORT_STATE_INVALID)
		return -EINVAL;

	switch (state) {
	case MTK_PORT_STATE_ENABLE:
		snprintf(msg, sizeof(msg), "enable %s", port_static->name);
		break;

	case MTK_PORT_STATE_DISABLE:
		snprintf(msg, sizeof(msg), "disable %s", port_static->name);
		break;

	default:
		snprintf(msg, sizeof(msg), "invalid operation");
		break;
	}

	return port_netlink_send_msg(port, PORT_STATE_BROADCAST_GROUP, msg, strlen(msg) + 1);
}

static int proxy_register_char_dev(void)
{
	dev_t dev = 0;
	int ret;

	if (port_prox->major) {
		dev = MKDEV(port_prox->major, port_prox->minor_base);
		ret = register_chrdev_region(dev, TTY_IPC_MINOR_BASE, DEVICE_NAME);
	} else {
		ret = alloc_chrdev_region(&dev, port_prox->minor_base,
					  TTY_IPC_MINOR_BASE, DEVICE_NAME);
		if (ret)
			dev_err(port_prox->dev, "failed to alloc chrdev region, ret=%d\n", ret);

		port_prox->major = MAJOR(dev);
	}

	return ret;
}

static int t7xx_proxy_alloc(struct t7xx_modem *md, enum port_cfg_id cfg_id)
{
	unsigned int port_number;
	struct device *dev = &md->t7xx_dev->pdev->dev;
	struct t7xx_port *ports_private;
	struct port_proxy *l_port_prox;
	int i, ret;

	l_port_prox = devm_kzalloc(dev, sizeof(*l_port_prox), GFP_KERNEL);
	if (!l_port_prox)
		return -ENOMEM;

	md->port_prox = l_port_prox;
	port_prox = l_port_prox;
	l_port_prox->dev = dev;

	ret = proxy_register_char_dev();
	if (ret)
		return ret;

	l_port_prox->port_number = port_get_cfg(&l_port_prox->ports_shared, cfg_id);
	port_number = l_port_prox->port_number;

	ports_private = devm_kzalloc(dev, sizeof(*ports_private) * port_number, GFP_KERNEL);
	if (!ports_private)
		return -ENOMEM;

	for (i = 0; i < port_number; i++) {
		ports_private[i].port_static = &l_port_prox->ports_shared[i];
		ports_private[i].flags = l_port_prox->ports_shared[i].flags;
	}

	l_port_prox->ports_private = ports_private;
	l_port_prox->current_cfg_id = cfg_id;
	t7xx_proxy_init_all_ports(md);
	return 0;
};

static int port_netlink_init(void)
{
	port_prox->netlink_sock = netlink_kernel_create(&init_net, PORT_NOTIFY_PROTOCOL, NULL);

	if (!port_prox->netlink_sock) {
		dev_err(port_prox->dev, "failed to create netlink socket\n");
		return -ENOMEM;
	}

	return 0;
}

static void port_netlink_uninit(void)
{
	netlink_kernel_release(port_prox->netlink_sock);
	port_prox->netlink_sock = NULL;
}

/**
 * t7xx_port_proxy_init() - Initialize ports.
 * @md: Modem.
 *
 * Create all port instances.
 *
 * Return:
 * * 0		- Success.
 * * -ERROR	- Error code from failure sub-initializations.
 */
int t7xx_port_proxy_init(struct t7xx_modem *md)
{
	int ret;

	dev_class = class_create(THIS_MODULE, "ccci_node");
	if (IS_ERR(dev_class))
		return PTR_ERR(dev_class);

	ret = t7xx_proxy_alloc(md, PORT_CFG1);
	if (ret)
		goto err_proxy;

	ret = port_netlink_init();
	if (ret)
		goto err_netlink;

	t7xx_cldma_set_recv_skb(md->md_ctrl[ID_CLDMA0], t7xx_port_proxy_recv_skb);
	t7xx_cldma_set_recv_skb(md->md_ctrl[ID_CLDMA1], t7xx_port_proxy_recv_skb);
	return 0;

err_netlink:
	t7xx_port_proxy_uninit(port_prox);
err_proxy:
	class_destroy(dev_class);
	return ret;
}

void t7xx_port_proxy_uninit(struct port_proxy *port_prox)
{
	struct t7xx_port *port;
	int i;

	for_each_proxy_port(i, port, port_prox) {
		struct t7xx_port_static *port_static = port->port_static;

		if (port_static->ops->uninit)
			port_static->ops->uninit(port);
	}

	unregister_chrdev_region(MKDEV(port_prox->major, port_prox->minor_base),
				 TTY_IPC_MINOR_BASE);
	port_netlink_uninit();
	class_destroy(dev_class);
}

/**
 * t7xx_port_proxy_node_control() - Create/remove node.
 * @md: Modem.
 * @port_msg: Message.
 *
 * Used to control create/remove device node.
 *
 * Return:
 * * 0		- Success.
 * * -EFAULT	- Message check failure.
 */
int t7xx_port_proxy_node_control(struct t7xx_modem *md, struct port_msg *port_msg)
{
	u32 *port_info_base = (void *)port_msg + sizeof(*port_msg);
	struct device *dev = &md->t7xx_dev->pdev->dev;
	unsigned int ports, i;
	unsigned int version;

	version = FIELD_GET(PORT_MSG_VERSION, le32_to_cpu(port_msg->info));
	if (version != PORT_ENUM_VER ||
	    le32_to_cpu(port_msg->head_pattern) != PORT_ENUM_HEAD_PATTERN ||
	    le32_to_cpu(port_msg->tail_pattern) != PORT_ENUM_TAIL_PATTERN) {
		dev_err(dev, "Port message enumeration invalid %x:%x:%x\n",
			version, le32_to_cpu(port_msg->head_pattern),
			le32_to_cpu(port_msg->tail_pattern));
		return -EFAULT;
	}

	ports = FIELD_GET(PORT_MSG_PRT_CNT, le32_to_cpu(port_msg->info));

	for (i = 0; i < ports; i++) {
		struct t7xx_port_static *port_static;
		u32 *port_info = port_info_base + i;
		struct t7xx_port *port;
		unsigned int ch_id;
		bool en_flag;

		ch_id = FIELD_GET(PORT_INFO_CH_ID, *port_info);
		port = t7xx_proxy_get_port_by_ch(md->port_prox, ch_id);
		if (!port) {
			dev_warn(dev, "Port:%x not found\n", ch_id);
			continue;
		}

		en_flag = !!FIELD_GET(PORT_INFO_ENFLG, *port_info);

		if (t7xx_fsm_get_md_state(md->fsm_ctl) == MD_STATE_READY) {
			port_static = port->port_static;

			if (en_flag) {
				if (port_static->ops->enable_chl)
					port_static->ops->enable_chl(port);
			} else {
				if (port_static->ops->disable_chl)
					port_static->ops->disable_chl(port);
			}
		} else {
			port->chan_enable = en_flag;
		}
	}

	return 0;
}

int port_ee_disable_wwan(void)
{
	struct t7xx_port *port;
	int i;

	if (!port_prox) {
		pr_notice("port_status notify: proxy not initiated\n");
		return -EFAULT;
	}

	/* port uninit */
	for_each_proxy_port(i, port, port_prox) {
		if (port->wwan_port)
			wwan_port_txoff(port->wwan_port);
	}

	return 0;
}
