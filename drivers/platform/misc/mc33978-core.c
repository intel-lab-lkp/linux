// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 David Jander <david@protonic.nl>, Protonic Holland
 * Copyright (C) 2026 Oleksij Rempel <kernel@pengutronix.de>, Pengutronix
 *
 * MC33978/MC34978 - Core functionality module
 *
 * This module implements the complex functionality:
 * - Custom regmap bus (pipelined SPI protocol)
 * - IRQ chip + IRQ domain (23 virtual IRQs)
 * - Event processing (pin changes + fault detection)
 * - Status harvesting from SPI responses
 *
 * The MFD driver (mc33978.c) calls mc33978_core_init() to initialize
 * this functionality, keeping the MFD driver simple for review.
 *
 * Custom SPI Regmap & Event Harvesting:
 * The device uses a non-standard pipelined SPI protocol where the MISO
 * response logically lags the MOSI command by one frame. Furthermore, the
 * hardware embeds volatile global status bits (INT_flg, FAULT_STAT) into the
 * high byte of almost every SPI response (with specific exceptions handled by
 * the decoder). This core implements a custom regmap_bus to handle the
 * 2-frame dummy fetches and transparently "harvests" these status bits in
 * the background to schedule event processing.
 *
 * Interrupt Quirks & Limitations (MC33978 Rev. 10, §9.10.27):
 * - Clear-on-Read INT_flg: The INT_B pin is cleared 1.0ms after CS_B falling
 * edge. Any SPI message that returns INT_flg will clear this flag. The
 * INT_flg bit is set on any interrupt event (pin change, fault). Harvesting
 * INT_flg from all SPI traffic is the ONLY way to know this device triggered
 * an interrupt (crucial for shared IRQ lines where another driver's regmap
 * access would silently clear the flag).
 * - FAULT_STAT Latching (§9.10.27): FAULT_STAT is a sticky summary bit that
 * latches HIGH when any fault occurs and remains HIGH in all subsequent SPI
 * responses until the Fault register (0x42) is read. Reading Fault register
 * clears the latch; hardware immediately re-latches if fault condition still
 * present. INT_flg clears on any SPI read regardless of persistence. This
 * allows transient detection: FAULT_STAT can appear in one SPI frame (latched
 * evidence) but be absent in the next (condition cleared before register read),
 * proving a transient fault occurred.
 * - Stateless Pin Edge Detection: Hardware lacks per-pin interrupt status
 * registers. READ_IN register (§9.10.27) returns instantaneous switch state
 * (Logic[1]=closed, Logic[0]=open). The driver XORs current state against
 * cached state to derive which pins changed.
 * - Missed Short Pulses: Pin interrupts are state-sampled, not edge-latched.
 * Physical pulses shorter than SPI read latency (~1-2ms) will be missed if
 * the pin reverts before READ_IN sampling. Inherent hardware limitation.
 * - Edge-Only Pin Interrupts: Hardware asserts INT_B only on state changes,
 * never continuously for sustained levels. Driver emulates edge interrupts
 * (RISING/FALLING) and rejects LEVEL configurations to match hardware.
 */

#include <linux/array_size.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cache.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/string.h>

#include <linux/mfd/mc33978.h>

/* Device identification signature returned by CHECK register */
#define MC33978_CHECK_SIGNATURE		0x123456

/*
 * Pipelined two-frame SPI transfer:
 * [REQ]  - Transmits command/write-data, receives dummy/previous response
 * [PIPE] - Transmits dummy CHECK, receives actual response to current command
 */
enum mc33978_frame_index {
	MC33978_FRAME_REQ = 0,
	MC33978_FRAME_PIPE,
	MC33978_FRAME_COUNT
};

/* SPI frame byte offsets (transmitted MSB first) */
enum mc33978_frame_offset {
	MC33978_FRAME_CMD = 0,
	MC33978_FRAME_DATA_HI,
	MC33978_FRAME_DATA_MID,
	MC33978_FRAME_DATA_LO
};

#define MC33978_FRAME_LEN		4

/* Regmap internal value buffer offsets */
enum mc33978_payload_offset {
	MC33978_PAYLOAD_HI = 0,
	MC33978_PAYLOAD_MID,
	MC33978_PAYLOAD_LO
};

#define MC33978_PAYLOAD_LEN		3

/*
 * SPI Command Byte (FRAME_CMD).
 * Maps to frame bit [24] in the datasheet.
 */
#define MC33978_CMD_BYTE_WRITE		BIT(0)

/* High Payload Byte Masks (FRAME_DATA_HI / PAYLOAD_HI). */
/* Maps to frame bit [23] */
#define MC33978_HI_BYTE_STAT_FAULT     BIT(7)
/* Maps to frame bit [22] */
#define MC33978_HI_BYTE_STAT_INT       BIT(6)

#define MC33978_HI_BYTE_STATUS_MASK    (MC33978_HI_BYTE_STAT_FAULT | \
					MC33978_HI_BYTE_STAT_INT)

/* Synthetic wakeup bit for harvested flags */
#define MC33978_HARVEST_WAKE_BIT	BIT(8)

/* Maps to frame bits [21:16] */
#define MC33978_HI_BYTE_DATA_MASK	GENMASK(5, 0)

#define MC33978_CACHE_SG_PIN_MASK	GENMASK(13, 0)
#define MC33978_CACHE_SP_PIN_MASK	GENMASK(21, 14)

