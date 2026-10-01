// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 Imre Kaloz <kaloz@kernel.org>
 *
 * SGI L1 system controller communication, ported from OpenBSD/sgi's
 * l1.c:
 *
 * Copyright (c) 2009 Miodrag Vallat.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * The L1 is the IP35-family (Bedrock hub) system controller, reached
 * over a polled UART with async-HDLC (PPP) framing. Every request fails
 * with -ENODEV on an IP27 (Hub) machine.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/serial_reg.h>
#include <linux/spinlock.h>
#include <linux/stdarg.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/crc-ccitt.h>
#include <linux/io.h>

#include <asm/sn/addrs.h>
#include <asm/sn/agent.h>
#include <asm/sn/l1.h>
#include <asm/sn/types.h>

/* packet types */
#define L1PKT_REQUEST		0x00
#define L1PKT_RESPONSE		0x20

/* packet subchannels */
#define L1CH_MISC		0x10	/* available for operating system */

/* argument encoding */
#define L1_ARG_INT		0x00	/* followed by 32 bit BE value */
#define L1_ARG_ASCII		0x01	/* followed by NUL terminated string */
#define L1_ARG_BINARY		0x80	/* length in low 7 bits */

/* command types and destination addresses */
#define L1_ADDRESS(type, addr)	(((type) << 28) | (addr))

#define L1_TYPE_L1		0x00

#define L1_ADDRESS_RACK_LOCAL	0x3ff
#define L1_ADDRESS_RACK_SHIFT	18
#define L1_ADDRESS_BAY_LOCAL	0x3f
#define L1_ADDRESS_BAY_SHIFT	12

#define L1_ADDRESS_LOCAL \
	((L1_ADDRESS_RACK_LOCAL << L1_ADDRESS_RACK_SHIFT) | \
	 (L1_ADDRESS_BAY_LOCAL << L1_ADDRESS_BAY_SHIFT))

#define L1_TASK_COMMAND		0x03
#define L1_TASK_GENERAL		0x06

/* response codes */
#define L1_RESP_OK		((u32)0)
#define L1_RESP_NXDATA		((u32)-0x68)
#define L1_RESP_INVAL		((u32)-0x6b)

/* L1_TASK_COMMAND requests */
#define L1_REQ_EXEC_CMD		0x0000	/* interpret plaintext command */

/* L1_TASK_GENERAL requests */
#define L1_REQ_BRICK_ID		0x0003	/* rack, slot and brick type */
#define L1_REQ_EEPROM		0x0006	/* access eeprom */

/* L1_REQ_EEPROM additional argument values (non C-brick component) */
#define L1_EEP_LOGIC		0x01	/* logic board */
/* ia code */
#define L1_EEP_BOARD		0x02	/* board ia */

#define EEPROM_CHUNK		0x40

/*
 * Information Area type/length byte: bits 7-6 are the type (0 binary,
 * 1 BCD, 2 packed 6-bit ascii, 3 8-bit ascii), bits 5-0 the length.
 */
#define IA_TYPE_SHIFT		6
#define IA_TYPE_BINARY		0
#define IA_TYPE_ASCII		3
#define IA_LENGTH_MASK		0x3f
#define IA_TL(t, l)		(((t) << IA_TYPE_SHIFT) | (l))
#define IA_EOF			IA_TL(IA_TYPE_ASCII, 1)

/* Async-HDLC framing bytes; linux/ppp_defs.h would drag in skbuff.h. */
#define L1_FLAG			0x7e
#define L1_ESC			0x7d
#define L1_TRANS		0x20
#define L1_FCS_INIT		0xffff
#define L1_FCS_GOOD		0xf0b8

/* A raw iteration count, not a time bound: this can run before jiffies. */
#define L1_POLL_ITERATIONS	1000000

/*
 * The L1 UART sits in the HSPEC "local register" window, reached
 * through the "remote HSPEC" indirection even for the local node.
 */
#define L1_RHSPEC_OFFSET	0x20000000	/* remote-HSPEC indirection */
#define L1_LREG_OFFSET		0x10000000	/* local register window */
#define L1_UART_REG(r)		(L1_LREG_OFFSET + 0x80 + ((r) << 3))

#define L1_UART_ADDRESS(nasid, r) \
	((u64 *)(NODE_HSPEC_BASE(nasid) + L1_RHSPEC_OFFSET + L1_UART_REG(r)))

