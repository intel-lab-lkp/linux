// SPDX-License-Identifier: GPL-2.0
/*
 * ACPI Event and Interrupt helper for GPIO API
 *
 * Copyright (C) 2026, Intel Corporation
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "gpiolib.h"
#include "gpiolib-acpi.h"

static irqreturn_t acpi_gpio_irq_handler(int irq, void *data)
{
	struct acpi_gpio_event *event = data;

	acpi_evaluate_object(event->handle, NULL, NULL, NULL);

	return IRQ_HANDLED;
}

static irqreturn_t acpi_gpio_irq_handler_evt(int irq, void *data)
{
	struct acpi_gpio_event *event = data;

	acpi_execute_simple_method(event->handle, NULL, event->pin);

	return IRQ_HANDLED;
}

static void acpi_gpiochip_request_irq(struct acpi_gpio_chip *acpi_gpio,
				      struct acpi_gpio_event *event)
{
	struct device *parent = acpi_gpio->chip->parent;
	int ret, value;

	ret = request_threaded_irq(event->irq, NULL, event->handler,
				   event->irqflags | IRQF_ONESHOT, "ACPI:Event", event);
	if (ret) {
		dev_err(parent, "Failed to setup interrupt handler for %d\n", event->irq);
		return;
	}

	if (event->irq_is_wake)
		enable_irq_wake(event->irq);

	event->irq_requested = true;

	/*
	 * Make sure we trigger the initial state of ActiveBoth IRQs.
	 *
	 * According to the Microsoft GPIO documentation, triggering GPIO
	 * interrupts marked as ActiveBoth during initialization is correct
	 * as long as the associated GPIO line is already "asserted"
	 * (logic level low). We should not trigger edge-based GPIO
	 * interrupts not marked as ActiveBoth.
	 *
	 * See: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/general-purpose-i-o--gpio-
	 * Section: "GPIO controllers and ActiveBoth interrupts"
	 */
	if (acpi_gpio_need_run_edge_events_on_boot() &&
	    ((event->irqflags & (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)) ==
	     (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING))) {
		value = gpiod_get_raw_value_cansleep(event->desc);
		if (value == 0)
			event->handler(event->irq, event);
	}
}

static void acpi_gpiochip_request_irqs(struct acpi_gpio_chip *acpi_gpio)
{
	struct acpi_gpio_event *event;

	list_for_each_entry(event, &acpi_gpio->events, node)
		acpi_gpiochip_request_irq(acpi_gpio, event);
}

bool acpi_gpio_irq_is_wake(struct device *parent,
			   const struct acpi_resource_gpio *agpio)
{
	unsigned int pin;

	if (agpio->pin_table_length == 0)
		return false;

	pin = agpio->pin_table[0];

	if (agpio->wake_capable != ACPI_WAKE_CAPABLE)
		return false;

	if (acpi_gpio_in_ignore_list(ACPI_GPIO_IGNORE_WAKE, dev_name(parent), pin)) {
		dev_info(parent, "Ignoring wakeup on pin %u\n", pin);
		return false;
	}

	return true;
}

/* Always returns AE_OK so that we keep looping over the resources */
static acpi_status acpi_gpiochip_alloc_event(struct acpi_resource *ares,
					     void *context)
{
	struct acpi_gpio_chip *acpi_gpio = context;
	struct gpio_chip *chip = acpi_gpio->chip;
	struct acpi_resource_gpio *agpio;
	acpi_handle handle, evt_handle;
	struct acpi_gpio_event *event;
	irq_handler_t handler = NULL;
	struct gpio_desc *desc;
	unsigned int pin;
	int ret, irq;

	if (!acpi_gpio_get_irq_resource(ares, &agpio))
		return AE_OK;

	if (agpio->pin_table_length == 0)
		return AE_OK;

	handle = ACPI_HANDLE(chip->parent);
	pin = agpio->pin_table[0];

	if (pin <= 255) {
		char ev_name[8];

		snprintf(ev_name, sizeof(ev_name), "_%c%02X",
			 agpio->triggering == ACPI_EDGE_SENSITIVE ? 'E' : 'L',
			 pin);
		if (ACPI_SUCCESS(acpi_get_handle(handle, ev_name, &evt_handle)))
			handler = acpi_gpio_irq_handler;
	}
	if (!handler) {
		if (ACPI_SUCCESS(acpi_get_handle(handle, "_EVT", &evt_handle)))
			handler = acpi_gpio_irq_handler_evt;
	}
	if (!handler)
		return AE_OK;

	if (acpi_gpio_in_ignore_list(ACPI_GPIO_IGNORE_INTERRUPT, dev_name(chip->parent), pin)) {
		dev_info(chip->parent, "Ignoring interrupt on pin %u\n", pin);
		return AE_OK;
	}

	desc = acpi_request_own_gpiod(chip, agpio, 0, "ACPI:Event");
	if (IS_ERR(desc)) {
		dev_err(chip->parent,
			"Failed to request GPIO for pin 0x%04X, err %pe\n",
			pin, desc);
		return AE_OK;
	}

	ret = gpiochip_lock_as_irq(chip, pin);
	if (ret) {
		dev_err(chip->parent,
			"Failed to lock GPIO pin 0x%04X as interrupt, err %d\n",
			pin, ret);
		goto fail_free_desc;
	}

	irq = gpiod_to_irq(desc);
	if (irq < 0) {
		dev_err(chip->parent,
			"Failed to translate GPIO pin 0x%04X to IRQ, err %d\n",
			pin, irq);
		goto fail_unlock_irq;
	}

	event = kzalloc_obj(*event);
	if (!event)
		goto fail_unlock_irq;

	event->irqflags = IRQF_ONESHOT;
	if (agpio->triggering == ACPI_LEVEL_SENSITIVE) {
		if (agpio->polarity == ACPI_ACTIVE_HIGH)
			event->irqflags |= IRQF_TRIGGER_HIGH;
		else
			event->irqflags |= IRQF_TRIGGER_LOW;
	} else {
		switch (agpio->polarity) {
		case ACPI_ACTIVE_HIGH:
			event->irqflags |= IRQF_TRIGGER_RISING;
			break;
		case ACPI_ACTIVE_LOW:
			event->irqflags |= IRQF_TRIGGER_FALLING;
			break;
		default:
			event->irqflags |= IRQF_TRIGGER_RISING |
					   IRQF_TRIGGER_FALLING;
			break;
		}
	}

	event->handle = evt_handle;
	event->handler = handler;
	event->irq = irq;
	event->irq_is_wake = acpi_gpio_irq_is_wake(chip->parent, agpio);
	event->pin = pin;
	event->desc = desc;

	list_add_tail(&event->node, &acpi_gpio->events);

	return AE_OK;

fail_unlock_irq:
	gpiochip_unlock_as_irq(chip, pin);
fail_free_desc:
	gpiochip_free_own_desc(desc);

	return AE_OK;
}

