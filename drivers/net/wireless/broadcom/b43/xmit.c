// SPDX-License-Identifier: GPL-2.0-or-later
/*

  Broadcom B43 wireless driver

  Transmission (TX/RX) related functions.

  Copyright (C) 2005 Martin Langer <martin-langer@gmx.de>
  Copyright (C) 2005 Stefano Brivio <stefano.brivio@polimi.it>
  Copyright (C) 2005, 2006 Michael Buesch <m@bues.ch>
  Copyright (C) 2005 Danny van Dyk <kugelfang@gentoo.org>
  Copyright (C) 2005 Andreas Jaggi <andreas.jaggi@waterwave.ch>


*/

#include "xmit.h"
#include "phy_common.h"
#include "dma.h"
#include "pio.h"

static const struct b43_tx_legacy_rate_phy_ctl_entry b43_tx_legacy_rate_phy_ctl[] = {
	{ B43_CCK_RATE_1MB,	0x0,			0x0 },
	{ B43_CCK_RATE_2MB,	0x0,			0x1 },
	{ B43_CCK_RATE_5MB,	0x0,			0x2 },
	{ B43_CCK_RATE_11MB,	0x0,			0x3 },
	{ B43_OFDM_RATE_6MB,	B43_TXH_PHY1_CRATE_1_2,	B43_TXH_PHY1_MODUL_BPSK },
	{ B43_OFDM_RATE_9MB,	B43_TXH_PHY1_CRATE_3_4,	B43_TXH_PHY1_MODUL_BPSK },
	{ B43_OFDM_RATE_12MB,	B43_TXH_PHY1_CRATE_1_2,	B43_TXH_PHY1_MODUL_QPSK },
	{ B43_OFDM_RATE_18MB,	B43_TXH_PHY1_CRATE_3_4,	B43_TXH_PHY1_MODUL_QPSK },
	{ B43_OFDM_RATE_24MB,	B43_TXH_PHY1_CRATE_1_2,	B43_TXH_PHY1_MODUL_QAM16 },
	{ B43_OFDM_RATE_36MB,	B43_TXH_PHY1_CRATE_3_4,	B43_TXH_PHY1_MODUL_QAM16 },
	{ B43_OFDM_RATE_48MB,	B43_TXH_PHY1_CRATE_2_3,	B43_TXH_PHY1_MODUL_QAM64 },
	{ B43_OFDM_RATE_54MB,	B43_TXH_PHY1_CRATE_3_4,	B43_TXH_PHY1_MODUL_QAM64 },
};

static const struct b43_tx_legacy_rate_phy_ctl_entry *
b43_tx_legacy_rate_phy_ctl_ent(u8 bitrate)
{
	const struct b43_tx_legacy_rate_phy_ctl_entry *e;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b43_tx_legacy_rate_phy_ctl); i++) {
		e = &(b43_tx_legacy_rate_phy_ctl[i]);
		if (e->bitrate == bitrate)
			return e;
	}

	B43_WARN_ON(1);
	return NULL;
}

/* Extract the bitrate index out of a CCK PLCP header. */
static int b43_plcp_get_bitrate_idx_cck(struct b43_plcp_hdr6 *plcp)
{
	switch (plcp->raw[0]) {
	case 0x0A:
		return 0;
	case 0x14:
		return 1;
	case 0x37:
		return 2;
	case 0x6E:
		return 3;
	}
	return -1;
}

/* Extract the bitrate index out of an OFDM PLCP header. */
static int b43_plcp_get_bitrate_idx_ofdm(struct b43_plcp_hdr6 *plcp, bool ghz5)
{
	/* For 2 GHz band first OFDM rate is at index 4, see main.c */
	int base = ghz5 ? 0 : 4;

	switch (plcp->raw[0] & 0xF) {
	case 0xB:
		return base + 0;
	case 0xF:
		return base + 1;
	case 0xA:
		return base + 2;
	case 0xE:
		return base + 3;
	case 0x9:
		return base + 4;
	case 0xD:
		return base + 5;
	case 0x8:
		return base + 6;
	case 0xC:
		return base + 7;
	}
	return -1;
}

u8 b43_plcp_get_ratecode_cck(const u8 bitrate)
{
	switch (bitrate) {
	case B43_CCK_RATE_1MB:
		return 0x0A;
	case B43_CCK_RATE_2MB:
		return 0x14;
	case B43_CCK_RATE_5MB:
		return 0x37;
	case B43_CCK_RATE_11MB:
		return 0x6E;
	}
	B43_WARN_ON(1);
	return 0;
}

u8 b43_plcp_get_ratecode_ofdm(const u8 bitrate)
{
	switch (bitrate) {
	case B43_OFDM_RATE_6MB:
		return 0xB;
	case B43_OFDM_RATE_9MB:
		return 0xF;
	case B43_OFDM_RATE_12MB:
		return 0xA;
	case B43_OFDM_RATE_18MB:
		return 0xE;
	case B43_OFDM_RATE_24MB:
		return 0x9;
	case B43_OFDM_RATE_36MB:
		return 0xD;
	case B43_OFDM_RATE_48MB:
		return 0x8;
	case B43_OFDM_RATE_54MB:
		return 0xC;
	}
	B43_WARN_ON(1);
	return 0;
}

void b43_generate_plcp_hdr(struct b43_plcp_hdr4 *plcp,
			   const u16 octets, const u8 bitrate)
{
	__u8 *raw = plcp->raw;

	if (b43_is_ofdm_rate(bitrate)) {
		u32 d;

		d = b43_plcp_get_ratecode_ofdm(bitrate);
		B43_WARN_ON(octets & 0xF000);
		d |= (octets << 5);
		plcp->data = cpu_to_le32(d);
	} else {
		u32 plen;

		plen = octets * 16 / bitrate;
		if ((octets * 16 % bitrate) > 0) {
			plen++;
			if ((bitrate == B43_CCK_RATE_11MB)
			    && ((octets * 8 % 11) < 4)) {
				raw[1] = 0x84;
			} else
				raw[1] = 0x04;
		} else
			raw[1] = 0x04;
		plcp->data |= cpu_to_le32(plen << 16);
		raw[0] = b43_plcp_get_ratecode_cck(bitrate);
	}
}

