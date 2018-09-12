/**************************************************************************/
/*                                                                        */
/*  IBM System i and System p Virtual NIC Device Driver                   */
/*  Copyright (C) 2014 IBM Corp.                                          */
/*  Santiago Leon (santi_leon@yahoo.com)                                  */
/*  Thomas Falcon (tlfalcon@linux.vnet.ibm.com)                           */
/*  John Allen (jallen@linux.vnet.ibm.com)                                */
/*                                                                        */
/*  This program is free software; you can redistribute it and/or modify  */
/*  it under the terms of the GNU General Public License as published by  */
/*  the Free Software Foundation; either version 2 of the License, or     */
/*  (at your option) any later version.                                   */
/*                                                                        */
/*  This program is distributed in the hope that it will be useful,       */
/*  but WITHOUT ANY WARRANTY; without even the implied warranty of        */
/*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         */
/*  GNU General Public License for more details.                          */
/*                                                                        */
/*  You should have received a copy of the GNU General Public License     */
/*  along with this program.                                              */
/*                                                                        */
/* This module contains the implementation of a virtual ethernet device   */
/* for use with IBM i/p Series LPAR Linux. It utilizes the logical LAN    */
/* option of the RS/6000 Platform Architecture to interface with virtual  */
/* ethernet NICs that are presented to the partition by the hypervisor.   */
/*									   */
/* Messages are passed between the VNIC driver and the VNIC server using  */
/* Command/Response Queues (CRQs) and sub CRQs (sCRQs). CRQs are used to  */
/* issue and receive commands that initiate communication with the server */
/* on driver initialization. Sub CRQs (sCRQs) are similar to CRQs, but    */
/* are used by the driver to notify the server that a packet is           */
/* ready for transmission or that a buffer has been added to receive a    */
/* packet. Subsequently, sCRQs are used by the server to notify the       */
/* driver that a packet transmission has been completed or that a packet  */
/* has been received and placed in a waiting buffer.                      */
/*                                                                        */
/* In lieu of a more conventional "on-the-fly" DMA mapping strategy in    */
/* which skbs are DMA mapped and immediately unmapped when the transmit   */
/* or receive has been completed, the VNIC driver is required to use      */
/* "long term mapping". This entails that large, continuous DMA mapped    */
/* buffers are allocated on driver initialization and these buffers are   */
/* then continuously reused to pass skbs to and from the VNIC server.     */
/*                                                                        */
/**************************************************************************/

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/completion.h>
#include <linux/ioport.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/ethtool.h>
#include <linux/proc_fs.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/irq.h>
#include <linux/kthread.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <net/net_namespace.h>
#include <asm/hvcall.h>
#include <linux/atomic.h>
#include <asm/vio.h>
#include <asm/iommu.h>
#include <linux/uaccess.h>
#include <asm/firmware.h>
#include <linux/workqueue.h>
#include <linux/if_vlan.h>

#include "ibmvnic.h"

static const char ibmvnic_driver_name[] = "ibmvnic";
static const char ibmvnic_driver_string[] = "IBM System i/p Virtual NIC Driver";

MODULE_AUTHOR("Santiago Leon");
MODULE_DESCRIPTION("IBM System i/p Virtual NIC Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(IBMVNIC_DRIVER_VERSION);

static int ibmvnic_version = IBMVNIC_INITIAL_VERSION;
static int ibmvnic_remove(struct vio_dev *);
static void release_sub_crqs(struct ibmvnic_adapter *);
static int ibmvnic_reset_crq(struct ibmvnic_adapter *);
static int ibmvnic_send_crq_init(struct ibmvnic_adapter *);
static int ibmvnic_reenable_crq_queue(struct ibmvnic_adapter *);
static int ibmvnic_send_crq(struct ibmvnic_adapter *, union ibmvnic_crq *);
static int send_subcrq(struct ibmvnic_adapter *adapter, u64 remote_handle,
		       union sub_crq *sub_crq);
static int send_subcrq_indirect(struct ibmvnic_adapter *, u64, u64, u64);
static irqreturn_t ibmvnic_interrupt_rx(int irq, void *instance);
static int enable_scrq_irq(struct ibmvnic_adapter *,
			   struct ibmvnic_sub_crq_queue *);
static int disable_scrq_irq(struct ibmvnic_adapter *,
			    struct ibmvnic_sub_crq_queue *);
static int pending_scrq(struct ibmvnic_adapter *,
			struct ibmvnic_sub_crq_queue *);
static union sub_crq *ibmvnic_next_scrq(struct ibmvnic_adapter *,
					struct ibmvnic_sub_crq_queue *);
static int ibmvnic_poll(struct napi_struct *napi, int data);
static void send_map_query(struct ibmvnic_adapter *adapter);
static void send_request_map(struct ibmvnic_adapter *, dma_addr_t, __be32, u8);
static void send_request_unmap(struct ibmvnic_adapter *, u8);
static void send_login(struct ibmvnic_adapter *adapter);
static void send_cap_queries(struct ibmvnic_adapter *adapter);
static int init_sub_crq_irqs(struct ibmvnic_adapter *adapter);
static int ibmvnic_init(struct ibmvnic_adapter *);
static void release_crq_queue(struct ibmvnic_adapter *);

struct ibmvnic_stat {
	char name[ETH_GSTRING_LEN];
	int offset;
};

#define IBMVNIC_STAT_OFF(stat) (offsetof(struct ibmvnic_adapter, stats) + \
			     offsetof(struct ibmvnic_statistics, stat))
#define IBMVNIC_GET_STAT(a, off) (*((u64 *)(((unsigned long)(a)) + off)))

static const struct ibmvnic_stat ibmvnic_stats[] = {
	{"rx_packets", IBMVNIC_STAT_OFF(rx_packets)},
	{"rx_bytes", IBMVNIC_STAT_OFF(rx_bytes)},
	{"tx_packets", IBMVNIC_STAT_OFF(tx_packets)},
	{"tx_bytes", IBMVNIC_STAT_OFF(tx_bytes)},
	{"ucast_tx_packets", IBMVNIC_STAT_OFF(ucast_tx_packets)},
	{"ucast_rx_packets", IBMVNIC_STAT_OFF(ucast_rx_packets)},
	{"mcast_tx_packets", IBMVNIC_STAT_OFF(mcast_tx_packets)},
	{"mcast_rx_packets", IBMVNIC_STAT_OFF(mcast_rx_packets)},
	{"bcast_tx_packets", IBMVNIC_STAT_OFF(bcast_tx_packets)},
	{"bcast_rx_packets", IBMVNIC_STAT_OFF(bcast_rx_packets)},
	{"align_errors", IBMVNIC_STAT_OFF(align_errors)},
	{"fcs_errors", IBMVNIC_STAT_OFF(fcs_errors)},
	{"single_collision_frames", IBMVNIC_STAT_OFF(single_collision_frames)},
	{"multi_collision_frames", IBMVNIC_STAT_OFF(multi_collision_frames)},
	{"sqe_test_errors", IBMVNIC_STAT_OFF(sqe_test_errors)},
	{"deferred_tx", IBMVNIC_STAT_OFF(deferred_tx)},
	{"late_collisions", IBMVNIC_STAT_OFF(late_collisions)},
	{"excess_collisions", IBMVNIC_STAT_OFF(excess_collisions)},
	{"internal_mac_tx_errors", IBMVNIC_STAT_OFF(internal_mac_tx_errors)},
	{"carrier_sense", IBMVNIC_STAT_OFF(carrier_sense)},
	{"too_long_frames", IBMVNIC_STAT_OFF(too_long_frames)},
	{"internal_mac_rx_errors", IBMVNIC_STAT_OFF(internal_mac_rx_errors)},
};

static long h_reg_sub_crq(unsigned long unit_address, unsigned long token,
			  unsigned long length, unsigned long *number,
			  unsigned long *irq)
{
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE];
	long rc;

	rc = plpar_hcall(H_REG_SUB_CRQ, retbuf, unit_address, token, length);
	*number = retbuf[0];
	*irq = retbuf[1];

	return rc;
}

static int alloc_long_term_buff(struct ibmvnic_adapter *adapter,
				struct ibmvnic_long_term_buff *ltb, int size)
{
	struct device *dev = &adapter->vdev->dev;

	ltb->size = size;
	ltb->buff = dma_alloc_coherent(dev, ltb->size, &ltb->addr,
				       GFP_KERNEL);

	if (!ltb->buff) {
		dev_err(dev, "Couldn't alloc long term buffer\n");
		return -ENOMEM;
	}
	ltb->map_id = adapter->map_id;
	adapter->map_id++;

	init_completion(&adapter->fw_done);
	send_request_map(adapter, ltb->addr,
			 ltb->size, ltb->map_id);
	wait_for_completion(&adapter->fw_done);
	return 0;
}

static void free_long_term_buff(struct ibmvnic_adapter *adapter,
				struct ibmvnic_long_term_buff *ltb)
{
	struct device *dev = &adapter->vdev->dev;

	if (!ltb->buff)
		return;

	if (adapter->reset_reason != VNIC_RESET_FAILOVER &&
	    adapter->reset_reason != VNIC_RESET_MOBILITY)
		send_request_unmap(adapter, ltb->map_id);
	dma_free_coherent(dev, ltb->size, ltb->buff, ltb->addr);
}

static void replenish_rx_pool(struct ibmvnic_adapter *adapter,
			      struct ibmvnic_rx_pool *pool)
{
	int count = pool->size - atomic_read(&pool->available);
	struct device *dev = &adapter->vdev->dev;
	int buffers_added = 0;
	unsigned long lpar_rc;
	union sub_crq sub_crq;
	struct sk_buff *skb;
	unsigned int offset;
	dma_addr_t dma_addr;
	unsigned char *dst;
	u64 *handle_array;
	int shift = 0;
	int index;
	int i;

	handle_array = (u64 *)((u8 *)(adapter->login_rsp_buf) +
				      be32_to_cpu(adapter->login_rsp_buf->
				      off_rxadd_subcrqs));

	for (i = 0; i < count; ++i) {
		skb = alloc_skb(pool->buff_size, GFP_ATOMIC);
		if (!skb) {
			dev_err(dev, "Couldn't replenish rx buff\n");
			adapter->replenish_no_mem++;
			break;
		}

		index = pool->free_map[pool->next_free];

		if (pool->rx_buff[index].skb)
			dev_err(dev, "Inconsistent free_map!\n");

		/* Copy the skb to the long term mapped DMA buffer */
		offset = index * pool->buff_size;
		dst = pool->long_term_buff.buff + offset;
		memset(dst, 0, pool->buff_size);
		dma_addr = pool->long_term_buff.addr + offset;
		pool->rx_buff[index].data = dst;

		pool->free_map[pool->next_free] = IBMVNIC_INVALID_MAP;
		pool->rx_buff[index].dma = dma_addr;
		pool->rx_buff[index].skb = skb;
		pool->rx_buff[index].pool_index = pool->index;
		pool->rx_buff[index].size = pool->buff_size;

		memset(&sub_crq, 0, sizeof(sub_crq));
		sub_crq.rx_add.first = IBMVNIC_CRQ_CMD;
		sub_crq.rx_add.correlator =
		    cpu_to_be64((u64)&pool->rx_buff[index]);
		sub_crq.rx_add.ioba = cpu_to_be32(dma_addr);
		sub_crq.rx_add.map_id = pool->long_term_buff.map_id;

		/* The length field of the sCRQ is defined to be 24 bits so the
		 * buffer size needs to be left shifted by a byte before it is
		 * converted to big endian to prevent the last byte from being
		 * truncated.
		 */
#ifdef __LITTLE_ENDIAN__
		shift = 8;
#endif
		sub_crq.rx_add.len = cpu_to_be32(pool->buff_size << shift);

		lpar_rc = send_subcrq(adapter, handle_array[pool->index],
				      &sub_crq);
		if (lpar_rc != H_SUCCESS)
			goto failure;

		buffers_added++;
		adapter->replenish_add_buff_success++;
		pool->next_free = (pool->next_free + 1) % pool->size;
	}
	atomic_add(buffers_added, &pool->available);
	return;

failure:
	dev_info(dev, "replenish pools failure\n");
	pool->free_map[pool->next_free] = index;
	pool->rx_buff[index].skb = NULL;
	if (!dma_mapping_error(dev, dma_addr))
		dma_unmap_single(dev, dma_addr, pool->buff_size,
				 DMA_FROM_DEVICE);

	dev_kfree_skb_any(skb);
	adapter->replenish_add_buff_failure++;
	atomic_add(buffers_added, &pool->available);
}

static void replenish_pools(struct ibmvnic_adapter *adapter)
{
	int i;

	adapter->replenish_task_cycles++;
	for (i = 0; i < be32_to_cpu(adapter->login_rsp_buf->num_rxadd_subcrqs);
	     i++) {
		if (adapter->rx_pool[i].active)
			replenish_rx_pool(adapter, &adapter->rx_pool[i]);
	}
}

static void release_stats_token(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;

	if (!adapter->stats_token)
		return;

	dma_unmap_single(dev, adapter->stats_token,
			 sizeof(struct ibmvnic_statistics),
			 DMA_FROM_DEVICE);
	adapter->stats_token = 0;
}

static int init_stats_token(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	dma_addr_t stok;

	stok = dma_map_single(dev, &adapter->stats,
			      sizeof(struct ibmvnic_statistics),
			      DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, stok)) {
		dev_err(dev, "Couldn't map stats buffer\n");
		return -1;
	}

	adapter->stats_token = stok;
	return 0;
}

static void release_rx_pools(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_rx_pool *rx_pool;
	int rx_scrqs;
	int i, j;

	if (!adapter->rx_pool)
		return;

	rx_scrqs = be32_to_cpu(adapter->login_rsp_buf->num_rxadd_subcrqs);
	for (i = 0; i < rx_scrqs; i++) {
		rx_pool = &adapter->rx_pool[i];

		kfree(rx_pool->free_map);
		free_long_term_buff(adapter, &rx_pool->long_term_buff);

		if (!rx_pool->rx_buff)
			continue;

		for (j = 0; j < rx_pool->size; j++) {
			if (rx_pool->rx_buff[j].skb) {
				dev_kfree_skb_any(rx_pool->rx_buff[i].skb);
				rx_pool->rx_buff[i].skb = NULL;
			}
		}

		kfree(rx_pool->rx_buff);
	}

	kfree(adapter->rx_pool);
	adapter->rx_pool = NULL;
}

static int init_rx_pools(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_rx_pool *rx_pool;
	int rxadd_subcrqs;
	u64 *size_array;
	int i, j;

	rxadd_subcrqs =
		be32_to_cpu(adapter->login_rsp_buf->num_rxadd_subcrqs);
	size_array = (u64 *)((u8 *)(adapter->login_rsp_buf) +
		be32_to_cpu(adapter->login_rsp_buf->off_rxadd_buff_size));

	adapter->rx_pool = kcalloc(rxadd_subcrqs,
				   sizeof(struct ibmvnic_rx_pool),
				   GFP_KERNEL);
	if (!adapter->rx_pool) {
		dev_err(dev, "Failed to allocate rx pools\n");
		return -1;
	}

	for (i = 0; i < rxadd_subcrqs; i++) {
		rx_pool = &adapter->rx_pool[i];

		netdev_dbg(adapter->netdev,
			   "Initializing rx_pool %d, %lld buffs, %lld bytes each\n",
			   i, adapter->req_rx_add_entries_per_subcrq,
			   be64_to_cpu(size_array[i]));

		rx_pool->size = adapter->req_rx_add_entries_per_subcrq;
		rx_pool->index = i;
		rx_pool->buff_size = be64_to_cpu(size_array[i]);
		rx_pool->active = 1;

		rx_pool->free_map = kcalloc(rx_pool->size, sizeof(int),
					    GFP_KERNEL);
		if (!rx_pool->free_map) {
			release_rx_pools(adapter);
			return -1;
		}

		rx_pool->rx_buff = kcalloc(rx_pool->size,
					   sizeof(struct ibmvnic_rx_buff),
					   GFP_KERNEL);
		if (!rx_pool->rx_buff) {
			dev_err(dev, "Couldn't alloc rx buffers\n");
			release_rx_pools(adapter);
			return -1;
		}

		if (alloc_long_term_buff(adapter, &rx_pool->long_term_buff,
					 rx_pool->size * rx_pool->buff_size)) {
			release_rx_pools(adapter);
			return -1;
		}

		for (j = 0; j < rx_pool->size; ++j)
			rx_pool->free_map[j] = j;

		atomic_set(&rx_pool->available, 0);
		rx_pool->next_alloc = 0;
		rx_pool->next_free = 0;
	}

	return 0;
}