#define MC33978_SG_PIN_MASK		GENMASK(13, 0)
#define MC33978_SP_PIN_MASK		GENMASK(7, 0)

enum mc33978_device_type {
	MC33978,
	MC34978,
};

struct mc33978_core_data {
	struct device *dev;
	struct spi_device *spi;
	struct regmap *map;
	struct regulator *vddq;
	struct regulator *vbatp;
	struct irq_domain *domain;

	/* Pre-built SPI messages (immutable after init) */
	struct spi_message msg_read;
	struct spi_message msg_write;
	struct spi_transfer xfer_read[MC33978_FRAME_COUNT];
	struct spi_transfer xfer_write;

	/* Protected by event_lock */
	struct mutex event_lock;
	/* Previous pin state for edge detection */
	u32 cached_pin_state;

	/*
	 * IRQ subsystem locking:
	 * - irq_lock: Mutex for sleeping operations (regmap/SPI access)
	 * - irq_state_lock: Raw spinlock for atomic state access
	 */
	struct mutex irq_lock;
	raw_spinlock_t irq_state_lock;
	/* IRQ mask for 23 IRQs: bits 0-21 for pins, bit 22 for fault */
	u32 cached_pin_mask;
	/* Rising edge IRQ enable mask (23 IRQs) */
	u32 irq_rise;
	/* Falling edge IRQ enable mask (23 IRQs) */
	u32 irq_fall;

	/* Protected by state_lock */
	spinlock_t state_lock;
	/* Prevents work scheduling during teardown */
	bool tearing_down;
	/* Prevents work scheduling before IRQ handler ready */
	bool irq_ready;
	/* Latest physical fault state on bus */
	bool bus_fault_active;
	/* Cached fault state from previous event */
	bool cached_fault_active;

	/* Atomic operations (no lock needed) */
	/* Status bits from SPI responses */
	atomic_t harvested_flags;

	/*
	 * Work scheduling protected by state_lock.
	 * Work execution serialized by workqueue subsystem.
	 */
	struct work_struct event_work;

	/*
	 * DMA buffers protected by SPI subsystem + regmap serialization.
	 * Modified before spi_sync(), read after it returns.
	 * Cache-line aligned to prevent DMA corruption from adjacent fields.
	 */
	u8 tx_frame[MC33978_FRAME_COUNT][MC33978_FRAME_LEN] ____cacheline_aligned;
	u8 rx_frame[MC33978_FRAME_COUNT][MC33978_FRAME_LEN] ____cacheline_aligned;
};

static void mc33978_irq_mask(struct irq_data *data)
{
	struct mc33978_core_data *cdata = irq_data_get_irq_chip_data(data);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	if (hwirq < MC33978_NUM_IRQS) {
		scoped_guard(raw_spinlock_irqsave, &cdata->irq_state_lock)
			cdata->cached_pin_mask &= ~BIT(hwirq);
	}
}

static void mc33978_irq_unmask(struct irq_data *data)
{
	struct mc33978_core_data *cdata = irq_data_get_irq_chip_data(data);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	if (hwirq < MC33978_NUM_IRQS) {
		scoped_guard(raw_spinlock_irqsave, &cdata->irq_state_lock)
			cdata->cached_pin_mask |= BIT(hwirq);
	}
}

static void mc33978_irq_bus_lock(struct irq_data *data)
{
	struct mc33978_core_data *cdata = irq_data_get_irq_chip_data(data);

	mutex_lock(&cdata->irq_lock);
}

/* Sync cached IRQ mask to hardware IE_SG/IE_SP registers, then unlock */
static void mc33978_irq_bus_sync_unlock(struct irq_data *data)
{
	struct mc33978_core_data *cdata = irq_data_get_irq_chip_data(data);
	u32 sg_mask, sp_mask, cached_mask;
	int ret;

	scoped_guard(raw_spinlock_irqsave, &cdata->irq_state_lock)
		cached_mask = cdata->cached_pin_mask;

	/*
	 * Split the cached 22-bit pin mask into hardware register format:
	 * - SG pins: bits [13:0] (14 pins, mask 0x3FFF)
	 * - SP pins: bits [21:14] (8 pins, mask 0xFF)
	 */
	sg_mask = FIELD_GET(MC33978_CACHE_SG_PIN_MASK, cached_mask);
	sp_mask = FIELD_GET(MC33978_CACHE_SP_PIN_MASK, cached_mask);

	ret = regmap_update_bits(cdata->map, MC33978_REG_IE_SG,
				 MC33978_SG_PIN_MASK, sg_mask);
	if (ret)
		dev_err(&cdata->spi->dev, "failed to sync SG IRQ mask: %d\n", ret);

	ret = regmap_update_bits(cdata->map, MC33978_REG_IE_SP,
				 MC33978_SP_PIN_MASK, sp_mask);
	if (ret)
		dev_err(&cdata->spi->dev, "failed to sync SP IRQ mask: %d\n", ret);

	mutex_unlock(&cdata->irq_lock);
}