/* TODO: verify if needed for SSLPN or LCN  */
static u16 b43_generate_tx_phy_ctl1(struct b43_wldev *dev, u8 bitrate)
{
	const struct b43_phy *phy = &dev->phy;
	const struct b43_tx_legacy_rate_phy_ctl_entry *e;
	u16 control = 0;
	u16 bw;

	if (phy->type == B43_PHYTYPE_LP)
		bw = B43_TXH_PHY1_BW_20;
	else /* FIXME */
		bw = B43_TXH_PHY1_BW_20;

	if (0) { /* FIXME: MIMO */
	} else if (b43_is_cck_rate(bitrate) && phy->type != B43_PHYTYPE_LP) {
		control = bw;
	} else {
		control = bw;
		e = b43_tx_legacy_rate_phy_ctl_ent(bitrate);
		if (e) {
			control |= e->coding_rate;
			control |= e->modulation;
		}
		control |= B43_TXH_PHY1_MODE_SISO;
	}

	return control;
}

static u8 b43_calc_fallback_rate(u8 bitrate, int gmode)
{
	switch (bitrate) {
	case B43_CCK_RATE_1MB:
		return B43_CCK_RATE_1MB;
	case B43_CCK_RATE_2MB:
		return B43_CCK_RATE_1MB;
	case B43_CCK_RATE_5MB:
		return B43_CCK_RATE_2MB;
	case B43_CCK_RATE_11MB:
		return B43_CCK_RATE_5MB;
	/*
	 * Don't just fallback to CCK; it may be in 5GHz operation
	 * and falling back to CCK won't work out very well.
	 */
	case B43_OFDM_RATE_6MB:
		if (gmode)
			return B43_CCK_RATE_5MB;
		else
			return B43_OFDM_RATE_6MB;
	case B43_OFDM_RATE_9MB:
		return B43_OFDM_RATE_6MB;
	case B43_OFDM_RATE_12MB:
		return B43_OFDM_RATE_9MB;
	case B43_OFDM_RATE_18MB:
		return B43_OFDM_RATE_12MB;
	case B43_OFDM_RATE_24MB:
		return B43_OFDM_RATE_18MB;
	case B43_OFDM_RATE_36MB:
		return B43_OFDM_RATE_24MB;
	case B43_OFDM_RATE_48MB:
		return B43_OFDM_RATE_36MB;
	case B43_OFDM_RATE_54MB:
		return B43_OFDM_RATE_48MB;
	}
	B43_WARN_ON(1);
	return 0;
}

/*
 * One VHT rate block (frame type 3), as the stock driver lays it out and
 * Alessio Ferri's driver builds it: the width in word 0 bits 15:14, the
 * subband of the primary within that width in word 1, MCS and NSS - 1 in
 * word 2, VHT-SIG-A1/A2 in the PLCP bytes (group ID 63, both reserved bits
 * set), FBW 0x0300 and the rate in 500 kbit/s.
 */
static void b43_txhdr_ac_vht_block(u8 *ri, const struct ieee80211_tx_rate *t,
				   u16 chanspec)
{
	static const u8 rate_bw[] = {
		RATE_INFO_BW_20, RATE_INFO_BW_40, RATE_INFO_BW_80,
	};
	u8 mcs = ieee80211_rate_get_vht_mcs(t);
	u8 nss = ieee80211_rate_get_vht_nss(t);
	unsigned int bw = 0;
	struct rate_info rinfo = { .flags = RATE_INFO_FLAGS_VHT_MCS };
	u16 sb = 0;
	u32 a1, a2;

	/* wider than 20 MHz only inside the 80 MHz channel */
	if ((chanspec & 0x3800) == 0x2000) {
		if (t->flags & IEEE80211_TX_RC_80_MHZ_WIDTH)
			bw = 2;
		else if (t->flags & IEEE80211_TX_RC_40_MHZ_WIDTH)
			bw = 1;
		sb = ((chanspec >> 8) & 0x7) >> bw;
	}
	rinfo.mcs = mcs;
	rinfo.nss = nss;
	rinfo.bw = rate_bw[bw];

	put_unaligned_le16(3 | 0x0004 |
			   ((nss > 1 ? 3 : 1) << 6) |
			   (bw << 14), ri + 0x00);
	put_unaligned_le16(sb, ri + 0x02);
	put_unaligned_le16((mcs & 0xf) | ((nss - 1) << 4), ri + 0x04);
	a1 = bw | BIT(2) | (63 << 4) | ((nss - 1) << 10) | BIT(23);
	a2 = (mcs << 4) | BIT(9);
	ri[0x06] = a1;
	ri[0x07] = a1 >> 8;
	ri[0x08] = a1 >> 16;
	ri[0x09] = a2;
	ri[0x0a] = a2 >> 8;
	ri[0x0b] = a2 >> 16;
	put_unaligned_le16(0x0300, ri + 0x0c);
	put_unaligned_le16(cfg80211_calculate_bitrate(&rinfo) / 5, ri + 0x0e);
}