static void release_tx_pools(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_tx_pool *tx_pool;
	int i, tx_scrqs;

	if (!adapter->tx_pool)
		return;

	tx_scrqs = be32_to_cpu(adapter->login_rsp_buf->num_txsubm_subcrqs);
	for (i = 0; i < tx_scrqs; i++) {
		tx_pool = &adapter->tx_pool[i];
		kfree(tx_pool->tx_buff);
		free_long_term_buff(adapter, &tx_pool->long_term_buff);
		kfree(tx_pool->free_map);
	}

	kfree(adapter->tx_pool);
	adapter->tx_pool = NULL;
}

static int init_tx_pools(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_tx_pool *tx_pool;
	int tx_subcrqs;
	int i, j;

	tx_subcrqs = be32_to_cpu(adapter->login_rsp_buf->num_txsubm_subcrqs);
	adapter->tx_pool = kcalloc(tx_subcrqs,
				   sizeof(struct ibmvnic_tx_pool), GFP_KERNEL);
	if (!adapter->tx_pool)
		return -1;

	for (i = 0; i < tx_subcrqs; i++) {
		tx_pool = &adapter->tx_pool[i];
		tx_pool->tx_buff = kcalloc(adapter->req_tx_entries_per_subcrq,
					   sizeof(struct ibmvnic_tx_buff),
					   GFP_KERNEL);
		if (!tx_pool->tx_buff) {
			dev_err(dev, "tx pool buffer allocation failed\n");
			release_tx_pools(adapter);
			return -1;
		}

		if (alloc_long_term_buff(adapter, &tx_pool->long_term_buff,
					 adapter->req_tx_entries_per_subcrq *
					 adapter->req_mtu)) {
			release_tx_pools(adapter);
			return -1;
		}

		tx_pool->free_map = kcalloc(adapter->req_tx_entries_per_subcrq,
					    sizeof(int), GFP_KERNEL);
		if (!tx_pool->free_map) {
			release_tx_pools(adapter);
			return -1;
		}

		for (j = 0; j < adapter->req_tx_entries_per_subcrq; j++)
			tx_pool->free_map[j] = j;

		tx_pool->consumer_index = 0;
		tx_pool->producer_index = 0;
	}

	return 0;
}

static void release_error_buffers(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_error_buff *error_buff, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&adapter->error_list_lock, flags);
	list_for_each_entry_safe(error_buff, tmp, &adapter->errors, list) {
		list_del(&error_buff->list);
		dma_unmap_single(dev, error_buff->dma, error_buff->len,
				 DMA_FROM_DEVICE);
		kfree(error_buff->buff);
		kfree(error_buff);
	}
	spin_unlock_irqrestore(&adapter->error_list_lock, flags);
}

static int ibmvnic_login(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	unsigned long timeout = msecs_to_jiffies(30000);
	struct device *dev = &adapter->vdev->dev;

	do {
		if (adapter->renegotiate) {
			adapter->renegotiate = false;
			release_sub_crqs(adapter);

			reinit_completion(&adapter->init_done);
			send_cap_queries(adapter);
			if (!wait_for_completion_timeout(&adapter->init_done,
							 timeout)) {
				dev_err(dev, "Capabilities query timeout\n");
				return -1;
			}
		}

		reinit_completion(&adapter->init_done);
		send_login(adapter);
		if (!wait_for_completion_timeout(&adapter->init_done,
						 timeout)) {
			dev_err(dev, "Login timeout\n");
			return -1;
		}
	} while (adapter->renegotiate);

	return 0;
}

static void release_resources(struct ibmvnic_adapter *adapter)
{
	int i;

	release_tx_pools(adapter);
	release_rx_pools(adapter);

	release_stats_token(adapter);
	release_error_buffers(adapter);

	if (adapter->napi) {
		for (i = 0; i < adapter->req_rx_queues; i++) {
			if (&adapter->napi[i])
				netif_napi_del(&adapter->napi[i]);
		}
	}
}

static int set_link_state(struct ibmvnic_adapter *adapter, u8 link_state)
{
	struct net_device *netdev = adapter->netdev;
	unsigned long timeout = msecs_to_jiffies(30000);
	union ibmvnic_crq crq;
	bool resend;
	int rc;

	netdev_err(netdev, "setting link state %d\n", link_state);
	memset(&crq, 0, sizeof(crq));
	crq.logical_link_state.first = IBMVNIC_CRQ_CMD;
	crq.logical_link_state.cmd = LOGICAL_LINK_STATE;
	crq.logical_link_state.link_state = link_state;

	do {
		resend = false;

		reinit_completion(&adapter->init_done);
		rc = ibmvnic_send_crq(adapter, &crq);
		if (rc) {
			netdev_err(netdev, "Failed to set link state\n");
			return rc;
		}

		if (!wait_for_completion_timeout(&adapter->init_done,
						 timeout)) {
			netdev_err(netdev, "timeout setting link state\n");
			return -1;
		}

		if (adapter->init_done_rc == 1) {
			/* Partuial success, delay and re-send */
			mdelay(1000);
			resend = true;
		}
	} while (resend);

	return 0;
}

static int set_real_num_queues(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int rc;

	rc = netif_set_real_num_tx_queues(netdev, adapter->req_tx_queues);
	if (rc) {
		netdev_err(netdev, "failed to set the number of tx queues\n");
		return rc;
	}

	rc = netif_set_real_num_rx_queues(netdev, adapter->req_rx_queues);
	if (rc)
		netdev_err(netdev, "failed to set the number of rx queues\n");

	return rc;
}

static int init_resources(struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int i, rc;

	rc = set_real_num_queues(netdev);
	if (rc)
		return rc;

	rc = init_sub_crq_irqs(adapter);
	if (rc) {
		netdev_err(netdev, "failed to initialize sub crq irqs\n");
		return -1;
	}

	rc = init_stats_token(adapter);
	if (rc)
		return rc;

	adapter->map_id = 1;
	adapter->napi = kcalloc(adapter->req_rx_queues,
				sizeof(struct napi_struct), GFP_KERNEL);
	if (!adapter->napi)
		return -ENOMEM;

	for (i = 0; i < adapter->req_rx_queues; i++) {
		netif_napi_add(netdev, &adapter->napi[i], ibmvnic_poll,
			       NAPI_POLL_WEIGHT);
	}

	send_map_query(adapter);

	rc = init_rx_pools(netdev);
	if (rc)
		return rc;

	rc = init_tx_pools(netdev);
	return rc;
}

static int __ibmvnic_open(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	enum vnic_state prev_state = adapter->state;
	int i, rc;

	adapter->state = VNIC_OPENING;
	replenish_pools(adapter);

	for (i = 0; i < adapter->req_rx_queues; i++)
		napi_enable(&adapter->napi[i]);

	/* We're ready to receive frames, enable the sub-crq interrupts and
	 * set the logical link state to up
	 */
	for (i = 0; i < adapter->req_rx_queues; i++) {
		if (prev_state == VNIC_CLOSED)
			enable_irq(adapter->rx_scrq[i]->irq);
		else
			enable_scrq_irq(adapter, adapter->rx_scrq[i]);
	}

	for (i = 0; i < adapter->req_tx_queues; i++) {
		if (prev_state == VNIC_CLOSED)
			enable_irq(adapter->tx_scrq[i]->irq);
		else
			enable_scrq_irq(adapter, adapter->tx_scrq[i]);
	}

	rc = set_link_state(adapter, IBMVNIC_LOGICAL_LNK_UP);
	if (rc) {
		for (i = 0; i < adapter->req_rx_queues; i++)
			napi_disable(&adapter->napi[i]);
		release_resources(adapter);
		return rc;
	}

	netif_tx_start_all_queues(netdev);

	if (prev_state == VNIC_CLOSED) {
		for (i = 0; i < adapter->req_rx_queues; i++)
			napi_schedule(&adapter->napi[i]);
	}

	adapter->state = VNIC_OPEN;
	return rc;
}

static int ibmvnic_open(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int rc;

	mutex_lock(&adapter->reset_lock);

	if (adapter->state != VNIC_CLOSED) {
		rc = ibmvnic_login(netdev);
		if (rc) {
			mutex_unlock(&adapter->reset_lock);
			return rc;
		}

		rc = init_resources(adapter);
		if (rc) {
			netdev_err(netdev, "failed to initialize resources\n");
			release_resources(adapter);
			mutex_unlock(&adapter->reset_lock);
			return rc;
		}
	}

	rc = __ibmvnic_open(netdev);
	mutex_unlock(&adapter->reset_lock);

	return rc;
}

static void clean_tx_pools(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_tx_pool *tx_pool;
	u64 tx_entries;
	int tx_scrqs;
	int i, j;

	if (!adapter->tx_pool)
		return;

	tx_scrqs = be32_to_cpu(adapter->login_rsp_buf->num_txsubm_subcrqs);
	tx_entries = adapter->req_tx_entries_per_subcrq;

	/* Free any remaining skbs in the tx buffer pools */
	for (i = 0; i < tx_scrqs; i++) {
		tx_pool = &adapter->tx_pool[i];
		if (!tx_pool)
			continue;

		for (j = 0; j < tx_entries; j++) {
			if (tx_pool->tx_buff[j].skb) {
				dev_kfree_skb_any(tx_pool->tx_buff[j].skb);
				tx_pool->tx_buff[j].skb = NULL;
			}
		}
	}
}

static int __ibmvnic_close(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int rc = 0;
	int i;

	adapter->state = VNIC_CLOSING;
	netif_tx_stop_all_queues(netdev);

	if (adapter->napi) {
		for (i = 0; i < adapter->req_rx_queues; i++)
			napi_disable(&adapter->napi[i]);
	}

	clean_tx_pools(adapter);

	if (adapter->tx_scrq) {
		for (i = 0; i < adapter->req_tx_queues; i++)
			if (adapter->tx_scrq[i]->irq)
				disable_irq(adapter->tx_scrq[i]->irq);
	}

	rc = set_link_state(adapter, IBMVNIC_LOGICAL_LNK_DN);
	if (rc)
		return rc;

	if (adapter->rx_scrq) {
		for (i = 0; i < adapter->req_rx_queues; i++) {
			int retries = 10;

			while (pending_scrq(adapter, adapter->rx_scrq[i])) {
				retries--;
				mdelay(100);

				if (retries == 0)
					break;
			}

			if (adapter->rx_scrq[i]->irq)
				disable_irq(adapter->rx_scrq[i]->irq);
		}
	}

	adapter->state = VNIC_CLOSED;
	return rc;
}

static int ibmvnic_close(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int rc;

	mutex_lock(&adapter->reset_lock);
	rc = __ibmvnic_close(netdev);
	mutex_unlock(&adapter->reset_lock);

	return rc;
}

/**
 * build_hdr_data - creates L2/L3/L4 header data buffer
 * @hdr_field - bitfield determining needed headers
 * @skb - socket buffer
 * @hdr_len - array of header lengths
 * @tot_len - total length of data
 *
 * Reads hdr_field to determine which headers are needed by firmware.
 * Builds a buffer containing these headers.  Saves individual header
 * lengths and total buffer length to be used to build descriptors.
 */
static int build_hdr_data(u8 hdr_field, struct sk_buff *skb,
			  int *hdr_len, u8 *hdr_data)
{
	int len = 0;
	u8 *hdr;

	hdr_len[0] = sizeof(struct ethhdr);

	if (skb->protocol == htons(ETH_P_IP)) {
		hdr_len[1] = ip_hdr(skb)->ihl * 4;
		if (ip_hdr(skb)->protocol == IPPROTO_TCP)
			hdr_len[2] = tcp_hdrlen(skb);
		else if (ip_hdr(skb)->protocol == IPPROTO_UDP)
			hdr_len[2] = sizeof(struct udphdr);
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
		hdr_len[1] = sizeof(struct ipv6hdr);
		if (ipv6_hdr(skb)->nexthdr == IPPROTO_TCP)
			hdr_len[2] = tcp_hdrlen(skb);
		else if (ipv6_hdr(skb)->nexthdr == IPPROTO_UDP)
			hdr_len[2] = sizeof(struct udphdr);
	}

	memset(hdr_data, 0, 120);
	if ((hdr_field >> 6) & 1) {
		hdr = skb_mac_header(skb);
		memcpy(hdr_data, hdr, hdr_len[0]);
		len += hdr_len[0];
	}

	if ((hdr_field >> 5) & 1) {
		hdr = skb_network_header(skb);
		memcpy(hdr_data + len, hdr, hdr_len[1]);
		len += hdr_len[1];
	}

	if ((hdr_field >> 4) & 1) {
		hdr = skb_transport_header(skb);
		memcpy(hdr_data + len, hdr, hdr_len[2]);
		len += hdr_len[2];
	}
	return len;
}

/**
 * create_hdr_descs - create header and header extension descriptors
 * @hdr_field - bitfield determining needed headers
 * @data - buffer containing header data
 * @len - length of data buffer
 * @hdr_len - array of individual header lengths
 * @scrq_arr - descriptor array
 *
 * Creates header and, if needed, header extension descriptors and
 * places them in a descriptor array, scrq_arr
 */

static void create_hdr_descs(u8 hdr_field, u8 *hdr_data, int len, int *hdr_len,
			     union sub_crq *scrq_arr)
{
	union sub_crq hdr_desc;
	int tmp_len = len;
	u8 *data, *cur;
	int tmp;

	while (tmp_len > 0) {
		cur = hdr_data + len - tmp_len;

		memset(&hdr_desc, 0, sizeof(hdr_desc));
		if (cur != hdr_data) {
			data = hdr_desc.hdr_ext.data;
			tmp = tmp_len > 29 ? 29 : tmp_len;
			hdr_desc.hdr_ext.first = IBMVNIC_CRQ_CMD;
			hdr_desc.hdr_ext.type = IBMVNIC_HDR_EXT_DESC;
			hdr_desc.hdr_ext.len = tmp;
		} else {
			data = hdr_desc.hdr.data;
			tmp = tmp_len > 24 ? 24 : tmp_len;
			hdr_desc.hdr.first = IBMVNIC_CRQ_CMD;
			hdr_desc.hdr.type = IBMVNIC_HDR_DESC;
			hdr_desc.hdr.len = tmp;
			hdr_desc.hdr.l2_len = (u8)hdr_len[0];
			hdr_desc.hdr.l3_len = cpu_to_be16((u16)hdr_len[1]);
			hdr_desc.hdr.l4_len = (u8)hdr_len[2];
			hdr_desc.hdr.flag = hdr_field << 1;
		}
		memcpy(data, cur, tmp);
		tmp_len -= tmp;
		*scrq_arr = hdr_desc;
		scrq_arr++;
	}
}

