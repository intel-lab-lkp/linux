// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) Tehuti Networks Ltd. */

#include "tn40.h"

#define SHORT_PACKET_SIZE 60

static void bdx_enable_interrupts(struct bdx_priv *priv)
{
	write_reg(priv, REG_IMR, priv->isr_mask);
}

static void bdx_disable_interrupts(struct bdx_priv *priv)
{
	write_reg(priv, REG_IMR, 0);
}

static int bdx_fifo_alloc(struct bdx_priv *priv, struct fifo *f, int fsz_type,
			  u16 reg_cfg0, u16 reg_cfg1, u16 reg_rptr, u16 reg_wptr)
{
	u16 memsz = FIFO_SIZE * (1 << fsz_type);

	memset(f, 0, sizeof(struct fifo));
	/* 1K extra space is allocated at the end of the fifo to simplify
	 * processing of descriptors that wraps around fifo's end.
	 */
	f->va = dma_alloc_coherent(&priv->pdev->dev,
				   memsz + FIFO_EXTRA_SPACE, &f->da, GFP_KERNEL);
	if (!f->va)
		return -ENOMEM;

	f->reg_cfg0 = reg_cfg0;
	f->reg_cfg1 = reg_cfg1;
	f->reg_rptr = reg_rptr;
	f->reg_wptr = reg_wptr;
	f->rptr = 0;
	f->wptr = 0;
	f->memsz = memsz;
	f->size_mask = memsz - 1;
	write_reg(priv, reg_cfg0, (u32)((f->da & TX_RX_CFG0_BASE) | fsz_type));
	write_reg(priv, reg_cfg1, H32_64(f->da));
	return 0;
}

static void bdx_fifo_free(struct bdx_priv *priv, struct fifo *f)
{
	dma_free_coherent(&priv->pdev->dev,
			  f->memsz + FIFO_EXTRA_SPACE, f->va, f->da);
}

/* TX HW/SW interaction overview
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * There are 2 types of TX communication channels between driver and NIC.
 * 1) TX Free Fifo - TXF - Holds ack descriptors for sent packets.
 * 2) TX Data Fifo - TXD - Holds descriptors of full buffers.
 *
 * Currently the NIC supports TSO, checksumming and gather DMA
 * UFO and IP fragmentation is on the way.
 *
 * RX SW Data Structures
 * ~~~~~~~~~~~~~~~~~~~~~
 * TXDB is used to keep track of all skbs owned by SW and their DMA addresses.
 * For TX case, ownership lasts from getting the packet via hard_xmit and
 * until the HW acknowledges sending the packet by TXF descriptors.
 * TXDB is implemented as a cyclic buffer.
 *
 * FIFO objects keep info about the fifo's size and location, relevant HW
 * registers, usage and skb db. Each RXD and RXF fifo has their own fifo
 * structure. Implemented as simple struct.
 *
 * TX SW Execution Flow
 * ~~~~~~~~~~~~~~~~~~~~
 * OS calls the driver's hard_xmit method with a packet to send. The driver
 * creates DMA mappings, builds TXD descriptors and kicks the HW by updating
 * TXD WPTR.
 *
 * When a packet is sent, The HW write a TXF descriptor and the SW
 * frees the original skb. To prevent TXD fifo overflow without
 * reading HW registers every time, the SW deploys "tx level"
 * technique. Upon startup, the tx level is initialized to TXD fifo
 * length. For every sent packet, the SW gets its TXD descriptor size
 * (from a pre-calculated array) and subtracts it from tx level.  The
 * size is also stored in txdb. When a TXF ack arrives, the SW fetched
 * the size of the original TXD descriptor from the txdb and adds it
 * to the tx level. When the Tx level drops below some predefined
 * threshold, the driver stops the TX queue. When the TX level rises
 * above that level, the tx queue is enabled again.
 *
 * This technique avoids excessive reading of RPTR and WPTR registers.
 * As our benchmarks shows, it adds 1.5 Gbit/sec to NIS's throughput.
 */
static inline int bdx_tx_db_size(struct txdb *db)
{
	int taken = db->wptr - db->rptr;

	if (taken < 0)
		taken = db->size + 1 + taken;	/* (size + 1) equals memsz */
	return db->size - taken;
}

static inline void __bdx_tx_db_ptr_next(struct txdb *db, struct tx_map **pptr)
{
	++*pptr;
	if (unlikely(*pptr == db->end))
		*pptr = db->start;
}

static inline void bdx_tx_db_inc_rptr(struct txdb *db)
{
	__bdx_tx_db_ptr_next(db, &db->rptr);
}

static inline void bdx_tx_db_inc_wptr(struct txdb *db)
{
	__bdx_tx_db_ptr_next(db, &db->wptr);
}

static int bdx_tx_db_init(struct txdb *d, int sz_type)
{
	int memsz = FIFO_SIZE * (1 << (sz_type + 1));

	d->start = vzalloc(memsz);
	if (!d->start)
		return -ENOMEM;
	/* In order to differentiate between an empty db state and a full db
	 * state at least one element should always be empty in order to
	 * avoid rptr == wptr, which means that the db is empty.
	 */
	d->size = memsz / sizeof(struct tx_map) - 1;
	d->end = d->start + d->size + 1;	/* just after last element */

	/* All dbs are created empty */
	d->rptr = d->start;
	d->wptr = d->start;
	return 0;
}

static void bdx_tx_db_close(struct txdb *d)
{
	if (d->start) {
		vfree(d->start);
		d->start = NULL;
	}
}

/* Sizes of tx desc (including padding if needed) as function of the SKB's
 * frag number
 */
static struct {
	u16 bytes;
	u16 qwords;		/* qword = 64 bit */
} txd_sizes[MAX_PBL];

inline void bdx_set_pbl(struct pbl *pbl, dma_addr_t dma, int len)
{
	pbl->len = cpu_to_le32(len);
	pbl->pa_lo = cpu_to_le32(L32_64(dma));
	pbl->pa_hi = cpu_to_le32(H32_64(dma));
}

static inline void bdx_set_txdb(struct txdb *db, dma_addr_t dma, int len)
{
	db->wptr->len = len;
	db->wptr->addr.dma = dma;
}

struct mapping_info {
	dma_addr_t dma;
	size_t size;
};

