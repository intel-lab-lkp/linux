// SPDX-License-Identifier: GPL-2.0
/*
 * ACPI Operation Region helper for GPIO API
 *
 * Copyright (C) 2026, Intel Corporation
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>

#include "gpiolib.h"
#include "gpiolib-acpi.h"

static acpi_status
acpi_gpio_adr_space_handler(u32 function, acpi_physical_address address,
			    u32 bits, u64 *value, void *handler_context,
			    void *region_context)
{
	struct acpi_gpio_chip *achip = region_context;
	struct gpio_chip *chip = achip->chip;
	struct acpi_resource_gpio *agpio;
	struct acpi_resource *ares;
	u16 pin_index = address;
	acpi_status status;
	int length;
	int i;

	status = acpi_buffer_to_resource(achip->conn_info.connection,
					 achip->conn_info.length, &ares);
	if (ACPI_FAILURE(status))
		return status;

	if (WARN_ON(ares->type != ACPI_RESOURCE_TYPE_GPIO)) {
		ACPI_FREE(ares);
		return AE_BAD_PARAMETER;
	}

	agpio = &ares->data.gpio;

	if (WARN_ON(agpio->io_restriction == ACPI_IO_RESTRICT_INPUT &&
	    function == ACPI_WRITE)) {
		ACPI_FREE(ares);
		return AE_BAD_PARAMETER;
	}

	if (pin_index >= agpio->pin_table_length) {
		ACPI_FREE(ares);
		return AE_BAD_PARAMETER;
	}

	length = min(agpio->pin_table_length, pin_index + bits);
	for (i = pin_index; i < length; ++i) {
		unsigned int pin = agpio->pin_table[i];
		struct acpi_gpio_connection *conn;
		struct gpio_desc *desc;
		u16 word, shift;
		bool found;

		mutex_lock(&achip->conn_lock);

		found = false;
		list_for_each_entry(conn, &achip->conns, node) {
			if (conn->pin == pin) {
				found = true;
				desc = conn->desc;
				break;
			}
		}

		/*
		 * The same GPIO can be shared between operation region and
		 * event but only if the access here is ACPI_READ. In that
		 * case we "borrow" the event GPIO instead.
		 */
		if (!found && agpio->shareable == ACPI_SHARED &&
		     function == ACPI_READ) {
			struct acpi_gpio_event *event;

			list_for_each_entry(event, &achip->events, node) {
				if (event->pin == pin) {
					desc = event->desc;
					found = true;
					break;
				}
			}
		}

		if (!found) {
			desc = acpi_request_own_gpiod(chip, agpio, i, "ACPI:OpRegion");
			if (IS_ERR(desc)) {
				mutex_unlock(&achip->conn_lock);
				status = AE_ERROR;
				goto out;
			}

			conn = kzalloc_obj(*conn);
			if (!conn) {
				gpiochip_free_own_desc(desc);
				mutex_unlock(&achip->conn_lock);
				status = AE_NO_MEMORY;
				goto out;
			}

			conn->pin = pin;
			conn->desc = desc;
			list_add_tail(&conn->node, &achip->conns);
		}

		mutex_unlock(&achip->conn_lock);

		/*
		 * For the cases when OperationRegion() consists of more than
		 * 64 bits calculate the word and bit shift to use that one to
		 * access the value.
		 */
		word = i / 64;
		shift = i % 64;

		if (function == ACPI_WRITE) {
			gpiod_set_raw_value_cansleep(desc, value[word] & BIT_ULL(shift));
		} else {
			if (gpiod_get_raw_value_cansleep(desc))
				value[word] |= BIT_ULL(shift);
			else
				value[word] &= ~BIT_ULL(shift);
		}
	}

out:
	ACPI_FREE(ares);
	return status;
}

void acpi_gpiochip_request_regions(struct acpi_gpio_chip *achip)
{
	struct gpio_chip *chip = achip->chip;
	acpi_handle handle = ACPI_HANDLE(chip->parent);
	acpi_status status;

	INIT_LIST_HEAD(&achip->conns);
	mutex_init(&achip->conn_lock);

	status = acpi_install_address_space_handler(handle, ACPI_ADR_SPACE_GPIO,
						    acpi_gpio_adr_space_handler,
						    NULL, achip);
	if (ACPI_FAILURE(status))
		dev_err(chip->parent,
			"Failed to install GPIO OpRegion handler\n");
}

void acpi_gpiochip_free_regions(struct acpi_gpio_chip *achip)
{
	struct gpio_chip *chip = achip->chip;
	acpi_handle handle = ACPI_HANDLE(chip->parent);
	struct acpi_gpio_connection *conn, *tmp;
	acpi_status status;

	status = acpi_remove_address_space_handler(handle, ACPI_ADR_SPACE_GPIO,
						   acpi_gpio_adr_space_handler);
	if (ACPI_FAILURE(status)) {
		dev_err(chip->parent,
			"Failed to remove GPIO OpRegion handler\n");
	}

	list_for_each_entry_safe_reverse(conn, tmp, &achip->conns, node) {
		gpiochip_free_own_desc(conn->desc);
		list_del(&conn->node);
		kfree(conn);
	}
}