/**
 * build_hdr_descs_arr - build a header descriptor array
 * @skb - socket buffer
 * @num_entries - number of descriptors to be sent
 * @subcrq - first TX descriptor
 * @hdr_field - bit field determining which headers will be sent
 *
 * This function will build a TX descriptor array with applicable
 * L2/L3/L4 packet header descriptors to be sent by send_subcrq_indirect.
 */

static void build_hdr_descs_arr(struct ibmvnic_tx_buff *txbuff,
				int *num_entries, u8 hdr_field)
{
	int hdr_len[3] = {0, 0, 0};
	int tot_len, len;
	u8 *hdr_data = txbuff->hdr_data;

	tot_len = build_hdr_data(hdr_field, txbuff->skb, hdr_len,
				 txbuff->hdr_data);
	len = tot_len;
	len -= 24;
	if (len > 0)
		num_entries += len % 29 ? len / 29 + 1 : len / 29;
	create_hdr_descs(hdr_field, hdr_data, tot_len, hdr_len,
			 txbuff->indir_arr + 1);
}

static int ibmvnic_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int queue_num = skb_get_queue_mapping(skb);
	u8 *hdrs = (u8 *)&adapter->tx_rx_desc_req;
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_tx_buff *tx_buff = NULL;
	struct ibmvnic_sub_crq_queue *tx_scrq;
	struct ibmvnic_tx_pool *tx_pool;
	unsigned int tx_send_failed = 0;
	unsigned int tx_map_failed = 0;
	unsigned int tx_dropped = 0;
	unsigned int tx_packets = 0;
	unsigned int tx_bytes = 0;
	dma_addr_t data_dma_addr;
	struct netdev_queue *txq;
	unsigned long lpar_rc;
	union sub_crq tx_crq;
	unsigned int offset;
	int num_entries = 1;
	unsigned char *dst;
	u64 *handle_array;
	int index = 0;
	int ret = 0;

	if (adapter->resetting) {
		if (!netif_subqueue_stopped(netdev, skb))
			netif_stop_subqueue(netdev, queue_num);
		dev_kfree_skb_any(skb);

		tx_send_failed++;
		tx_dropped++;
		ret = NETDEV_TX_OK;
		goto out;
	}

	tx_pool = &adapter->tx_pool[queue_num];
	tx_scrq = adapter->tx_scrq[queue_num];
	txq = netdev_get_tx_queue(netdev, skb_get_queue_mapping(skb));
	handle_array = (u64 *)((u8 *)(adapter->login_rsp_buf) +
		be32_to_cpu(adapter->login_rsp_buf->off_txsubm_subcrqs));

	index = tx_pool->free_map[tx_pool->consumer_index];
	offset = index * adapter->req_mtu;
	dst = tx_pool->long_term_buff.buff + offset;
	memset(dst, 0, adapter->req_mtu);
	skb_copy_from_linear_data(skb, dst, skb->len);
	data_dma_addr = tx_pool->long_term_buff.addr + offset;

	tx_pool->consumer_index =
	    (tx_pool->consumer_index + 1) %
		adapter->req_tx_entries_per_subcrq;

	tx_buff = &tx_pool->tx_buff[index];
	tx_buff->skb = skb;
	tx_buff->data_dma[0] = data_dma_addr;
	tx_buff->data_len[0] = skb->len;
	tx_buff->index = index;
	tx_buff->pool_index = queue_num;
	tx_buff->last_frag = true;

	memset(&tx_crq, 0, sizeof(tx_crq));
	tx_crq.v1.first = IBMVNIC_CRQ_CMD;
	tx_crq.v1.type = IBMVNIC_TX_DESC;
	tx_crq.v1.n_crq_elem = 1;
	tx_crq.v1.n_sge = 1;
	tx_crq.v1.flags1 = IBMVNIC_TX_COMP_NEEDED;
	tx_crq.v1.correlator = cpu_to_be32(index);
	tx_crq.v1.dma_reg = cpu_to_be16(tx_pool->long_term_buff.map_id);
	tx_crq.v1.sge_len = cpu_to_be32(skb->len);
	tx_crq.v1.ioba = cpu_to_be64(data_dma_addr);

	if (adapter->vlan_header_insertion) {
		tx_crq.v1.flags2 |= IBMVNIC_TX_VLAN_INSERT;
		tx_crq.v1.vlan_id = cpu_to_be16(skb->vlan_tci);
	}

	if (skb->protocol == htons(ETH_P_IP)) {
		if (ip_hdr(skb)->version == 4)
			tx_crq.v1.flags1 |= IBMVNIC_TX_PROT_IPV4;
		else if (ip_hdr(skb)->version == 6)
			tx_crq.v1.flags1 |= IBMVNIC_TX_PROT_IPV6;

		if (ip_hdr(skb)->protocol == IPPROTO_TCP)
			tx_crq.v1.flags1 |= IBMVNIC_TX_PROT_TCP;
		else if (ip_hdr(skb)->protocol != IPPROTO_TCP)
			tx_crq.v1.flags1 |= IBMVNIC_TX_PROT_UDP;
	}

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		tx_crq.v1.flags1 |= IBMVNIC_TX_CHKSUM_OFFLOAD;
		hdrs += 2;
	}
	/* determine if l2/3/4 headers are sent to firmware */
	if ((*hdrs >> 7) & 1 &&
	    (skb->protocol == htons(ETH_P_IP) ||
	     skb->protocol == htons(ETH_P_IPV6))) {
		build_hdr_descs_arr(tx_buff, &num_entries, *hdrs);
		tx_crq.v1.n_crq_elem = num_entries;
		tx_buff->indir_arr[0] = tx_crq;
		tx_buff->indir_dma = dma_map_single(dev, tx_buff->indir_arr,
						    sizeof(tx_buff->indir_arr),
						    DMA_TO_DEVICE);
		if (dma_mapping_error(dev, tx_buff->indir_dma)) {
			dev_kfree_skb_any(skb);
			tx_buff->skb = NULL;
			if (!firmware_has_feature(FW_FEATURE_CMO))
				dev_err(dev, "tx: unable to map descriptor array\n");
			tx_map_failed++;
			tx_dropped++;
			ret = NETDEV_TX_OK;
			goto out;
		}
		lpar_rc = send_subcrq_indirect(adapter, handle_array[queue_num],
					       (u64)tx_buff->indir_dma,
					       (u64)num_entries);
	} else {
		lpar_rc = send_subcrq(adapter, handle_array[queue_num],
				      &tx_crq);
	}
	if (lpar_rc != H_SUCCESS) {
		dev_err(dev, "tx failed with code %ld\n", lpar_rc);

		if (tx_pool->consumer_index == 0)
			tx_pool->consumer_index =
				adapter->req_tx_entries_per_subcrq - 1;
		else
			tx_pool->consumer_index--;

		dev_kfree_skb_any(skb);
		tx_buff->skb = NULL;

		if (lpar_rc == H_CLOSED)
			netif_stop_subqueue(netdev, queue_num);

		tx_send_failed++;
		tx_dropped++;
		ret = NETDEV_TX_OK;
		goto out;
	}

	if (atomic_inc_return(&tx_scrq->used)
					>= adapter->req_tx_entries_per_subcrq) {
		netdev_info(netdev, "Stopping queue %d\n", queue_num);
		netif_stop_subqueue(netdev, queue_num);
	}

	tx_packets++;
	tx_bytes += skb->len;
	txq->trans_start = jiffies;
	ret = NETDEV_TX_OK;

out:
	netdev->stats.tx_dropped += tx_dropped;
	netdev->stats.tx_bytes += tx_bytes;
	netdev->stats.tx_packets += tx_packets;
	adapter->tx_send_failed += tx_send_failed;
	adapter->tx_map_failed += tx_map_failed;

	return ret;
}

static void ibmvnic_set_multi(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	struct netdev_hw_addr *ha;
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.request_capability.first = IBMVNIC_CRQ_CMD;
	crq.request_capability.cmd = REQUEST_CAPABILITY;

	if (netdev->flags & IFF_PROMISC) {
		if (!adapter->promisc_supported)
			return;
	} else {
		if (netdev->flags & IFF_ALLMULTI) {
			/* Accept all multicast */
			memset(&crq, 0, sizeof(crq));
			crq.multicast_ctrl.first = IBMVNIC_CRQ_CMD;
			crq.multicast_ctrl.cmd = MULTICAST_CTRL;
			crq.multicast_ctrl.flags = IBMVNIC_ENABLE_ALL;
			ibmvnic_send_crq(adapter, &crq);
		} else if (netdev_mc_empty(netdev)) {
			/* Reject all multicast */
			memset(&crq, 0, sizeof(crq));
			crq.multicast_ctrl.first = IBMVNIC_CRQ_CMD;
			crq.multicast_ctrl.cmd = MULTICAST_CTRL;
			crq.multicast_ctrl.flags = IBMVNIC_DISABLE_ALL;
			ibmvnic_send_crq(adapter, &crq);
		} else {
			/* Accept one or more multicast(s) */
			netdev_for_each_mc_addr(ha, netdev) {
				memset(&crq, 0, sizeof(crq));
				crq.multicast_ctrl.first = IBMVNIC_CRQ_CMD;
				crq.multicast_ctrl.cmd = MULTICAST_CTRL;
				crq.multicast_ctrl.flags = IBMVNIC_ENABLE_MC;
				ether_addr_copy(&crq.multicast_ctrl.mac_addr[0],
						ha->addr);
				ibmvnic_send_crq(adapter, &crq);
			}
		}
	}
}

static int ibmvnic_set_mac(struct net_device *netdev, void *p)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	struct sockaddr *addr = p;
	union ibmvnic_crq crq;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	memset(&crq, 0, sizeof(crq));
	crq.change_mac_addr.first = IBMVNIC_CRQ_CMD;
	crq.change_mac_addr.cmd = CHANGE_MAC_ADDR;
	ether_addr_copy(&crq.change_mac_addr.mac_addr[0], addr->sa_data);
	ibmvnic_send_crq(adapter, &crq);
	/* netdev->dev_addr is changed in handle_change_mac_rsp function */
	return 0;
}

/**
 * do_reset returns zero if we are able to keep processing reset events, or
 * non-zero if we hit a fatal error and must halt.
 */
static int do_reset(struct ibmvnic_adapter *adapter,
		    struct ibmvnic_rwi *rwi, u32 reset_state)
{
	struct net_device *netdev = adapter->netdev;
	int i, rc;

	netif_carrier_off(netdev);
	adapter->reset_reason = rwi->reset_reason;

	if (rwi->reset_reason == VNIC_RESET_MOBILITY) {
		rc = ibmvnic_reenable_crq_queue(adapter);
		if (rc)
			return 0;
	}

	rc = __ibmvnic_close(netdev);
	if (rc)
		return rc;

	/* remove the closed state so when we call open it appears
	 * we are coming from the probed state.
	 */
	adapter->state = VNIC_PROBED;

	release_resources(adapter);
	release_sub_crqs(adapter);
	release_crq_queue(adapter);

	rc = ibmvnic_init(adapter);
	if (rc)
		return 0;

	/* If the adapter was in PROBE state prior to the reset, exit here. */
	if (reset_state == VNIC_PROBED)
		return 0;

	rc = ibmvnic_login(netdev);
	if (rc) {
		adapter->state = VNIC_PROBED;
		return 0;
	}

	rtnl_lock();
	rc = init_resources(adapter);
	rtnl_unlock();
	if (rc)
		return rc;

	if (reset_state == VNIC_CLOSED)
		return 0;

	rc = __ibmvnic_open(netdev);
	if (rc) {
		if (list_empty(&adapter->rwi_list))
			adapter->state = VNIC_CLOSED;
		else
			adapter->state = reset_state;

		return 0;
	}

	netif_carrier_on(netdev);

	/* kick napi */
	for (i = 0; i < adapter->req_rx_queues; i++)
		napi_schedule(&adapter->napi[i]);

	return 0;
}

static struct ibmvnic_rwi *get_next_rwi(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_rwi *rwi;

	mutex_lock(&adapter->rwi_lock);

	if (!list_empty(&adapter->rwi_list)) {
		rwi = list_first_entry(&adapter->rwi_list, struct ibmvnic_rwi,
				       list);
		list_del(&rwi->list);
	} else {
		rwi = NULL;
	}

	mutex_unlock(&adapter->rwi_lock);
	return rwi;
}

static void free_all_rwi(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_rwi *rwi;

	rwi = get_next_rwi(adapter);
	while (rwi) {
		kfree(rwi);
		rwi = get_next_rwi(adapter);
	}
}

static void __ibmvnic_reset(struct work_struct *work)
{
	struct ibmvnic_rwi *rwi;
	struct ibmvnic_adapter *adapter;
	struct net_device *netdev;
	u32 reset_state;
	int rc;

	adapter = container_of(work, struct ibmvnic_adapter, ibmvnic_reset);
	netdev = adapter->netdev;

	mutex_lock(&adapter->reset_lock);
	adapter->resetting = true;
	reset_state = adapter->state;

	rwi = get_next_rwi(adapter);
	while (rwi) {
		rc = do_reset(adapter, rwi, reset_state);
		kfree(rwi);
		if (rc)
			break;

		rwi = get_next_rwi(adapter);
	}

	if (rc) {
		free_all_rwi(adapter);
		return;
	}

	adapter->resetting = false;
	mutex_unlock(&adapter->reset_lock);
}

static void ibmvnic_reset(struct ibmvnic_adapter *adapter,
			  enum ibmvnic_reset_reason reason)
{
	struct ibmvnic_rwi *rwi, *tmp;
	struct net_device *netdev = adapter->netdev;
	struct list_head *entry;

	if (adapter->state == VNIC_REMOVING ||
	    adapter->state == VNIC_REMOVED) {
		netdev_dbg(netdev, "Adapter removing, skipping reset\n");
		return;
	}

	mutex_lock(&adapter->rwi_lock);

	list_for_each(entry, &adapter->rwi_list) {
		tmp = list_entry(entry, struct ibmvnic_rwi, list);
		if (tmp->reset_reason == reason) {
			netdev_err(netdev, "Matching reset found, skipping\n");
			mutex_unlock(&adapter->rwi_lock);
			return;
		}
	}

	rwi = kzalloc(sizeof(*rwi), GFP_KERNEL);
	if (!rwi) {
		mutex_unlock(&adapter->rwi_lock);
		ibmvnic_close(netdev);
		return;
	}

	rwi->reset_reason = reason;
	list_add_tail(&rwi->list, &adapter->rwi_list);
	mutex_unlock(&adapter->rwi_lock);
	schedule_work(&adapter->ibmvnic_reset);
}

static void ibmvnic_tx_timeout(struct net_device *dev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(dev);

	ibmvnic_reset(adapter, VNIC_RESET_TIMEOUT);
}

static void remove_buff_from_pool(struct ibmvnic_adapter *adapter,
				  struct ibmvnic_rx_buff *rx_buff)
{
	struct ibmvnic_rx_pool *pool = &adapter->rx_pool[rx_buff->pool_index];

	rx_buff->skb = NULL;

	pool->free_map[pool->next_alloc] = (int)(rx_buff - pool->rx_buff);
	pool->next_alloc = (pool->next_alloc + 1) % pool->size;

	atomic_dec(&pool->available);
}