/**
 * txdb_map_skb - create and store DMA mappings for skb's data blocks
 * @priv: NIC private structure
 * @skb: socket buffer to map
 * @txdd: pointer to tx descriptor to be updated
 * @pkt_len: pointer to unsigned long value
 *
 * This function creates DMA mappings for skb's data blocks and writes them to
 * PBL of a new tx descriptor. It also stores them in the tx db, so they could
 * be unmapped after the data has been sent. It is the responsibility of the
 * caller to make sure that there is enough space in the txdb. The last
 * element holds a pointer to skb itself and is marked with a zero length.
 *
 * Return: 0 on success and negative value on error.
 */
static inline int bdx_tx_map_skb(struct bdx_priv *priv, struct sk_buff *skb,
				 struct txd_desc *txdd, unsigned int *pkt_len)
{
	dma_addr_t dma;
	int i, len, err;
	struct txdb *db = &priv->txdb;
	struct pbl *pbl = &txdd->pbl[0];
	int nr_frags = skb_shinfo(skb)->nr_frags;
	unsigned int size;
	struct mapping_info info[MAX_PBL];

	netdev_dbg(priv->ndev, "TX skb %p skbLen %d dataLen %d frags %d\n", skb,
		   skb->len, skb->data_len, nr_frags);
	if (nr_frags > MAX_PBL - 1) {
		err = skb_linearize(skb);
		if (err)
			return -1;
		nr_frags = skb_shinfo(skb)->nr_frags;
	}
	/* initial skb */
	len = skb->len - skb->data_len;
	dma = dma_map_single(&priv->pdev->dev, skb->data, len,
			     DMA_TO_DEVICE);
	if (dma_mapping_error(&priv->pdev->dev, dma))
		return -1;

	bdx_set_txdb(db, dma, len);
	bdx_set_pbl(pbl++, db->wptr->addr.dma, db->wptr->len);
	*pkt_len = db->wptr->len;

	for (i = 0; i < nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		size = skb_frag_size(frag);
		dma = skb_frag_dma_map(&priv->pdev->dev, frag, 0,
				       size, DMA_TO_DEVICE);

		if (dma_mapping_error(&priv->pdev->dev, dma))
			goto mapping_error;
		info[i].dma = dma;
		info[i].size = size;
	}

	for (i = 0; i < nr_frags; i++) {
		bdx_tx_db_inc_wptr(db);
		bdx_set_txdb(db, info[i].dma, info[i].size);
		bdx_set_pbl(pbl++, db->wptr->addr.dma, db->wptr->len);
		*pkt_len += db->wptr->len;
	}

	/* SHORT_PKT_FIX */
	if (skb->len < SHORT_PACKET_SIZE)
		++nr_frags;

	/* Add skb clean up info. */
	bdx_tx_db_inc_wptr(db);
	db->wptr->len = -txd_sizes[nr_frags].bytes;
	db->wptr->addr.skb = skb;
	bdx_tx_db_inc_wptr(db);

	return 0;
 mapping_error:
	dma_unmap_page(&priv->pdev->dev, db->wptr->addr.dma, db->wptr->len, DMA_TO_DEVICE);
	for (; i > 0; i--)
		dma_unmap_page(&priv->pdev->dev, info[i - 1].dma, info[i - 1].size, DMA_TO_DEVICE);
	return -1;
}

static void init_txd_sizes(void)
{
	int i, lwords;

	if (txd_sizes[0].bytes)
		return;

	/* 7 - is number of lwords in txd with one phys buffer
	 * 3 - is number of lwords used for every additional phys buffer
	 */
	for (i = 0; i < MAX_PBL; i++) {
		lwords = 7 + (i * 3);
		if (lwords & 1)
			lwords++;	/* pad it with 1 lword */
		txd_sizes[i].qwords = lwords >> 1;
		txd_sizes[i].bytes = lwords << 2;
	}
}

static int create_tx_ring(struct bdx_priv *priv)
{
	int ret;

	ret = bdx_fifo_alloc(priv, &priv->txd_fifo0.m, priv->txd_size,
			     REG_TXD_CFG0_0, REG_TXD_CFG1_0,
			     REG_TXD_RPTR_0, REG_TXD_WPTR_0);
	if (ret)
		return ret;

	ret = bdx_fifo_alloc(priv, &priv->txf_fifo0.m, priv->txf_size,
			     REG_TXF_CFG0_0, REG_TXF_CFG1_0,
			     REG_TXF_RPTR_0, REG_TXF_WPTR_0);
	if (ret)
		goto err_free_txd;

	/* The TX db has to keep mappings for all packets sent (on
	 * TxD) and not yet reclaimed (on TxF).
	 */
	ret = bdx_tx_db_init(&priv->txdb, max(priv->txd_size, priv->txf_size));
	if (ret)
		goto err_free_txf;

	/* SHORT_PKT_FIX */
	priv->b0_len = 64;
	priv->b0_va =
		dma_alloc_coherent(&priv->pdev->dev, priv->b0_len, &priv->b0_dma, GFP_KERNEL);
	if (!priv->b0_va)
		goto err_free_db;

	priv->tx_level = BDX_MAX_TX_LEVEL;
	priv->tx_update_mark = priv->tx_level - 1024;
	return 0;
err_free_db:
	bdx_tx_db_close(&priv->txdb);
err_free_txf:
	bdx_fifo_free(priv, &priv->txf_fifo0.m);
err_free_txd:
	bdx_fifo_free(priv, &priv->txd_fifo0.m);
	return ret;
}

/**
 * bdx_tx_space - Calculate the available space in the TX fifo.
 *
 * @priv - NIC private structure
 * Return: available space in TX fifo in bytes
 */
static inline int bdx_tx_space(struct bdx_priv *priv)
{
	struct txd_fifo *f = &priv->txd_fifo0;
	int fsize;

	f->m.rptr = read_reg(priv, f->m.reg_rptr) & TXF_WPTR_WR_PTR;
	fsize = f->m.rptr - f->m.wptr;
	if (fsize <= 0)
		fsize = f->m.memsz + fsize;
	return fsize;
}