static int mc33978_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct mc33978_core_data *cdata = irq_data_get_irq_chip_data(data);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	u32 mask;

	if (hwirq >= MC33978_NUM_IRQS)
		return -EINVAL;

	/*
	 * Safe to use BIT(hwirq) with u32: we support 23 IRQs (0-22),
	 * validated by check above. Using u32 for consistency with
	 * irq_rise/irq_fall/cached_pin_mask fields.
	 */
	mask = BIT(hwirq);

	if (type == IRQ_TYPE_NONE)
		return -EINVAL;

	if (type & (IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW))
		return -EINVAL;

	scoped_guard(raw_spinlock_irqsave, &cdata->irq_state_lock) {
		if (type & IRQ_TYPE_EDGE_RISING)
			cdata->irq_rise |= mask;
		else
			cdata->irq_rise &= ~mask;

		if (type & IRQ_TYPE_EDGE_FALLING)
			cdata->irq_fall |= mask;
		else
			cdata->irq_fall &= ~mask;
	}

	return 0;
}

static int mc33978_irq_set_wake(struct irq_data *data, unsigned int on)
{
	struct mc33978_core_data *cdata = irq_data_get_irq_chip_data(data);

	return irq_set_irq_wake(cdata->spi->irq, on);
}

static struct irq_chip mc33978_irq_chip = {
	.name			= "mc33978",
	.irq_mask		= mc33978_irq_mask,
	.irq_unmask		= mc33978_irq_unmask,
	.irq_bus_lock		= mc33978_irq_bus_lock,
	.irq_bus_sync_unlock	= mc33978_irq_bus_sync_unlock,
	.irq_set_type		= mc33978_irq_set_type,
	.irq_set_wake		= mc33978_irq_set_wake,
};

static void mc33978_irq_setup(struct irq_domain *domain, unsigned int virq,
			      irq_hw_number_t hwirq)
{
	struct mc33978_core_data *cdata = domain->host_data;

	irq_domain_set_info(domain, virq, hwirq, &mc33978_irq_chip, cdata,
			    handle_simple_irq, NULL, NULL);
	irq_set_nested_thread(virq, 1);
	irq_clear_status_flags(virq, IRQ_NOREQUEST | IRQ_NOPROBE);
}

static int mc33978_irq_map(struct irq_domain *d, unsigned int virq,
			   irq_hw_number_t hw)
{
	mc33978_irq_setup(d, virq, hw);
	return 0;
}

static int mc33978_irq_domain_alloc(struct irq_domain *domain,
				    unsigned int virq,
				    unsigned int nr_irqs, void *arg)
{
	struct irq_fwspec *fwspec = arg;
	irq_hw_number_t hwirq;
	int i;

	if (fwspec->param_count < 1)
		return -EINVAL;

	hwirq = fwspec->param[0];

	if (hwirq >= MC33978_NUM_IRQS ||
	    nr_irqs > MC33978_NUM_IRQS - hwirq)
		return -EINVAL;

	for (i = 0; i < nr_irqs; i++)
		mc33978_irq_setup(domain, virq + i, hwirq + i);

	return 0;
}

static void mc33978_irq_domain_free(struct irq_domain *domain,
				    unsigned int virq,
				    unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++)
		irq_domain_reset_irq_data(irq_domain_get_irq_data(domain,
								  virq + i));
}

/*
 * Dual-mode IRQ domain: supports both direct MFD child (hwmon via .map)
 * and hierarchical child (pinctrl GPIO IRQ chip via .alloc). The .xlate
 * translates DT 2-cell format (hwirq 0-22, type flags).
 * IRQ_DOMAIN_FLAG_HIERARCHY enables the pinctrl hierarchical chain.
 */
static const struct irq_domain_ops mc33978_irq_domain_ops = {
	.map	= mc33978_irq_map,
	.alloc	= mc33978_irq_domain_alloc,
	.free	= mc33978_irq_domain_free,
	.xlate	= irq_domain_xlate_twocell,
};

static void mc33978_irq_domain_remove(void *data)
{
	struct mc33978_core_data *cdata = data;
	struct irq_domain *domain = cdata->domain;
	int hwirq;

	/*
	 * Must manually dispose mappings before irq_domain_remove().
	 * Child platform_get_irq() creates mappings via irq_create_of_mapping().
	 * devm_request_irq() only calls free_irq(), not irq_dispose_mapping().
	 * irq_domain_remove() expects empty radix tree (has WARN_ON check).
	 */
	for (hwirq = 0; hwirq < MC33978_NUM_IRQS; hwirq++) {
		unsigned int virq;

		virq = irq_find_mapping(domain, hwirq);
		if (virq)
			irq_dispose_mapping(virq);
	}

	irq_domain_remove(domain);
}

static void mc33978_handle_pin_changes(struct mc33978_core_data *cdata,
				       unsigned int pin_state)
{
	unsigned long fired_pins = 0;
	u32 changed_pins;
	u32 rise, fall, pin_mask;
	int i;

	changed_pins = pin_state ^ cdata->cached_pin_state;
	if (!changed_pins)
		return;

	cdata->cached_pin_state = pin_state;

	scoped_guard(raw_spinlock_irqsave, &cdata->irq_state_lock) {
		pin_mask = cdata->cached_pin_mask;
		rise = cdata->irq_rise;
		fall = cdata->irq_fall;
	}

	changed_pins &= pin_mask;

	if (!changed_pins)
		return;

	fired_pins |= (changed_pins & pin_state) & rise;
	fired_pins |= (changed_pins & ~pin_state) & fall;

	for_each_set_bit(i, &fired_pins, MC33978_NUM_PINS) {
		int virq = irq_find_mapping(cdata->domain, i);

		if (virq)
			handle_nested_irq(virq);
	}
}