static int b43_generate_txhdr_ac(struct b43_wldev *dev, u8 *h,
				 struct sk_buff *skb,
				 struct ieee80211_tx_info *info, u16 cookie)
{
	static const u8 cck[] = { B43_CCK_RATE_1MB, B43_CCK_RATE_2MB,
				  B43_CCK_RATE_5MB, B43_CCK_RATE_11MB };
	static const u8 ofdm[] = { B43_OFDM_RATE_6MB, B43_OFDM_RATE_9MB,
				   B43_OFDM_RATE_12MB, B43_OFDM_RATE_18MB,
				   B43_OFDM_RATE_24MB, B43_OFDM_RATE_36MB,
				   B43_OFDM_RATE_48MB, B43_OFDM_RATE_54MB };
	const struct ieee80211_hdr *wlhdr = (const void *)skb->data;
	struct ieee80211_rate *txrate =
		ieee80211_get_tx_rate(dev->wl->hw, info);
	u8 rate = txrate ? txrate->hw_value : B43_CCK_RATE_1MB;
	bool is_ofdm = b43_is_ofdm_rate(rate);
	unsigned int len = skb->len + FCS_LEN;
	u8 *ri = h + B43_TXH_AC_RATE(0);
	u16 mac_lo = 0, phy0, idx = 0, chanspec, subband;
	int ht_mcs = -1;
	/* Only HT rates go out as session frames: a legacy probe frame with the
	 * aggregate header fails at every attempt.
	 */
	bool vht = info->control.rates[0].flags & IEEE80211_TX_RC_VHT_MCS;
	bool agg = (info->flags & IEEE80211_TX_CTL_AMPDU) &&
		   (info->control.rates[0].flags &
		    (IEEE80211_TX_RC_MCS | IEEE80211_TX_RC_VHT_MCS));
	const u8 *tbl = is_ofdm ? ofdm : cck;
	unsigned int n = is_ofdm ? ARRAY_SIZE(ofdm) : ARRAY_SIZE(cck);

	/* Hardware crypto needs the AC key fields; load with nohwcrypt=1. */
	if (info->control.hw_key)
		return -EOPNOTSUPP;

	memset(h, 0, B43_TXH_AC_LEN);
	h[0] = 0x02;	/* passthrough prefix, as wl sends it */
	h[2] = 0x02;
	h += B43_TXH_AC_PREFIX;
	ri = h + B43_TXH_AC_RATE(0);
	while (idx < n && tbl[idx] != rate)
		idx++;
	if (idx == n)
		idx = 0;

	if (!is_multicast_ether_addr(wlhdr->addr1) &&
	    !(info->flags & IEEE80211_TX_CTL_NO_ACK))
		mac_lo |= 0x0080;	/* expect immediate ACK */
	if (!(le16_to_cpu(wlhdr->seq_ctrl) & IEEE80211_SCTL_FRAG))
		mac_lo |= 0x4000;	/* start of MSDU */
	if (ieee80211_is_beacon(wlhdr->frame_control))
		mac_lo |= 0x0200;	/* ignore PMQ */

	chanspec = b43_ac_chanspec(dev, dev->phy.channel);
	/* A 20 MHz frame inside an 80 MHz channel names its subband (word 1) */
	subband = (chanspec & 0x3800) == 0x2000 ? (chanspec >> 8) & 0x7 : 0;

	if (agg) {
		/* MPDU of a block ack session, as the stock driver posts it
		 * (rxtx-1s-ht20-40-80.zip): MAC control 0x45c0, no fixed rate,
		 * the sequence number, and the cache info at +0x64: no cipher
		 * (we encrypt in software; the stock 50 04 makes the microcode
		 * encrypt again), MPDUs per aggregate at the primary and the
		 * fallback rate, duration 5414 us, window 63, max length 0x14.
		 * Without the cache info the AP deauthenticates us.
		 */
		static const u8 cache[8] = { 0x00, 0x00, 0x04, 0x04,
					     0x26, 0x15, 0x3f, 0x14 };

		mac_lo = 0x45c0;
		memcpy(h + 0x64, cache, sizeof(cache));
	}
	put_unaligned_le16(mac_lo, h + 0x02);
	put_unaligned_le16(agg ? 0 : 0x0002, h + 0x04);	/* fixed rate */
	put_unaligned_le16(chanspec, h + 0x06);
	h[0x08] = ieee80211_hdrlen(wlhdr->frame_control);
	put_unaligned_le16(len, h + 0x0a);
	put_unaligned_le16(cookie, h + 0x0c);
	if (agg)
		put_unaligned_le16(le16_to_cpu(wlhdr->seq_ctrl), h + 0x0e);

	/* Rate entry 0: frame type, always-set bit 2, TX core 0 mask. */
	phy0 = (is_ofdm ? 1 : 0) | 0x0004 | (1 << 6);
	if (!is_ofdm && rate != B43_CCK_RATE_1MB &&
	    (info->control.rates[0].flags & IEEE80211_TX_RC_USE_SHORT_PREAMBLE))
		phy0 |= 0x0010;
	if (info->control.rates[0].flags & IEEE80211_TX_RC_MCS) {
		ht_mcs = info->control.rates[0].idx;
	} else if (vht && !agg) {
		/*
		 * A VHT frame always goes out as an A-MPDU; outside a session
		 * send the HT20 rate of the same stream count instead.
		 */
		const struct ieee80211_tx_rate *r = &info->control.rates[0];

		ht_mcs = min_t(int, ieee80211_rate_get_vht_mcs(r), 7) +
			 8 * (ieee80211_rate_get_vht_nss(r) - 1);
	}