static int bdx_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct bdx_priv *priv = netdev_priv(ndev);
	struct txd_fifo *f = &priv->txd_fifo0;
	int txd_checksum = 7;	/* full checksum */
	int txd_lgsnd = 0;
	int txd_vlan_id = 0;
	int txd_vtag = 0;
	int txd_mss = 0;
	unsigned int pkt_len;
	struct txd_desc *txdd;
	int nr_frags, len, err;

	/* Build tx descriptor */
	txdd = (struct txd_desc *)(f->m.va + f->m.wptr);
	err = bdx_tx_map_skb(priv, skb, txdd, &pkt_len);
	if (err) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}
	nr_frags = skb_shinfo(skb)->nr_frags;
	if (unlikely(skb->ip_summed != CHECKSUM_PARTIAL))
		txd_checksum = 0;

	if (skb_shinfo(skb)->gso_size) {
		txd_mss = skb_shinfo(skb)->gso_size;
		txd_lgsnd = 1;
		netdev_dbg(priv->ndev, "skb %p pkt len %d gso size = %d\n", skb,
			   pkt_len, txd_mss);
	}
	if (skb_vlan_tag_present(skb)) {
		/* Don't cut VLAN ID to 12 bits */
		txd_vlan_id = skb_vlan_tag_get(skb);
		txd_vtag = 1;
	}
	txdd->va_hi = 0;
	txdd->va_lo = (u32)((u64)skb);
	txdd->length = cpu_to_le16(pkt_len);
	txdd->mss = cpu_to_le16(txd_mss);
	txdd->txd_val1 =
		cpu_to_le32(TXD_W1_VAL
			    (txd_sizes[nr_frags].qwords, txd_checksum,
			     txd_vtag, txd_lgsnd, txd_vlan_id));
	netdev_dbg(priv->ndev, "=== w1 qwords[%d] %d =====\n", nr_frags,
		   txd_sizes[nr_frags].qwords);
	netdev_dbg(priv->ndev, "=== TxD desc =====================\n");
	netdev_dbg(priv->ndev, "=== w1: 0x%x ================\n", txdd->txd_val1);
	netdev_dbg(priv->ndev, "=== w2: mss 0x%x len 0x%x\n", txdd->mss,
		   txdd->length);
	/* SHORT_PKT_FIX */
	if (pkt_len < SHORT_PACKET_SIZE) {
		struct pbl *pbl = &txdd->pbl[++nr_frags];

		txdd->length = cpu_to_le16(SHORT_PACKET_SIZE);
		txdd->txd_val1 =
			cpu_to_le32(TXD_W1_VAL
				    (txd_sizes[nr_frags].qwords,
				     txd_checksum, txd_vtag, txd_lgsnd,
				     txd_vlan_id));
		pbl->len = cpu_to_le32(SHORT_PACKET_SIZE - pkt_len);
		pbl->pa_lo = cpu_to_le32(L32_64(priv->b0_dma));
		pbl->pa_hi = cpu_to_le32(H32_64(priv->b0_dma));
		netdev_dbg(priv->ndev, "=== SHORT_PKT_FIX   ================\n");
		netdev_dbg(priv->ndev, "=== nr_frags : %d   ================\n",
			   nr_frags);
	}

	/* Increment TXD write pointer. In case of fifo wrapping copy
	 * reminder of the descriptor to the beginning.
	 */
	f->m.wptr += txd_sizes[nr_frags].bytes;
	len = f->m.wptr - f->m.memsz;
	if (unlikely(len >= 0)) {
		f->m.wptr = len;
		if (len > 0)
			memcpy(f->m.va, f->m.va + f->m.memsz, len);
	}
	/* Force memory writes to complete before letting the HW know
	 * there are new descriptors to fetch.
	 */
	wmb();

	priv->tx_level -= txd_sizes[nr_frags].bytes;
	if (priv->tx_level > priv->tx_update_mark) {
		write_reg(priv, f->m.reg_wptr,
			  f->m.wptr & TXF_WPTR_WR_PTR);
	} else {
		if (priv->tx_noupd++ > BDX_NO_UPD_PACKETS) {
			priv->tx_noupd = 0;
			write_reg(priv, f->m.reg_wptr,
				  f->m.wptr & TXF_WPTR_WR_PTR);
		}
	}

	netif_trans_update(ndev);
	priv->net_stats.tx_packets++;
	priv->net_stats.tx_bytes += pkt_len;
	if (priv->tx_level < BDX_MIN_TX_LEVEL) {
		netdev_dbg(priv->ndev, "TX Q STOP level %d\n", priv->tx_level);
		netif_stop_queue(ndev);
	}

	return NETDEV_TX_OK;
}

static void bdx_tx_free_skbs(struct bdx_priv *priv)
{
	struct txdb *db = &priv->txdb;

	while (db->rptr != db->wptr) {
		if (likely(db->rptr->len))
			dma_unmap_page(&priv->pdev->dev, db->rptr->addr.dma,
				       db->rptr->len, DMA_TO_DEVICE);
		else
			dev_kfree_skb(db->rptr->addr.skb);
		bdx_tx_db_inc_rptr(db);
	}
}

static void destroy_tx_ring(struct bdx_priv *priv)
{
	bdx_tx_free_skbs(priv);
	bdx_fifo_free(priv, &priv->txd_fifo0.m);
	bdx_fifo_free(priv, &priv->txf_fifo0.m);
	bdx_tx_db_close(&priv->txdb);
	/* SHORT_PKT_FIX */
	if (priv->b0_len) {
		dma_free_coherent(&priv->pdev->dev, priv->b0_len, priv->b0_va,
				  priv->b0_dma);
		priv->b0_len = 0;
	}
}

/**
 * bdx_tx_push_desc - Push a descriptor to TxD fifo.
 *
 * @priv: NIC private structure
 * @data: desc's data
 * @size: desc's size
 *
 * This function pushes desc to TxD fifo and overlaps it if needed.
 *
 * This function does not check for available space, nor does it check
 * that the data size is smaller than the fifo size. Checking for
 * space is the responsibility of the caller.
 */
static void bdx_tx_push_desc(struct bdx_priv *priv, void *data, int size)
{
	struct txd_fifo *f = &priv->txd_fifo0;
	int i = f->m.memsz - f->m.wptr;

	if (size == 0)
		return;

	if (i > size) {
		memcpy(f->m.va + f->m.wptr, data, size);
		f->m.wptr += size;
	} else {
		memcpy(f->m.va + f->m.wptr, data, i);
		f->m.wptr = size - i;
		memcpy(f->m.va, data + i, f->m.wptr);
	}
	write_reg(priv, f->m.reg_wptr, f->m.wptr & TXF_WPTR_WR_PTR);
}

