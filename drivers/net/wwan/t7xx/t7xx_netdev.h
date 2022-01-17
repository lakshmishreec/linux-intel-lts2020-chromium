/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2021, MediaTek Inc.
 * Copyright (c) 2021, Intel Corporation.
 *
 * Authors:
 *  Haijun Liu <haijun.liu@mediatek.com>
 *  Moises Veleta <moises.veleta@intel.com>
 *
 * Contributors:
 *  Amir Hanania <amir.hanania@intel.com>
 *  Chiranjeevi Rapolu <chiranjeevi.rapolu@intel.com>
 *  Ricardo Martinez<ricardo.martinez@linux.intel.com>
 */

#ifndef __T7XX_NETDEV_H__
#define __T7XX_NETDEV_H__

#include <linux/bits.h>
#include <linux/netdevice.h>
#include <linux/types.h>

#include "t7xx_common.h"
#include "t7xx_hif_dpmaif.h"
#include "t7xx_pci.h"
#include "t7xx_state_monitor.h"

#define RXQ_NUM				DPMAIF_RXQ_NUM
#define NIC_DEV_MAX			21
#define NIC_DEV_DEFAULT			2
#define NIC_CAP_TXBUSY_STOP		BIT(0)
#define NIC_CAP_SGIO			BIT(1)
#define NIC_CAP_DATA_ACK_DVD		BIT(2)
#define NIC_CAP_CCMNI_MQ		BIT(3)

/* Must be less than DPMAIF_HW_MTU_SIZE (3*1024 + 8) */
#define CCMNI_MTU_MAX			3000
#define CCMNI_NETDEV_WDT_TO		(1 * HZ)

struct t7xx_ccmni {
	u8				index;
	atomic_t			usage;
	struct net_device		*dev;
	struct t7xx_ccmni_ctrl		*ctlb;
};

struct t7xx_ccmni_ctrl {
	struct t7xx_pci_dev		*t7xx_dev;
	struct dpmaif_ctrl		*hif_ctrl;
	struct t7xx_ccmni		*ccmni_inst[NIC_DEV_MAX];
	struct dpmaif_callbacks		callbacks;
	unsigned int			nic_dev_num;
	unsigned int			md_sta;
	unsigned int			capability;
	struct t7xx_fsm_notifier	md_status_notify;
};

int t7xx_ccmni_init(struct t7xx_pci_dev *t7xx_dev);
void t7xx_ccmni_exit(struct t7xx_pci_dev *t7xx_dev);
int t7xx_ccmni_late_init(struct t7xx_pci_dev *mtk_dev);

#endif /* __T7XX_NETDEV_H__ */
