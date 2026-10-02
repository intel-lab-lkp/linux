// SPDX-License-Identifier: GPL-2.0
/* Experimental M3000 Open Firmware pseudo-console backend. */
#include <linux/console.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <asm/oplib.h>
#include <asm/spitfire.h>
#include "hvc_console.h"

#define M3000_WRITE_ATTEMPTS 8

static int m3000_stdin;
static bool m3000_ready;
/* Keep firmware buffers within the locked kernel image mappings. */
static u8 m3000_buffer[256];
static DEFINE_RAW_SPINLOCK(m3000_buffer_lock);
static struct timer_list m3000_poll_timer;
static bool m3000_poll_active;

/*
 * HVC's idle backoff can exceed the firmware input FIFO's capacity.
 * Wake its worker each tick while open; keep PROM calls out of the timer.
 */
static void m3000_poll_tick(struct timer_list *timer)
{
	if (!READ_ONCE(m3000_poll_active))
		return;
	hvc_kick();
	mod_timer(&m3000_poll_timer, jiffies + 1);
}

static int m3000_open(struct hvc_struct *hp, int data)
{
	WRITE_ONCE(m3000_poll_active, true);
	mod_timer(&m3000_poll_timer, jiffies + 1);
	return 0;
}

static void m3000_close(struct hvc_struct *hp, int data)
{
	WRITE_ONCE(m3000_poll_active, false);
	timer_delete_sync(&m3000_poll_timer);
}

static ssize_t m3000_io(u8 *in, const u8 *out, size_t count)
{
	unsigned long args[7], flags;
	bool input = in != NULL;
	int ret;

	if (!count)
		return 0;
	count = min_t(size_t, count, input ? 1 : sizeof(m3000_buffer));
	/* Serialize the bounce buffer; p1275 separately serializes firmware. */
	raw_spin_lock_irqsave(&m3000_buffer_lock, flags);
	if (!input)
		memcpy(m3000_buffer, out, count);
	args[0] = (unsigned long)(input ? "read" : "write");
	args[1] = 3;
	args[2] = 1;
	args[3] = (unsigned int)(input ? m3000_stdin : prom_stdout);
	args[4] = (unsigned long)m3000_buffer;
	args[5] = count;
	args[6] = (unsigned long)-1;
	p1275_cmd_direct(args);
	ret = (int)args[6];
	if (ret > 0 && ret <= count && input)
		memcpy(in, m3000_buffer, ret);
	raw_spin_unlock_irqrestore(&m3000_buffer_lock, flags);
	if (ret == -2 || ret == 0)
		return input ? 0 : -EAGAIN;
	if (ret < 0 || ret > count)
		return -EIO;
	return ret;
}

static ssize_t m3000_get(u32 termno, u8 *buf, size_t count)
{
	return m3000_io(buf, NULL, count);
}

static ssize_t m3000_put(u32 termno, const u8 *buf, size_t count)
{
	ssize_t ret;
	int attempt;

	/*
	 * HVC retries -EAGAIN forever, so drop this chunk without logging when
	 * the no-progress budget expires. This cannot bound a stalled firmware
	 * call or lock acquisition.
	 */
	for (attempt = 0; attempt < M3000_WRITE_ATTEMPTS; attempt++) {
		ret = m3000_io(NULL, buf, count);
		if (ret != -EAGAIN)
			return ret;
		cpu_relax();
	}
	return -EIO;
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
	phandle node;
	int ret;

	if (tlb_type != sparc64_vii)
		return -ENODEV;
	if (!m3000_property_matches(prom_finddevice("/"), "model", "IKKAKU"))
		return -ENODEV;
	m3000_stdin = prom_getint(prom_chosen_node, "stdin");
	if (!m3000_stdin || m3000_stdin == -1 || !prom_stdout || prom_stdout == -1)
		return -ENODEV;
	node = prom_inst2pkg(prom_stdout);
	if (!m3000_property_matches(node, "name", "pseudo-console"))
		return -ENODEV;
	node = prom_inst2pkg(m3000_stdin);
	if (!m3000_property_matches(node, "name", "pseudo-console"))
		return -ENODEV;
	ret = hvc_instantiate(0, 0, &m3000_ops);
	if (ret < 0)
		return ret;
	m3000_ready = true;
	return 0;
}
console_initcall(m3000_console_init);

static int __init m3000_tty_init(void)
{
	struct hvc_struct *hp;

	if (!m3000_ready)
		return -ENODEV;
	timer_setup(&m3000_poll_timer, m3000_poll_tick, 0);
	hp = hvc_alloc(0, 0, &m3000_ops, sizeof(m3000_buffer));
	if (IS_ERR(hp))
		return PTR_ERR(hp);
	hp->ws.ws_row = 24;
	hp->ws.ws_col = 80;
	pr_info("M3000: firmware-backed hvc0 tty ready (serialized, polled input)\n");
	return 0;
}
device_initcall(m3000_tty_init);
