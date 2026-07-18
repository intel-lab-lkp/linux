// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IBM ASM Service Processor Device Driver
 *
 * Copyright (C) IBM Corporation, 2004
 *
 * Author: Max Asböck <amax@us.ibm.com>
 */

#include "ibmasm.h"
#include "lowlevel.h"
#include "dot_command.h"
#include "remote.h"

static struct i2o_header header = I2O_HEADER_TEMPLATE;

struct i2o_message *get_i2o_message(void __iomem *base_address,
				    resource_size_t mapped_size,
				    u32 mfa, size_t msg_size)
{
	u32 offset = GET_MFA_ADDR(mfa);

	/* Prevent read/write beyond the ioremap region and avoid integer underflow/overflow */
	if (unlikely(offset > mapped_size || msg_size > mapped_size - offset))
		return NULL;

	return (struct i2o_message *)(offset + base_address);
}

int ibmasm_send_i2o_message(struct service_processor *sp)
{
	u32 mfa;
	size_t command_size;
	struct i2o_message *message;
	struct command *command = sp->current_command;

	command_size = get_dot_command_size(command->buffer);
	if (command_size > command->buffer_size)
		return 1;
	if (command_size > I2O_COMMAND_SIZE)
		command_size = I2O_COMMAND_SIZE;

	mfa = get_mfa_inbound(sp->base_address);
	if (!mfa)
		return 1;

	header.message_size = outgoing_message_size((unsigned int)command_size);
	message = get_i2o_message(sp->base_address, sp->mapped_size, mfa,
				  sizeof(struct i2o_header) + command_size);
	if (!message)
		return 1;

	memcpy_toio(&message->header, &header, sizeof(struct i2o_header));
	memcpy_toio(&message->data, command->buffer, command_size);

	set_mfa_inbound(sp->base_address, mfa);

	return 0;
}

irqreturn_t ibmasm_interrupt_handler(int irq, void * dev_id)
{
	u32	mfa;
	struct service_processor *sp = (struct service_processor *)dev_id;
	void __iomem *base_address = sp->base_address;
	char tsbuf[32];

	if (!sp_interrupt_pending(base_address))
		return IRQ_NONE;

	dbg("respond to interrupt at %s\n", get_timestamp(tsbuf));

	if (mouse_interrupt_pending(sp)) {
		ibmasm_handle_mouse_interrupt(sp);
		clear_mouse_interrupt(sp);
	}

	mfa = get_mfa_outbound(base_address);
	if (valid_mfa(mfa)) {
		struct i2o_message *msg = get_i2o_message(base_address,
							  sp->mapped_size, mfa,
							  sizeof(struct i2o_header));
		if (msg) {
			u32 data_size = incoming_data_size(msg);
			u32 offset = GET_MFA_ADDR(mfa);

			/*
			 * Secondary check: total message size must not exceed mapped
			 * space, and must be at least as large as the header itself.
			 */
			if (unlikely(data_size < sizeof(struct i2o_header) ||
				     data_size > sp->mapped_size - offset)) {
				dbg("received mfa payload out of bounds or invalid size\n");
			} else {
				/* Pass only the payload size to prevent +12 byte OOB read */
				ibmasm_receive_message(sp, &msg->data,
						       data_size - sizeof(struct i2o_header));
			}
		} else {
			dbg("received mfa header out of bounds\n");
		}
	} else
		dbg("didn't get a valid MFA\n");

	set_mfa_outbound(base_address, mfa);
	dbg("finished interrupt at   %s\n", get_timestamp(tsbuf));

	return IRQ_HANDLED;
}