/* Board IA record buffer; the probed length is checked against it. */
#define L1_IA_BUF_SIZE		512
static u8 l1_ia_buf[L1_IA_BUF_SIZE];

static int l1_serial_getc(nasid_t nasid)
{
	unsigned int n;
	u64 lsr;

	for (n = L1_POLL_ITERATIONS; n != 0; n--) {
		lsr = __raw_readq(L1_UART_ADDRESS(nasid, UART_LSR));
		if (lsr & UART_LSR_DR)
			break;
		cpu_relax();
	}
	if (n == 0)
		return -ETIMEDOUT;

	return __raw_readq(L1_UART_ADDRESS(nasid, UART_RX)) & 0xff;
}

static int l1_serial_putc(nasid_t nasid, u8 val)
{
	unsigned int n;
	u64 lsr;

	for (n = L1_POLL_ITERATIONS; n != 0; n--) {
		lsr = __raw_readq(L1_UART_ADDRESS(nasid, UART_LSR));
		if (lsr & UART_LSR_THRE)
			break;
		cpu_relax();
	}
	if (n == 0)
		return -ETIMEDOUT;

	__raw_writeq(val, L1_UART_ADDRESS(nasid, UART_TX));
	return 0;
}

static int l1_serial_ppp_write(nasid_t nasid, u16 *crc, u8 data, bool escape)
{
	int rc;

	if (crc)
		*crc = crc_ccitt_byte(*crc, data);

	if (escape && (data == L1_FLAG || data == L1_ESC)) {
		rc = l1_serial_putc(nasid, L1_ESC);
		if (rc)
			return rc;
		data ^= L1_TRANS;
	}

	return l1_serial_putc(nasid, data);
}

static int l1_packet_put(nasid_t nasid, const u8 *packet, size_t len)
{
	u16 crc = L1_FCS_INIT;
	int rc;

	rc = l1_serial_ppp_write(nasid, NULL, L1_FLAG, false);
	if (rc)
		return rc;

	while (len-- != 0) {
		rc = l1_serial_ppp_write(nasid, &crc, *packet++, true);
		if (rc)
			return rc;
	}

	crc ^= L1_FCS_INIT;
	rc = l1_serial_ppp_write(nasid, NULL, crc & 0xff, true);
	if (rc)
		return rc;
	rc = l1_serial_ppp_write(nasid, NULL, (crc >> 8) & 0xff, true);
	if (rc)
		return rc;

	return l1_serial_ppp_write(nasid, NULL, L1_FLAG, false);
}

static int l1_packet_get(nasid_t nasid, u8 *buf, size_t buflen, size_t *rlen)
{
	u16 crc;
	size_t rcvlen;
	int data;

	for (;;) {
		data = l1_serial_getc(nasid);
		if (data < 0)
			return data;
		if (data == L1_FLAG)
			break;
	}

	rcvlen = 0;
	crc = L1_FCS_INIT;
	for (;;) {
		data = l1_serial_getc(nasid);
		if (data < 0)
			return data;
		if (data == L1_FLAG)	/* end of packet */
			break;
		if (data == L1_ESC) {
			data = l1_serial_getc(nasid);
			if (data < 0)
				return data;
			data ^= L1_TRANS;
		}
		if (rcvlen < buflen)
			buf[rcvlen] = data;
		rcvlen++;
		crc = crc_ccitt_byte(crc, data);
	}

	if (rcvlen > buflen)
		return -EMSGSIZE;	/* did not fit the buffer */
	if (rcvlen < 2)
		return -EBADMSG;	/* short packet */

	rcvlen -= 2;			/* crc bytes */
	if (crc != L1_FCS_GOOD)
		return -EBADMSG;	/* CRC error */

	*rlen = rcvlen;
	return 0;
}

static int l1_packet_get_int(u8 **buf, size_t *buflen, u32 *rval)
{
	u8 *b = *buf;

	if (*buflen < 5)
		return -EIO;
	if (*b++ != L1_ARG_INT)
		return -EINVAL;

	*rval = get_unaligned_be32(b);
	b += 4;
	*buf = b;
	*buflen -= 5;

	return 0;
}