/*
 * Fault Signaling Variants (hwirq 22, consumed by hwmon driver):
 *
 * The driver distinguishes three fault event types based on timing and
 * hardware FAULT_STAT latch behavior:
 *
 * 1. Sustained Fault Edges (normal operation):
 *    - Fault persists through detection cycle
 *    - Rising edge: fault_active=1, cached_fault=0 -> dispatch if rise enabled
 *    - Falling edge: fault_active=0, cached_fault=1 -> dispatch if fall enabled
 *    - Example: Overtemperature condition that lasts seconds
 *
 * 2. Transient Fault (brief pulse <1ms):
 *    - Fault occurs and clears before Fault register (0x42) read
 *    - Detection: hw_flags contains FAULT_STAT bit (latched evidence from
 *      pipelined SPI REQ frame), but both fault_active=0 and cached_fault=0
 *      (PIPE frame showed condition cleared)
 *    - Dispatch: Single IRQ if ANY edge enabled (represents both edges)
 *    - Example: VBATP voltage glitch during inductive load switching, or fault
 *      clears coincidentally between SPI frames (threshold boundary oscillation)
 *    - Rationale: Without preserving FAULT_STAT bit, these events are invisible
 *
 * 3. No Event (filtered):
 *    - Fault state unchanged (fault_active == cached_fault) AND no transient
 *      evidence in hw_flags
 *    - Common during polling: hardware still overtemp, state already reflected
 *    - No IRQ dispatch (prevents duplicate notifications)
 *
 * State Tracking:
 * - bus_fault_active: Live FAULT_STAT from most recent SPI response
 * - cached_fault_active: Last fault state dispatched to consumer
 * - hw_flags: Accumulated FAULT_STAT bits from pipelined SPI (transient evidence)
 *
 * Edge Configuration:
 * - Consumers use irq_set_irq_type(virq, IRQ_TYPE_EDGE_RISING/FALLING/BOTH)
 * - Level types rejected (hardware limitation: edge-only assertion)
 * - Masking via disable_irq() prevents dispatch but harvesting continues
 */
static void mc33978_handle_fault_condition(struct mc33978_core_data *cdata,
					   unsigned int hw_flags)
{
	bool fault_active, cached_fault, transient, changed;
	u32 rise, fall;
	int virq;

	scoped_guard(spinlock_irqsave, &cdata->state_lock) {
		fault_active = cdata->bus_fault_active;
		cached_fault = cdata->cached_fault_active;

		changed = fault_active ^ cached_fault;
		if (changed)
			cdata->cached_fault_active = fault_active;
	}

	/*
	 * Transient fault detection (§9.10.27): FAULT_STAT latches on fault
	 * occurrence and persists until Fault register (0x42) read clears it.
	 * Hwmon driver reads Fault register, which triggers regmap SPI read with
	 * pipelined frames: REQ frame (old response, FAULT_STAT=1 latched) and
	 * PIPE frame (Fault register response clears latch). If fault condition
	 * cleared between latch and read, FAULT_STAT won't re-latch (datasheet:
	 * "immediately set again if condition still present").
	 *
	 * Transient detection: hw_flags contains harvested FAULT_STAT bit
	 * (latched evidence from REQ frame), but bus_fault_active=0 and
	 * cached_fault_active=0 (PIPE frame showed cleared, condition gone).
	 * This proves brief fault (VBATP voltage glitch, threshold oscillation)
	 * occurred and cleared before Fault register read. Edge-triggered
	 * consumers need this event notification.
	 *
	 * WAKE_BIT exclusion: WAKE_BIT is synthetic (bus_fault_active changes),
	 * not hardware evidence. Including it causes phantom transients when
	 * normal faults deassert (WAKE_BIT present but no actual transient).
	 */
	transient = !changed && !fault_active && !cached_fault &&
		    (hw_flags & MC33978_HI_BYTE_STAT_FAULT);

	if (!changed && !transient)
		return;

	scoped_guard(raw_spinlock_irqsave, &cdata->irq_state_lock) {
		rise = cdata->irq_rise;
		fall = cdata->irq_fall;
	}

	virq = irq_find_mapping(cdata->domain, MC33978_HWIRQ_FAULT);
	if (!virq)
		return;

	if (transient) {
		/*
		 * Transient pulse: both edges occurred. Dispatch once if
		 * any edge is enabled. Dispatching both edges separately
		 * would incorrectly report two interrupts for one event.
		 */
		if ((rise | fall) & BIT(MC33978_HWIRQ_FAULT))
			handle_nested_irq(virq);
	} else if ((fault_active && (rise & BIT(MC33978_HWIRQ_FAULT))) ||
		    (!fault_active && (fall & BIT(MC33978_HWIRQ_FAULT)))) {
		/* Normal edge */
		handle_nested_irq(virq);
	}
}

static void mc33978_process_single_event(struct mc33978_core_data *cdata)
{
	unsigned int harvested;
	unsigned int pin_state;
	int ret;

	/*
	 * Grab harvested_flags BEFORE hardware read. Flags harvested during
	 * the read trigger another loop pass. This intentionally forces a
	 * redundant SPI read on most interrupts, but is necessary to prevent
	 * lost events when concurrent regmap access races with IRQ thread
	 * (hardware has clear-on-read INT_flg).
	 */
	harvested = atomic_xchg(&cdata->harvested_flags, 0);

	ret = regmap_read(cdata->map, MC33978_REG_READ_IN, &pin_state);
	if (ret)
		dev_err_ratelimited(&cdata->spi->dev, "failed to read pin state: %d\n",
				    ret);
	else
		mc33978_handle_pin_changes(cdata, pin_state);

	mc33978_handle_fault_condition(cdata, harvested);
}

