// SPDX-License-Identifier: GPL-2.0
/*
 * M3000 Open Firmware pseudo-console backend.
 * Copyright (C) 2026 Magnus Lindholm
 */
#include <linux/cleanup.h>
#include <linux/console.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <asm/oplib.h>
#include <asm/spitfire.h>
#include "hvc_console.h"

#define M3000_PROM_IO_ARGS	3
#define M3000_PROM_IO_RETURNS	1
#define M3000_PROM_ERROR		(-1)
#define M3000_PROM_NO_DATA	(-2)

struct m3000_console {
	struct timer_list poll_timer;
	struct mutex poll_mutex; /* Serialize open against synchronous close. */
	raw_spinlock_t buffer_lock;
	spinlock_t poll_lock; /* Protect active state and timer rearming. */
	int stdin;
	u8 buffer[256];
	bool ready;
	bool poll_active;
};

/* Keep the firmware buffer within the locked kernel image mappings. */
static struct m3000_console m3000_consoles[] = {
	{
		.buffer_lock = __RAW_SPIN_LOCK_UNLOCKED(m3000_consoles[0].buffer_lock),
		.poll_mutex = __MUTEX_INITIALIZER(m3000_consoles[0].poll_mutex),
		.poll_lock = __SPIN_LOCK_UNLOCKED(m3000_consoles[0].poll_lock),
	},
};

static struct m3000_console *m3000_console(u32 termno)
{
	if (termno >= ARRAY_SIZE(m3000_consoles))
		return NULL;
	return &m3000_consoles[termno];
}

/*
 * HVC's idle backoff can exceed the firmware input FIFO's capacity.
 * Wake its worker each tick while open; keep PROM calls out of the timer.
 */
static void m3000_poll_tick(struct timer_list *timer)
{
	struct m3000_console *con = timer_container_of(con, timer, poll_timer);

	guard(spinlock_irqsave)(&con->poll_lock);
	if (con->poll_active) {
		hvc_kick();
		mod_timer(&con->poll_timer, jiffies + 1);
	}
}

static int m3000_open(struct hvc_struct *hp, int data)
{
	struct m3000_console *con = m3000_console(hp->vtermno);

	if (!con)
		return -ENODEV;
	guard(mutex)(&con->poll_mutex);
	guard(spinlock_irqsave)(&con->poll_lock);
	con->poll_active = true;
	mod_timer(&con->poll_timer, jiffies + 1);
	return 0;
}

static void m3000_close(struct hvc_struct *hp, int data)
{
	struct m3000_console *con = m3000_console(hp->vtermno);

	if (!con)
		return;
	guard(mutex)(&con->poll_mutex);
	scoped_guard(spinlock_irqsave, &con->poll_lock)
		con->poll_active = false;
	/* Block reopen until the callback has finished and cannot rearm. */
	timer_delete_sync(&con->poll_timer);
}

static ssize_t m3000_io(struct m3000_console *con, u8 *in,
			const u8 *out, size_t count)
{
	unsigned long args[3 + M3000_PROM_IO_ARGS + M3000_PROM_IO_RETURNS];
	int ret;

	if (!count)
		return 0;
	count = min(count, in ? 1 : sizeof(con->buffer));
	/* Console writes can run with interrupts disabled; this lock cannot sleep. */
	scoped_guard(raw_spinlock_irqsave, &con->buffer_lock) {
		if (!in)
			memcpy(con->buffer, out, count);
		args[0] = (unsigned long)(in ? "read" : "write");
		args[1] = M3000_PROM_IO_ARGS;
		args[2] = M3000_PROM_IO_RETURNS;
		args[3] = (unsigned int)(in ? con->stdin : prom_stdout);
		args[4] = (unsigned long)con->buffer;
		args[5] = count;
		args[6] = (unsigned long)M3000_PROM_ERROR;
		p1275_cmd_direct(args);
		ret = (int)args[6];
		if (ret > 0 && ret <= count && in)
			memcpy(in, con->buffer, ret);
	}
	if (ret == M3000_PROM_NO_DATA || ret == 0)
		return 0;
	if (ret < 0 || ret > count)
		return -EIO;
	return ret;
}

static ssize_t m3000_get(u32 termno, u8 *buf, size_t count)
{
	struct m3000_console *con = m3000_console(termno);

	return con ? m3000_io(con, buf, NULL, count) : -ENODEV;
}

static ssize_t m3000_put(u32 termno, const u8 *buf, size_t count)
{
	struct m3000_console *con = m3000_console(termno);

	/* HVC retries zero for TTY writes but drops stalled console output. */
	return con ? m3000_io(con, NULL, buf, count) : -ENODEV;
}

static const struct hv_ops m3000_ops = {
	.get_chars = m3000_get,
	.put_chars = m3000_put,
	.notifier_add = m3000_open,
	.notifier_del = m3000_close,
	.notifier_hangup = m3000_close,
};

static bool __init m3000_property_matches(phandle node, const char *prop,
					  const char *expected)
{
	char value[64];
	int len;

	len = prom_getproperty(node, prop, value, sizeof(value) - 1);
	if (len <= 0)
		return false;
	value[len] = '\0';
	return !strcmp(value, expected);
}

static int __init m3000_console_init(void)
{
	struct m3000_console *con = &m3000_consoles[0];
	phandle node;
	int ret;

	if (tlb_type != sparc64_vii)
		return -ENODEV;
	if (!m3000_property_matches(prom_finddevice("/"), "model", "IKKAKU"))
		return -ENODEV;
	con->stdin = prom_getint(prom_chosen_node, "stdin");
	if (!con->stdin || con->stdin == -1 || !prom_stdout || prom_stdout == -1)
		return -ENODEV;
	node = prom_inst2pkg(prom_stdout);
	if (!m3000_property_matches(node, "name", "pseudo-console"))
		return -ENODEV;
	node = prom_inst2pkg(con->stdin);
	if (!m3000_property_matches(node, "name", "pseudo-console"))
		return -ENODEV;
	ret = hvc_instantiate(0, 0, &m3000_ops);
	if (ret < 0)
		return ret;
	con->ready = true;
	return 0;
}
console_initcall(m3000_console_init);

static int __init m3000_tty_init(void)
{
	struct m3000_console *con = &m3000_consoles[0];
	struct hvc_struct *hp;

	if (!con->ready)
		return -ENODEV;
	timer_setup(&con->poll_timer, m3000_poll_tick, 0);
	hp = hvc_alloc(0, 0, &m3000_ops, sizeof(con->buffer));
	if (IS_ERR(hp))
		return PTR_ERR(hp);
	hp->ws.ws_row = 24;
	hp->ws.ws_col = 80;
	return 0;
}
device_initcall(m3000_tty_init);