static int ibmvnic_poll(struct napi_struct *napi, int budget)
{
	struct net_device *netdev = napi->dev;
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int scrq_num = (int)(napi - adapter->napi);
	int frames_processed = 0;
restart_poll:
	while (frames_processed < budget) {
		struct sk_buff *skb;
		struct ibmvnic_rx_buff *rx_buff;
		union sub_crq *next;
		u32 length;
		u16 offset;
		u8 flags = 0;

		if (!pending_scrq(adapter, adapter->rx_scrq[scrq_num]))
			break;
		next = ibmvnic_next_scrq(adapter, adapter->rx_scrq[scrq_num]);
		rx_buff =
		    (struct ibmvnic_rx_buff *)be64_to_cpu(next->
							  rx_comp.correlator);
		/* do error checking */
		if (next->rx_comp.rc) {
			netdev_err(netdev, "rx error %x\n", next->rx_comp.rc);
			/* free the entry */
			next->rx_comp.first = 0;
			remove_buff_from_pool(adapter, rx_buff);
			continue;
		}

		length = be32_to_cpu(next->rx_comp.len);
		offset = be16_to_cpu(next->rx_comp.off_frame_data);
		flags = next->rx_comp.flags;
		skb = rx_buff->skb;
		skb_copy_to_linear_data(skb, rx_buff->data + offset,
					length);

		/* VLAN Header has been stripped by the system firmware and
		 * needs to be inserted by the driver
		 */
		if (adapter->rx_vlan_header_insertion &&
		    (flags & IBMVNIC_VLAN_STRIPPED))
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
					       ntohs(next->rx_comp.vlan_tci));

		/* free the entry */
		next->rx_comp.first = 0;
		remove_buff_from_pool(adapter, rx_buff);

		skb_put(skb, length);
		skb->protocol = eth_type_trans(skb, netdev);
		skb_record_rx_queue(skb, scrq_num);

		if (flags & IBMVNIC_IP_CHKSUM_GOOD &&
		    flags & IBMVNIC_TCP_UDP_CHKSUM_GOOD) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		}

		length = skb->len;
		napi_gro_receive(napi, skb); /* send it up */
		netdev->stats.rx_packets++;
		netdev->stats.rx_bytes += length;
		frames_processed++;
	}
	replenish_rx_pool(adapter, &adapter->rx_pool[scrq_num]);

	if (frames_processed < budget) {
		enable_scrq_irq(adapter, adapter->rx_scrq[scrq_num]);
		napi_complete_done(napi, frames_processed);
		if (pending_scrq(adapter, adapter->rx_scrq[scrq_num]) &&
		    napi_reschedule(napi)) {
			disable_scrq_irq(adapter, adapter->rx_scrq[scrq_num]);
			goto restart_poll;
		}
	}
	return frames_processed;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void ibmvnic_netpoll_controller(struct net_device *dev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(dev);
	int i;

	replenish_pools(netdev_priv(dev));
	for (i = 0; i < adapter->req_rx_queues; i++)
		ibmvnic_interrupt_rx(adapter->rx_scrq[i]->irq,
				     adapter->rx_scrq[i]);
}
#endif

static const struct net_device_ops ibmvnic_netdev_ops = {
	.ndo_open		= ibmvnic_open,
	.ndo_stop		= ibmvnic_close,
	.ndo_start_xmit		= ibmvnic_xmit,
	.ndo_set_rx_mode	= ibmvnic_set_multi,
	.ndo_set_mac_address	= ibmvnic_set_mac,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_tx_timeout		= ibmvnic_tx_timeout,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= ibmvnic_netpoll_controller,
#endif
};

/* ethtool functions */

static int ibmvnic_get_link_ksettings(struct net_device *netdev,
				      struct ethtool_link_ksettings *cmd)
{
	u32 supported, advertising;

	supported = (SUPPORTED_1000baseT_Full | SUPPORTED_Autoneg |
			  SUPPORTED_FIBRE);
	advertising = (ADVERTISED_1000baseT_Full | ADVERTISED_Autoneg |
			    ADVERTISED_FIBRE);
	cmd->base.speed = SPEED_1000;
	cmd->base.duplex = DUPLEX_FULL;
	cmd->base.port = PORT_FIBRE;
	cmd->base.phy_address = 0;
	cmd->base.autoneg = AUTONEG_ENABLE;

	ethtool_convert_legacy_u32_to_link_mode(cmd->link_modes.supported,
						supported);
	ethtool_convert_legacy_u32_to_link_mode(cmd->link_modes.advertising,
						advertising);

	return 0;
}

static void ibmvnic_get_drvinfo(struct net_device *dev,
				struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, ibmvnic_driver_name, sizeof(info->driver));
	strlcpy(info->version, IBMVNIC_DRIVER_VERSION, sizeof(info->version));
}

static u32 ibmvnic_get_msglevel(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	return adapter->msg_enable;
}

static void ibmvnic_set_msglevel(struct net_device *netdev, u32 data)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	adapter->msg_enable = data;
}

static u32 ibmvnic_get_link(struct net_device *netdev)
{
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	/* Don't need to send a query because we request a logical link up at
	 * init and then we wait for link state indications
	 */
	return adapter->logical_link_state;
}

static void ibmvnic_get_ringparam(struct net_device *netdev,
				  struct ethtool_ringparam *ring)
{
	ring->rx_max_pending = 0;
	ring->tx_max_pending = 0;
	ring->rx_mini_max_pending = 0;
	ring->rx_jumbo_max_pending = 0;
	ring->rx_pending = 0;
	ring->tx_pending = 0;
	ring->rx_mini_pending = 0;
	ring->rx_jumbo_pending = 0;
}

static void ibmvnic_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(ibmvnic_stats); i++, data += ETH_GSTRING_LEN)
		memcpy(data, ibmvnic_stats[i].name, ETH_GSTRING_LEN);
}

static int ibmvnic_get_sset_count(struct net_device *dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(ibmvnic_stats);
	default:
		return -EOPNOTSUPP;
	}
}

static void ibmvnic_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct ibmvnic_adapter *adapter = netdev_priv(dev);
	union ibmvnic_crq crq;
	int i;

	memset(&crq, 0, sizeof(crq));
	crq.request_statistics.first = IBMVNIC_CRQ_CMD;
	crq.request_statistics.cmd = REQUEST_STATISTICS;
	crq.request_statistics.ioba = cpu_to_be32(adapter->stats_token);
	crq.request_statistics.len =
	    cpu_to_be32(sizeof(struct ibmvnic_statistics));

	/* Wait for data to be written */
	init_completion(&adapter->stats_done);
	ibmvnic_send_crq(adapter, &crq);
	wait_for_completion(&adapter->stats_done);

	for (i = 0; i < ARRAY_SIZE(ibmvnic_stats); i++)
		data[i] = IBMVNIC_GET_STAT(adapter, ibmvnic_stats[i].offset);
}

static const struct ethtool_ops ibmvnic_ethtool_ops = {
	.get_drvinfo		= ibmvnic_get_drvinfo,
	.get_msglevel		= ibmvnic_get_msglevel,
	.set_msglevel		= ibmvnic_set_msglevel,
	.get_link		= ibmvnic_get_link,
	.get_ringparam		= ibmvnic_get_ringparam,
	.get_strings            = ibmvnic_get_strings,
	.get_sset_count         = ibmvnic_get_sset_count,
	.get_ethtool_stats	= ibmvnic_get_ethtool_stats,
	.get_link_ksettings	= ibmvnic_get_link_ksettings,
};

/* Routines for managing CRQs/sCRQs  */

static void release_sub_crq_queue(struct ibmvnic_adapter *adapter,
				  struct ibmvnic_sub_crq_queue *scrq)
{
	struct device *dev = &adapter->vdev->dev;
	long rc;

	netdev_dbg(adapter->netdev, "Releasing sub-CRQ\n");

	/* Close the sub-crqs */
	do {
		rc = plpar_hcall_norets(H_FREE_SUB_CRQ,
					adapter->vdev->unit_address,
					scrq->crq_num);
	} while (rc == H_BUSY || H_IS_LONG_BUSY(rc));

	if (rc) {
		netdev_err(adapter->netdev,
			   "Failed to release sub-CRQ %16lx, rc = %ld\n",
			   scrq->crq_num, rc);
	}

	dma_unmap_single(dev, scrq->msg_token, 4 * PAGE_SIZE,
			 DMA_BIDIRECTIONAL);
	free_pages((unsigned long)scrq->msgs, 2);
	kfree(scrq);
}

static struct ibmvnic_sub_crq_queue *init_sub_crq_queue(struct ibmvnic_adapter
							*adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_sub_crq_queue *scrq;
	int rc;

	scrq = kzalloc(sizeof(*scrq), GFP_KERNEL);
	if (!scrq)
		return NULL;

	scrq->msgs =
		(union sub_crq *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 2);
	if (!scrq->msgs) {
		dev_warn(dev, "Couldn't allocate crq queue messages page\n");
		goto zero_page_failed;
	}

	scrq->msg_token = dma_map_single(dev, scrq->msgs, 4 * PAGE_SIZE,
					 DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, scrq->msg_token)) {
		dev_warn(dev, "Couldn't map crq queue messages page\n");
		goto map_failed;
	}

	rc = h_reg_sub_crq(adapter->vdev->unit_address, scrq->msg_token,
			   4 * PAGE_SIZE, &scrq->crq_num, &scrq->hw_irq);

	if (rc == H_RESOURCE)
		rc = ibmvnic_reset_crq(adapter);

	if (rc == H_CLOSED) {
		dev_warn(dev, "Partner adapter not ready, waiting.\n");
	} else if (rc) {
		dev_warn(dev, "Error %d registering sub-crq\n", rc);
		goto reg_failed;
	}

	scrq->adapter = adapter;
	scrq->size = 4 * PAGE_SIZE / sizeof(*scrq->msgs);
	spin_lock_init(&scrq->lock);

	netdev_dbg(adapter->netdev,
		   "sub-crq initialized, num %lx, hw_irq=%lx, irq=%x\n",
		   scrq->crq_num, scrq->hw_irq, scrq->irq);

	return scrq;

reg_failed:
	dma_unmap_single(dev, scrq->msg_token, 4 * PAGE_SIZE,
			 DMA_BIDIRECTIONAL);
map_failed:
	free_pages((unsigned long)scrq->msgs, 2);
zero_page_failed:
	kfree(scrq);

	return NULL;
}

static void release_sub_crqs(struct ibmvnic_adapter *adapter)
{
	int i;

	if (adapter->tx_scrq) {
		for (i = 0; i < adapter->req_tx_queues; i++) {
			if (!adapter->tx_scrq[i])
				continue;

			if (adapter->tx_scrq[i]->irq) {
				free_irq(adapter->tx_scrq[i]->irq,
					 adapter->tx_scrq[i]);
				irq_dispose_mapping(adapter->tx_scrq[i]->irq);
				adapter->tx_scrq[i]->irq = 0;
			}

			release_sub_crq_queue(adapter, adapter->tx_scrq[i]);
		}

		kfree(adapter->tx_scrq);
		adapter->tx_scrq = NULL;
	}

	if (adapter->rx_scrq) {
		for (i = 0; i < adapter->req_rx_queues; i++) {
			if (!adapter->rx_scrq[i])
				continue;

			if (adapter->rx_scrq[i]->irq) {
				free_irq(adapter->rx_scrq[i]->irq,
					 adapter->rx_scrq[i]);
				irq_dispose_mapping(adapter->rx_scrq[i]->irq);
				adapter->rx_scrq[i]->irq = 0;
			}

			release_sub_crq_queue(adapter, adapter->rx_scrq[i]);
		}

		kfree(adapter->rx_scrq);
		adapter->rx_scrq = NULL;
	}
}

static int disable_scrq_irq(struct ibmvnic_adapter *adapter,
			    struct ibmvnic_sub_crq_queue *scrq)
{
	struct device *dev = &adapter->vdev->dev;
	unsigned long rc;

	rc = plpar_hcall_norets(H_VIOCTL, adapter->vdev->unit_address,
				H_DISABLE_VIO_INTERRUPT, scrq->hw_irq, 0, 0);
	if (rc)
		dev_err(dev, "Couldn't disable scrq irq 0x%lx. rc=%ld\n",
			scrq->hw_irq, rc);
	return rc;
}

static int enable_scrq_irq(struct ibmvnic_adapter *adapter,
			   struct ibmvnic_sub_crq_queue *scrq)
{
	struct device *dev = &adapter->vdev->dev;
	unsigned long rc;

	if (scrq->hw_irq > 0x100000000ULL) {
		dev_err(dev, "bad hw_irq = %lx\n", scrq->hw_irq);
		return 1;
	}

	rc = plpar_hcall_norets(H_VIOCTL, adapter->vdev->unit_address,
				H_ENABLE_VIO_INTERRUPT, scrq->hw_irq, 0, 0);
	if (rc)
		dev_err(dev, "Couldn't enable scrq irq 0x%lx. rc=%ld\n",
			scrq->hw_irq, rc);
	return rc;
}

static int ibmvnic_complete_tx(struct ibmvnic_adapter *adapter,
			       struct ibmvnic_sub_crq_queue *scrq)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_tx_buff *txbuff;
	union sub_crq *next;
	int index;
	int i, j;
	u8 first;

restart_loop:
	while (pending_scrq(adapter, scrq)) {
		unsigned int pool = scrq->pool_index;

		next = ibmvnic_next_scrq(adapter, scrq);
		for (i = 0; i < next->tx_comp.num_comps; i++) {
			if (next->tx_comp.rcs[i]) {
				dev_err(dev, "tx error %x\n",
					next->tx_comp.rcs[i]);
				continue;
			}
			index = be32_to_cpu(next->tx_comp.correlators[i]);
			txbuff = &adapter->tx_pool[pool].tx_buff[index];

			for (j = 0; j < IBMVNIC_MAX_FRAGS_PER_CRQ; j++) {
				if (!txbuff->data_dma[j])
					continue;

				txbuff->data_dma[j] = 0;
			}
			/* if sub_crq was sent indirectly */
			first = txbuff->indir_arr[0].generic.first;
			if (first == IBMVNIC_CRQ_CMD) {
				dma_unmap_single(dev, txbuff->indir_dma,
						 sizeof(txbuff->indir_arr),
						 DMA_TO_DEVICE);
			}

			if (txbuff->last_frag) {
				dev_kfree_skb_any(txbuff->skb);
				txbuff->skb = NULL;
			}

			adapter->tx_pool[pool].free_map[adapter->tx_pool[pool].
						     producer_index] = index;
			adapter->tx_pool[pool].producer_index =
			    (adapter->tx_pool[pool].producer_index + 1) %
			    adapter->req_tx_entries_per_subcrq;
		}
		/* remove tx_comp scrq*/
		next->tx_comp.first = 0;

		if (atomic_sub_return(next->tx_comp.num_comps, &scrq->used) <=
		    (adapter->req_tx_entries_per_subcrq / 2) &&
		    __netif_subqueue_stopped(adapter->netdev,
					     scrq->pool_index)) {
			netif_wake_subqueue(adapter->netdev, scrq->pool_index);
			netdev_info(adapter->netdev, "Started queue %d\n",
				    scrq->pool_index);
		}
	}

	enable_scrq_irq(adapter, scrq);

	if (pending_scrq(adapter, scrq)) {
		disable_scrq_irq(adapter, scrq);
		goto restart_loop;
	}

	return 0;
}

static irqreturn_t ibmvnic_interrupt_tx(int irq, void *instance)
{
	struct ibmvnic_sub_crq_queue *scrq = instance;
	struct ibmvnic_adapter *adapter = scrq->adapter;

	disable_scrq_irq(adapter, scrq);
	ibmvnic_complete_tx(adapter, scrq);

	return IRQ_HANDLED;
}

static irqreturn_t ibmvnic_interrupt_rx(int irq, void *instance)
{
	struct ibmvnic_sub_crq_queue *scrq = instance;
	struct ibmvnic_adapter *adapter = scrq->adapter;

	if (napi_schedule_prep(&adapter->napi[scrq->scrq_num])) {
		disable_scrq_irq(adapter, scrq);
		__napi_schedule(&adapter->napi[scrq->scrq_num]);
	}

	return IRQ_HANDLED;
}