	if (vht && agg) {
		b43_txhdr_ac_vht_block(ri, &info->control.rates[0], chanspec);
	} else if (ht_mcs >= 0 && ht_mcs <= 15) {
		/*
		 * HT20: frame type 2, PLCP as brcmsmac lays it out, rate
		 * index = MCS
		 */
		static const u8 ht20_500k[8] = { 13, 26,  39,  52,
						 78, 104, 117, 130 };
		u8 mcs = ht_mcs;
		u8 *plcp = ri + 0x06;

		phy0 = 2 | 0x0004 | ((mcs > 7 ? 3 : 1) << 6);
		put_unaligned_le16(phy0, ri + 0x00);
		put_unaligned_le16(subband, ri + 0x02);
		put_unaligned_le16(mcs, ri + 0x04);
		plcp[0] = mcs;
		plcp[1] = len & 0xff;
		plcp[2] = len >> 8;
		/* smoothing, not sounding, reserved (brcmsmac) */
		plcp[3] = 0x07;
		plcp[4] = 0;
		plcp[5] = 0;
		put_unaligned_le16(ht20_500k[mcs & 7] * (mcs > 7 ? 2 : 1),
				   ri + 0x0e);
	} else {
		put_unaligned_le16(phy0, ri + 0x00);
		put_unaligned_le16(subband, ri + 0x02);
		put_unaligned_le16(idx, ri + 0x04);
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)(ri + 0x06),
				      len, rate);
		put_unaligned_le16(rate, ri + 0x0e);	/* 500 kbit/s units */
	}
	if (vht && agg && info->control.rates[1].idx >= 0 &&
	    (info->control.rates[1].flags & IEEE80211_TX_RC_VHT_MCS)) {
		put_unaligned_le16(0, ri + 0x10);
		ri += 0x14;
		b43_txhdr_ac_vht_block(ri, &info->control.rates[1], chanspec);
	} else if (agg && !vht && info->control.rates[1].idx >= 0 &&
	    info->control.rates[1].idx <= 15 &&
	    (info->control.rates[1].flags & IEEE80211_TX_RC_MCS)) {
		/* Second rate block: the fallback rate of the session frame */
		static const u8 ht20_500k[8] = { 13, 26,  39,  52,
						 78, 104, 117, 130 };
		u8 mcs = info->control.rates[1].idx;

		put_unaligned_le16(0, ri + 0x10);
		ri += 0x14;
		put_unaligned_le16(2 | 0x0004 | ((mcs > 7 ? 3 : 1) << 6),
				   ri + 0x00);
		put_unaligned_le16(subband, ri + 0x02);
		put_unaligned_le16(mcs, ri + 0x04);
		ri[0x06] = mcs;
		ri[0x07] = len & 0xff;
		ri[0x08] = len >> 8;
		ri[0x09] = 0x07;
		put_unaligned_le16(ht20_500k[mcs & 7] * (mcs > 7 ? 2 : 1),
				   ri + 0x0e);
	}
	put_unaligned_le16(0x0020, ri + 0x10);	/* last rate entry */

	return 0;
}

