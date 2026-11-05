/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * phylib header
 */

#ifndef __PHYLIB_H
#define __PHYLIB_H

struct device_node;
struct phy_device;
struct mii_bus;

struct device_node *phy_package_get_node(struct phy_device *phydev);
void *phy_package_get_priv(struct phy_device *phydev);

void phy_package_lock(struct phy_device *phydev);
void phy_package_unlock(struct phy_device *phydev);
u8 phy_package_offset(struct phy_device *phydev);
int __phy_package_read(struct phy_device *phydev, unsigned int addr_offset,
		       u32 regnum);
int __phy_package_write(struct phy_device *phydev, unsigned int addr_offset,
			u32 regnum, u16 val);
int __phy_package_read_mmd(struct phy_device *phydev,
			   unsigned int addr_offset, int devad,
			   u32 regnum);
int __phy_package_write_mmd(struct phy_device *phydev,
			    unsigned int addr_offset, int devad,
			    u32 regnum, u16 val);
int __phy_package_modify_mmd(struct phy_device *phydev,
			     unsigned int addr_offset, int devad,
			     u32 regnum, u16 mask, u16 set);

/**
 * __phy_package_set_bits_mmd - Convenience function for setting bits in a register
 * on MMD relative to PHY package base addr
 * @phydev: the phy_device struct
 * @addr_offset: the offset to be added to PHY package base_addr
 * @devad: the MMD containing register to modify
 * @regnum: register number to modify
 * @val: bits to set
 */
static inline int __phy_package_set_bits_mmd(struct phy_device *phydev,
					     unsigned int addr_offset, int devad,
					     u32 regnum, u16 val)
{
	return __phy_package_modify_mmd(phydev, addr_offset, devad, regnum, 0, val);
}

/**
 * __phy_package_clear_bits_mmd - Convenience function for clearing bits in a register
 * on MMD relative to PHY package base addr
 * @phydev: the phy_device struct
 * @addr_offset: the offset to be added to PHY package base_addr
 * @devad: the MMD containing register to modify
 * @regnum: register number to modify
 * @val: bits to clear
 */
static inline int __phy_package_clear_bits_mmd(struct phy_device *phydev,
					       unsigned int addr_offset, int devad,
					       u32 regnum, u16 val)
{
	return __phy_package_modify_mmd(phydev, addr_offset, devad, regnum, val, 0);
}

/**
 * phy_read_mmd_poll_timeout - Periodically poll a PHY register relative to PHY package
 *                             until a condition is met or a timeout occurs
 *
 * @phydev: The phy_device struct
 * @addr_offset: The offset to be added to PHY package base_addr
 * @devaddr: The MMD to read from
 * @regnum: The register on the MMD to read
 * @val: Variable to read the register into
 * @cond: Break condition (usually involving @val)
 * @sleep_us: Maximum time to sleep between reads in us (0 tight-loops). Please
 *            read usleep_range() function description for details and
 *            limitations.
 * @timeout_us: Timeout in us, 0 means never timeout
 * @sleep_before_read: if it is true, sleep @sleep_us before read.
 *
 * Returns: 0 on success and -ETIMEDOUT upon a timeout. In either
 * case, the last read value at @args is stored in @val. Must not
 * be called from atomic context if sleep_us or timeout_us are used.
 */
#define __phy_package_read_mmd_poll_timeout(phydev, addr_offset, devaddr, regnum, val, \
					    cond, sleep_us, timeout_us, sleep_before_read) \
({ \
	int __ret, __val; \
	__ret = read_poll_timeout(__val = __phy_package_read_mmd, val, \
				  __val < 0 || (cond), \
				  sleep_us, timeout_us, sleep_before_read, \
				  phydev, addr_offset, devaddr, regnum); \
	if (__val < 0) \
		__ret = __val; \
	if (__ret) \
		phydev_err(phydev, "%s failed: %d\n", __func__, __ret); \
	__ret; \
})

bool phy_package_init_once(struct phy_device *phydev);
bool phy_package_probe_once(struct phy_device *phydev);
int phy_package_join(struct phy_device *phydev, int base_addr, size_t priv_size);
int of_phy_package_join(struct phy_device *phydev, size_t priv_size);
void phy_package_leave(struct phy_device *phydev);
int devm_phy_package_join(struct device *dev, struct phy_device *phydev,
			  int base_addr, size_t priv_size);
int devm_of_phy_package_join(struct device *dev, struct phy_device *phydev,
			     size_t priv_size);
int mmd_phy_read(struct mii_bus *bus, int phy_addr, bool is_c45,
		 int devad, u32 regnum);
int mmd_phy_write(struct mii_bus *bus, int phy_addr, bool is_c45,
		  int devad, u32 regnum, u16 val);

#endif /* __PHYLIB_H */