/**
 * bdx_tx_push_desc_safe - push descriptor to TxD fifo in a safe way.
 *
 * @priv: NIC private structure
 * @data: descriptor data
 * @size: descriptor size
 *
 * This function does check for available space and, if necessary,
 * waits for the NIC to read existing data before writing new data.
 */
static void bdx_tx_push_desc_safe(struct bdx_priv *priv, void *data, int size)
{
	int timer = 0;

	while (size > 0) {
		/* We subtract 8 because when the fifo is full rptr ==
		 * wptr, which also means that fifo is empty, we can
		 * understand the difference, but could the HW do the
		 * same ???
		 */
		int avail = bdx_tx_space(priv) - 8;

		if (avail <= 0) {
			if (timer++ > 300) {	/* Prevent endless loop */
				netdev_dbg(priv->ndev, "timeout while writing desc to TxD fifo\n");
				break;
			}
			udelay(50);	/* Give the HW a chance to clean the fifo */
			continue;
		}
		avail = min(avail, size);
		netdev_dbg(priv->ndev, "about to push  %d bytes starting %p size %d\n", avail,
			   data, size);
		bdx_tx_push_desc(priv, data, avail);
		size -= avail;
		data += avail;
	}
}

static int bdx_set_link_speed(struct bdx_priv *priv, u32 speed)
{
	int i;
	u32 val;

	netdev_dbg(priv->ndev, "speed %d\n", speed);

	switch (speed) {
	case SPEED_10000:
	case SPEED_5000:
	case SPEED_2500:
		netdev_dbg(priv->ndev, "link_speed %d\n", speed);

		write_reg(priv, 0x1010, 0x217);	/*ETHSD.REFCLK_CONF  */
		write_reg(priv, 0x104c, 0x4c);	/*ETHSD.L0_RX_PCNT  */
		write_reg(priv, 0x1050, 0x4c);	/*ETHSD.L1_RX_PCNT  */
		write_reg(priv, 0x1054, 0x4c);	/*ETHSD.L2_RX_PCNT  */
		write_reg(priv, 0x1058, 0x4c);	/*ETHSD.L3_RX_PCNT  */
		write_reg(priv, 0x102c, 0x434);	/*ETHSD.L0_TX_PCNT  */
		write_reg(priv, 0x1030, 0x434);	/*ETHSD.L1_TX_PCNT  */
		write_reg(priv, 0x1034, 0x434);	/*ETHSD.L2_TX_PCNT  */
		write_reg(priv, 0x1038, 0x434);	/*ETHSD.L3_TX_PCNT  */
		write_reg(priv, 0x6300, 0x0400);	/*MAC.PCS_CTRL */

		write_reg(priv, 0x1018, 0x00);	/*Mike2 */
		udelay(5);
		write_reg(priv, 0x1018, 0x04);	/*Mike2 */
		udelay(5);
		write_reg(priv, 0x1018, 0x06);	/*Mike2 */
		udelay(5);
		/*MikeFix1 */
		/*L0: 0x103c , L1: 0x1040 , L2: 0x1044 , L3: 0x1048 =0x81644 */
		write_reg(priv, 0x103c, 0x81644);	/*ETHSD.L0_TX_DCNT  */
		write_reg(priv, 0x1040, 0x81644);	/*ETHSD.L1_TX_DCNT  */
		write_reg(priv, 0x1044, 0x81644);	/*ETHSD.L2_TX_DCNT  */
		write_reg(priv, 0x1048, 0x81644);	/*ETHSD.L3_TX_DCNT  */
		write_reg(priv, 0x1014, 0x043);	/*ETHSD.INIT_STAT */
		for (i = 1000; i; i--) {
			udelay(50);
			val = read_reg(priv, 0x1014);	/*ETHSD.INIT_STAT */
			if (val & (1 << 9)) {
				write_reg(priv, 0x1014, 0x3);	/*ETHSD.INIT_STAT */
				val = read_reg(priv, 0x1014);	/*ETHSD.INIT_STAT */

				break;
			}
		}
		if (!i)
			netdev_err(priv->ndev, "MAC init timeout!\n");

		write_reg(priv, 0x6350, 0x0);	/*MAC.PCS_IF_MODE */
		write_reg(priv, REG_CTRLST, 0xC13);	/*0x93//0x13 */
		write_reg(priv, 0x111c, 0x7ff);	/*MAC.MAC_RST_CNT */
		for (i = 40; i--;)
			udelay(50);

		write_reg(priv, 0x111c, 0x0);	/*MAC.MAC_RST_CNT */
		break;

	case SPEED_1000:
	case SPEED_100:
		write_reg(priv, 0x1010, 0x613);	/*ETHSD.REFCLK_CONF  */
		write_reg(priv, 0x104c, 0x4d);	/*ETHSD.L0_RX_PCNT  */
		write_reg(priv, 0x1050, 0x0);	/*ETHSD.L1_RX_PCNT  */
		write_reg(priv, 0x1054, 0x0);	/*ETHSD.L2_RX_PCNT  */
		write_reg(priv, 0x1058, 0x0);	/*ETHSD.L3_RX_PCNT  */
		write_reg(priv, 0x102c, 0x35);	/*ETHSD.L0_TX_PCNT  */
		write_reg(priv, 0x1030, 0x0);	/*ETHSD.L1_TX_PCNT  */
		write_reg(priv, 0x1034, 0x0);	/*ETHSD.L2_TX_PCNT  */
		write_reg(priv, 0x1038, 0x0);	/*ETHSD.L3_TX_PCNT  */
		write_reg(priv, 0x6300, 0x01140);	/*MAC.PCS_CTRL */

		write_reg(priv, 0x1014, 0x043);	/*ETHSD.INIT_STAT */
		for (i = 1000; i; i--) {
			udelay(50);
			val = read_reg(priv, 0x1014);	/*ETHSD.INIT_STAT */
			if (val & (1 << 9)) {
				write_reg(priv, 0x1014, 0x3);	/*ETHSD.INIT_STAT */
				val = read_reg(priv, 0x1014);	/*ETHSD.INIT_STAT */

				break;
			}
		}
		if (!i)
			netdev_err(priv->ndev, "MAC init timeout!\n");

		write_reg(priv, 0x6350, 0x2b);	/*MAC.PCS_IF_MODE 1g */
		write_reg(priv, 0x6310, 0x9801);	/*MAC.PCS_DEV_AB */

		write_reg(priv, 0x6314, 0x1);	/*MAC.PCS_PART_AB */
		write_reg(priv, 0x6348, 0xc8);	/*MAC.PCS_LINK_LO */
		write_reg(priv, 0x634c, 0xc8);	/*MAC.PCS_LINK_HI */
		udelay(50);
		write_reg(priv, REG_CTRLST, 0xC13);	/*0x93//0x13 */
		write_reg(priv, 0x111c, 0x7ff);	/*MAC.MAC_RST_CNT */
		for (i = 40; i--;)
			udelay(50);

		write_reg(priv, 0x111c, 0x0);	/*MAC.MAC_RST_CNT */
		write_reg(priv, 0x6300, 0x1140);	/*MAC.PCS_CTRL */
		break;

	case 0:		/* Link down */
		write_reg(priv, 0x104c, 0x0);	/*ETHSD.L0_RX_PCNT  */
		write_reg(priv, 0x1050, 0x0);	/*ETHSD.L1_RX_PCNT  */
		write_reg(priv, 0x1054, 0x0);	/*ETHSD.L2_RX_PCNT  */
		write_reg(priv, 0x1058, 0x0);	/*ETHSD.L3_RX_PCNT  */
		write_reg(priv, 0x102c, 0x0);	/*ETHSD.L0_TX_PCNT  */
		write_reg(priv, 0x1030, 0x0);	/*ETHSD.L1_TX_PCNT  */
		write_reg(priv, 0x1034, 0x0);	/*ETHSD.L2_TX_PCNT  */
		write_reg(priv, 0x1038, 0x0);	/*ETHSD.L3_TX_PCNT  */

		write_reg(priv, REG_CTRLST, 0x800);
		write_reg(priv, 0x111c, 0x7ff);	/*MAC.MAC_RST_CNT */
		for (i = 40; i--;)
			udelay(50);
		write_reg(priv, 0x111c, 0x0);	/*MAC.MAC_RST_CNT */
		break;

	default:
		netdev_err(priv->ndev, "Link speed was not identified yet (%d)\n", speed);
		speed = 0;
		break;
	}

	return speed;
}