/* Generate a TX data header. */
int b43_generate_txhdr(struct b43_wldev *dev,
		       u8 *_txhdr,
		       struct sk_buff *skb_frag,
		       struct ieee80211_tx_info *info,
		       u16 cookie)
{
	const unsigned char *fragment_data = skb_frag->data;
	unsigned int fragment_len = skb_frag->len;
	struct b43_txhdr *txhdr = (struct b43_txhdr *)_txhdr;
	const struct b43_phy *phy = &dev->phy;
	const struct ieee80211_hdr *wlhdr =
	    (const struct ieee80211_hdr *)fragment_data;
	int use_encryption = !!info->control.hw_key;
	__le16 fctl = wlhdr->frame_control;
	struct ieee80211_rate *fbrate;
	u8 rate, rate_fb;
	int rate_ofdm, rate_fb_ofdm;
	unsigned int plcp_fragment_len;
	u32 mac_ctl = 0;
	u16 phy_ctl = 0;
	bool fill_phy_ctl1 = (phy->type == B43_PHYTYPE_LP ||
			      phy->type == B43_PHYTYPE_N ||
			      phy->type == B43_PHYTYPE_HT);
	u8 extra_ft = 0;
	struct ieee80211_rate *txrate;
	struct ieee80211_tx_rate *rates;

	if (b43_ucode_is_ac(dev))
		return b43_generate_txhdr_ac(dev, _txhdr, skb_frag, info,
					     cookie);

	memset(txhdr, 0, sizeof(*txhdr));

	txrate = ieee80211_get_tx_rate(dev->wl->hw, info);
	rate = txrate ? txrate->hw_value : B43_CCK_RATE_1MB;
	rate_ofdm = b43_is_ofdm_rate(rate);
	fbrate = ieee80211_get_alt_retry_rate(dev->wl->hw, info, 0) ? : txrate;
	rate_fb = fbrate->hw_value;
	rate_fb_ofdm = b43_is_ofdm_rate(rate_fb);

	if (rate_ofdm)
		txhdr->phy_rate = b43_plcp_get_ratecode_ofdm(rate);
	else
		txhdr->phy_rate = b43_plcp_get_ratecode_cck(rate);
	txhdr->mac_frame_ctl = wlhdr->frame_control;
	memcpy(txhdr->tx_receiver, wlhdr->addr1, ETH_ALEN);

	/* Calculate duration for fallback rate */
	if ((rate_fb == rate) ||
	    (wlhdr->duration_id & cpu_to_le16(0x8000)) ||
	    (wlhdr->duration_id == cpu_to_le16(0))) {
		/* If the fallback rate equals the normal rate or the
		 * dur_id field contains an AID, CFP magic or 0,
		 * use the original dur_id field. */
		txhdr->dur_fb = wlhdr->duration_id;
	} else {
		txhdr->dur_fb = ieee80211_generic_frame_duration(
			dev->wl->hw, info->control.vif, info->band,
			fragment_len, fbrate);
	}

	plcp_fragment_len = fragment_len + FCS_LEN;
	if (use_encryption) {
		u8 key_idx = info->control.hw_key->hw_key_idx;
		struct b43_key *key;
		int wlhdr_len;
		size_t iv_len;

		B43_WARN_ON(key_idx >= ARRAY_SIZE(dev->key));
		key = &(dev->key[key_idx]);

		if (unlikely(!key->keyconf)) {
			/* This key is invalid. This might only happen
			 * in a short timeframe after machine resume before
			 * we were able to reconfigure keys.
			 * Drop this packet completely. Do not transmit it
			 * unencrypted to avoid leaking information. */
			return -ENOKEY;
		}

		/* Hardware appends ICV. */
		plcp_fragment_len += info->control.hw_key->icv_len;

		key_idx = b43_kidx_to_fw(dev, key_idx);
		mac_ctl |= (key_idx << B43_TXH_MAC_KEYIDX_SHIFT) &
			   B43_TXH_MAC_KEYIDX;
		mac_ctl |= (key->algorithm << B43_TXH_MAC_KEYALG_SHIFT) &
			   B43_TXH_MAC_KEYALG;
		wlhdr_len = ieee80211_hdrlen(fctl);
		if (key->algorithm == B43_SEC_ALGO_TKIP) {
			u16 phase1key[5];
			int i;
			/* we give the phase1key and iv16 here, the key is stored in
			 * shm. With that the hardware can do phase 2 and encryption.
			 */
			ieee80211_get_tkip_p1k(info->control.hw_key, skb_frag, phase1key);
			/* phase1key is in host endian. Copy to little-endian txhdr->iv. */
			for (i = 0; i < 5; i++) {
				txhdr->iv[i * 2 + 0] = phase1key[i];
				txhdr->iv[i * 2 + 1] = phase1key[i] >> 8;
			}
			/* iv16 */
			memcpy(txhdr->iv + 10, ((u8 *) wlhdr) + wlhdr_len, 3);
		} else {
			iv_len = min_t(size_t, info->control.hw_key->iv_len,
				     ARRAY_SIZE(txhdr->iv));
			memcpy(txhdr->iv, ((u8 *) wlhdr) + wlhdr_len, iv_len);
		}
	}
	switch (dev->fw.hdr_format) {
	case B43_FW_HDR_598:
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)(&txhdr->format_598.plcp),
				      plcp_fragment_len, rate);
		break;
	case B43_FW_HDR_351:
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)(&txhdr->format_351.plcp),
				      plcp_fragment_len, rate);
		break;
	case B43_FW_HDR_410:
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)(&txhdr->format_410.plcp),
				      plcp_fragment_len, rate);
		break;
	}
	b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)(&txhdr->plcp_fb),
			      plcp_fragment_len, rate_fb);

	/* Extra Frame Types */
	if (rate_fb_ofdm)
		extra_ft |= B43_TXH_EFT_FB_OFDM;
	else
		extra_ft |= B43_TXH_EFT_FB_CCK;

	/* Set channel radio code. Note that the micrcode ORs 0x100 to
	 * this value before comparing it to the value in SHM, if this
	 * is a 5Ghz packet.
	 */
	txhdr->chan_radio_code = phy->channel;

	/* PHY TX Control word */
	if (rate_ofdm)
		phy_ctl |= B43_TXH_PHY_ENC_OFDM;
	else
		phy_ctl |= B43_TXH_PHY_ENC_CCK;
	if (info->control.rates[0].flags & IEEE80211_TX_RC_USE_SHORT_PREAMBLE)
		phy_ctl |= B43_TXH_PHY_SHORTPRMBL;

	switch (b43_ieee80211_antenna_sanitize(dev, 0)) {
	case 0: /* Default */
		phy_ctl |= B43_TXH_PHY_ANT01AUTO;
		break;
	case 1: /* Antenna 0 */
		phy_ctl |= B43_TXH_PHY_ANT0;
		break;
	case 2: /* Antenna 1 */
		phy_ctl |= B43_TXH_PHY_ANT1;
		break;
	case 3: /* Antenna 2 */
		phy_ctl |= B43_TXH_PHY_ANT2;
		break;
	case 4: /* Antenna 3 */
		phy_ctl |= B43_TXH_PHY_ANT3;
		break;
	default:
		B43_WARN_ON(1);
	}

	rates = info->control.rates;
	/* MAC control */
	if (!(info->flags & IEEE80211_TX_CTL_NO_ACK))
		mac_ctl |= B43_TXH_MAC_ACK;
	/* use hardware sequence counter as the non-TID counter */
	if (info->flags & IEEE80211_TX_CTL_ASSIGN_SEQ)
		mac_ctl |= B43_TXH_MAC_HWSEQ;
	if (info->flags & IEEE80211_TX_CTL_FIRST_FRAGMENT)
		mac_ctl |= B43_TXH_MAC_STMSDU;
	if (!phy->gmode)
		mac_ctl |= B43_TXH_MAC_5GHZ;

	/* Overwrite rates[0].count to make the retry calculation
	 * in the tx status easier. need the actual retry limit to
	 * detect whether the fallback rate was used.
	 */
	if ((rates[0].flags & IEEE80211_TX_RC_USE_RTS_CTS) ||
	    (rates[0].count <= dev->wl->hw->conf.long_frame_max_tx_count)) {
		rates[0].count = dev->wl->hw->conf.long_frame_max_tx_count;
		mac_ctl |= B43_TXH_MAC_LONGFRAME;
	} else {
		rates[0].count = dev->wl->hw->conf.short_frame_max_tx_count;
	}

	/* Generate the RTS or CTS-to-self frame */
	if ((rates[0].flags & IEEE80211_TX_RC_USE_RTS_CTS) ||
	    (rates[0].flags & IEEE80211_TX_RC_USE_CTS_PROTECT)) {
		unsigned int len;
		struct ieee80211_hdr *hdr;
		int rts_rate, rts_rate_fb;
		int rts_rate_ofdm, rts_rate_fb_ofdm;
		struct b43_plcp_hdr6 *plcp;
		struct ieee80211_rate *rts_cts_rate;

		rts_cts_rate = ieee80211_get_rts_cts_rate(dev->wl->hw, info);

		rts_rate = rts_cts_rate ? rts_cts_rate->hw_value : B43_CCK_RATE_1MB;
		rts_rate_ofdm = b43_is_ofdm_rate(rts_rate);
		rts_rate_fb = b43_calc_fallback_rate(rts_rate, phy->gmode);
		rts_rate_fb_ofdm = b43_is_ofdm_rate(rts_rate_fb);

		if (rates[0].flags & IEEE80211_TX_RC_USE_CTS_PROTECT) {
			struct ieee80211_cts *cts;

			switch (dev->fw.hdr_format) {
			case B43_FW_HDR_598:
				cts = (struct ieee80211_cts *)
					(txhdr->format_598.rts_frame);
				break;
			case B43_FW_HDR_351:
				cts = (struct ieee80211_cts *)
					(txhdr->format_351.rts_frame);
				break;
			case B43_FW_HDR_410:
				cts = (struct ieee80211_cts *)
					(txhdr->format_410.rts_frame);
				break;
			}
			ieee80211_ctstoself_get(dev->wl->hw, info->control.vif,
						fragment_data, fragment_len,
						info, cts);
			mac_ctl |= B43_TXH_MAC_SENDCTS;
			len = sizeof(struct ieee80211_cts);
		} else {
			struct ieee80211_rts *rts;

			switch (dev->fw.hdr_format) {
			case B43_FW_HDR_598:
				rts = (struct ieee80211_rts *)
					(txhdr->format_598.rts_frame);
				break;
			case B43_FW_HDR_351:
				rts = (struct ieee80211_rts *)
					(txhdr->format_351.rts_frame);
				break;
			case B43_FW_HDR_410:
				rts = (struct ieee80211_rts *)
					(txhdr->format_410.rts_frame);
				break;
			}
			ieee80211_rts_get(dev->wl->hw, info->control.vif,
					  fragment_data, fragment_len,
					  info, rts);
			mac_ctl |= B43_TXH_MAC_SENDRTS;
			len = sizeof(struct ieee80211_rts);
		}
		len += FCS_LEN;

		/* Generate the PLCP headers for the RTS/CTS frame */
		switch (dev->fw.hdr_format) {
		case B43_FW_HDR_598:
			plcp = &txhdr->format_598.rts_plcp;
			break;
		case B43_FW_HDR_351:
			plcp = &txhdr->format_351.rts_plcp;
			break;
		case B43_FW_HDR_410:
			plcp = &txhdr->format_410.rts_plcp;
			break;
		}
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)plcp,
				      len, rts_rate);
		plcp = &txhdr->rts_plcp_fb;
		b43_generate_plcp_hdr((struct b43_plcp_hdr4 *)plcp,
				      len, rts_rate_fb);

		switch (dev->fw.hdr_format) {
		case B43_FW_HDR_598:
			hdr = (struct ieee80211_hdr *)
				(&txhdr->format_598.rts_frame);
			break;
		case B43_FW_HDR_351:
			hdr = (struct ieee80211_hdr *)
				(&txhdr->format_351.rts_frame);
			break;
		case B43_FW_HDR_410:
			hdr = (struct ieee80211_hdr *)
				(&txhdr->format_410.rts_frame);
			break;
		}
		txhdr->rts_dur_fb = hdr->duration_id;

		if (rts_rate_ofdm) {
			extra_ft |= B43_TXH_EFT_RTS_OFDM;
			txhdr->phy_rate_rts =
			    b43_plcp_get_ratecode_ofdm(rts_rate);
		} else {
			extra_ft |= B43_TXH_EFT_RTS_CCK;
			txhdr->phy_rate_rts =
			    b43_plcp_get_ratecode_cck(rts_rate);
		}
		if (rts_rate_fb_ofdm)
			extra_ft |= B43_TXH_EFT_RTSFB_OFDM;
		else
			extra_ft |= B43_TXH_EFT_RTSFB_CCK;

		if (rates[0].flags & IEEE80211_TX_RC_USE_RTS_CTS &&
		    fill_phy_ctl1) {
			txhdr->phy_ctl1_rts = cpu_to_le16(
				b43_generate_tx_phy_ctl1(dev, rts_rate));
			txhdr->phy_ctl1_rts_fb = cpu_to_le16(
				b43_generate_tx_phy_ctl1(dev, rts_rate_fb));
		}
	}

	/* Magic cookie */
	switch (dev->fw.hdr_format) {
	case B43_FW_HDR_598:
		txhdr->format_598.cookie = cpu_to_le16(cookie);
		break;
	case B43_FW_HDR_351:
		txhdr->format_351.cookie = cpu_to_le16(cookie);
		break;
	case B43_FW_HDR_410:
		txhdr->format_410.cookie = cpu_to_le16(cookie);
		break;
	}

	if (fill_phy_ctl1) {
		txhdr->phy_ctl1 =
			cpu_to_le16(b43_generate_tx_phy_ctl1(dev, rate));
		txhdr->phy_ctl1_fb =
			cpu_to_le16(b43_generate_tx_phy_ctl1(dev, rate_fb));
	}

	/* Apply the bitfields */
	txhdr->mac_ctl = cpu_to_le32(mac_ctl);
	txhdr->phy_ctl = cpu_to_le16(phy_ctl);
	txhdr->extra_ft = extra_ft;

	return 0;
}

