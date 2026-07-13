/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/bitfield.h>
#include <linux/bits.h>

/* PORTSC - Port Status and Control Register - port_status_base bitmasks */
/* true: device connected */
#define PORT_CONNECT	BIT(0)
/* true: port enabled */
#define PORT_PE		BIT(1)
/* bit 2 reserved and zeroed */
/* true: port has an over-current condition */
#define PORT_OC		BIT(3)
/* true: port reset signaling asserted */
#define PORT_RESET	BIT(4)
/*
 * bits 8:5 - Port Link State, by default '5'.
 * Reading gives the current link PM state of the port.
 * Writing sets the link state, Port Link State Write Strobe (LWS) must be set.
 * PLS values 0-11 are defined in USB chaper 11.
 */
#define PORT_PLS_MASK	GENMASK(8, 5)
#define PLS_U0		0
#define PLS_U1		1
#define PLS_U2		2
#define PLS_U3		3
#define PLS_DISABLED	4
#define PLS_RXDETECT	5
#define PLS_INACTIVE	6
#define PLS_POLLING	7
#define PLS_RECOVERY	8
#define PLS_HOT_RESET	9
#define PLS_COMP_MODE	10
#define PLS_TEST_MODE	11
/* Values 12-14 are Reserved */
#define PLS_RESUME	15
/* bit 9 - Port Power (PP) */
#define PORT_POWER	BIT(9)
/*
 * bits 13:10 - Port Speed
 * Values defined in xHCI specification 7.2.2.1.1:
 * 0 - undefined speed - port hasn't be initialized by a reset yet
 * 1 - Full-speed
 * 2 - Low-speed
 * 3 - High-speed
 * 4 - SuperSpeed Gen1 x1
 * 5 - SuperSpeed Gen2 x1
 * 6 - SuperSpeed Gen1 x2
 * 7 - SuperSpeed Gen2 x2
 * 8-15 Reserved
 */
#define PORT_SPEED_MASK		GENMASK(13, 10)
#define	PORT_SPEED_FS		1
#define	PORT_SPEED_LS		2
#define	PORT_SPEED_HS		3
#define	PORT_SPEED_SS		4
#define	PORT_SPEED_SSP		5

/* bits 15:14 - Port Indicator Control */
#define PORT_PIC_MASK	GENMASK(15, 14)
#define PIC_OFF		0
#define PIC_AMBER	1
#define PIC_GREEN	2
/* Port Link State Write Strobe - set this when changing link state */
#define PORT_LINK_STROBE	BIT(16)
/* true: connect status change */
#define PORT_CSC	BIT(17)
/* true: port enable change */
#define PORT_PEC	BIT(18)
/* true: warm reset for a USB 3.0 device is done.  A "hot" reset puts the port
 * into an enabled state, and the device into the default state.  A "warm" reset
 * also resets the link, forcing the device through the link training sequence.
 * SW can also look at the Port Reset register to see when warm reset is done.
 */
#define PORT_WRC	BIT(19)
/* true: over-current change */
#define PORT_OCC	BIT(20)
/* true: reset change - 1 to 0 transition of PORT_RESET */
#define PORT_RC		BIT(21)
/* port link status change - set on some port link state transitions:
 *  Transition				Reason
 *  ------------------------------------------------------------------------------
 *  - U3 to Resume			Wakeup signaling from a device
 *  - Resume to Recovery to U0		USB 3.0 device resume
 *  - Resume to U0			USB 2.0 device resume
 *  - U3 to Recovery to U0		Software resume of USB 3.0 device complete
 *  - U3 to U0				Software resume of USB 2.0 device complete
 *  - U2 to U0				L1 resume of USB 2.1 device complete
 *  - U0 to U0 (???)			L1 entry rejection by USB 2.1 device
 *  - U0 to disabled			L1 entry error with USB 2.1 device
 *  - Any state to inactive		Error on USB 3.0 port
 */