static int init_sub_crq_irqs(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_sub_crq_queue *scrq;
	int i = 0, j = 0;
	int rc = 0;

	for (i = 0; i < adapter->req_tx_queues; i++) {
		scrq = adapter->tx_scrq[i];
		scrq->irq = irq_create_mapping(NULL, scrq->hw_irq);

		if (!scrq->irq) {
			rc = -EINVAL;
			dev_err(dev, "Error mapping irq\n");
			goto req_tx_irq_failed;
		}

		rc = request_irq(scrq->irq, ibmvnic_interrupt_tx,
				 0, "ibmvnic_tx", scrq);

		if (rc) {
			dev_err(dev, "Couldn't register tx irq 0x%x. rc=%d\n",
				scrq->irq, rc);
			irq_dispose_mapping(scrq->irq);
			goto req_rx_irq_failed;
		}
	}

	for (i = 0; i < adapter->req_rx_queues; i++) {
		scrq = adapter->rx_scrq[i];
		scrq->irq = irq_create_mapping(NULL, scrq->hw_irq);
		if (!scrq->irq) {
			rc = -EINVAL;
			dev_err(dev, "Error mapping irq\n");
			goto req_rx_irq_failed;
		}
		rc = request_irq(scrq->irq, ibmvnic_interrupt_rx,
				 0, "ibmvnic_rx", scrq);
		if (rc) {
			dev_err(dev, "Couldn't register rx irq 0x%x. rc=%d\n",
				scrq->irq, rc);
			irq_dispose_mapping(scrq->irq);
			goto req_rx_irq_failed;
		}
	}
	return rc;

req_rx_irq_failed:
	for (j = 0; j < i; j++) {
		free_irq(adapter->rx_scrq[j]->irq, adapter->rx_scrq[j]);
		irq_dispose_mapping(adapter->rx_scrq[j]->irq);
	}
	i = adapter->req_tx_queues;
req_tx_irq_failed:
	for (j = 0; j < i; j++) {
		free_irq(adapter->tx_scrq[j]->irq, adapter->tx_scrq[j]);
		irq_dispose_mapping(adapter->rx_scrq[j]->irq);
	}
	release_sub_crqs(adapter);
	return rc;
}

static int init_sub_crqs(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_sub_crq_queue **allqueues;
	int registered_queues = 0;
	int total_queues;
	int more = 0;
	int i;

	total_queues = adapter->req_tx_queues + adapter->req_rx_queues;

	allqueues = kcalloc(total_queues, sizeof(*allqueues), GFP_KERNEL);
	if (!allqueues)
		return -1;

	for (i = 0; i < total_queues; i++) {
		allqueues[i] = init_sub_crq_queue(adapter);
		if (!allqueues[i]) {
			dev_warn(dev, "Couldn't allocate all sub-crqs\n");
			break;
		}
		registered_queues++;
	}

	/* Make sure we were able to register the minimum number of queues */
	if (registered_queues <
	    adapter->min_tx_queues + adapter->min_rx_queues) {
		dev_err(dev, "Fatal: Couldn't init  min number of sub-crqs\n");
		goto tx_failed;
	}

	/* Distribute the failed allocated queues*/
	for (i = 0; i < total_queues - registered_queues + more ; i++) {
		netdev_dbg(adapter->netdev, "Reducing number of queues\n");
		switch (i % 3) {
		case 0:
			if (adapter->req_rx_queues > adapter->min_rx_queues)
				adapter->req_rx_queues--;
			else
				more++;
			break;
		case 1:
			if (adapter->req_tx_queues > adapter->min_tx_queues)
				adapter->req_tx_queues--;
			else
				more++;
			break;
		}
	}

	adapter->tx_scrq = kcalloc(adapter->req_tx_queues,
				   sizeof(*adapter->tx_scrq), GFP_KERNEL);
	if (!adapter->tx_scrq)
		goto tx_failed;

	for (i = 0; i < adapter->req_tx_queues; i++) {
		adapter->tx_scrq[i] = allqueues[i];
		adapter->tx_scrq[i]->pool_index = i;
	}

	adapter->rx_scrq = kcalloc(adapter->req_rx_queues,
				   sizeof(*adapter->rx_scrq), GFP_KERNEL);
	if (!adapter->rx_scrq)
		goto rx_failed;

	for (i = 0; i < adapter->req_rx_queues; i++) {
		adapter->rx_scrq[i] = allqueues[i + adapter->req_tx_queues];
		adapter->rx_scrq[i]->scrq_num = i;
	}

	kfree(allqueues);
	return 0;

rx_failed:
	kfree(adapter->tx_scrq);
	adapter->tx_scrq = NULL;
tx_failed:
	for (i = 0; i < registered_queues; i++)
		release_sub_crq_queue(adapter, allqueues[i]);
	kfree(allqueues);
	return -1;
}

static void ibmvnic_send_req_caps(struct ibmvnic_adapter *adapter, int retry)
{
	struct device *dev = &adapter->vdev->dev;
	union ibmvnic_crq crq;

	if (!retry) {
		/* Sub-CRQ entries are 32 byte long */
		int entries_page = 4 * PAGE_SIZE / (sizeof(u64) * 4);

		if (adapter->min_tx_entries_per_subcrq > entries_page ||
		    adapter->min_rx_add_entries_per_subcrq > entries_page) {
			dev_err(dev, "Fatal, invalid entries per sub-crq\n");
			return;
		}

		/* Get the minimum between the queried max and the entries
		 * that fit in our PAGE_SIZE
		 */
		adapter->req_tx_entries_per_subcrq =
		    adapter->max_tx_entries_per_subcrq > entries_page ?
		    entries_page : adapter->max_tx_entries_per_subcrq;
		adapter->req_rx_add_entries_per_subcrq =
		    adapter->max_rx_add_entries_per_subcrq > entries_page ?
		    entries_page : adapter->max_rx_add_entries_per_subcrq;

		adapter->req_tx_queues = adapter->opt_tx_comp_sub_queues;
		adapter->req_rx_queues = adapter->opt_rx_comp_queues;
		adapter->req_rx_add_queues = adapter->max_rx_add_queues;

		adapter->req_mtu = adapter->netdev->mtu + ETH_HLEN;
	}

	memset(&crq, 0, sizeof(crq));
	crq.request_capability.first = IBMVNIC_CRQ_CMD;
	crq.request_capability.cmd = REQUEST_CAPABILITY;

	crq.request_capability.capability = cpu_to_be16(REQ_TX_QUEUES);
	crq.request_capability.number = cpu_to_be64(adapter->req_tx_queues);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability = cpu_to_be16(REQ_RX_QUEUES);
	crq.request_capability.number = cpu_to_be64(adapter->req_rx_queues);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability = cpu_to_be16(REQ_RX_ADD_QUEUES);
	crq.request_capability.number = cpu_to_be64(adapter->req_rx_add_queues);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability =
	    cpu_to_be16(REQ_TX_ENTRIES_PER_SUBCRQ);
	crq.request_capability.number =
	    cpu_to_be64(adapter->req_tx_entries_per_subcrq);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability =
	    cpu_to_be16(REQ_RX_ADD_ENTRIES_PER_SUBCRQ);
	crq.request_capability.number =
	    cpu_to_be64(adapter->req_rx_add_entries_per_subcrq);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.request_capability.capability = cpu_to_be16(REQ_MTU);
	crq.request_capability.number = cpu_to_be64(adapter->req_mtu);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	if (adapter->netdev->flags & IFF_PROMISC) {
		if (adapter->promisc_supported) {
			crq.request_capability.capability =
			    cpu_to_be16(PROMISC_REQUESTED);
			crq.request_capability.number = cpu_to_be64(1);
			atomic_inc(&adapter->running_cap_crqs);
			ibmvnic_send_crq(adapter, &crq);
		}
	} else {
		crq.request_capability.capability =
		    cpu_to_be16(PROMISC_REQUESTED);
		crq.request_capability.number = cpu_to_be64(0);
		atomic_inc(&adapter->running_cap_crqs);
		ibmvnic_send_crq(adapter, &crq);
	}
}

static int pending_scrq(struct ibmvnic_adapter *adapter,
			struct ibmvnic_sub_crq_queue *scrq)
{
	union sub_crq *entry = &scrq->msgs[scrq->cur];

	if (entry->generic.first & IBMVNIC_CRQ_CMD_RSP ||
	    adapter->state == VNIC_CLOSING)
		return 1;
	else
		return 0;
}

static union sub_crq *ibmvnic_next_scrq(struct ibmvnic_adapter *adapter,
					struct ibmvnic_sub_crq_queue *scrq)
{
	union sub_crq *entry;
	unsigned long flags;

	spin_lock_irqsave(&scrq->lock, flags);
	entry = &scrq->msgs[scrq->cur];
	if (entry->generic.first & IBMVNIC_CRQ_CMD_RSP) {
		if (++scrq->cur == scrq->size)
			scrq->cur = 0;
	} else {
		entry = NULL;
	}
	spin_unlock_irqrestore(&scrq->lock, flags);

	return entry;
}

static union ibmvnic_crq *ibmvnic_next_crq(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_crq_queue *queue = &adapter->crq;
	union ibmvnic_crq *crq;

	crq = &queue->msgs[queue->cur];
	if (crq->generic.first & IBMVNIC_CRQ_CMD_RSP) {
		if (++queue->cur == queue->size)
			queue->cur = 0;
	} else {
		crq = NULL;
	}

	return crq;
}

static int send_subcrq(struct ibmvnic_adapter *adapter, u64 remote_handle,
		       union sub_crq *sub_crq)
{
	unsigned int ua = adapter->vdev->unit_address;
	struct device *dev = &adapter->vdev->dev;
	u64 *u64_crq = (u64 *)sub_crq;
	int rc;

	netdev_dbg(adapter->netdev,
		   "Sending sCRQ %016lx: %016lx %016lx %016lx %016lx\n",
		   (unsigned long int)cpu_to_be64(remote_handle),
		   (unsigned long int)cpu_to_be64(u64_crq[0]),
		   (unsigned long int)cpu_to_be64(u64_crq[1]),
		   (unsigned long int)cpu_to_be64(u64_crq[2]),
		   (unsigned long int)cpu_to_be64(u64_crq[3]));

	/* Make sure the hypervisor sees the complete request */
	mb();

	rc = plpar_hcall_norets(H_SEND_SUB_CRQ, ua,
				cpu_to_be64(remote_handle),
				cpu_to_be64(u64_crq[0]),
				cpu_to_be64(u64_crq[1]),
				cpu_to_be64(u64_crq[2]),
				cpu_to_be64(u64_crq[3]));

	if (rc) {
		if (rc == H_CLOSED)
			dev_warn(dev, "CRQ Queue closed\n");
		dev_err(dev, "Send error (rc=%d)\n", rc);
	}

	return rc;
}

static int send_subcrq_indirect(struct ibmvnic_adapter *adapter,
				u64 remote_handle, u64 ioba, u64 num_entries)
{
	unsigned int ua = adapter->vdev->unit_address;
	struct device *dev = &adapter->vdev->dev;
	int rc;

	/* Make sure the hypervisor sees the complete request */
	mb();
	rc = plpar_hcall_norets(H_SEND_SUB_CRQ_INDIRECT, ua,
				cpu_to_be64(remote_handle),
				ioba, num_entries);

	if (rc) {
		if (rc == H_CLOSED)
			dev_warn(dev, "CRQ Queue closed\n");
		dev_err(dev, "Send (indirect) error (rc=%d)\n", rc);
	}

	return rc;
}

static int ibmvnic_send_crq(struct ibmvnic_adapter *adapter,
			    union ibmvnic_crq *crq)
{
	unsigned int ua = adapter->vdev->unit_address;
	struct device *dev = &adapter->vdev->dev;
	u64 *u64_crq = (u64 *)crq;
	int rc;

	netdev_dbg(adapter->netdev, "Sending CRQ: %016lx %016lx\n",
		   (unsigned long int)cpu_to_be64(u64_crq[0]),
		   (unsigned long int)cpu_to_be64(u64_crq[1]));

	/* Make sure the hypervisor sees the complete request */
	mb();

	rc = plpar_hcall_norets(H_SEND_CRQ, ua,
				cpu_to_be64(u64_crq[0]),
				cpu_to_be64(u64_crq[1]));

	if (rc) {
		if (rc == H_CLOSED)
			dev_warn(dev, "CRQ Queue closed\n");
		dev_warn(dev, "Send error (rc=%d)\n", rc);
	}

	return rc;
}

static int ibmvnic_send_crq_init(struct ibmvnic_adapter *adapter)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.generic.first = IBMVNIC_CRQ_INIT_CMD;
	crq.generic.cmd = IBMVNIC_CRQ_INIT;
	netdev_dbg(adapter->netdev, "Sending CRQ init\n");

	return ibmvnic_send_crq(adapter, &crq);
}

static int send_version_xchg(struct ibmvnic_adapter *adapter)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.version_exchange.first = IBMVNIC_CRQ_CMD;
	crq.version_exchange.cmd = VERSION_EXCHANGE;
	crq.version_exchange.version = cpu_to_be16(ibmvnic_version);

	return ibmvnic_send_crq(adapter, &crq);
}

static void send_login(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_login_rsp_buffer *login_rsp_buffer;
	struct ibmvnic_login_buffer *login_buffer;
	struct device *dev = &adapter->vdev->dev;
	dma_addr_t rsp_buffer_token;
	dma_addr_t buffer_token;
	size_t rsp_buffer_size;
	union ibmvnic_crq crq;
	size_t buffer_size;
	__be64 *tx_list_p;
	__be64 *rx_list_p;
	int i;

	buffer_size =
	    sizeof(struct ibmvnic_login_buffer) +
	    sizeof(u64) * (adapter->req_tx_queues + adapter->req_rx_queues);

	login_buffer = kmalloc(buffer_size, GFP_ATOMIC);
	if (!login_buffer)
		goto buf_alloc_failed;

	buffer_token = dma_map_single(dev, login_buffer, buffer_size,
				      DMA_TO_DEVICE);
	if (dma_mapping_error(dev, buffer_token)) {
		dev_err(dev, "Couldn't map login buffer\n");
		goto buf_map_failed;
	}

	rsp_buffer_size = sizeof(struct ibmvnic_login_rsp_buffer) +
			  sizeof(u64) * adapter->req_tx_queues +
			  sizeof(u64) * adapter->req_rx_queues +
			  sizeof(u64) * adapter->req_rx_queues +
			  sizeof(u8) * IBMVNIC_TX_DESC_VERSIONS;

	login_rsp_buffer = kmalloc(rsp_buffer_size, GFP_ATOMIC);
	if (!login_rsp_buffer)
		goto buf_rsp_alloc_failed;

	rsp_buffer_token = dma_map_single(dev, login_rsp_buffer,
					  rsp_buffer_size, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, rsp_buffer_token)) {
		dev_err(dev, "Couldn't map login rsp buffer\n");
		goto buf_rsp_map_failed;
	}

	adapter->login_buf = login_buffer;
	adapter->login_buf_token = buffer_token;
	adapter->login_buf_sz = buffer_size;
	adapter->login_rsp_buf = login_rsp_buffer;
	adapter->login_rsp_buf_token = rsp_buffer_token;
	adapter->login_rsp_buf_sz = rsp_buffer_size;

	login_buffer->len = cpu_to_be32(buffer_size);
	login_buffer->version = cpu_to_be32(INITIAL_VERSION_LB);
	login_buffer->num_txcomp_subcrqs = cpu_to_be32(adapter->req_tx_queues);
	login_buffer->off_txcomp_subcrqs =
	    cpu_to_be32(sizeof(struct ibmvnic_login_buffer));
	login_buffer->num_rxcomp_subcrqs = cpu_to_be32(adapter->req_rx_queues);
	login_buffer->off_rxcomp_subcrqs =
	    cpu_to_be32(sizeof(struct ibmvnic_login_buffer) +
			sizeof(u64) * adapter->req_tx_queues);
	login_buffer->login_rsp_ioba = cpu_to_be32(rsp_buffer_token);
	login_buffer->login_rsp_len = cpu_to_be32(rsp_buffer_size);

	tx_list_p = (__be64 *)((char *)login_buffer +
				      sizeof(struct ibmvnic_login_buffer));
	rx_list_p = (__be64 *)((char *)login_buffer +
				      sizeof(struct ibmvnic_login_buffer) +
				      sizeof(u64) * adapter->req_tx_queues);

	for (i = 0; i < adapter->req_tx_queues; i++) {
		if (adapter->tx_scrq[i]) {
			tx_list_p[i] = cpu_to_be64(adapter->tx_scrq[i]->
						   crq_num);
		}
	}

	for (i = 0; i < adapter->req_rx_queues; i++) {
		if (adapter->rx_scrq[i]) {
			rx_list_p[i] = cpu_to_be64(adapter->rx_scrq[i]->
						   crq_num);
		}
	}

	netdev_dbg(adapter->netdev, "Login Buffer:\n");
	for (i = 0; i < (adapter->login_buf_sz - 1) / 8 + 1; i++) {
		netdev_dbg(adapter->netdev, "%016lx\n",
			   ((unsigned long int *)(adapter->login_buf))[i]);
	}

	memset(&crq, 0, sizeof(crq));
	crq.login.first = IBMVNIC_CRQ_CMD;
	crq.login.cmd = LOGIN;
	crq.login.ioba = cpu_to_be32(buffer_token);
	crq.login.len = cpu_to_be32(buffer_size);
	ibmvnic_send_crq(adapter, &crq);

	return;