static s8 b43_rssi_postprocess(struct b43_wldev *dev,
			       u8 in_rssi, int ofdm,
			       int adjust_2053, int adjust_2050)
{
	struct b43_phy *phy = &dev->phy;
	struct b43_phy_g *gphy = phy->g;
	s32 tmp;

	switch (phy->radio_ver) {
	case 0x2050:
		if (ofdm) {
			tmp = in_rssi;
			if (tmp > 127)
				tmp -= 256;
			tmp *= 73;
			tmp /= 64;
			if (adjust_2050)
				tmp += 25;
			else
				tmp -= 3;
		} else {
			if (dev->dev->bus_sprom->
			    boardflags_lo & B43_BFL_RSSI) {
				if (in_rssi > 63)
					in_rssi = 63;
				B43_WARN_ON(phy->type != B43_PHYTYPE_G);
				tmp = gphy->nrssi_lt[in_rssi];
				tmp = 31 - tmp;
				tmp *= -131;
				tmp /= 128;
				tmp -= 57;
			} else {
				tmp = in_rssi;
				tmp = 31 - tmp;
				tmp *= -149;
				tmp /= 128;
				tmp -= 68;
			}
			if (phy->type == B43_PHYTYPE_G && adjust_2050)
				tmp += 25;
		}
		break;
	case 0x2060:
		if (in_rssi > 127)
			tmp = in_rssi - 256;
		else
			tmp = in_rssi;
		break;
	default:
		tmp = in_rssi;
		tmp -= 11;
		tmp *= 103;
		tmp /= 64;
		if (adjust_2053)
			tmp -= 109;
		else
			tmp -= 83;
	}

	return (s8) tmp;
}