#define LINK_LOOP_MAX 10

static void bdx_link_changed(struct bdx_priv *priv)
{
	u32 link = read_reg(priv, REG_MAC_LNK_STAT) & MAC_LINK_STAT;

	if (!link) {
		if (netif_carrier_ok(priv->ndev) && priv->link) {
			netif_stop_queue(priv->ndev);
			netif_carrier_off(priv->ndev);
			netdev_info(priv->ndev, "Device link is down.\n");
		}
		priv->link = 0;
		netif_carrier_off(priv->ndev);
		if (priv->link_loop_cnt++ > LINK_LOOP_MAX) {
			/* MAC reset */
			bdx_set_link_speed(priv, 0);
			priv->link_loop_cnt = 0;
		}
		write_reg(priv, 0x5150, 1000000);
		return;
	}
	priv->link = link;
}

static inline void bdx_isr_extra(struct bdx_priv *priv, u32 isr)
{
	if (isr & (IR_LNKCHG0 | IR_LNKCHG1 | IR_TMR0)) {
		netdev_dbg(priv->ndev, "isr = 0x%x\n", isr);
		bdx_link_changed(priv);
	}
}

static irqreturn_t bdx_isr_napi(int irq, void *dev)
{
	struct net_device *ndev = dev;
	struct bdx_priv *priv = netdev_priv(ndev);
	u32 isr;

	isr = read_reg(priv, REG_ISR_MSK0);

	if (unlikely(!isr)) {
		bdx_enable_interrupts(priv);
		return IRQ_NONE;	/* Not our interrupt */
	}

	if (isr & IR_EXTRA)
		bdx_isr_extra(priv, isr);

	if (isr & (IR_RX_DESC_0 | IR_TX_FREE_0 | IR_TMR1)) {
		/* We get here if an interrupt has slept into the
		 * small time window between these lines in
		 * bdx_poll: bdx_enable_interrupts(priv); return 0;
		 *
		 * Currently interrupts are disabled (since we read
		 * the ISR register) and we have failed to register
		 * the next poll. So we read the regs to trigger the
		 * chip and allow further interrupts.
		 */
		read_reg(priv, REG_TXF_WPTR_0);
		read_reg(priv, REG_RXD_WPTR_0);
	}

	bdx_enable_interrupts(priv);
	return IRQ_HANDLED;
}

static int bdx_fw_load(struct bdx_priv *priv)
{
	int master, i, ret;
	const struct firmware *fw = NULL;

	ret = request_firmware(&fw, "tn40xx-14.fw", &priv->pdev->dev);
	if (ret)
		return ret;

	master = read_reg(priv, REG_INIT_SEMAPHORE);
	if (!read_reg(priv, REG_INIT_STATUS) && master) {
		netdev_dbg(priv->ndev, "Loading FW...\n");
		bdx_tx_push_desc_safe(priv, (void *)fw->data, fw->size);
		mdelay(100);
	}
	for (i = 0; i < 200; i++) {
		if (read_reg(priv, REG_INIT_STATUS))
			break;
		mdelay(2);
	}
	if (master)
		write_reg(priv, REG_INIT_SEMAPHORE, 1);

	if (i == 200) {
		netdev_err(priv->ndev, "%s firmware loading failed\n", priv->ndev->name);
		netdev_dbg(priv->ndev, "VPC = 0x%x VIC = 0x%x INIT_STATUS = 0x%x i =%d\n",
			   read_reg(priv, REG_VPC),
			   read_reg(priv, REG_VIC), read_reg(priv, REG_INIT_STATUS), i);
		ret = -EIO;
	} else {
		netdev_dbg(priv->ndev, "%s firmware loading success\n", priv->ndev->name);
	}
	release_firmware(fw);
	return ret;
}