/**
 * acpi_gpiochip_request_interrupts() - Register isr for gpio chip ACPI events
 * @chip:      GPIO chip
 *
 * ACPI5 platforms can use GPIO signaled ACPI events. These GPIO interrupts are
 * handled by ACPI event methods which need to be called from the GPIO
 * chip's interrupt handler. acpi_gpiochip_request_interrupts() finds out which
 * GPIO pins have ACPI event methods and assigns interrupt handlers that calls
 * the ACPI event methods for those pins.
 */
void acpi_gpiochip_request_interrupts(struct gpio_chip *chip)
{
	struct acpi_gpio_chip *acpi_gpio;
	acpi_handle handle;
	acpi_status status;

	if (!chip->parent || !chip->to_irq)
		return;

	handle = ACPI_HANDLE(chip->parent);
	if (!handle)
		return;

	status = acpi_get_data(handle, acpi_gpio_chip_dh, (void **)&acpi_gpio);
	if (ACPI_FAILURE(status))
		return;

	if (acpi_quirk_skip_gpio_event_handlers())
		return;

	acpi_walk_resources(handle, METHOD_NAME__AEI,
			    acpi_gpiochip_alloc_event, acpi_gpio);

	if (acpi_gpio_add_to_deferred_list(&acpi_gpio->deferred_req_irqs_list_entry))
		return;

	acpi_gpiochip_request_irqs(acpi_gpio);
}
EXPORT_SYMBOL_GPL(acpi_gpiochip_request_interrupts);

/**
 * acpi_gpiochip_free_interrupts() - Free GPIO ACPI event interrupts.
 * @chip:      GPIO chip
 *
 * Free interrupts associated with GPIO ACPI event method for the given
 * GPIO chip.
 */
void acpi_gpiochip_free_interrupts(struct gpio_chip *chip)
{
	struct acpi_gpio_chip *acpi_gpio;
	struct acpi_gpio_event *event, *ep;
	acpi_handle handle;
	acpi_status status;

	if (!chip->parent || !chip->to_irq)
		return;

	handle = ACPI_HANDLE(chip->parent);
	if (!handle)
		return;

	status = acpi_get_data(handle, acpi_gpio_chip_dh, (void **)&acpi_gpio);
	if (ACPI_FAILURE(status))
		return;

	acpi_gpio_remove_from_deferred_list(&acpi_gpio->deferred_req_irqs_list_entry);

	list_for_each_entry_safe_reverse(event, ep, &acpi_gpio->events, node) {
		if (event->irq_requested) {
			if (event->irq_is_wake)
				disable_irq_wake(event->irq);

			free_irq(event->irq, event);
		}

		gpiochip_unlock_as_irq(chip, event->pin);
		gpiochip_free_own_desc(event->desc);
		list_del(&event->node);
		kfree(event);
	}
}
EXPORT_SYMBOL_GPL(acpi_gpiochip_free_interrupts);

void __init acpi_gpio_process_deferred_list(struct list_head *list)
{
	struct acpi_gpio_chip *acpi_gpio, *tmp;

	list_for_each_entry_safe(acpi_gpio, tmp, list, deferred_req_irqs_list_entry)
		acpi_gpiochip_request_irqs(acpi_gpio);
}