static void mc33978_handle_events(struct mc33978_core_data *cdata)
{
	guard(mutex)(&cdata->event_lock);

	do {
		mc33978_process_single_event(cdata);
	} while (atomic_read(&cdata->harvested_flags) != 0);
}

static irqreturn_t mc33978_irq_thread(int irq, void *data)
{
	mc33978_handle_events(data);

	return IRQ_HANDLED;
}

static void mc33978_teardown(void *data)
{
	struct mc33978_core_data *cdata = data;

	/*
	 * Set teardown flag before cancel_work_sync(). Prevents debugfs
	 * regmap reads from rescheduling work after cancellation during
	 * the devres LIFO teardown window.
	 */
	scoped_guard(spinlock_irqsave, &cdata->state_lock) {
		cdata->tearing_down = true;
	}

	cancel_work_sync(&cdata->event_work);
}

static int mc33978_irq_init(struct mc33978_core_data *cdata,
			    struct fwnode_handle *fwnode)
{
	struct device *dev = &cdata->spi->dev;
	int ret;

	mutex_init(&cdata->irq_lock);

	/*
	 * Create IRQ domain with 23 interrupts:
	 * - hwirq 0-21: Pin change interrupts (22 pins)
	 * - hwirq 22: Fault interrupt (for hwmon driver)
	 */
	cdata->domain = irq_domain_create_linear(fwnode, MC33978_NUM_IRQS,
						 &mc33978_irq_domain_ops, cdata);
	if (!cdata->domain)
		return dev_err_probe(dev, -ENOMEM, "failed to create IRQ domain\n");

	/*
	 * Use DOMAIN_BUS_NEXUS to distinguish this intermediate demux domain
	 * from child domains sharing the same fwnode. Matches the pattern used
	 * by other MFD drivers (e.g., crystalcove).
	 */
	irq_domain_update_bus_token(cdata->domain, DOMAIN_BUS_NEXUS);

	/*
	 * Enable hierarchical IRQ domain support for pinctrl's GPIO IRQ chip.
	 * See mc33978_irq_domain_ops for detailed architecture explanation.
	 */
	cdata->domain->flags |= IRQ_DOMAIN_FLAG_HIERARCHY;

	ret = devm_add_action_or_reset(dev, mc33978_irq_domain_remove, cdata);
	if (ret)
		return ret;

	return 0;
}

static void mc33978_event_work(struct work_struct *work)
{
	struct mc33978_core_data *cdata =
		container_of(work, struct mc33978_core_data, event_work);

	mc33978_handle_events(cdata);
}

/*
 * Status Harvesting: Opportunistic Event Detection
 *
 * The hardware embeds volatile status bits (FAULT_STAT, INT_flg) in the high
 * byte of almost every SPI response. These bits are harvested from all regmap
 * operations (reads, writes, any register) to detect events regardless of
 * which code path triggered the SPI transaction.
 *
 * Rationale for Harvesting All Traffic:
 *
 * The INT_flg bit is clear-on-read: any SPI transaction clears it, even if
 * unrelated to interrupt handling. On shared IRQ lines, another driver's
 * regmap access could clear INT_flg before this driver's IRQ thread runs,
 * making it impossible to determine if this device triggered the interrupt.
 * Harvesting INT_flg from all traffic ensures we see it before it's cleared.
 *
 * Current Usage:
 *
 * The driver does NOT use IRQF_SHARED (see mc33978_core_init comment), so
 * shared IRQ protection is currently defensive/future-proofing. The harvesting
 * architecture supports shared IRQs if the design changes.
 *
 * The Fault register (0x42) is marked volatile+precious in regmap config,
 * which excludes it from regmap debugfs dumps, so unintended side effects
 * from debug inspection cannot occur. Harvesting still applies to intentional
 * Fault register reads from the hwmon driver.
 *
 * Harvesting Call Sites:
 * - mc33978_spi_write(): Single frame (1 harvest from response)
 * - mc33978_spi_read(): Pipelined (2 harvests: REQ frame + PIPE frame)
 */
static void mc33978_harvest_status(struct mc33978_core_data *cdata, int status)
{
	bool fault_active;

	fault_active = !!(status & MC33978_HI_BYTE_STAT_FAULT);

	scoped_guard(spinlock_irqsave, &cdata->state_lock) {
		cdata->bus_fault_active = fault_active;

		/*
		 * If the bus state changed from what the IRQ thread last
		 * evaluated, wake it up using a synthetic software bit to avoid
		 * overloading the hardware STAT_FAULT bit and causing phantom
		 * transient faults.
		 */
		if (fault_active != cdata->cached_fault_active)
			atomic_or(MC33978_HARVEST_WAKE_BIT,
				  &cdata->harvested_flags);
	}

	if (status & MC33978_HI_BYTE_STAT_INT)
		atomic_or(MC33978_HI_BYTE_STAT_INT, &cdata->harvested_flags);

	/*
	 * Preserve FAULT_STAT bit for transient detection: FAULT_STAT is sticky
	 * (latched until Fault register read). When hwmon reads Fault register,
	 * pipelined SPI produces two harvest calls: first with FAULT_STAT=1
	 * (latched evidence), second with FAULT_STAT=0 (if condition cleared).
	 * Transient detection in mc33978_handle_fault_condition() needs the
	 * harvested FAULT_STAT bit as proof the fault occurred, even if both
	 * bus_fault_active and cached_fault_active are false (condition cleared
	 * before Fault register read could re-latch it).
	 *
	 * Always harvest when present; mc33978_handle_fault_condition() filters
	 * transients vs sustained faults using bus_fault_active state tracking.
	 */
	if (status & MC33978_HI_BYTE_STAT_FAULT)
		atomic_or(MC33978_HI_BYTE_STAT_FAULT, &cdata->harvested_flags);

	/*
	 * Barrier required: atomic_or() is RELAXED, spin_lock() is ACQUIRE.
	 * Without barrier, atomic_or() can be reordered past the lock, causing
	 * both work's final check and our check below to miss the flag.
	 */
	smp_mb__after_atomic();

	scoped_guard(spinlock_irqsave, &cdata->state_lock) {
		if (cdata->irq_ready && !cdata->tearing_down &&
		    atomic_read(&cdata->harvested_flags))
			schedule_work(&cdata->event_work);
	}
}

