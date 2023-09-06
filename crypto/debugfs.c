// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/delay.h>

static struct dentry *crypto_debugfs_dir;
static bool simd_force_async;
static u32 cryptd_delay;

bool crypto_simd_force_async(void)
{
	return simd_force_async;
}

void crypto_cryptd_delay(void)
{
	unsigned int d = READ_ONCE(cryptd_delay);

	if (d)
		msleep(d);
}

void __init crypto_init_debugfs(void)
{
	crypto_debugfs_dir = debugfs_create_dir("crypto", NULL);

#if IS_ENABLED(CONFIG_CRYPTO_SIMD)
	debugfs_create_bool("simd_force_async", 0644, crypto_debugfs_dir, &simd_force_async);
#endif

#if IS_ENABLED(CONFIG_CRYPTO_CRYPTD)
	debugfs_create_u32("cryptd_delay_ms", 0644, crypto_debugfs_dir, &cryptd_delay);
#endif
}

void __exit crypto_exit_debugfs(void)
{
	debugfs_remove_recursive(crypto_debugfs_dir);
}