static int l1_packet_get_binary(u8 **buf, size_t *buflen, u8 **rval,
				size_t *rlen)
{
	u8 *b = *buf;
	size_t datalen;

	if (*buflen < 1)
		return -EIO;
	if ((*b & L1_ARG_BINARY) == 0)
		return -EINVAL;

	datalen = *b & ~L1_ARG_BINARY;
	if (*buflen < 1 + datalen)
		return -ENOMEM;

	b++;
	*rval = b;
	*rlen = datalen;
	*buflen -= 1 + datalen;
	b += datalen;
	*buf = b;

	return 0;
}

/*
 * Returns the would-be length even when the buffer is too small, and
 * SIZE_MAX for an unknown argument type.
 */
static size_t l1_command_build(u8 *buf, size_t buflen, u32 address,
			       u16 request, int nargs, va_list ap)
{
	u32 data;
	size_t len = 0;
	int argtype;
	const char *str;

	if (buflen >= 1) {
		*buf++ = L1PKT_REQUEST | L1CH_MISC;
		buflen--;
	}
	len++;

	if (buflen >= 4) {
		put_unaligned_be32(address, buf);
		buf += 4;
		buflen -= 4;
	}
	len += 4;

	if (buflen >= 2) {
		put_unaligned_be16(request, buf);
		buf += 2;
		buflen -= 2;
	}
	len += 2;

	if (buflen >= 1) {
		*buf++ = nargs;
		buflen--;
	}
	len++;

	while (nargs-- != 0) {
		argtype = va_arg(ap, int);
		switch (argtype) {
		case L1_ARG_INT:
			data = va_arg(ap, u32);
			if (buflen >= 5) {
				*buf++ = L1_ARG_INT;
				put_unaligned_be32(data, buf);
				buf += 4;
				buflen -= 5;
			}
			len += 5;
			break;
		case L1_ARG_ASCII:
			str = va_arg(ap, const char *);
			data = strlen(str);
			if (buflen >= data + 2) {
				*buf++ = L1_ARG_ASCII;
				memcpy(buf, str, data + 1);
				buf += data + 1;
				buflen -= data + 2;
			}
			len += data + 2;
			break;
		case L1_ARG_BINARY:
			data = (u32)va_arg(ap, size_t);	/* size */
			str = va_arg(ap, const char *);	/* data */
			if (buflen >= 1 + data) {
				*buf++ = L1_ARG_BINARY | data;
				memcpy(buf, str, data);
				buf += data;
				buflen -= data + 1;
			}
			len += data + 1;
			break;
		default:
			WARN_ON_ONCE(1);
			return SIZE_MAX;
		}
	}

	return len;
}

/* The L1 multiplexes unsolicited event and console traffic onto one wire. */
static int l1_receive_response(nasid_t nasid, u8 *pkt, size_t *pktlen)
{
	size_t rcvlen;
	int rc;

	for (;;) {
		rc = l1_packet_get(nasid, pkt, *pktlen, &rcvlen);
		if (rc == -ETIMEDOUT)
			return rc;
		if (rc)		/* bad packet */
			continue;
		if (pkt[0] != (L1PKT_RESPONSE | L1CH_MISC))
			continue;

		*pktlen = rcvlen;
		return 0;
	}
}

static int l1_response_to_errno(u32 response)
{
	switch (response) {
	case L1_RESP_OK:
		return 0;
	case L1_RESP_INVAL:
		return -EINVAL;
	case L1_RESP_NXDATA:
		return -ENXIO;
	default:
		return -EIO;
	}
}

/*
 * One request and its response are a single transaction on the wire,
 * so every entry point holds this.
 */
static DEFINE_SPINLOCK(l1_lock);

/*
 * Run one request/response exchange in pkt, which has to be large enough
 * for both; returns the response's argument count.
 */
static int l1_transact(nasid_t nasid, u8 *pkt, size_t pktsize, u32 address,
		       u16 request, u8 **rargs, size_t *rarglen, int nargs, ...)
{
	va_list ap;
	size_t pktlen;
	int rc;

	if (!system_is_ip35)
		return -ENODEV;

	va_start(ap, nargs);
	pktlen = l1_command_build(pkt, pktsize, address, request, nargs, ap);
	va_end(ap);
	if (pktlen > pktsize)
		return -ENOMEM;

	rc = l1_packet_put(nasid, pkt, pktlen);
	if (rc)
		return rc;

	pktlen = pktsize;
	rc = l1_receive_response(nasid, pkt, &pktlen);
	if (rc)
		return rc;

	if (pktlen < 6)
		return -EIO;

	rc = l1_response_to_errno(get_unaligned_be32(&pkt[1]));
	if (rc)
		return rc;

	if (rargs) {
		*rargs = pkt + 6;
		*rarglen = pktlen - 6;
	}

	return pkt[5];
}