void b43_rx(struct b43_wldev *dev, struct sk_buff *skb, const void *_rxhdr)
{
	struct ieee80211_rx_status status;
	struct b43_plcp_hdr6 *plcp;
	struct ieee80211_hdr *wlhdr;
	const struct b43_rxhdr_fw4 *rxhdr = _rxhdr;
	__le16 fctl;
	u16 phystat0, phystat3;
	u16 chanstat, mactime;
	u32 macstat;
	u16 chanid;
	int padding, rate_idx;

	memset(&status, 0, sizeof(status));

	/* Get metadata about the frame from the header. */
	phystat0 = le16_to_cpu(rxhdr->phy_status0);
	phystat3 = le16_to_cpu(rxhdr->phy_status3);
	switch (dev->fw.hdr_format) {
	case B43_FW_HDR_598:
		macstat = le32_to_cpu(rxhdr->format_598.mac_status);
		mactime = le16_to_cpu(rxhdr->format_598.mac_time);
		chanstat = le16_to_cpu(rxhdr->format_598.channel);
		break;
	case B43_FW_HDR_410:
	case B43_FW_HDR_351:
		macstat = le32_to_cpu(rxhdr->format_351.mac_status);
		mactime = le16_to_cpu(rxhdr->format_351.mac_time);
		chanstat = le16_to_cpu(rxhdr->format_351.channel);
		break;
	}

	if (unlikely(macstat & B43_RX_MAC_FCSERR)) {
		dev->wl->ieee_stats.dot11FCSErrorCount++;
		status.flag |= RX_FLAG_FAILED_FCS_CRC;
	}
	if (unlikely(phystat0 & (B43_RX_PHYST0_PLCPHCF | B43_RX_PHYST0_PLCPFV)))
		status.flag |= RX_FLAG_FAILED_PLCP_CRC;
	if (phystat0 & B43_RX_PHYST0_SHORTPRMBL)
		status.enc_flags |= RX_ENC_FLAG_SHORTPRE;
	if (macstat & B43_RX_MAC_DECERR) {
		/* Decryption with the given key failed.
		 * Drop the packet. We also won't be able to decrypt it with
		 * the key in software. */
		goto drop;
	}

	/* Skip PLCP and padding */
	padding = (macstat & B43_RX_MAC_PADDING) ? 2 : 0;
	if (unlikely(skb->len < (sizeof(struct b43_plcp_hdr6) + padding))) {
		b43dbg(dev->wl, "RX: Packet size underrun (1)\n");
		goto drop;
	}
	plcp = (struct b43_plcp_hdr6 *)(skb->data + padding);
	skb_pull(skb, sizeof(struct b43_plcp_hdr6) + padding);
	/* The skb contains the Wireless Header + payload data now */
	if (unlikely(skb->len < (2 + 2 + 6 /*minimum hdr */  + FCS_LEN))) {
		b43dbg(dev->wl, "RX: Packet size underrun (2)\n");
		goto drop;
	}
	wlhdr = (struct ieee80211_hdr *)(skb->data);
	fctl = wlhdr->frame_control;

	if (macstat & B43_RX_MAC_DEC) {
		unsigned int keyidx;
		int wlhdr_len;

		keyidx = ((macstat & B43_RX_MAC_KEYIDX)
			  >> B43_RX_MAC_KEYIDX_SHIFT);
		/* We must adjust the key index here. We want the "physical"
		 * key index, but the ucode passed it slightly different.
		 */
		keyidx = b43_kidx_to_raw(dev, keyidx);
		if (B43_WARN_ON(keyidx >= ARRAY_SIZE(dev->key)))
			goto drop;

		if (dev->key[keyidx].algorithm != B43_SEC_ALGO_NONE) {
			wlhdr_len = ieee80211_hdrlen(fctl);
			if (unlikely(skb->len < (wlhdr_len + 3))) {
				b43dbg(dev->wl,
				       "RX: Packet size underrun (3)\n");
				goto drop;
			}
			status.flag |= RX_FLAG_DECRYPTED;
		}
	}

	/* Link quality statistics */
	switch (chanstat & B43_RX_CHAN_PHYTYPE) {
	case B43_PHYTYPE_HT:
		/* TODO: is max the right choice? */
		status.signal = max_t(__s8,
			max(rxhdr->phy_ht_power0, rxhdr->phy_ht_power1),
			rxhdr->phy_ht_power2);
		break;
	case B43_PHYTYPE_N:
		/* Broadcom has code for min and avg, but always uses max */
		if (rxhdr->power0 == 16 || rxhdr->power0 == 32)
			status.signal = max(rxhdr->power1, rxhdr->power2);
		else
			status.signal = max(rxhdr->power0, rxhdr->power1);
		break;
	case B43_PHYTYPE_B:
	case B43_PHYTYPE_G:
	case B43_PHYTYPE_LP:
		status.signal = b43_rssi_postprocess(dev, rxhdr->jssi,
						  (phystat0 & B43_RX_PHYST0_OFDM),
						  (phystat0 & B43_RX_PHYST0_GAINCTL),
						  (phystat3 & B43_RX_PHYST3_TRSTATE));
		break;
	}

	if (phystat0 & B43_RX_PHYST0_OFDM)
		rate_idx = b43_plcp_get_bitrate_idx_ofdm(plcp,
					!!(chanstat & B43_RX_CHAN_5GHZ));
	else
		rate_idx = b43_plcp_get_bitrate_idx_cck(plcp);
	if (unlikely(rate_idx == -1)) {
		/* PLCP seems to be corrupted.
		 * Drop the frame, if we are not interested in corrupted frames. */
		if (!(dev->wl->filter_flags & FIF_PLCPFAIL))
			goto drop;
	}
	status.rate_idx = rate_idx;
	status.antenna = !!(phystat0 & B43_RX_PHYST0_ANT);

	/*
	 * All frames on monitor interfaces and beacons always need a full
	 * 64-bit timestamp. Monitor interfaces need it for diagnostic
	 * purposes and beacons for IBSS merging.
	 * This code assumes we get to process the packet within 16 bits
	 * of timestamp, i.e. about 65 milliseconds after the PHY received
	 * the first symbol.
	 */
	if (ieee80211_is_beacon(fctl) || dev->wl->radiotap_enabled) {
		u16 low_mactime_now;

		b43_tsf_read(dev, &status.mactime);
		low_mactime_now = status.mactime;
		status.mactime = status.mactime & ~0xFFFFULL;
		status.mactime += mactime;
		if (low_mactime_now <= mactime)
			status.mactime -= 0x10000;
		status.flag |= RX_FLAG_MACTIME_START;
	}

	chanid = (chanstat & B43_RX_CHAN_ID) >> B43_RX_CHAN_ID_SHIFT;
	switch (chanstat & B43_RX_CHAN_PHYTYPE) {
	case B43_PHYTYPE_G:
		status.band = NL80211_BAND_2GHZ;
		/* Somewhere between 478.104 and 508.1084 firmware for G-PHY
		 * has been modified to be compatible with N-PHY and others.
		 */
		if (dev->fw.rev >= 508)
			status.freq = ieee80211_channel_to_frequency(chanid, status.band);
		else
			status.freq = chanid + 2400;
		break;
	case B43_PHYTYPE_N:
	case B43_PHYTYPE_LP:
	case B43_PHYTYPE_HT:
		/* chanid is the SHM channel cookie. Which is the plain
		 * channel number in b43. */
		if (chanstat & B43_RX_CHAN_5GHZ)
			status.band = NL80211_BAND_5GHZ;
		else
			status.band = NL80211_BAND_2GHZ;
		status.freq =
			ieee80211_channel_to_frequency(chanid, status.band);
		break;
	default:
		B43_WARN_ON(1);
		goto drop;
	}

	memcpy(IEEE80211_SKB_RXCB(skb), &status, sizeof(status));
	ieee80211_rx_ni(dev->wl->hw, skb);

#if B43_DEBUG
	dev->rx_count++;
#endif
	return;
drop:
	dev_kfree_skb_any(skb);
}