/*
 * Initialize persistent SPI messages.
 * Write: 1 frame. Read: 2 frames (MISO lags by 1 frame, needs dummy fetch).
 */
static void mc33978_prepare_messages(struct mc33978_core_data *cdata)
{
	/* --- Prepare Write Message (1 Frame) --- */
	spi_message_init(&cdata->msg_write);

	cdata->xfer_write.tx_buf = cdata->tx_frame[MC33978_FRAME_REQ];
	cdata->xfer_write.rx_buf = cdata->rx_frame[MC33978_FRAME_REQ];
	cdata->xfer_write.len = MC33978_FRAME_LEN;

	spi_message_add_tail(&cdata->xfer_write, &cdata->msg_write);

	/* --- Prepare Read Message (2 Frames) --- */
	spi_message_init(&cdata->msg_read);

	/* Frame 1: Request */
	cdata->xfer_read[MC33978_FRAME_REQ].tx_buf =
		cdata->tx_frame[MC33978_FRAME_REQ];
	cdata->xfer_read[MC33978_FRAME_REQ].rx_buf =
		cdata->rx_frame[MC33978_FRAME_REQ];
	cdata->xfer_read[MC33978_FRAME_REQ].len = MC33978_FRAME_LEN;
	/* Latch command */
	cdata->xfer_read[MC33978_FRAME_REQ].cs_change = 1;

	/* Frame 2: Fetch (Dummy CHECK) */
	cdata->xfer_read[MC33978_FRAME_PIPE].tx_buf =
		cdata->tx_frame[MC33978_FRAME_PIPE];
	cdata->xfer_read[MC33978_FRAME_PIPE].rx_buf =
		cdata->rx_frame[MC33978_FRAME_PIPE];
	cdata->xfer_read[MC33978_FRAME_PIPE].len = MC33978_FRAME_LEN;

	/* Preload the dummy CHECK command statically */
	cdata->tx_frame[MC33978_FRAME_PIPE][MC33978_FRAME_CMD] = MC33978_REG_CHECK;

	spi_message_add_tail(&cdata->xfer_read[MC33978_FRAME_REQ], &cdata->msg_read);
	spi_message_add_tail(&cdata->xfer_read[MC33978_FRAME_PIPE], &cdata->msg_read);
}

/*
 * Decode 4-byte SPI frame to 3-byte regmap payload, extract status bits.
 *
 * Semi-global status flags (§9.10.27): FAULT_STAT and INT_flg bits are
 * returned in most register responses for opportunistic harvesting, with
 * documented exceptions: SPICheck (REG_CHECK) and Wetting Current config
 * registers (REG_WET_*) use those bit positions for device configuration
 * instead of status flags.
 *
 * Return: status bits (MC33978_HI_BYTE_STATUS_MASK) or -ENODATA if register
 *         has no status bits.
 */
static int mc33978_rx_decode(const u8 *rx_frame, u8 *val_buf)
{
	u8 cmd = rx_frame[MC33978_FRAME_CMD] & ~MC33978_CMD_BYTE_WRITE;
	bool has_status;
	u8 status = 0;

	switch (cmd) {
	case MC33978_REG_CHECK:
	case MC33978_REG_WET_SP:
	case MC33978_REG_WET_SG0:
	case MC33978_REG_WET_SG1:
		has_status = false;
		break;
	default:
		has_status = true;
		break;
	}

	if (has_status)
		status = rx_frame[MC33978_FRAME_DATA_HI] &
						MC33978_HI_BYTE_STATUS_MASK;

	if (val_buf) {
		memcpy(val_buf, &rx_frame[MC33978_FRAME_DATA_HI],
		       MC33978_PAYLOAD_LEN);

		if (has_status)
			val_buf[MC33978_PAYLOAD_HI] &= MC33978_HI_BYTE_DATA_MASK;
	}

	return has_status ? status : -ENODATA;
}

static int mc33978_spi_write(void *ctx, const void *data, size_t count)
{
	struct mc33978_core_data *cdata = ctx;
	int status;
	int ret;

	if (count != MC33978_FRAME_LEN)
		return -EINVAL;

	memcpy(cdata->tx_frame[MC33978_FRAME_REQ], data, MC33978_FRAME_LEN);

	ret = spi_sync(cdata->spi, &cdata->msg_write);
	if (ret)
		return ret;

	status = mc33978_rx_decode(cdata->rx_frame[MC33978_FRAME_REQ], NULL);
	if (status >= 0)
		mc33978_harvest_status(cdata, status);

	return 0;
}