/*
 * Send a plaintext command to the L1's own console command parser, the
 * same strings a human types at the L1 console. Such a command can reset
 * the Hub, or cut power to it, before the L1 answers, so an error here
 * means "no response", never "retry".
 */
int l1_exec_command(const char *cmd)
{
	u32 address = L1_ADDRESS(L1_TYPE_L1,
				 L1_ADDRESS_LOCAL | L1_TASK_COMMAND);
	u8 pkt[64 + 64];	/* command and response packet buffer */
	unsigned long flags;
	bool locked;
	int rc;

	/*
	 * Reset and poweroff reach this after smp_send_stop(), so a stopped
	 * CPU may hold the lock for good; go to the wire either way.
	 */
	locked = spin_trylock_irqsave(&l1_lock, flags);

	rc = l1_transact(get_nasid(), pkt, sizeof(pkt), address,
			 L1_REQ_EXEC_CMD, NULL, NULL, 1,
			 L1_ARG_ASCII, cmd);

	if (locked)
		spin_unlock_irqrestore(&l1_lock, flags);

	if (rc < 0)
		return rc;

	/* nothing is expected back besides the response code */
	return rc == 0 ? 0 : -EIO;
}

/* Read a component's board IA record from EEPROM. */
static int l1_read_board_ia(nasid_t nasid, int type, unsigned int component,
			    u8 **ria, size_t *rialen)
{
	u32 address = L1_ADDRESS(type, L1_ADDRESS_LOCAL | L1_TASK_GENERAL);
	u8 pkt[64 + EEPROM_CHUNK];
	u8 *args, *chunk;
	size_t arglen, chunklen, ialen, iapos;
	u32 data;
	int rc;

	rc = l1_transact(nasid, pkt, sizeof(pkt), address, L1_REQ_EEPROM,
			 &args, &arglen, 4,
			 L1_ARG_INT, (u32)component,
			 L1_ARG_INT, (u32)L1_EEP_BOARD,
			 L1_ARG_INT, (u32)0,	/* offset */
			 L1_ARG_INT, (u32)0);	/* size (0 = probe) */
	if (rc < 0)
		return rc;
	if (rc != 1)
		return -EIO;

	rc = l1_packet_get_int(&args, &arglen, &data);
	if (rc)
		return -EIO;

	ialen = (size_t)data;
	if (ialen == 0 || ialen > L1_IA_BUF_SIZE)
		return -ENOMEM;

	iapos = 0;
	while (iapos < ialen) {
		rc = l1_transact(nasid, pkt, sizeof(pkt), address,
				 L1_REQ_EEPROM, &args, &arglen, 4,
				 L1_ARG_INT, (u32)component,
				 L1_ARG_INT, (u32)L1_EEP_BOARD,
				 L1_ARG_INT, (u32)iapos,
				 L1_ARG_INT, (u32)EEPROM_CHUNK);
		if (rc < 0)
			return rc;
		if (rc != 2)
			return -EIO;

		rc = l1_packet_get_int(&args, &arglen, &data);
		if (rc)
			return -EIO;

		rc = l1_packet_get_binary(&args, &arglen, &chunk, &chunklen);
		if (rc)
			return -EIO;

		/* should not happen; the bound keeps the loop finite */
		if (chunklen == 0)
			return -EIO;

		if (chunklen > ialen - iapos)
			chunklen = ialen - iapos;

		memcpy(l1_ia_buf + iapos, chunk, chunklen);
		iapos += chunklen;
	}

	/* the sum of all IA bytes has to be 0 mod 256 */
	data = 0;
	for (iapos = 0; iapos < ialen; iapos++)
		data += l1_ia_buf[iapos];
	if ((data & 0xff) != 0)
		return -EINVAL;

	*ria = l1_ia_buf;
	*rialen = ialen;
	return 0;
}