#define PORT_PLC	BIT(22)
/* port configure error change - port failed to configure its link partner */
#define PORT_CEC	BIT(23)
#define PORT_CHANGE_MASK	(PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | \
				 PORT_RC | PORT_PLC | PORT_CEC)


/* Cold Attach Status - xHC can set this bit to report device attached during
 * Sx state. Warm port reset should be perfomed to clear this bit and move port
 * to connected state.
 */
#define PORT_CAS	BIT(24)
/* wake on connect (enable) */
#define PORT_WKCONN_E	BIT(25)
/* wake on disconnect (enable) */
#define PORT_WKDISC_E	BIT(26)
/* wake on over-current (enable) */
#define PORT_WKOC_E	BIT(27)
/* bits 28:29 reserved */
/* true: device is non-removable - for USB 3.0 roothub emulation */
#define PORT_DEV_REMOVE	BIT(30)
/* Initiate a warm port reset - complete when PORT_WRC is '1' */
#define PORT_WR		BIT(31)

/* We mark duplicate entries with -1 */
#define DUPLICATE_ENTRY ((u8)(-1))

/* Port Power Management Status and Control - port_power_base bitmasks */
/* Inactivity timer value for transitions into U1, in microseconds.
 * Timeout can be up to 127us.  0xFF means an infinite timeout.
 */
#define PORT_U1_TIMEOUT(p)	((p) & 0xff)
#define PORT_U1_TIMEOUT_MASK	0xff
/* Inactivity timer value for transitions into U2 */
#define PORT_U2_TIMEOUT(p)	(((p) & 0xff) << 8)
#define PORT_U2_TIMEOUT_MASK	(0xff << 8)
/* Bits 24:31 for port testing */

/* USB2 Protocol PORTSPMSC */
#define	PORT_L1S_MASK		7
#define	PORT_L1S_SUCCESS	1
#define	PORT_RWE		BIT(3)
#define	PORT_HIRD(p)		(((p) & 0xf) << 4)
#define	PORT_HIRD_MASK		(0xf << 4)
#define	PORT_L1DS_MASK		(0xff << 8)
#define	PORT_L1DS(p)		(((p) & 0xff) << 8)
#define	PORT_HLE		BIT(16)
#define PORT_TEST_MODE_SHIFT	28

/* USB3 Protocol PORTLI  Port Link Information */
#define PORT_LEC(p)		((p) & 0xffff)
#define PORT_RX_LANES(p)	(((p) >> 16) & 0xf)
#define PORT_TX_LANES(p)	(((p) >> 20) & 0xf)

/* eUSB2v2 protocol PORTLI Port Link information, RsvdP for normal USB2 */
#define PORTLI_RDR(p)		((p) & 0xf)
#define PORTLI_TDR(p)		(((p) >> 4) & 0xf)

/* USB2 Protocol PORTHLPMC */
#define PORT_HIRDM(p)((p) & 3)
#define PORT_L1_TIMEOUT(p)(((p) & 0xff) << 2)
#define PORT_BESLD(p)(((p) & 0xf) << 10)

/* use 512 microseconds as USB2 LPM L1 default timeout. */
#define XHCI_L1_TIMEOUT		512

/* Set default HIRD/BESL value to 4 (350/400us) for USB2 L1 LPM resume latency.
 * Safe to use with mixed HIRD and BESL systems (host and device) and is used
 * by other operating systems.
 *
 * XHCI 1.0 errata 8/14/12 Table 13 notes:
 * "Software should choose xHC BESL/BESLD field values that do not violate a
 * device's resume latency requirements,
 * e.g. not program values > '4' if BLC = '1' and a HIRD device is attached,
 * or not program values < '4' if BLC = '0' and a BESL device is attached.
 */
#define XHCI_DEFAULT_BESL	4

/*
 * USB3 specification define a 360ms tPollingLFPSTiemout for USB3 ports
 * to complete link training. usually link trainig completes much faster
 * so check status 10 times with 36ms sleep in places we need to wait for
 * polling to complete.
 */
#define XHCI_PORT_POLLING_LFPS_TIME  36