buf_rsp_map_failed:
	kfree(login_rsp_buffer);
buf_rsp_alloc_failed:
	dma_unmap_single(dev, buffer_token, buffer_size, DMA_TO_DEVICE);
buf_map_failed:
	kfree(login_buffer);
buf_alloc_failed:
	return;
}

static void send_request_map(struct ibmvnic_adapter *adapter, dma_addr_t addr,
			     u32 len, u8 map_id)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.request_map.first = IBMVNIC_CRQ_CMD;
	crq.request_map.cmd = REQUEST_MAP;
	crq.request_map.map_id = map_id;
	crq.request_map.ioba = cpu_to_be32(addr);
	crq.request_map.len = cpu_to_be32(len);
	ibmvnic_send_crq(adapter, &crq);
}

static void send_request_unmap(struct ibmvnic_adapter *adapter, u8 map_id)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.request_unmap.first = IBMVNIC_CRQ_CMD;
	crq.request_unmap.cmd = REQUEST_UNMAP;
	crq.request_unmap.map_id = map_id;
	ibmvnic_send_crq(adapter, &crq);
}

static void send_map_query(struct ibmvnic_adapter *adapter)
{
	union ibmvnic_crq crq;

	memset(&crq, 0, sizeof(crq));
	crq.query_map.first = IBMVNIC_CRQ_CMD;
	crq.query_map.cmd = QUERY_MAP;
	ibmvnic_send_crq(adapter, &crq);
}

/* Send a series of CRQs requesting various capabilities of the VNIC server */
static void send_cap_queries(struct ibmvnic_adapter *adapter)
{
	union ibmvnic_crq crq;

	atomic_set(&adapter->running_cap_crqs, 0);
	memset(&crq, 0, sizeof(crq));
	crq.query_capability.first = IBMVNIC_CRQ_CMD;
	crq.query_capability.cmd = QUERY_CAPABILITY;

	crq.query_capability.capability = cpu_to_be16(MIN_TX_QUEUES);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(MIN_RX_QUEUES);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(MIN_RX_ADD_QUEUES);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(MAX_TX_QUEUES);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(MAX_RX_QUEUES);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(MAX_RX_ADD_QUEUES);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability =
	    cpu_to_be16(MIN_TX_ENTRIES_PER_SUBCRQ);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability =
	    cpu_to_be16(MIN_RX_ADD_ENTRIES_PER_SUBCRQ);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability =
	    cpu_to_be16(MAX_TX_ENTRIES_PER_SUBCRQ);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability =
	    cpu_to_be16(MAX_RX_ADD_ENTRIES_PER_SUBCRQ);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(TCP_IP_OFFLOAD);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(PROMISC_SUPPORTED);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(MIN_MTU);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(MAX_MTU);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(MAX_MULTICAST_FILTERS);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(VLAN_HEADER_INSERTION);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(RX_VLAN_HEADER_INSERTION);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(MAX_TX_SG_ENTRIES);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(RX_SG_SUPPORTED);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(OPT_TX_COMP_SUB_QUEUES);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(OPT_RX_COMP_QUEUES);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability =
			cpu_to_be16(OPT_RX_BUFADD_Q_PER_RX_COMP_Q);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability =
			cpu_to_be16(OPT_TX_ENTRIES_PER_SUBCRQ);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability =
			cpu_to_be16(OPT_RXBA_ENTRIES_PER_SUBCRQ);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);

	crq.query_capability.capability = cpu_to_be16(TX_RX_DESC_REQ);
	atomic_inc(&adapter->running_cap_crqs);
	ibmvnic_send_crq(adapter, &crq);
}

static void handle_query_ip_offload_rsp(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_query_ip_offload_buffer *buf = &adapter->ip_offload_buf;
	union ibmvnic_crq crq;
	int i;

	dma_unmap_single(dev, adapter->ip_offload_tok,
			 sizeof(adapter->ip_offload_buf), DMA_FROM_DEVICE);

	netdev_dbg(adapter->netdev, "Query IP Offload Buffer:\n");
	for (i = 0; i < (sizeof(adapter->ip_offload_buf) - 1) / 8 + 1; i++)
		netdev_dbg(adapter->netdev, "%016lx\n",
			   ((unsigned long int *)(buf))[i]);

	netdev_dbg(adapter->netdev, "ipv4_chksum = %d\n", buf->ipv4_chksum);
	netdev_dbg(adapter->netdev, "ipv6_chksum = %d\n", buf->ipv6_chksum);
	netdev_dbg(adapter->netdev, "tcp_ipv4_chksum = %d\n",
		   buf->tcp_ipv4_chksum);
	netdev_dbg(adapter->netdev, "tcp_ipv6_chksum = %d\n",
		   buf->tcp_ipv6_chksum);
	netdev_dbg(adapter->netdev, "udp_ipv4_chksum = %d\n",
		   buf->udp_ipv4_chksum);
	netdev_dbg(adapter->netdev, "udp_ipv6_chksum = %d\n",
		   buf->udp_ipv6_chksum);
	netdev_dbg(adapter->netdev, "large_tx_ipv4 = %d\n",
		   buf->large_tx_ipv4);
	netdev_dbg(adapter->netdev, "large_tx_ipv6 = %d\n",
		   buf->large_tx_ipv6);
	netdev_dbg(adapter->netdev, "large_rx_ipv4 = %d\n",
		   buf->large_rx_ipv4);
	netdev_dbg(adapter->netdev, "large_rx_ipv6 = %d\n",
		   buf->large_rx_ipv6);
	netdev_dbg(adapter->netdev, "max_ipv4_hdr_sz = %d\n",
		   buf->max_ipv4_header_size);
	netdev_dbg(adapter->netdev, "max_ipv6_hdr_sz = %d\n",
		   buf->max_ipv6_header_size);
	netdev_dbg(adapter->netdev, "max_tcp_hdr_size = %d\n",
		   buf->max_tcp_header_size);
	netdev_dbg(adapter->netdev, "max_udp_hdr_size = %d\n",
		   buf->max_udp_header_size);
	netdev_dbg(adapter->netdev, "max_large_tx_size = %d\n",
		   buf->max_large_tx_size);
	netdev_dbg(adapter->netdev, "max_large_rx_size = %d\n",
		   buf->max_large_rx_size);
	netdev_dbg(adapter->netdev, "ipv6_ext_hdr = %d\n",
		   buf->ipv6_extension_header);
	netdev_dbg(adapter->netdev, "tcp_pseudosum_req = %d\n",
		   buf->tcp_pseudosum_req);
	netdev_dbg(adapter->netdev, "num_ipv6_ext_hd = %d\n",
		   buf->num_ipv6_ext_headers);
	netdev_dbg(adapter->netdev, "off_ipv6_ext_hd = %d\n",
		   buf->off_ipv6_ext_headers);

	adapter->ip_offload_ctrl_tok =
	    dma_map_single(dev, &adapter->ip_offload_ctrl,
			   sizeof(adapter->ip_offload_ctrl), DMA_TO_DEVICE);

	if (dma_mapping_error(dev, adapter->ip_offload_ctrl_tok)) {
		dev_err(dev, "Couldn't map ip offload control buffer\n");
		return;
	}

	adapter->ip_offload_ctrl.version = cpu_to_be32(INITIAL_VERSION_IOB);
	adapter->ip_offload_ctrl.tcp_ipv4_chksum = buf->tcp_ipv4_chksum;
	adapter->ip_offload_ctrl.udp_ipv4_chksum = buf->udp_ipv4_chksum;
	adapter->ip_offload_ctrl.tcp_ipv6_chksum = buf->tcp_ipv6_chksum;
	adapter->ip_offload_ctrl.udp_ipv6_chksum = buf->udp_ipv6_chksum;

	/* large_tx/rx disabled for now, additional features needed */
	adapter->ip_offload_ctrl.large_tx_ipv4 = 0;
	adapter->ip_offload_ctrl.large_tx_ipv6 = 0;
	adapter->ip_offload_ctrl.large_rx_ipv4 = 0;
	adapter->ip_offload_ctrl.large_rx_ipv6 = 0;

	adapter->netdev->features = NETIF_F_GSO;

	if (buf->tcp_ipv4_chksum || buf->udp_ipv4_chksum)
		adapter->netdev->features |= NETIF_F_IP_CSUM;

	if (buf->tcp_ipv6_chksum || buf->udp_ipv6_chksum)
		adapter->netdev->features |= NETIF_F_IPV6_CSUM;

	if ((adapter->netdev->features &
	    (NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM)))
		adapter->netdev->features |= NETIF_F_RXCSUM;

	memset(&crq, 0, sizeof(crq));
	crq.control_ip_offload.first = IBMVNIC_CRQ_CMD;
	crq.control_ip_offload.cmd = CONTROL_IP_OFFLOAD;
	crq.control_ip_offload.len =
	    cpu_to_be32(sizeof(adapter->ip_offload_ctrl));
	crq.control_ip_offload.ioba = cpu_to_be32(adapter->ip_offload_ctrl_tok);
	ibmvnic_send_crq(adapter, &crq);
}

static void handle_error_info_rsp(union ibmvnic_crq *crq,
				  struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_error_buff *error_buff, *tmp;
	unsigned long flags;
	bool found = false;
	int i;

	if (!crq->request_error_rsp.rc.code) {
		dev_info(dev, "Request Error Rsp returned with rc=%x\n",
			 crq->request_error_rsp.rc.code);
		return;
	}

	spin_lock_irqsave(&adapter->error_list_lock, flags);
	list_for_each_entry_safe(error_buff, tmp, &adapter->errors, list)
		if (error_buff->error_id == crq->request_error_rsp.error_id) {
			found = true;
			list_del(&error_buff->list);
			break;
		}
	spin_unlock_irqrestore(&adapter->error_list_lock, flags);

	if (!found) {
		dev_err(dev, "Couldn't find error id %x\n",
			be32_to_cpu(crq->request_error_rsp.error_id));
		return;
	}

	dev_err(dev, "Detailed info for error id %x:",
		be32_to_cpu(crq->request_error_rsp.error_id));

	for (i = 0; i < error_buff->len; i++) {
		pr_cont("%02x", (int)error_buff->buff[i]);
		if (i % 8 == 7)
			pr_cont(" ");
	}
	pr_cont("\n");

	dma_unmap_single(dev, error_buff->dma, error_buff->len,
			 DMA_FROM_DEVICE);
	kfree(error_buff->buff);
	kfree(error_buff);
}

static void request_error_information(struct ibmvnic_adapter *adapter,
				      union ibmvnic_crq *err_crq)
{
	struct device *dev = &adapter->vdev->dev;
	struct net_device *netdev = adapter->netdev;
	struct ibmvnic_error_buff *error_buff;
	unsigned long timeout = msecs_to_jiffies(30000);
	union ibmvnic_crq crq;
	unsigned long flags;
	int rc, detail_len;

	error_buff = kmalloc(sizeof(*error_buff), GFP_ATOMIC);
	if (!error_buff)
		return;

	detail_len = be32_to_cpu(err_crq->error_indication.detail_error_sz);
	error_buff->buff = kmalloc(detail_len, GFP_ATOMIC);
	if (!error_buff->buff) {
		kfree(error_buff);
		return;
	}

	error_buff->dma = dma_map_single(dev, error_buff->buff, detail_len,
					 DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, error_buff->dma)) {
		netdev_err(netdev, "Couldn't map error buffer\n");
		kfree(error_buff->buff);
		kfree(error_buff);
		return;
	}

	error_buff->len = detail_len;
	error_buff->error_id = err_crq->error_indication.error_id;

	spin_lock_irqsave(&adapter->error_list_lock, flags);
	list_add_tail(&error_buff->list, &adapter->errors);
	spin_unlock_irqrestore(&adapter->error_list_lock, flags);

	memset(&crq, 0, sizeof(crq));
	crq.request_error_info.first = IBMVNIC_CRQ_CMD;
	crq.request_error_info.cmd = REQUEST_ERROR_INFO;
	crq.request_error_info.ioba = cpu_to_be32(error_buff->dma);
	crq.request_error_info.len = cpu_to_be32(detail_len);
	crq.request_error_info.error_id = err_crq->error_indication.error_id;

	rc = ibmvnic_send_crq(adapter, &crq);
	if (rc) {
		netdev_err(netdev, "failed to request error information\n");
		goto err_info_fail;
	}

	if (!wait_for_completion_timeout(&adapter->init_done, timeout)) {
		netdev_err(netdev, "timeout waiting for error information\n");
		goto err_info_fail;
	}

	return;

err_info_fail:
	spin_lock_irqsave(&adapter->error_list_lock, flags);
	list_del(&error_buff->list);
	spin_unlock_irqrestore(&adapter->error_list_lock, flags);

	kfree(error_buff->buff);
	kfree(error_buff);
}

static void handle_error_indication(union ibmvnic_crq *crq,
				    struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;

	dev_err(dev, "Firmware reports %serror id %x, cause %d\n",
		crq->error_indication.flags
			& IBMVNIC_FATAL_ERROR ? "FATAL " : "",
		be32_to_cpu(crq->error_indication.error_id),
		be16_to_cpu(crq->error_indication.error_cause));

	if (be32_to_cpu(crq->error_indication.error_id))
		request_error_information(adapter, crq);

	if (crq->error_indication.flags & IBMVNIC_FATAL_ERROR)
		ibmvnic_reset(adapter, VNIC_RESET_FATAL);
}

static void handle_change_mac_rsp(union ibmvnic_crq *crq,
				  struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->vdev->dev;
	long rc;

	rc = crq->change_mac_addr_rsp.rc.code;
	if (rc) {
		dev_err(dev, "Error %ld in CHANGE_MAC_ADDR_RSP\n", rc);
		return;
	}
	memcpy(netdev->dev_addr, &crq->change_mac_addr_rsp.mac_addr[0],
	       ETH_ALEN);
}