/* IA record walk helper: advance past one record, without passing EOF. */
static size_t ia_skip(const u8 *ia, size_t iapos, size_t ialen)
{
	size_t npos;

	if (ia[iapos] == IA_EOF)
		return iapos;

	npos = iapos + 1 + (ia[iapos] & IA_LENGTH_MASK);
	return npos >= ialen ? iapos : npos;
}

static int l1_get_brick_ethernet_address(nasid_t nasid, u8 *enaddr)
{
	u8 *ia;
	u8 group;
	size_t iapos, ialen;
	int i, rc;

	/* assumes the local brick's L1 answers for this node */
	rc = l1_read_board_ia(nasid, L1_TYPE_L1, L1_EEP_LOGIC, &ia, &ialen);
	if (rc)
		return rc;

	/*
	 * ia[0] is the board-IA format version. Below 2 the encryption-key
	 * (G/P/Y) and Ethernet-address groups follow the fixed records
	 * implicitly; from 2 on an extra selector record comes first: 0
	 * keys only, 1 keys then address, 2 neither, 3 a config group.
	 * Otherwise the walk follows l1_get_brick_ethernet_address() in
	 * OpenBSD/sgi's l1.c.
	 */
	if (ia[1] < 2 || ia[1] * 8UL > ialen)
		return -EINVAL;

	/* skip the fixed part, then mfg, product, serial, part */
	iapos = 6;
	iapos = ia_skip(ia, iapos, ialen);
	iapos = ia_skip(ia, iapos, ialen);
	iapos = ia_skip(ia, iapos, ialen);
	iapos = ia_skip(ia, iapos, ialen);
	/* skip FRU (1 raw byte) */
	if (iapos < ialen - 1)
		iapos++;
	/* skip board rev, eeprom size, temp waiver */
	iapos = ia_skip(ia, iapos, ialen);
	iapos = ia_skip(ia, iapos, ialen);
	iapos = ia_skip(ia, iapos, ialen);

	if (ia[0] >= 2) {
		if (iapos + 1 >= ialen)
			return -ENXIO;
		group = ia[iapos + 1];
		iapos = ia_skip(ia, iapos, ialen);
		if (group != 1)
			return -ENXIO;	/* no ethernet-address group here */
		/* the three encryption-key records precede the address */
		iapos = ia_skip(ia, iapos, ialen);
		iapos = ia_skip(ia, iapos, ialen);
		iapos = ia_skip(ia, iapos, ialen);
	} else {
		if (iapos < ialen && ia[iapos] == IA_TL(IA_TYPE_BINARY, 4)) {
			iapos = ia_skip(ia, iapos, ialen);
			iapos = ia_skip(ia, iapos, ialen);
			iapos = ia_skip(ia, iapos, ialen);
		}
	}

	if (iapos >= ialen - 1 - 12 || ia[iapos] != IA_TL(IA_TYPE_ASCII, 12))
		return -EINVAL;

	iapos++;
	for (i = 0; i < ETH_ALEN; i++) {
		int hi = hex_to_bin(ia[iapos + i * 2]);
		int lo = hex_to_bin(ia[iapos + i * 2 + 1]);

		if (hi < 0 || lo < 0)
			return -EINVAL;
		enaddr[i] = (hi << 4) | lo;
	}

	return is_valid_ether_addr(enaddr) ? 0 : -EINVAL;
}

/*
 * The L1 stores SGI assembly numbers with underscores where the printed
 * form has dashes: "030_1707_003" for "030-1707-003".
 */
static int l1_get_board_partnum(nasid_t nasid, unsigned int component,
				char *buf, size_t len)
{
	size_t iapos, ialen, fieldlen, i;
	u8 *ia;
	int rc;

	rc = l1_read_board_ia(nasid, L1_TYPE_L1, component, &ia, &ialen);
	if (rc)
		return rc;

	if (ia[1] < 2 || ia[1] * 8UL > ialen)
		return -EINVAL;

	/* skip the fixed part, then mfg, product, serial */
	iapos = 6;
	iapos = ia_skip(ia, iapos, ialen);
	iapos = ia_skip(ia, iapos, ialen);
	iapos = ia_skip(ia, iapos, ialen);

	if (iapos >= ialen || (ia[iapos] >> IA_TYPE_SHIFT) != IA_TYPE_ASCII)
		return -EINVAL;

	fieldlen = ia[iapos] & IA_LENGTH_MASK;
	if (fieldlen == 0 || iapos + 1 + fieldlen > ialen || fieldlen >= len)
		return -EINVAL;

	iapos++;
	for (i = 0; i < fieldlen; i++)
		buf[i] = ia[iapos + i] == '_' ? '-' : ia[iapos + i];
	buf[fieldlen] = '\0';

	return 0;
}

