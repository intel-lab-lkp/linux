/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xHCI Host Controller USB Port Register Set
 * xHCI Specification Section 5.4, Revision 1.2.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>

/* Port Status and Control (PORTSC) 5.4.8 */
/* bit 0 - Current Connect Status */
#define PORT_CCS	BIT(0)
/* bit 1 - Port Enabled/Disabled */
#define PORT_PED	BIT(1)
/* bit 2 - Rsvd */
/* bit 3 - Over-current Active */
#define PORT_OCA	BIT(3)
/* bit 4 - Port Reset */
#define PORT_PR		BIT(4)
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
/* bit 9 - Port Power */
#define PORT_PP		BIT(9)
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
/* bit 16 - Port Link State Write Strobe, set this when changing link state */
#define PORT_LWS	BIT(16)
/* bit 17 - Connect Status Change */
#define PORT_CSC	BIT(17)
/* bit 18 - Port Enabled/Disabled Change */
#define PORT_PEC	BIT(18)
/*
 * bit 19 - Warm Port Reset Change
 * Warm reset for a USB 3.0 device is done.  A "hot" reset puts the port
 * into an enabled state, and the device into the default state.  A "warm" reset
 * also resets the link, forcing the device through the link training sequence.
 * SW can also look at the Port Reset register to see when warm reset is done.
 */
#define PORT_WRC	BIT(19)
/* bit 20 - Over-current Change */
#define PORT_OCC	BIT(20)
/* bit 21 - Port Reset Change */
#define PORT_PRC	BIT(21)
/*
 * bit 22 - Port Link State Change, set on some port link state transitions:
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
/* bit 23 - Port Config Error Change, port failed to configure its link partner */
#define PORT_CEC	BIT(23)
/*
 * bit 24 - Cold Attach Status
 * xHC can set this bit to report device attached during Sx state.
 * Warm port reset should be perfomed to clear this bit and move port to connected state.
 */
#define PORT_CAS	BIT(24)
/* bit 25 - Wake on Connect Enable */
#define PORT_WCE	BIT(25)
/* bit 26 - Wake on Disconnect Enable */
#define PORT_WDE	BIT(26)
/* bit 27 - Wake on Over-current Enable */
#define PORT_WOE	BIT(27)
/* bits 29:28 - RsvdZ */
/* bit 30 - Device Removable, for USB 3.0 roothub emulation */
#define PORT_DR		BIT(30)
/* bit 31 - Warm Port Reset, complete when PORT_WRC is '1' */
#define PORT_WPR		BIT(31)
#define PORT_CHANGE_MASK	(PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | \
				 PORT_PRC | PORT_PLC | PORT_CEC)

/* We mark duplicate entries with -1 */
#define DUPLICATE_ENTRY ((u8)(-1))

/* USB3 Port Power Management Status and Control (PORTPMSC) 5.4.9.1 */
/*
 * bits 7:0 - U1 Timeout, inactivity timer value for transitions into U1.
 * Timeout can be 0us to 127us (0x7f), in 1us increments.
 * Value 0xff means an infinite timeout.
 */
#define PORT_U1_TIMEOUT_MASK	GENMASK(7, 0)
/*
 * bits 15:8 - U2 Timeout, inactivity timer value for transitions into U2.
 * Timeout can be 0us to 65024ms (0xfe), in 256us increments.
 * Value 0xff means an infinite timeout.
 */
#define PORT_U2_TIMEOUT_MASK	GENMASK(15, 8)
/* bit 16 - Force Link PM Accept (FLA) */
/* bits 31:17 - RsvdP */

/* USB2 Port Power Management Status and Control (PORTPMSC) 5.4.9.2 */
/* bits 2:0 - L1 Status */
#define	PORT_L1S_MASK		GENMASK(2, 0)
/* bit 3 - Remote Wake Enable */
#define	PORT_RWE		BIT(3)
/*
 * bits 7:4 - Best Effort Service Latency
 * Some host controllers may implement the pre-2011 USB 2.0 LPM definition,
 * where this field is interpreted as Host Initiated Resume Duration (HIRD).
 */
#define	PORT_BESL_MASK		GENMASK(7, 4)
/* bits 15:8 - L1 Device Slot */
#define	PORT_L1DS_MASK		GENMASK(15, 8)
/* bit 16 - Hardware LPM Enable */
#define	PORT_HLE		BIT(16)
/* bits 27:17 - RsvdP */
/* bits 31:28 - Port Test Control */
#define PORT_TEST_MODE_MASK	GENMASK(31, 28)

/* USB3 Port Link Info Register (PORTLI) 5.4.10.1 */
/* bits 15:0 - Link Error Count */
#define PORT_LEC_MASK		GENMASK(15, 0)
/* bits 19:16 - Rx Lane Count */
#define PORT_RLC_MASK		GENMASK(19, 16)
/* bits 23:20 - Tx Lane Count */
#define PORT_TLC_MASK		GENMASK(23, 20)
/* bits 31:24 - RsvdP */

/* USB2 Port Link Info Register (PORTLI) 5.4.10.2 */
/* bits 3:0 - Rx Data Rate, if E2V2C=1 else RsvdP */
#define PORT_RDR_MASK		GENMASK(3, 0)
/* bits 7:4 - Tx Data Rate, if E2V2C=1 else RsvdP */
#define PORT_TDR_MASK		GENMASK(7, 4)
/* bits 31:8 - RsvdP */

/* USB3 Port Hardware LPM Control Register (PORTHLPMC) 5.4.11.1 */
/* bits 15:0 - Link Soft Error Count, if LSECC=1 else RsvdP */
/* bits 31:16 - RsvdP */

/* USB2 Port Hardware LPM Control Register (PORTHLPMC) 5.4.11.2 */
/* bits 1:0 - Host Initiated Resume Duration Mode */
#define PORT_HIRDM_MASK		GENMASK(1, 0)
/*
 * bits 9:2 - L1 Timeout, can be 128us to 65280us (0xff), in 128us increments.
 * The default timeout is 128us.
 */
#define PORT_L1_TIMEOUT_MASK	GENMASK(9, 2)
#define XHCI_L1_TIMEOUT		512
/* bits 13:10 - Best Effort Service Latency Deep */
#define PORT_BESLD_MASK		GENMASK(13, 10)
/* bits 31:14 - RsvdP */

/*
 * Set default HIRD/BESL value to 4 (350/400us) for USB2 L1 LPM resume latency.
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