static int mc33978_spi_read(void *ctx, const void *reg_buf, size_t reg_size,
			    void *val_buf, size_t val_size)
{
	struct mc33978_core_data *cdata = ctx;
	int status_req, status_pipe;
	int ret;

	if (reg_size != 1 || val_size != MC33978_PAYLOAD_LEN)
		return -EINVAL;

	memset(&cdata->tx_frame[MC33978_FRAME_REQ][MC33978_FRAME_DATA_HI], 0,
	       MC33978_PAYLOAD_LEN);
	cdata->tx_frame[MC33978_FRAME_REQ][MC33978_FRAME_CMD] =
		((const u8 *)reg_buf)[0];

	ret = spi_sync(cdata->spi, &cdata->msg_read);
	if (ret)
		return ret;

	status_req = mc33978_rx_decode(cdata->rx_frame[MC33978_FRAME_REQ], NULL);
	status_pipe = mc33978_rx_decode(cdata->rx_frame[MC33978_FRAME_PIPE],
					val_buf);

	if (status_req >= 0)
		mc33978_harvest_status(cdata, status_req);
	if (status_pipe >= 0)
		mc33978_harvest_status(cdata, status_pipe);

	return 0;
}

static const struct regmap_bus mc33978_regmap_bus = {
	.read = mc33978_spi_read,
	.write = mc33978_spi_write,
};

static const struct regmap_range mc33978_volatile_range[] = {
	regmap_reg_range(MC33978_REG_ENTER_LPM, MC33978_REG_ENTER_LPM),
	regmap_reg_range(MC33978_REG_READ_IN, MC33978_REG_RESET),
};

static const struct regmap_access_table mc33978_volatile_table = {
	.yes_ranges = mc33978_volatile_range,
	.n_yes_ranges = ARRAY_SIZE(mc33978_volatile_range),
};

static const struct regmap_range mc33978_precious_range[] = {
	regmap_reg_range(MC33978_REG_ENTER_LPM, MC33978_REG_ENTER_LPM),
	regmap_reg_range(MC33978_REG_READ_IN, MC33978_REG_RESET),
};

static const struct regmap_access_table mc33978_precious_table = {
	.yes_ranges = mc33978_precious_range,
	.n_yes_ranges = ARRAY_SIZE(mc33978_precious_range),
};

/*
 * NOTE: Need to fake REG_ENTER_LPM, REG_IRQ and REG_RESET as readable, so
 * regcache will NOT write them on a cache sync. Sounds counterintuitive, but
 * marking a reg as "precious" or "volatile" is the only way to avoid this,
 * and that works only with readable regs.
 */
static const struct regmap_range mc33978_readable_range[] = {
	regmap_reg_range(MC33978_REG_CHECK, MC33978_REG_WET_SG1),
	regmap_reg_range(MC33978_REG_CWET_SP, MC33978_REG_ENTER_LPM),
	regmap_reg_range(MC33978_REG_AMUX_CTRL, MC33978_REG_RESET),
};

static const struct regmap_access_table mc33978_readable_table = {
	.yes_ranges = mc33978_readable_range,
	.n_yes_ranges = ARRAY_SIZE(mc33978_readable_range),
};

static const struct regmap_range mc33978_writable_range[] = {
	regmap_reg_range(MC33978_REG_CONFIG, MC33978_REG_WET_SG1),
	regmap_reg_range(MC33978_REG_CWET_SP, MC33978_REG_AMUX_CTRL),
	regmap_reg_range(MC33978_REG_IRQ, MC33978_REG_RESET),
};

static const struct regmap_access_table mc33978_writable_table = {
	.yes_ranges = mc33978_writable_range,
	.n_yes_ranges = ARRAY_SIZE(mc33978_writable_range),
};

static const struct regmap_config mc33978_regmap_config = {
	.name = "mc33978",
	.reg_bits = 8,
	.val_bits = 24,
	.reg_stride = 2,
	.write_flag_mask = MC33978_CMD_BYTE_WRITE,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.use_single_read = true,
	.use_single_write = true,
	.volatile_table = &mc33978_volatile_table,
	.precious_table = &mc33978_precious_table,
	.rd_table = &mc33978_readable_table,
	.wr_table = &mc33978_writable_table,
	.cache_type = REGCACHE_MAPLE,
	.max_register = MC33978_REG_RESET,
};

/* Verify SPI communication via CHECK register signature (0x123456) */
static int mc33978_check_device(struct mc33978_core_data *cdata)
{
	struct device *dev = &cdata->spi->dev;
	unsigned int check;
	int ret;

	ret = regmap_read(cdata->map, MC33978_REG_CHECK, &check);
	if (ret)
		return ret;

	if (check != MC33978_CHECK_SIGNATURE)
		return dev_err_probe(dev, -ENODEV,
				     "SPI check failed. Expected: 0x%06x, got: 0x%06x\n",
				     MC33978_CHECK_SIGNATURE, check);

	return 0;
}

/**
 * mc33978_core_init() - Initialize core functionality
 * @dev: Parent device (for devm allocation)
 * @spi: SPI device (already powered)
 * @domain_out: Returns created IRQ domain
 *
 * Called by MFD driver to initialize regmap, IRQ domain, and event handling.
 * All resources are devm-managed and tied to @dev lifecycle.
 *
 * Return: 0 on success, negative error code on failure
 */