static void bdx_restore_mac(struct net_device *ndev, struct bdx_priv *priv)
{
	u32 val;

	netdev_dbg(priv->ndev, "mac0 =%x mac1 =%x mac2 =%x\n",
		   read_reg(priv, REG_UNC_MAC0_A),
		   read_reg(priv, REG_UNC_MAC1_A), read_reg(priv, REG_UNC_MAC2_A));

	val = (ndev->dev_addr[0] << 8) | (ndev->dev_addr[1]);
	write_reg(priv, REG_UNC_MAC2_A, val);
	val = (ndev->dev_addr[2] << 8) | (ndev->dev_addr[3]);
	write_reg(priv, REG_UNC_MAC1_A, val);
	val = (ndev->dev_addr[4] << 8) | (ndev->dev_addr[5]);
	write_reg(priv, REG_UNC_MAC0_A, val);

	/* More then IP MAC address */
	write_reg(priv, REG_MAC_ADDR_0,
		  (ndev->dev_addr[3] << 24) | (ndev->dev_addr[2] << 16) |
		  (ndev->dev_addr[1]
		   << 8) | (ndev->dev_addr[0]));
	write_reg(priv, REG_MAC_ADDR_1,
		  (ndev->dev_addr[5] << 8) | (ndev->dev_addr[4]));

	netdev_dbg(priv->ndev, "mac0 =%x mac1 =%x mac2 =%x\n",
		   read_reg(priv, REG_UNC_MAC0_A),
		   read_reg(priv, REG_UNC_MAC1_A), read_reg(priv, REG_UNC_MAC2_A));
}

static int bdx_hw_start(struct bdx_priv *priv)
{
	write_reg(priv, REG_FRM_LENGTH, 0X3FE0);
	write_reg(priv, REG_GMAC_RXF_A, 0X10fd);
	/*MikeFix1 */
	/*L0: 0x103c , L1: 0x1040 , L2: 0x1044 , L3: 0x1048 =0x81644 */
	write_reg(priv, 0x103c, 0x81644);	/*ETHSD.L0_TX_DCNT  */
	write_reg(priv, 0x1040, 0x81644);	/*ETHSD.L1_TX_DCNT  */
	write_reg(priv, 0x1044, 0x81644);	/*ETHSD.L2_TX_DCNT  */
	write_reg(priv, 0x1048, 0x81644);	/*ETHSD.L3_TX_DCNT  */
	write_reg(priv, REG_RX_FIFO_SECTION, 0x10);
	write_reg(priv, REG_TX_FIFO_SECTION, 0xE00010);
	write_reg(priv, REG_RX_FULLNESS, 0);
	write_reg(priv, REG_TX_FULLNESS, 0);

	write_reg(priv, REG_VGLB, 0);
	write_reg(priv, REG_RDINTCM0, priv->rdintcm);
	write_reg(priv, REG_RDINTCM2, 0);

	write_reg(priv, REG_TDINTCM0, priv->tdintcm);	/* old val = 0x300064 */

	/* Enable timer interrupt once in 2 secs. */
	bdx_restore_mac(priv->ndev, priv);

	/* Pause frame */
	write_reg(priv, 0x12E0, 0x28);
	write_reg(priv, REG_PAUSE_QUANT, 0xFFFF);
	write_reg(priv, 0x6064, 0xF);

	write_reg(priv, REG_GMAC_RXF_A,
		  GMAC_RX_FILTER_OSEN | GMAC_RX_FILTER_TXFC | GMAC_RX_FILTER_AM
		  | GMAC_RX_FILTER_AB);

	bdx_link_changed(priv);
	bdx_enable_interrupts(priv);
	return 0;
}

static int bdx_hw_reset(struct bdx_priv *priv)
{
	u32 val, i;

	/* Reset sequences: read, write 1, read, write 0 */
	val = read_reg(priv, REG_CLKPLL);
	write_reg(priv, REG_CLKPLL, (val | CLKPLL_SFTRST) + 0x8);
	udelay(50);
	val = read_reg(priv, REG_CLKPLL);
	write_reg(priv, REG_CLKPLL, val & ~CLKPLL_SFTRST);

	/* Check that the PLLs are locked and reset ended */
	for (i = 0; i < 70; i++, mdelay(10)) {
		if ((read_reg(priv, REG_CLKPLL) & CLKPLL_LKD) == CLKPLL_LKD) {
			udelay(50);
			/* Do any PCI-E read transaction */
			read_reg(priv, REG_RXD_CFG0_0);
			return 0;
		}
	}
	return 1;
}

static int bdx_sw_reset(struct bdx_priv *priv)
{
	int i;

	/* 1. load MAC (obsolete) */
	/* 2. disable Rx (and Tx) */
	write_reg(priv, REG_GMAC_RXF_A, 0);
	mdelay(100);
	/* 3. Disable port */
	write_reg(priv, REG_DIS_PORT, 1);
	/* 4. Disable queue */
	write_reg(priv, REG_DIS_QU, 1);
	/* 5. Wait until hw is disabled */
	for (i = 0; i < 50; i++) {
		if (read_reg(priv, REG_RST_PORT) & 1)
			break;
		mdelay(10);
	}
	if (i == 50) {
		netdev_err(priv->ndev, "%s SW reset timeout. continuing anyway\n",
			   priv->ndev->name);
	}
	/* 6. Disable interrupts */
	write_reg(priv, REG_RDINTCM0, 0);
	write_reg(priv, REG_TDINTCM0, 0);
	write_reg(priv, REG_IMR, 0);
	read_reg(priv, REG_ISR);

	/* 7. Reset queue */
	write_reg(priv, REG_RST_QU, 1);
	/* 8. Reset port */
	write_reg(priv, REG_RST_PORT, 1);
	/* 9. Zero all read and write pointers */
	for (i = REG_TXD_WPTR_0; i <= REG_TXF_RPTR_3; i += 0x10)
		write_reg(priv, i, 0);
	/* 10. Unset port disable */
	write_reg(priv, REG_DIS_PORT, 0);
	/* 11. Unset queue disable */
	write_reg(priv, REG_DIS_QU, 0);
	/* 12. Unset queue reset */
	write_reg(priv, REG_RST_QU, 0);
	/* 13. Unset port reset */
	write_reg(priv, REG_RST_PORT, 0);
	/* 14. Enable Rx */
	/* Skipped. will be done later */
	return 0;
}

static int bdx_start(struct bdx_priv *priv)
{
	int ret;

	ret = create_tx_ring(priv);
	if (ret) {
		netdev_err(priv->ndev, "failed to tx init %d\n", ret);
		return ret;
	}

	ret = request_irq(priv->pdev->irq, &bdx_isr_napi, IRQF_SHARED,
			  priv->ndev->name, priv->ndev);
	if (ret) {
		netdev_err(priv->ndev, "failed to request irq %d\n", ret);
		goto err_tx_ring;
	}

	ret = bdx_hw_start(priv);
	if (ret) {
		netdev_err(priv->ndev, "failed to hw start %d\n", ret);
		goto err_free_irq;
	}
	return 0;
err_free_irq:
	free_irq(priv->pdev->irq, priv->ndev);
err_tx_ring:
	destroy_tx_ring(priv);
	return ret;
}

