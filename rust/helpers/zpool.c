#include <linux/zpool.h>

void rust_helper_zpool_register_driver(struct zpool_driver *driver)
{
	zpool_register_driver(driver);
}