static void handle_request_cap_rsp(union ibmvnic_crq *crq,
				   struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	u64 *req_value;
	char *name;

	atomic_dec(&adapter->running_cap_crqs);
	switch (be16_to_cpu(crq->request_capability_rsp.capability)) {
	case REQ_TX_QUEUES:
		req_value = &adapter->req_tx_queues;
		name = "tx";
		break;
	case REQ_RX_QUEUES:
		req_value = &adapter->req_rx_queues;
		name = "rx";
		break;
	case REQ_RX_ADD_QUEUES:
		req_value = &adapter->req_rx_add_queues;
		name = "rx_add";
		break;
	case REQ_TX_ENTRIES_PER_SUBCRQ:
		req_value = &adapter->req_tx_entries_per_subcrq;
		name = "tx_entries_per_subcrq";
		break;
	case REQ_RX_ADD_ENTRIES_PER_SUBCRQ:
		req_value = &adapter->req_rx_add_entries_per_subcrq;
		name = "rx_add_entries_per_subcrq";
		break;
	case REQ_MTU:
		req_value = &adapter->req_mtu;
		name = "mtu";
		break;
	case PROMISC_REQUESTED:
		req_value = &adapter->promisc;
		name = "promisc";
		break;
	default:
		dev_err(dev, "Got invalid cap request rsp %d\n",
			crq->request_capability.capability);
		return;
	}

	switch (crq->request_capability_rsp.rc.code) {
	case SUCCESS:
		break;
	case PARTIALSUCCESS:
		dev_info(dev, "req=%lld, rsp=%ld in %s queue, retrying.\n",
			 *req_value,
			 (long int)be64_to_cpu(crq->request_capability_rsp.
					       number), name);
		release_sub_crqs(adapter);
		*req_value = be64_to_cpu(crq->request_capability_rsp.number);
		ibmvnic_send_req_caps(adapter, 1);
		return;
	default:
		dev_err(dev, "Error %d in request cap rsp\n",
			crq->request_capability_rsp.rc.code);
		return;
	}

	/* Done receiving requested capabilities, query IP offload support */
	if (atomic_read(&adapter->running_cap_crqs) == 0) {
		union ibmvnic_crq newcrq;
		int buf_sz = sizeof(struct ibmvnic_query_ip_offload_buffer);
		struct ibmvnic_query_ip_offload_buffer *ip_offload_buf =
		    &adapter->ip_offload_buf;

		adapter->wait_capability = false;
		adapter->ip_offload_tok = dma_map_single(dev, ip_offload_buf,
							 buf_sz,
							 DMA_FROM_DEVICE);

		if (dma_mapping_error(dev, adapter->ip_offload_tok)) {
			if (!firmware_has_feature(FW_FEATURE_CMO))
				dev_err(dev, "Couldn't map offload buffer\n");
			return;
		}

		memset(&newcrq, 0, sizeof(newcrq));
		newcrq.query_ip_offload.first = IBMVNIC_CRQ_CMD;
		newcrq.query_ip_offload.cmd = QUERY_IP_OFFLOAD;
		newcrq.query_ip_offload.len = cpu_to_be32(buf_sz);
		newcrq.query_ip_offload.ioba =
		    cpu_to_be32(adapter->ip_offload_tok);

		ibmvnic_send_crq(adapter, &newcrq);
	}
}

static int handle_login_rsp(union ibmvnic_crq *login_rsp_crq,
			    struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	struct ibmvnic_login_rsp_buffer *login_rsp = adapter->login_rsp_buf;
	struct ibmvnic_login_buffer *login = adapter->login_buf;
	int i;

	dma_unmap_single(dev, adapter->login_buf_token, adapter->login_buf_sz,
			 DMA_BIDIRECTIONAL);
	dma_unmap_single(dev, adapter->login_rsp_buf_token,
			 adapter->login_rsp_buf_sz, DMA_BIDIRECTIONAL);

	/* If the number of queues requested can't be allocated by the
	 * server, the login response will return with code 1. We will need
	 * to resend the login buffer with fewer queues requested.
	 */
	if (login_rsp_crq->generic.rc.code) {
		adapter->renegotiate = true;
		complete(&adapter->init_done);
		return 0;
	}

	netdev_dbg(adapter->netdev, "Login Response Buffer:\n");
	for (i = 0; i < (adapter->login_rsp_buf_sz - 1) / 8 + 1; i++) {
		netdev_dbg(adapter->netdev, "%016lx\n",
			   ((unsigned long int *)(adapter->login_rsp_buf))[i]);
	}

	/* Sanity checks */
	if (login->num_txcomp_subcrqs != login_rsp->num_txsubm_subcrqs ||
	    (be32_to_cpu(login->num_rxcomp_subcrqs) *
	     adapter->req_rx_add_queues !=
	     be32_to_cpu(login_rsp->num_rxadd_subcrqs))) {
		dev_err(dev, "FATAL: Inconsistent login and login rsp\n");
		ibmvnic_remove(adapter->vdev);
		return -EIO;
	}
	complete(&adapter->init_done);

	return 0;
}

static void handle_request_map_rsp(union ibmvnic_crq *crq,
				   struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	u8 map_id = crq->request_map_rsp.map_id;
	int tx_subcrqs;
	int rx_subcrqs;
	long rc;
	int i;

	tx_subcrqs = be32_to_cpu(adapter->login_rsp_buf->num_txsubm_subcrqs);
	rx_subcrqs = be32_to_cpu(adapter->login_rsp_buf->num_rxadd_subcrqs);

	rc = crq->request_map_rsp.rc.code;
	if (rc) {
		dev_err(dev, "Error %ld in REQUEST_MAP_RSP\n", rc);
		adapter->map_id--;
		/* need to find and zero tx/rx_pool map_id */
		for (i = 0; i < tx_subcrqs; i++) {
			if (adapter->tx_pool[i].long_term_buff.map_id == map_id)
				adapter->tx_pool[i].long_term_buff.map_id = 0;
		}
		for (i = 0; i < rx_subcrqs; i++) {
			if (adapter->rx_pool[i].long_term_buff.map_id == map_id)
				adapter->rx_pool[i].long_term_buff.map_id = 0;
		}
	}
	complete(&adapter->fw_done);
}

static void handle_request_unmap_rsp(union ibmvnic_crq *crq,
				     struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	long rc;

	rc = crq->request_unmap_rsp.rc.code;
	if (rc)
		dev_err(dev, "Error %ld in REQUEST_UNMAP_RSP\n", rc);
}

static void handle_query_map_rsp(union ibmvnic_crq *crq,
				 struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->vdev->dev;
	long rc;

	rc = crq->query_map_rsp.rc.code;
	if (rc) {
		dev_err(dev, "Error %ld in QUERY_MAP_RSP\n", rc);
		return;
	}
	netdev_dbg(netdev, "page_size = %d\ntot_pages = %d\nfree_pages = %d\n",
		   crq->query_map_rsp.page_size, crq->query_map_rsp.tot_pages,
		   crq->query_map_rsp.free_pages);
}

static void handle_query_cap_rsp(union ibmvnic_crq *crq,
				 struct ibmvnic_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->vdev->dev;
	long rc;

	atomic_dec(&adapter->running_cap_crqs);
	netdev_dbg(netdev, "Outstanding queries: %d\n",
		   atomic_read(&adapter->running_cap_crqs));
	rc = crq->query_capability.rc.code;
	if (rc) {
		dev_err(dev, "Error %ld in QUERY_CAP_RSP\n", rc);
		goto out;
	}

	switch (be16_to_cpu(crq->query_capability.capability)) {
	case MIN_TX_QUEUES:
		adapter->min_tx_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_tx_queues = %lld\n",
			   adapter->min_tx_queues);
		break;
	case MIN_RX_QUEUES:
		adapter->min_rx_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_rx_queues = %lld\n",
			   adapter->min_rx_queues);
		break;
	case MIN_RX_ADD_QUEUES:
		adapter->min_rx_add_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_rx_add_queues = %lld\n",
			   adapter->min_rx_add_queues);
		break;
	case MAX_TX_QUEUES:
		adapter->max_tx_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_tx_queues = %lld\n",
			   adapter->max_tx_queues);
		break;
	case MAX_RX_QUEUES:
		adapter->max_rx_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_rx_queues = %lld\n",
			   adapter->max_rx_queues);
		break;
	case MAX_RX_ADD_QUEUES:
		adapter->max_rx_add_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_rx_add_queues = %lld\n",
			   adapter->max_rx_add_queues);
		break;
	case MIN_TX_ENTRIES_PER_SUBCRQ:
		adapter->min_tx_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_tx_entries_per_subcrq = %lld\n",
			   adapter->min_tx_entries_per_subcrq);
		break;
	case MIN_RX_ADD_ENTRIES_PER_SUBCRQ:
		adapter->min_rx_add_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "min_rx_add_entrs_per_subcrq = %lld\n",
			   adapter->min_rx_add_entries_per_subcrq);
		break;
	case MAX_TX_ENTRIES_PER_SUBCRQ:
		adapter->max_tx_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_tx_entries_per_subcrq = %lld\n",
			   adapter->max_tx_entries_per_subcrq);
		break;
	case MAX_RX_ADD_ENTRIES_PER_SUBCRQ:
		adapter->max_rx_add_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_rx_add_entrs_per_subcrq = %lld\n",
			   adapter->max_rx_add_entries_per_subcrq);
		break;
	case TCP_IP_OFFLOAD:
		adapter->tcp_ip_offload =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "tcp_ip_offload = %lld\n",
			   adapter->tcp_ip_offload);
		break;
	case PROMISC_SUPPORTED:
		adapter->promisc_supported =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "promisc_supported = %lld\n",
			   adapter->promisc_supported);
		break;
	case MIN_MTU:
		adapter->min_mtu = be64_to_cpu(crq->query_capability.number);
		netdev->min_mtu = adapter->min_mtu - ETH_HLEN;
		netdev_dbg(netdev, "min_mtu = %lld\n", adapter->min_mtu);
		break;
	case MAX_MTU:
		adapter->max_mtu = be64_to_cpu(crq->query_capability.number);
		netdev->max_mtu = adapter->max_mtu - ETH_HLEN;
		netdev_dbg(netdev, "max_mtu = %lld\n", adapter->max_mtu);
		break;
	case MAX_MULTICAST_FILTERS:
		adapter->max_multicast_filters =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_multicast_filters = %lld\n",
			   adapter->max_multicast_filters);
		break;
	case VLAN_HEADER_INSERTION:
		adapter->vlan_header_insertion =
		    be64_to_cpu(crq->query_capability.number);
		if (adapter->vlan_header_insertion)
			netdev->features |= NETIF_F_HW_VLAN_STAG_TX;
		netdev_dbg(netdev, "vlan_header_insertion = %lld\n",
			   adapter->vlan_header_insertion);
		break;
	case RX_VLAN_HEADER_INSERTION:
		adapter->rx_vlan_header_insertion =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "rx_vlan_header_insertion = %lld\n",
			   adapter->rx_vlan_header_insertion);
		break;
	case MAX_TX_SG_ENTRIES:
		adapter->max_tx_sg_entries =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "max_tx_sg_entries = %lld\n",
			   adapter->max_tx_sg_entries);
		break;
	case RX_SG_SUPPORTED:
		adapter->rx_sg_supported =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "rx_sg_supported = %lld\n",
			   adapter->rx_sg_supported);
		break;
	case OPT_TX_COMP_SUB_QUEUES:
		adapter->opt_tx_comp_sub_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_tx_comp_sub_queues = %lld\n",
			   adapter->opt_tx_comp_sub_queues);
		break;
	case OPT_RX_COMP_QUEUES:
		adapter->opt_rx_comp_queues =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_rx_comp_queues = %lld\n",
			   adapter->opt_rx_comp_queues);
		break;
	case OPT_RX_BUFADD_Q_PER_RX_COMP_Q:
		adapter->opt_rx_bufadd_q_per_rx_comp_q =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_rx_bufadd_q_per_rx_comp_q = %lld\n",
			   adapter->opt_rx_bufadd_q_per_rx_comp_q);
		break;
	case OPT_TX_ENTRIES_PER_SUBCRQ:
		adapter->opt_tx_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_tx_entries_per_subcrq = %lld\n",
			   adapter->opt_tx_entries_per_subcrq);
		break;
	case OPT_RXBA_ENTRIES_PER_SUBCRQ:
		adapter->opt_rxba_entries_per_subcrq =
		    be64_to_cpu(crq->query_capability.number);
		netdev_dbg(netdev, "opt_rxba_entries_per_subcrq = %lld\n",
			   adapter->opt_rxba_entries_per_subcrq);
		break;
	case TX_RX_DESC_REQ:
		adapter->tx_rx_desc_req = crq->query_capability.number;
		netdev_dbg(netdev, "tx_rx_desc_req = %llx\n",
			   adapter->tx_rx_desc_req);
		break;

	default:
		netdev_err(netdev, "Got invalid cap rsp %d\n",
			   crq->query_capability.capability);
	}

out:
	if (atomic_read(&adapter->running_cap_crqs) == 0) {
		adapter->wait_capability = false;
		ibmvnic_send_req_caps(adapter, 0);
	}
}