static int bdx_close(struct net_device *ndev)
{
	struct bdx_priv *priv = netdev_priv(ndev);

	netif_carrier_off(ndev);

	bdx_disable_interrupts(priv);
	free_irq(priv->pdev->irq, priv->ndev);
	bdx_sw_reset(priv);
	destroy_tx_ring(priv);
	return 0;
}

static int bdx_open(struct net_device *dev)
{
	struct bdx_priv *priv = netdev_priv(dev);
	int ret;

	bdx_sw_reset(priv);
	ret = bdx_start(priv);
	if (ret) {
		netdev_err(dev, "failed to start %d\n", ret);
		return ret;
	}
	return 0;
}

static void __bdx_vlan_rx_vid(struct net_device *ndev, uint16_t vid, int enable)
{
	struct bdx_priv *priv = netdev_priv(ndev);
	u32 reg, bit, val;

	netdev_dbg(priv->ndev, "vid =%d value =%d\n", (int)vid, enable);
	if (unlikely(vid >= 4096)) {
		netdev_err(priv->ndev, "invalid VID: %u (> 4096)\n", vid);
		return;
	}
	reg = REG_VLAN_0 + (vid / 32) * 4;
	bit = 1 << vid % 32;
	val = read_reg(priv, reg);
	netdev_dbg(priv->ndev, "reg =%x, val =%x, bit =%d\n", reg, val, bit);
	if (enable)
		val |= bit;
	else
		val &= ~bit;
	netdev_dbg(priv->ndev, "new val %x\n", val);
	write_reg(priv, reg, val);
}

static int bdx_vlan_rx_add_vid(struct net_device *ndev,
			       __always_unused __be16 proto, u16 vid)
{
	__bdx_vlan_rx_vid(ndev, vid, 1);
	return 0;
}

static int bdx_vlan_rx_kill_vid(struct net_device *ndev,
				__always_unused __be16 proto, u16 vid)
{
	__bdx_vlan_rx_vid(ndev, vid, 0);
	return 0;
}

static void bdx_setmulti(struct net_device *ndev)
{
	struct bdx_priv *priv = netdev_priv(ndev);

	u32 rxf_val =
	    GMAC_RX_FILTER_AM | GMAC_RX_FILTER_AB | GMAC_RX_FILTER_OSEN |
	    GMAC_RX_FILTER_TXFC;
	int i;

	/* IMF - imperfect (hash) rx multicast filter */
	/* PMF - perfect rx multicast filter */

	/* FIXME: RXE(OFF) */
	if (ndev->flags & IFF_PROMISC) {
		rxf_val |= GMAC_RX_FILTER_PRM;
	} else if (ndev->flags & IFF_ALLMULTI) {
		/* set IMF to accept all multicast frames */
		for (i = 0; i < MAC_MCST_HASH_NUM; i++)
			write_reg(priv, REG_RX_MCST_HASH0 + i * 4, ~0);
	} else if (netdev_mc_count(ndev)) {
		u8 hash;
		struct netdev_hw_addr *mclist;
		u32 reg, val;

		/* Set IMF to deny all multicast frames */
		for (i = 0; i < MAC_MCST_HASH_NUM; i++)
			write_reg(priv, REG_RX_MCST_HASH0 + i * 4, 0);

		/* Set PMF to deny all multicast frames */
		for (i = 0; i < MAC_MCST_NUM; i++) {
			write_reg(priv, REG_RX_MAC_MCST0 + i * 8, 0);
			write_reg(priv, REG_RX_MAC_MCST1 + i * 8, 0);
		}
		/* Use PMF to accept first MAC_MCST_NUM (15) addresses */

		/* TBD: Sort the addresses and write them in ascending
		 * order into RX_MAC_MCST regs. we skip this phase now
		 * and accept ALL multicast frames through IMF. Accept
		 * the rest of addresses throw IMF.
		 */
		netdev_for_each_mc_addr(mclist, ndev) {
			hash = 0;
			for (i = 0; i < ETH_ALEN; i++)
				hash ^= mclist->addr[i];

			reg = REG_RX_MCST_HASH0 + ((hash >> 5) << 2);
			val = read_reg(priv, reg);
			val |= (1 << (hash % 32));
			write_reg(priv, reg, val);
		}
	} else {
		rxf_val |= GMAC_RX_FILTER_AB;
	}
	write_reg(priv, REG_GMAC_RXF_A, rxf_val);
	/* Enable RX */
	/* FIXME: RXE(ON) */
}

static int bdx_set_mac(struct net_device *ndev, void *p)
{
	struct bdx_priv *priv = netdev_priv(ndev);
	struct sockaddr *addr = p;

	eth_hw_addr_set(ndev, addr->sa_data);
	bdx_restore_mac(ndev, priv);
	return 0;
}

static void bdx_mac_init(struct bdx_priv *priv)
{
	u8 addr[ETH_ALEN];
	u64 val;

	val = (u64)read_reg(priv, REG_UNC_MAC0_A);
	val |= (u64)read_reg(priv, REG_UNC_MAC1_A) << 16;
	val |= (u64)read_reg(priv, REG_UNC_MAC2_A) << 32;

	u64_to_ether_addr(val, addr);
	eth_hw_addr_set(priv->ndev, addr);
}

static struct net_device_stats *bdx_get_stats(struct net_device *ndev)
{
	struct bdx_priv *priv = netdev_priv(ndev);
	struct net_device_stats *net_stat = &priv->net_stats;
	return net_stat;
}

static const struct net_device_ops bdx_netdev_ops = {
	.ndo_open = bdx_open,
	.ndo_stop = bdx_close,
	.ndo_start_xmit = bdx_start_xmit,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_set_rx_mode = bdx_setmulti,
	.ndo_get_stats = bdx_get_stats,
	.ndo_set_mac_address = bdx_set_mac,
	.ndo_vlan_rx_add_vid = bdx_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid = bdx_vlan_rx_kill_vid,
};