int mc33978_core_init(struct device *dev, struct spi_device *spi,
		      struct irq_domain **domain_out)
{
	struct fwnode_handle *fwnode;
	struct mc33978_core_data *cdata;
	int ret;

	/* Initialize output parameter to NULL for error paths */
	*domain_out = NULL;

	fwnode = dev_fwnode(dev);
	if (!fwnode)
		return dev_err_probe(dev, -ENODEV, "missing firmware node\n");

	cdata = devm_kzalloc(dev, sizeof(*cdata), GFP_KERNEL);
	if (!cdata)
		return -ENOMEM;

	cdata->spi = spi;

	mutex_init(&cdata->event_lock);
	mutex_init(&cdata->irq_lock);
	spin_lock_init(&cdata->state_lock);
	raw_spin_lock_init(&cdata->irq_state_lock);

	INIT_WORK(&cdata->event_work, mc33978_event_work);

	atomic_set(&cdata->harvested_flags, 0);

	mc33978_prepare_messages(cdata);

	ret = mc33978_irq_init(cdata, fwnode);
	if (ret)
		return ret;

	cdata->map = devm_regmap_init(dev, &mc33978_regmap_bus, cdata,
				      &mc33978_regmap_config);
	if (IS_ERR(cdata->map))
		return dev_err_probe(dev, PTR_ERR(cdata->map),
				     "failed to initialize regmap\n");

	/*
	 * Register teardown action to cancel event_work before resource cleanup.
	 * Critical devm LIFO ordering (registered AFTER regmap/IRQ init above):
	 *
	 * Teardown sequence:
	 * 1. MFD: devm_mfd_add_devices() cleanup
	 *    - Child devices removed, child IRQ handlers freed
	 * 2. THIS MODULE: devm_request_threaded_irq() cleanup (below)
	 *    - Parent IRQ handler freed, stops new event triggers
	 * 3. THIS ACTION: mc33978_teardown() via devm_add_action
	 *    - Calls cancel_work_sync(&cdata->event_work)
	 * 4. THIS MODULE: devm_regmap_init() cleanup (above)
	 *    - Regmap destroyed
	 * 5. THIS MODULE: devm_add_action(mc33978_irq_domain_remove) in
	 *    mc33978_irq_init()
	 *    - IRQ domain removed
	 *
	 * event_work (via mc33978_handle_events) accesses both cdata->map and
	 * cdata->domain. Registering this action AFTER their creation but BEFORE
	 * devm_request_threaded_irq() ensures LIFO cleanup: work is canceled in
	 * step 3, guaranteeing no worker is running when resources are destroyed
	 * in steps 4-5.
	 *
	 * Additionally, mc33978_teardown() sets tearing_down flag to prevent
	 * debugfs regmap operations from rescheduling work after cancellation.
	 */
	ret = devm_add_action_or_reset(dev, mc33978_teardown, cdata);
	if (ret)
		return ret;

	ret = mc33978_check_device(cdata);
	if (ret)
		return ret;

	/*
	 * POR state (§9.10.27): After power-on reset, both FAULT_STAT and
	 * INT_flg are set high. These will be harvested during initialization
	 * but discarded (no child IRQ handlers registered yet). Disable
	 * interrupts before priming to prevent storms during state setup.
	 */
	ret = regmap_write(cdata->map, MC33978_REG_IE_SP, 0);
	if (ret)
		return ret;

	ret = regmap_write(cdata->map, MC33978_REG_IE_SG, 0);
	if (ret)
		return ret;

	/*
	 * Prime the cached pin state under lock to prevent spurious events.
	 * Work scheduling is disabled (irq_ready=false) to prevent the work
	 * feedback loop that would occur during init: regmap_read() harvests
	 * status -> schedules work -> work does regmap_read() -> schedules more
	 * work -> infinite loop on single-core systems where work monopolizes
	 * CPU before init can complete.
	 */
	scoped_guard(mutex, &cdata->event_lock) {
		ret = regmap_read(cdata->map, MC33978_REG_READ_IN,
				  &cdata->cached_pin_state);
	}
	if (ret)
		return dev_err_probe(dev, ret, "failed to read initial pin state\n");

	if (spi->irq <= 0)
		return dev_err_probe(dev, -EINVAL,
				     "no valid IRQ provided for INT_B pin\n");

	/*
	 * Not using IRQF_SHARED: threaded handler with IRQF_ONESHOT may hold
	 * line masked too long on slow SPI, making shared operation impractical.
	 */
	ret = devm_request_threaded_irq(dev, spi->irq,
					NULL,
					mc33978_irq_thread,
					IRQF_ONESHOT,
					dev_name(dev), cdata);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	/*
	 * Enable work scheduling now that IRQ handler is registered.
	 * This prevents the work feedback loop during initialization while
	 * allowing proper event processing after setup completes.
	 */
	scoped_guard(spinlock_irqsave, &cdata->state_lock)
		cdata->irq_ready = true;

	/* Return IRQ domain for MFD to use */
	*domain_out = cdata->domain;

	return 0;
}
EXPORT_SYMBOL_GPL(mc33978_core_init);

MODULE_AUTHOR("David Jander <david@protonic.nl>");
MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
MODULE_DESCRIPTION("NXP MC33978/MC34978 Core Module");
MODULE_LICENSE("GPL");