static void ibmvnic_handle_crq(union ibmvnic_crq *crq,
			       struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_generic_crq *gen_crq = &crq->generic;
	struct net_device *netdev = adapter->netdev;
	struct device *dev = &adapter->vdev->dev;
	u64 *u64_crq = (u64 *)crq;
	long rc;

	netdev_dbg(netdev, "Handling CRQ: %016lx %016lx\n",
		   (unsigned long int)cpu_to_be64(u64_crq[0]),
		   (unsigned long int)cpu_to_be64(u64_crq[1]));
	switch (gen_crq->first) {
	case IBMVNIC_CRQ_INIT_RSP:
		switch (gen_crq->cmd) {
		case IBMVNIC_CRQ_INIT:
			dev_info(dev, "Partner initialized\n");
			break;
		case IBMVNIC_CRQ_INIT_COMPLETE:
			dev_info(dev, "Partner initialization complete\n");
			send_version_xchg(adapter);
			break;
		default:
			dev_err(dev, "Unknown crq cmd: %d\n", gen_crq->cmd);
		}
		return;
	case IBMVNIC_CRQ_XPORT_EVENT:
		netif_carrier_off(netdev);
		if (gen_crq->cmd == IBMVNIC_PARTITION_MIGRATED) {
			dev_info(dev, "Migrated, re-enabling adapter\n");
			ibmvnic_reset(adapter, VNIC_RESET_MOBILITY);
		} else if (gen_crq->cmd == IBMVNIC_DEVICE_FAILOVER) {
			dev_info(dev, "Backing device failover detected\n");
			ibmvnic_reset(adapter, VNIC_RESET_FAILOVER);
		} else {
			/* The adapter lost the connection */
			dev_err(dev, "Virtual Adapter failed (rc=%d)\n",
				gen_crq->cmd);
			ibmvnic_reset(adapter, VNIC_RESET_FATAL);
		}
		return;
	case IBMVNIC_CRQ_CMD_RSP:
		break;
	default:
		dev_err(dev, "Got an invalid msg type 0x%02x\n",
			gen_crq->first);
		return;
	}

	switch (gen_crq->cmd) {
	case VERSION_EXCHANGE_RSP:
		rc = crq->version_exchange_rsp.rc.code;
		if (rc) {
			dev_err(dev, "Error %ld in VERSION_EXCHG_RSP\n", rc);
			break;
		}
		dev_info(dev, "Partner protocol version is %d\n",
			 crq->version_exchange_rsp.version);
		if (be16_to_cpu(crq->version_exchange_rsp.version) <
		    ibmvnic_version)
			ibmvnic_version =
			    be16_to_cpu(crq->version_exchange_rsp.version);
		send_cap_queries(adapter);
		break;
	case QUERY_CAPABILITY_RSP:
		handle_query_cap_rsp(crq, adapter);
		break;
	case QUERY_MAP_RSP:
		handle_query_map_rsp(crq, adapter);
		break;
	case REQUEST_MAP_RSP:
		handle_request_map_rsp(crq, adapter);
		break;
	case REQUEST_UNMAP_RSP:
		handle_request_unmap_rsp(crq, adapter);
		break;
	case REQUEST_CAPABILITY_RSP:
		handle_request_cap_rsp(crq, adapter);
		break;
	case LOGIN_RSP:
		netdev_dbg(netdev, "Got Login Response\n");
		handle_login_rsp(crq, adapter);
		break;
	case LOGICAL_LINK_STATE_RSP:
		netdev_dbg(netdev,
			   "Got Logical Link State Response, state: %d rc: %d\n",
			   crq->logical_link_state_rsp.link_state,
			   crq->logical_link_state_rsp.rc.code);
		adapter->logical_link_state =
		    crq->logical_link_state_rsp.link_state;
		adapter->init_done_rc = crq->logical_link_state_rsp.rc.code;
		complete(&adapter->init_done);
		break;
	case LINK_STATE_INDICATION:
		netdev_dbg(netdev, "Got Logical Link State Indication\n");
		adapter->phys_link_state =
		    crq->link_state_indication.phys_link_state;
		adapter->logical_link_state =
		    crq->link_state_indication.logical_link_state;
		break;
	case CHANGE_MAC_ADDR_RSP:
		netdev_dbg(netdev, "Got MAC address change Response\n");
		handle_change_mac_rsp(crq, adapter);
		break;
	case ERROR_INDICATION:
		netdev_dbg(netdev, "Got Error Indication\n");
		handle_error_indication(crq, adapter);
		break;
	case REQUEST_ERROR_RSP:
		netdev_dbg(netdev, "Got Error Detail Response\n");
		handle_error_info_rsp(crq, adapter);
		break;
	case REQUEST_STATISTICS_RSP:
		netdev_dbg(netdev, "Got Statistics Response\n");
		complete(&adapter->stats_done);
		break;
	case QUERY_IP_OFFLOAD_RSP:
		netdev_dbg(netdev, "Got Query IP offload Response\n");
		handle_query_ip_offload_rsp(adapter);
		break;
	case MULTICAST_CTRL_RSP:
		netdev_dbg(netdev, "Got multicast control Response\n");
		break;
	case CONTROL_IP_OFFLOAD_RSP:
		netdev_dbg(netdev, "Got Control IP offload Response\n");
		dma_unmap_single(dev, adapter->ip_offload_ctrl_tok,
				 sizeof(adapter->ip_offload_ctrl),
				 DMA_TO_DEVICE);
		complete(&adapter->init_done);
		break;
	case COLLECT_FW_TRACE_RSP:
		netdev_dbg(netdev, "Got Collect firmware trace Response\n");
		complete(&adapter->fw_done);
		break;
	default:
		netdev_err(netdev, "Got an invalid cmd type 0x%02x\n",
			   gen_crq->cmd);
	}
}

static irqreturn_t ibmvnic_interrupt(int irq, void *instance)
{
	struct ibmvnic_adapter *adapter = instance;

	tasklet_schedule(&adapter->tasklet);
	return IRQ_HANDLED;
}

static void ibmvnic_tasklet(void *data)
{
	struct ibmvnic_adapter *adapter = data;
	struct ibmvnic_crq_queue *queue = &adapter->crq;
	union ibmvnic_crq *crq;
	unsigned long flags;
	bool done = false;

	spin_lock_irqsave(&queue->lock, flags);
	while (!done) {
		/* Pull all the valid messages off the CRQ */
		while ((crq = ibmvnic_next_crq(adapter)) != NULL) {
			ibmvnic_handle_crq(crq, adapter);
			crq->generic.first = 0;
		}

		/* remain in tasklet until all
		 * capabilities responses are received
		 */
		if (!adapter->wait_capability)
			done = true;
	}
	/* if capabilities CRQ's were sent in this tasklet, the following
	 * tasklet must wait until all responses are received
	 */
	if (atomic_read(&adapter->running_cap_crqs) != 0)
		adapter->wait_capability = true;
	spin_unlock_irqrestore(&queue->lock, flags);
}

static int ibmvnic_reenable_crq_queue(struct ibmvnic_adapter *adapter)
{
	struct vio_dev *vdev = adapter->vdev;
	int rc;

	do {
		rc = plpar_hcall_norets(H_ENABLE_CRQ, vdev->unit_address);
	} while (rc == H_IN_PROGRESS || rc == H_BUSY || H_IS_LONG_BUSY(rc));

	if (rc)
		dev_err(&vdev->dev, "Error enabling adapter (rc=%d)\n", rc);

	return rc;
}

static int ibmvnic_reset_crq(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_crq_queue *crq = &adapter->crq;
	struct device *dev = &adapter->vdev->dev;
	struct vio_dev *vdev = adapter->vdev;
	int rc;

	/* Close the CRQ */
	do {
		rc = plpar_hcall_norets(H_FREE_CRQ, vdev->unit_address);
	} while (rc == H_BUSY || H_IS_LONG_BUSY(rc));

	/* Clean out the queue */
	memset(crq->msgs, 0, PAGE_SIZE);
	crq->cur = 0;

	/* And re-open it again */
	rc = plpar_hcall_norets(H_REG_CRQ, vdev->unit_address,
				crq->msg_token, PAGE_SIZE);

	if (rc == H_CLOSED)
		/* Adapter is good, but other end is not ready */
		dev_warn(dev, "Partner adapter not ready\n");
	else if (rc != 0)
		dev_warn(dev, "Couldn't register crq (rc=%d)\n", rc);

	return rc;
}

static void release_crq_queue(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_crq_queue *crq = &adapter->crq;
	struct vio_dev *vdev = adapter->vdev;
	long rc;

	if (!crq->msgs)
		return;

	netdev_dbg(adapter->netdev, "Releasing CRQ\n");
	free_irq(vdev->irq, adapter);
	tasklet_kill(&adapter->tasklet);
	do {
		rc = plpar_hcall_norets(H_FREE_CRQ, vdev->unit_address);
	} while (rc == H_BUSY || H_IS_LONG_BUSY(rc));

	dma_unmap_single(&vdev->dev, crq->msg_token, PAGE_SIZE,
			 DMA_BIDIRECTIONAL);
	free_page((unsigned long)crq->msgs);
	crq->msgs = NULL;
}

static int init_crq_queue(struct ibmvnic_adapter *adapter)
{
	struct ibmvnic_crq_queue *crq = &adapter->crq;
	struct device *dev = &adapter->vdev->dev;
	struct vio_dev *vdev = adapter->vdev;
	int rc, retrc = -ENOMEM;

	if (crq->msgs)
		return 0;

	crq->msgs = (union ibmvnic_crq *)get_zeroed_page(GFP_KERNEL);
	/* Should we allocate more than one page? */

	if (!crq->msgs)
		return -ENOMEM;

	crq->size = PAGE_SIZE / sizeof(*crq->msgs);
	crq->msg_token = dma_map_single(dev, crq->msgs, PAGE_SIZE,
					DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, crq->msg_token))
		goto map_failed;

	rc = plpar_hcall_norets(H_REG_CRQ, vdev->unit_address,
				crq->msg_token, PAGE_SIZE);

	if (rc == H_RESOURCE)
		/* maybe kexecing and resource is busy. try a reset */
		rc = ibmvnic_reset_crq(adapter);
	retrc = rc;

	if (rc == H_CLOSED) {
		dev_warn(dev, "Partner adapter not ready\n");
	} else if (rc) {
		dev_warn(dev, "Error %d opening adapter\n", rc);
		goto reg_crq_failed;
	}

	retrc = 0;

	tasklet_init(&adapter->tasklet, (void *)ibmvnic_tasklet,
		     (unsigned long)adapter);

	netdev_dbg(adapter->netdev, "registering irq 0x%x\n", vdev->irq);
	rc = request_irq(vdev->irq, ibmvnic_interrupt, 0, IBMVNIC_NAME,
			 adapter);
	if (rc) {
		dev_err(dev, "Couldn't register irq 0x%x. rc=%d\n",
			vdev->irq, rc);
		goto req_irq_failed;
	}

	rc = vio_enable_interrupts(vdev);
	if (rc) {
		dev_err(dev, "Error %d enabling interrupts\n", rc);
		goto req_irq_failed;
	}

	crq->cur = 0;
	spin_lock_init(&crq->lock);

	return retrc;

req_irq_failed:
	tasklet_kill(&adapter->tasklet);
	do {
		rc = plpar_hcall_norets(H_FREE_CRQ, vdev->unit_address);
	} while (rc == H_BUSY || H_IS_LONG_BUSY(rc));
reg_crq_failed:
	dma_unmap_single(dev, crq->msg_token, PAGE_SIZE, DMA_BIDIRECTIONAL);
map_failed:
	free_page((unsigned long)crq->msgs);
	crq->msgs = NULL;
	return retrc;
}

static int ibmvnic_init(struct ibmvnic_adapter *adapter)
{
	struct device *dev = &adapter->vdev->dev;
	unsigned long timeout = msecs_to_jiffies(30000);
	int rc;

	rc = init_crq_queue(adapter);
	if (rc) {
		dev_err(dev, "Couldn't initialize crq. rc=%d\n", rc);
		return rc;
	}

	init_completion(&adapter->init_done);
	ibmvnic_send_crq_init(adapter);
	if (!wait_for_completion_timeout(&adapter->init_done, timeout)) {
		dev_err(dev, "Initialization sequence timed out\n");
		release_crq_queue(adapter);
		return -1;
	}

	rc = init_sub_crqs(adapter);
	if (rc) {
		dev_err(dev, "Initialization of sub crqs failed\n");
		release_crq_queue(adapter);
	}

	return rc;
}

static int ibmvnic_probe(struct vio_dev *dev, const struct vio_device_id *id)
{
	struct ibmvnic_adapter *adapter;
	struct net_device *netdev;
	unsigned char *mac_addr_p;
	int rc;

	dev_dbg(&dev->dev, "entering ibmvnic_probe for UA 0x%x\n",
		dev->unit_address);

	mac_addr_p = (unsigned char *)vio_get_attribute(dev,
							VETH_MAC_ADDR, NULL);
	if (!mac_addr_p) {
		dev_err(&dev->dev,
			"(%s:%3.3d) ERROR: Can't find MAC_ADDR attribute\n",
			__FILE__, __LINE__);
		return 0;
	}

	netdev = alloc_etherdev_mq(sizeof(struct ibmvnic_adapter),
				   IBMVNIC_MAX_TX_QUEUES);
	if (!netdev)
		return -ENOMEM;

	adapter = netdev_priv(netdev);
	adapter->state = VNIC_PROBING;
	dev_set_drvdata(&dev->dev, netdev);
	adapter->vdev = dev;
	adapter->netdev = netdev;

	ether_addr_copy(adapter->mac_addr, mac_addr_p);
	ether_addr_copy(netdev->dev_addr, adapter->mac_addr);
	netdev->irq = dev->irq;
	netdev->netdev_ops = &ibmvnic_netdev_ops;
	netdev->ethtool_ops = &ibmvnic_ethtool_ops;
	SET_NETDEV_DEV(netdev, &dev->dev);

	spin_lock_init(&adapter->stats_lock);

	INIT_LIST_HEAD(&adapter->errors);
	spin_lock_init(&adapter->error_list_lock);

	INIT_WORK(&adapter->ibmvnic_reset, __ibmvnic_reset);
	INIT_LIST_HEAD(&adapter->rwi_list);
	mutex_init(&adapter->reset_lock);
	mutex_init(&adapter->rwi_lock);
	adapter->resetting = false;

	rc = ibmvnic_init(adapter);
	if (rc) {
		free_netdev(netdev);
		return rc;
	}

	netdev->mtu = adapter->req_mtu - ETH_HLEN;

	rc = register_netdev(netdev);
	if (rc) {
		dev_err(&dev->dev, "failed to register netdev rc=%d\n", rc);
		free_netdev(netdev);
		return rc;
	}
	dev_info(&dev->dev, "ibmvnic registered\n");

	adapter->state = VNIC_PROBED;
	return 0;
}

static int ibmvnic_remove(struct vio_dev *dev)
{
	struct net_device *netdev = dev_get_drvdata(&dev->dev);
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);

	adapter->state = VNIC_REMOVING;
	unregister_netdev(netdev);
	mutex_lock(&adapter->reset_lock);

	release_resources(adapter);
	release_sub_crqs(adapter);
	release_crq_queue(adapter);

	adapter->state = VNIC_REMOVED;

	mutex_unlock(&adapter->reset_lock);
	free_netdev(netdev);
	dev_set_drvdata(&dev->dev, NULL);

	return 0;
}

static unsigned long ibmvnic_get_desired_dma(struct vio_dev *vdev)
{
	struct net_device *netdev = dev_get_drvdata(&vdev->dev);
	struct ibmvnic_adapter *adapter;
	struct iommu_table *tbl;
	unsigned long ret = 0;
	int i;

	tbl = get_iommu_table_base(&vdev->dev);

	/* netdev inits at probe time along with the structures we need below*/
	if (!netdev)
		return IOMMU_PAGE_ALIGN(IBMVNIC_IO_ENTITLEMENT_DEFAULT, tbl);

	adapter = netdev_priv(netdev);

	ret += PAGE_SIZE; /* the crq message queue */
	ret += IOMMU_PAGE_ALIGN(sizeof(struct ibmvnic_statistics), tbl);

	for (i = 0; i < adapter->req_tx_queues + adapter->req_rx_queues; i++)
		ret += 4 * PAGE_SIZE; /* the scrq message queue */

	for (i = 0; i < be32_to_cpu(adapter->login_rsp_buf->num_rxadd_subcrqs);
	     i++)
		ret += adapter->rx_pool[i].size *
		    IOMMU_PAGE_ALIGN(adapter->rx_pool[i].buff_size, tbl);

	return ret;
}

static int ibmvnic_resume(struct device *dev)
{
	struct net_device *netdev = dev_get_drvdata(dev);
	struct ibmvnic_adapter *adapter = netdev_priv(netdev);
	int i;

	/* kick the interrupt handlers just in case we lost an interrupt */
	for (i = 0; i < adapter->req_rx_queues; i++)
		ibmvnic_interrupt_rx(adapter->rx_scrq[i]->irq,
				     adapter->rx_scrq[i]);

	return 0;
}

static struct vio_device_id ibmvnic_device_table[] = {
	{"network", "IBM,vnic"},
	{"", "" }
};
MODULE_DEVICE_TABLE(vio, ibmvnic_device_table);

static const struct dev_pm_ops ibmvnic_pm_ops = {
	.resume = ibmvnic_resume
};

static struct vio_driver ibmvnic_driver = {
	.id_table       = ibmvnic_device_table,
	.probe          = ibmvnic_probe,
	.remove         = ibmvnic_remove,
	.get_desired_dma = ibmvnic_get_desired_dma,
	.name		= ibmvnic_driver_name,
	.pm		= &ibmvnic_pm_ops,
};

/* module functions */
static int __init ibmvnic_module_init(void)
{
	pr_info("%s: %s %s\n", ibmvnic_driver_name, ibmvnic_driver_string,
		IBMVNIC_DRIVER_VERSION);

	return vio_register_driver(&ibmvnic_driver);
}

static void __exit ibmvnic_module_exit(void)
{
	vio_unregister_driver(&ibmvnic_driver);
}

module_init(ibmvnic_module_init);
module_exit(ibmvnic_module_exit);