static int bdx_priv_init(struct bdx_priv *priv)
{
	int ret;

	ret = bdx_hw_reset(priv);
	if (ret)
		return ret;

	/* Set GPIO[9:0] to output 0 */
	write_reg(priv, 0x51E0, 0x30010006);	/* GPIO_OE_ WR CMD */
	write_reg(priv, 0x51F0, 0x0);	/* GPIO_OE_ DATA */
	write_reg(priv, REG_MDIO_CMD_STAT, 0x3ec8);

	// we use tx descriptors to load a firmware.
	ret = create_tx_ring(priv);
	if (ret)
		return ret;
	ret = bdx_fw_load(priv);
	destroy_tx_ring(priv);
	return ret;
}

static struct net_device *bdx_netdev_alloc(struct pci_dev *pdev)
{
	struct net_device *ndev;

	ndev = alloc_etherdev(sizeof(struct bdx_priv));
	if (!ndev)
		return NULL;
	ndev->netdev_ops = &bdx_netdev_ops;
	ndev->tx_queue_len = BDX_NDEV_TXQ_LEN;
	ndev->mem_start = pci_resource_start(pdev, 0);
	ndev->mem_end = pci_resource_end(pdev, 0);
	ndev->min_mtu = ETH_ZLEN;
	ndev->max_mtu = BDX_MAX_MTU;

	ndev->features = NETIF_F_IP_CSUM |
		NETIF_F_SG |
		NETIF_F_FRAGLIST |
		NETIF_F_TSO | NETIF_F_GRO |
		NETIF_F_RXCSUM |
		NETIF_F_RXHASH |
		NETIF_F_HW_VLAN_CTAG_TX |
		NETIF_F_HW_VLAN_CTAG_RX |
		NETIF_F_HW_VLAN_CTAG_FILTER;
	ndev->vlan_features = NETIF_F_IP_CSUM |
			       NETIF_F_SG |
			       NETIF_F_TSO | NETIF_F_GRO | NETIF_F_RXHASH;

	if (dma_get_mask(&pdev->dev) == DMA_BIT_MASK(64)) {
		ndev->features |= NETIF_F_HIGHDMA;
		ndev->vlan_features |= NETIF_F_HIGHDMA;
	}
	ndev->hw_features |= ndev->features;

	SET_NETDEV_DEV(ndev, &pdev->dev);
	netif_carrier_off(ndev);
	netif_stop_queue(ndev);

	return ndev;
}

static int bdx_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct net_device *ndev;
	struct bdx_priv *priv;
	int ret;
	unsigned int nvec = 1;
	void __iomem *regs;

	init_txd_sizes();

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64))) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "failed to set DMA mask.\n");
			goto err_disable_device;
		}
	}

	ret = pci_request_regions(pdev, BDX_DRV_NAME);
	if (ret) {
		dev_err(&pdev->dev, "failed to request PCI regions.\n");
		goto err_disable_device;
	}

	pci_set_master(pdev);

	regs = pci_iomap(pdev, 0, BDX_REGS_SIZE);
	if (!regs) {
		ret = -EIO;
		dev_err(&pdev->dev, "failed to map PCI bar.\n");
		goto err_free_regions;
	}

	ndev = bdx_netdev_alloc(pdev);
	if (!ndev) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "failed to allocate netdev.\n");
		goto err_iounmap;
	}

	priv = netdev_priv(ndev);
	pci_set_drvdata(pdev, priv);

	priv->regs = regs;
	priv->pdev = pdev;
	priv->ndev = ndev;
	/* Initialize fifo sizes. */
	priv->txd_size = 3;
	priv->txf_size = 3;
	priv->rxd_size = 3;
	priv->rxf_size = 3;
	/* Initialize the initial coalescing registers. */
	priv->rdintcm = INT_REG_VAL(0x20, 1, 4, 12);
	priv->tdintcm = INT_REG_VAL(0x20, 1, 0, 12);

	ret = bdx_hw_reset(priv);
	if (ret) {
		dev_err(&pdev->dev, "failed to reset HW.\n");
		goto err_free_netdev;
	}

	ret = pci_alloc_irq_vectors(pdev, 1, nvec, PCI_IRQ_MSI);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to allocate irq.\n");
		goto err_free_netdev;
	}

	priv->stats_flag = ((read_reg(priv, FPGA_VER) & 0xFFF) != 308);

	priv->isr_mask =
	    IR_RX_FREE_0 | IR_LNKCHG0 | IR_PSE | IR_TMR0 | IR_RX_DESC_0 |
	    IR_TX_FREE_0 | IR_TMR1;

	bdx_mac_init(priv);
	ret = register_netdev(ndev);
	if (ret) {
		dev_err(&pdev->dev, "failed to register netdev.\n");
		goto err_free_irq;
	}

	ret = bdx_priv_init(priv);
	if (ret) {
		dev_err(&pdev->dev, "failed to initialize bdx_priv.\n");
		goto err_unregister_netdev;
	}

	return 0;
err_unregister_netdev:
	unregister_netdev(ndev);
err_free_irq:
	pci_free_irq_vectors(pdev);
err_free_netdev:
	pci_set_drvdata(pdev, NULL);
	free_netdev(ndev);
err_iounmap:
	iounmap(regs);
err_free_regions:
	pci_release_regions(pdev);
err_disable_device:
	pci_disable_device(pdev);
	return ret;
}

static void bdx_remove(struct pci_dev *pdev)
{
	struct bdx_priv *priv = pci_get_drvdata(pdev);
	struct net_device *ndev = priv->ndev;

	unregister_netdev(ndev);

	pci_free_irq_vectors(priv->pdev);
	pci_set_drvdata(pdev, NULL);
	iounmap(priv->regs);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	free_netdev(ndev);
}

static const struct pci_device_id bdx_id_table[] = {
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_TEHUTI, 0x4022,
			 PCI_VENDOR_ID_TEHUTI, 0x3015) },
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_TEHUTI, 0x4022,
			 PCI_VENDOR_ID_DLINK, 0x4d00) },
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_TEHUTI, 0x4022,
			 PCI_VENDOR_ID_ASUSTEK, 0x8709) },
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_TEHUTI, 0x4022,
			 PCI_VENDOR_ID_EDIMAX, 0x8103) },
	{ }
};

static struct pci_driver bdx_driver = {
	.name = BDX_DRV_NAME,
	.id_table = bdx_id_table,
	.probe = bdx_probe,
	.remove = bdx_remove,
};

module_pci_driver(bdx_driver);

MODULE_DEVICE_TABLE(pci, bdx_id_table);
MODULE_AUTHOR("Tehuti networks");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Tehuti Network TN30xx Driver");
MODULE_VERSION(BDX_DRV_VERSION);
