// SPDX-License-Identifier: GPL-2.0
/*
 * M3000 Open Firmware pseudo-console backend.
 * Copyright (C) 2026 Magnus Lindholm
 */
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

struct m3000_console {
	int stdin;
	bool ready;
	u8 buffer[256];
	raw_spinlock_t buffer_lock;
	struct timer_list poll_timer;
	struct mutex poll_mutex; /* Serialize open against synchronous close. */
	spinlock_t poll_lock; /* Protect active state and timer rearming. */
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
	unsigned long flags;

	spin_lock_irqsave(&con->poll_lock, flags);
	if (con->poll_active) {
		hvc_kick();
		mod_timer(&con->poll_timer, jiffies + 1);
	}
	spin_unlock_irqrestore(&con->poll_lock, flags);
}

static int m3000_open(struct hvc_struct *hp, int data)
{
	struct m3000_console *con = m3000_console(hp->vtermno);
	unsigned long flags;

	if (!con)
		return -ENODEV;
	mutex_lock(&con->poll_mutex);
	spin_lock_irqsave(&con->poll_lock, flags);
	con->poll_active = true;
	mod_timer(&con->poll_timer, jiffies + 1);
	spin_unlock_irqrestore(&con->poll_lock, flags);
	mutex_unlock(&con->poll_mutex);
	return 0;
}

static void m3000_close(struct hvc_struct *hp, int data)
{
	struct m3000_console *con = m3000_console(hp->vtermno);
	unsigned long flags;

	if (!con)
		return;
	mutex_lock(&con->poll_mutex);
	spin_lock_irqsave(&con->poll_lock, flags);
	con->poll_active = false;
	spin_unlock_irqrestore(&con->poll_lock, flags);
	/* Block reopen until the callback has finished and cannot rearm. */
	timer_delete_sync(&con->poll_timer);
	mutex_unlock(&con->poll_mutex);
}

static ssize_t m3000_io(struct m3000_console *con, u8 *in,
			const u8 *out, size_t count)
{
	unsigned long args[7], flags;
	bool input = in;
	int ret;

	if (!count)
		return 0;
	count = min(count, input ? 1 : sizeof(con->buffer));
	/* Console writes can run with interrupts disabled; this lock cannot sleep. */
	raw_spin_lock_irqsave(&con->buffer_lock, flags);
	if (!input)
		memcpy(con->buffer, out, count);
	args[0] = (unsigned long)(input ? "read" : "write");
	args[1] = 3;
	args[2] = 1;
	args[3] = (unsigned int)(input ? con->stdin : prom_stdout);
	args[4] = (unsigned long)con->buffer;
	args[5] = count;
	args[6] = (unsigned long)-1;
	p1275_cmd_direct(args);
	ret = (int)args[6];
	if (ret > 0 && ret <= count && input)
		memcpy(in, con->buffer, ret);
	raw_spin_unlock_irqrestore(&con->buffer_lock, flags);
	if (ret == -2 || ret == 0)
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
