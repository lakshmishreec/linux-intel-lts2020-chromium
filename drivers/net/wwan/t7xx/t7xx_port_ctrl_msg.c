// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, MediaTek Inc.
 * Copyright (c) 2021, Intel Corporation.
 *
 * Authors:
 *  Haijun Liu <haijun.liu@mediatek.com>
 *  Ricardo Martinez<ricardo.martinez@linux.intel.com>
 *  Moises Veleta <moises.veleta@intel.com>
 *
 * Contributors:
 *  Amir Hanania <amir.hanania@intel.com>
 *  Chiranjeevi Rapolu <chiranjeevi.rapolu@intel.com>
 *  Eliot Lee <eliot.lee@intel.com>
 *  Sreehari Kancharla <sreehari.kancharla@intel.com>
 */

#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>

#include "t7xx_common.h"
#include "t7xx_port.h"
#include "t7xx_port_proxy.h"
#include "t7xx_state_monitor.h"

static void fsm_ee_message_handler(struct t7xx_fsm_ctl *ctl, struct sk_buff *skb)
{
	struct ctrl_msg_header *ctrl_msg_h = (struct ctrl_msg_header *)skb->data;
	struct device *dev = &ctl->md->t7xx_dev->pdev->dev;
	struct port_proxy *port_prox = ctl->md->port_prox;
	enum md_state md_state;

	md_state = t7xx_fsm_get_md_state(ctl);
	if (md_state != MD_STATE_EXCEPTION) {
		dev_err(dev, "Receive invalid MD_EX %x when MD state is %d\n",
			ctrl_msg_h->ex_msg, md_state);
		return;
	}

	switch (le32_to_cpu(ctrl_msg_h->ctrl_msg_id)) {
	case CTL_ID_MD_EX:
		if (le32_to_cpu(ctrl_msg_h->ex_msg) != MD_EX_CHK_ID) {
			dev_err(dev, "Receive invalid MD_EX %x\n", ctrl_msg_h->ex_msg);
		} else {
			t7xx_port_proxy_send_msg_to_md(port_prox, PORT_CH_CONTROL_TX, CTL_ID_MD_EX,
						       MD_EX_CHK_ID);
			t7xx_fsm_append_event(ctl, FSM_EVENT_MD_EX, NULL, 0);
		}

		break;

	case CTL_ID_MD_EX_ACK:
		if (le32_to_cpu(ctrl_msg_h->ex_msg) != MD_EX_CHK_ACK_ID)
			dev_err(dev, "Receive invalid MD_EX_ACK %x\n", ctrl_msg_h->ex_msg);
		else
			t7xx_fsm_append_event(ctl, FSM_EVENT_MD_EX_REC_OK, NULL, 0);

		break;

	case CTL_ID_MD_EX_PASS:
		t7xx_fsm_append_event(ctl, FSM_EVENT_MD_EX_PASS, NULL, 0);
		break;

	case CTL_ID_DRV_VER_ERROR:
		dev_err(dev, "AP/MD driver version mismatch\n");
	}
}

static void control_msg_handler(struct t7xx_port *port, struct sk_buff *skb)
{
	struct t7xx_port_static *port_static = port->port_static;
	struct t7xx_fsm_ctl *ctl = port->t7xx_dev->md->fsm_ctl;
	struct port_proxy *port_prox = ctl->md->port_prox;
	struct ctrl_msg_header *ctrl_msg_h;
	int ret = 0;

	skb_pull(skb, sizeof(struct ccci_header));

	ctrl_msg_h = (struct ctrl_msg_header *)skb->data;
	switch (le32_to_cpu(ctrl_msg_h->ctrl_msg_id)) {
	case CTL_ID_HS2_MSG:
		skb_pull(skb, sizeof(*ctrl_msg_h));

		if (port_static->rx_ch == PORT_CH_CONTROL_RX)
			t7xx_fsm_append_event(ctl, FSM_EVENT_MD_HS2,
					      skb->data, le32_to_cpu(ctrl_msg_h->data_length));

		if (port_static->rx_ch == CCCI_SAP_CONTROL_RX)
			t7xx_fsm_append_event(ctl, FSM_EVENT_AP_HS2,
					      skb->data, le32_to_cpu(ctrl_msg_h->data_length));

		dev_kfree_skb_any(skb);
		break;

	case CTL_ID_MD_EX:
	case CTL_ID_MD_EX_ACK:
	case CTL_ID_MD_EX_PASS:
	case CTL_ID_DRV_VER_ERROR:
		fsm_ee_message_handler(ctl, skb);
		dev_kfree_skb_any(skb);
		break;

	case CTL_ID_PORT_ENUM:
		skb_pull(skb, sizeof(*ctrl_msg_h));
		ret = t7xx_port_proxy_node_control(ctl->md, (struct port_msg *)skb->data);
		if (!ret)
			t7xx_port_proxy_send_msg_to_md(port_prox, PORT_CH_CONTROL_TX,
						       CTL_ID_PORT_ENUM, 0);
		else
			t7xx_port_proxy_send_msg_to_md(port_prox, PORT_CH_CONTROL_TX,
						       CTL_ID_PORT_ENUM, PORT_ENUM_VER_MISMATCH);

		break;

	default:
		dev_err(port->dev, "Unknown control message ID to FSM %x\n",
			le32_to_cpu(ctrl_msg_h->ctrl_msg_id));
		break;
	}

	if (ret)
		dev_err(port->dev, "%s control message handle error: %d\n", port_static->name,
			ret);
}

static int port_ctl_init(struct t7xx_port *port)
{
	struct t7xx_port_static *port_static = port->port_static;

	port->skb_handler = &control_msg_handler;
	port->thread = kthread_run(t7xx_port_kthread_handler, port, "%s", port_static->name);
	if (IS_ERR(port->thread)) {
		dev_err(port->dev, "Failed to start port control thread\n");
		return PTR_ERR(port->thread);
	}

	port->rx_length_th = MAX_CTRL_QUEUE_LENGTH;
	return 0;
}

static void port_ctl_uninit(struct t7xx_port *port)
{
	unsigned long flags;
	struct sk_buff *skb;

	if (port->thread)
		kthread_stop(port->thread);

	spin_lock_irqsave(&port->rx_wq.lock, flags);
	while ((skb = __skb_dequeue(&port->rx_skb_list)) != NULL)
		dev_kfree_skb_any(skb);

	spin_unlock_irqrestore(&port->rx_wq.lock, flags);
}

struct port_ops ctl_port_ops = {
	.init = &port_ctl_init,
	.recv_skb = &t7xx_port_recv_skb,
	.uninit = &port_ctl_uninit,
};
