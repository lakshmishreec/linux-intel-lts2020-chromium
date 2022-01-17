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

#ifndef __T7XX_PORT_PROXY_H__
#define __T7XX_PORT_PROXY_H__

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/skbuff.h>
#include <linux/types.h>

#include "t7xx_common.h"
#include "t7xx_modem_ops.h"
#include "t7xx_port.h"

/* CCCI logic channel enable & disable flag */
#define CCCI_CHAN_ENABLE	1
#define CCCI_CHAN_DISABLE	0

#define MTK_MAX_QUEUE_NUM	16
#define MAX_RX_QUEUE_LENGTH	32
#define MAX_CTRL_QUEUE_LENGTH	16

#define MTK_PORT_STATE_ENABLE	0
#define MTK_PORT_STATE_DISABLE	1
#define MTK_PORT_STATE_INVALID	2

#define CLDMA_TXQ_MTU		MTK_SKB_4K

#define PORT_NETLINK_MSG_MAX_PAYLOAD           32
#define PORT_STATE_BROADCAST_GROUP		21
#define CCCI_MTU		3568 /* 3.5kB -16 */

struct port_proxy {
	int				port_number;
	struct t7xx_port_static		*ports_shared;
	struct t7xx_port		*ports_private;
	struct t7xx_port		*dedicated_ports[CLDMA_NUM][MTK_MAX_QUEUE_NUM];
	struct list_head		rx_ch_ports[PORT_CH_ID_MASK];
	struct list_head		queue_ports[CLDMA_NUM][MTK_MAX_QUEUE_NUM];
	struct device			*dev;
	unsigned char			current_cfg_id;
	unsigned int			major;
	unsigned int			minor_base;
	struct sock			*netlink_sock;
};

struct ctrl_msg_header {
	__le32	ctrl_msg_id;
	__le32	ex_msg;
	__le32	data_length;
};

struct port_msg {
	__le32	head_pattern;
	__le32	info;
	__le32	tail_pattern;
};

enum port_cfg_id {
	PORT_CFG0,
	PORT_CFG1,
};

#define PORT_INFO_RSRVD		GENMASK(31, 16)
#define PORT_INFO_ENFLG		GENMASK(15, 15)
#define PORT_INFO_CH_ID		GENMASK(14, 0)

#define PORT_MSG_VERSION	GENMASK(31, 16)
#define PORT_MSG_PRT_CNT	GENMASK(15, 0)

#define PORT_ENUM_VER		0
#define PORT_ENUM_HEAD_PATTERN	0x5a5a5a5a
#define PORT_ENUM_TAIL_PATTERN	0xa5a5a5a5
#define PORT_ENUM_VER_MISMATCH	0x00657272

/* Port operations mapping */
extern struct port_ops wwan_sub_port_ops;
extern struct port_ops ctl_port_ops;
extern struct port_ops char_port_ops;
extern struct port_ops tty_port_ops;
extern struct tty_dev_ops tty_ops;

int t7xx_port_proxy_send_skb(struct t7xx_port *port, struct sk_buff *skb);
void t7xx_port_proxy_set_seq_num(struct t7xx_port *port, struct ccci_header *ccci_h);
int t7xx_port_proxy_node_control(struct t7xx_modem *md, struct port_msg *port_msg);
void t7xx_port_proxy_reset(struct port_proxy *port_prox);
void t7xx_port_proxy_send_msg_to_md(struct port_proxy *port_prox, enum port_ch ch,
				    unsigned int msg, unsigned int ex_msg);
void t7xx_port_proxy_uninit(struct port_proxy *port_prox);
int t7xx_port_proxy_init(struct t7xx_modem *md);
void t7xx_port_proxy_md_status_notify(struct port_proxy *port_prox, unsigned int state);
void port_switch_cfg(struct t7xx_modem *md, enum port_cfg_id cfg_id);
struct t7xx_port *port_proxy_get_port(int major, int minor);
int port_proxy_broadcast_state(struct t7xx_port *port, int state);
struct t7xx_port *port_get_by_name(char *port_name);
struct t7xx_port *port_get_by_minor(int minor);
int port_ee_disable_wwan(void);

#endif /* __T7XX_PORT_PROXY_H__ */