void b43_handle_txstatus(struct b43_wldev *dev,
			 const struct b43_txstatus *status)
{
	b43_debugfs_log_txstat(dev, status);

	if (status->intermediate)
		return;
	if (status->for_ampdu)
		return;
	if (!status->acked)
		dev->wl->ieee_stats.dot11ACKFailureCount++;
	if (status->rts_count) {
		if (status->rts_count == 0xF)	//FIXME
			dev->wl->ieee_stats.dot11RTSFailureCount++;
		else
			dev->wl->ieee_stats.dot11RTSSuccessCount++;
	}

	if (b43_using_pio_transfers(dev))
		b43_pio_handle_txstatus(dev, status);
	else
		b43_dma_handle_txstatus(dev, status);

	b43_phy_txpower_check(dev, 0);
}

/* Fill out the mac80211 TXstatus report based on the b43-specific
 * txstatus report data. This returns a boolean whether the frame was
 * successfully transmitted. */
bool b43_fill_txstatus_report(struct b43_wldev *dev,
			      struct ieee80211_tx_info *report,
			      const struct b43_txstatus *status)
{
	bool frame_success = true;
	int retry_limit;

	/* preserve the confiured retry limit before clearing the status
	 * The xmit function has overwritten the rc's value with the actual
	 * retry limit done by the hardware */
	retry_limit = report->status.rates[0].count;
	ieee80211_tx_info_clear_status(report);

	if (status->acked) {
		/* The frame was ACKed. */
		report->flags |= IEEE80211_TX_STAT_ACK;
	} else {
		/* The frame was not ACKed... */
		if (!(report->flags & IEEE80211_TX_CTL_NO_ACK)) {
			/* ...but we expected an ACK. */
			frame_success = false;
		}
	}
	if (status->frame_count == 0) {
		/* The frame was not transmitted at all. */
		report->status.rates[0].count = 0;
	} else if (status->rts_count > dev->wl->hw->conf.short_frame_max_tx_count) {
		/*
		 * If the short retries (RTS, not data frame) have exceeded
		 * the limit, the hw will not have tried the selected rate,
		 * but will have used the fallback rate instead.
		 * Don't let the rate control count attempts for the selected
		 * rate in this case, otherwise the statistics will be off.
		 */
		report->status.rates[0].count = 0;
		report->status.rates[1].count = status->frame_count;
	} else {
		if (status->frame_count > retry_limit) {
			report->status.rates[0].count = retry_limit;
			report->status.rates[1].count = status->frame_count -
					retry_limit;

		} else {
			report->status.rates[0].count = status->frame_count;
			report->status.rates[1].idx = -1;
		}
	}

	return frame_success;
}

/* Stop any TX operation on the device (suspend the hardware queues) */
void b43_tx_suspend(struct b43_wldev *dev)
{
	if (b43_using_pio_transfers(dev))
		b43_pio_tx_suspend(dev);
	else
		b43_dma_tx_suspend(dev);
}

/* Resume any TX operation on the device (resume the hardware queues) */
void b43_tx_resume(struct b43_wldev *dev)
{
	if (b43_using_pio_transfers(dev))
		b43_pio_tx_resume(dev);
	else
		b43_dma_tx_resume(dev);
}