/*
 * Part numbers cannot change at runtime, so failures are cached along
 * with successes and each component is queried once.
 */
#define L1_PARTNUM_SIZE		24
static char l1_partnum[L1_BOARD_COMPONENT_MAX][L1_PARTNUM_SIZE];
static int l1_partnum_rc[L1_BOARD_COMPONENT_MAX];
static bool l1_partnum_cached[L1_BOARD_COMPONENT_MAX];

int l1_read_board_partnum(unsigned int component, char *buf, size_t len)
{
	unsigned long flags;

	if (component >= L1_BOARD_COMPONENT_MAX)
		return -EINVAL;

	spin_lock_irqsave(&l1_lock, flags);

	if (!l1_partnum_cached[component]) {
		l1_partnum_rc[component] =
			l1_get_board_partnum(get_nasid(), component,
					     l1_partnum[component],
					     sizeof(l1_partnum[component]));
		l1_partnum_cached[component] = true;

		if (l1_partnum_rc[component])
			pr_debug("l1: component %u: no part number (%d)\n",
				 component, l1_partnum_rc[component]);
		else
			pr_debug("l1: component %u: part number %s\n",
				 component, l1_partnum[component]);
	}

	spin_unlock_irqrestore(&l1_lock, flags);

	if (l1_partnum_rc[component])
		return l1_partnum_rc[component];

	if (strscpy(buf, l1_partnum[component], len) < 0)
		return -E2BIG;

	return 0;
}

/*
 * The L1 answers two integers; the low byte of the second is the
 * one-character brick-type code of its chassis.
 */
static int l1_get_brick_type(void)
{
	u32 address = L1_ADDRESS(L1_TYPE_L1,
				 L1_ADDRESS_LOCAL | L1_TASK_GENERAL);
	u8 pkt[64];		/* command and response packet buffer */
	u8 *args;
	size_t arglen;
	u32 rack_slot, brick_type;
	int rc;

	rc = l1_transact(get_nasid(), pkt, sizeof(pkt), address,
			 L1_REQ_BRICK_ID, &args, &arglen, 0);
	if (rc < 0)
		return rc;
	if (rc != 2)
		return -EIO;

	rc = l1_packet_get_int(&args, &arglen, &rack_slot);
	if (rc)
		return rc;

	rc = l1_packet_get_int(&args, &arglen, &brick_type);
	if (rc)
		return rc;

	return brick_type & 0xff;
}

int l1_read_brick_type(void)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&l1_lock, flags);
	rc = l1_get_brick_type();
	spin_unlock_irqrestore(&l1_lock, flags);

	return rc;
}

static u8 ip27_l1_mac_addr[ETH_ALEN];
static bool ip27_l1_mac_addr_valid;
static bool ip27_l1_mac_addr_tried;

/*
 * A board that sources its MAC elsewhere, such as Tezro's tg3, yields
 * nothing here, so a failure logs at debug level rather than warning.
 */
unsigned char *arch_get_platform_mac_address(void)
{
	unsigned long flags;
	int rc;

	if (!system_is_ip35)
		return NULL;

	spin_lock_irqsave(&l1_lock, flags);

	if (!ip27_l1_mac_addr_tried) {
		ip27_l1_mac_addr_tried = true;

		rc = l1_get_brick_ethernet_address(get_nasid(),
						   ip27_l1_mac_addr);
		if (rc) {
			pr_debug("l1: EEPROM MAC address read failed (%d)\n",
				 rc);
		} else {
			ip27_l1_mac_addr_valid = true;
			pr_info("l1: EEPROM Ethernet address %pM\n",
				ip27_l1_mac_addr);
		}
	}

	spin_unlock_irqrestore(&l1_lock, flags);

	return ip27_l1_mac_addr_valid ? ip27_l1_mac_addr : NULL;
}
