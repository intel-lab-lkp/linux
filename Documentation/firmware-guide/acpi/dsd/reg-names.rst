.. SPDX-License-Identifier: GPL-2.0

==========================
_DSD "reg-names" property
==========================

"reg-names" is a Device Tree binding property that assigns names to the
memory regions listed in "reg", allowing drivers to look up resources by
name with platform_get_resource_byname().

The kernel resolves the same property for platform devices regardless of
the firmware interface: the i-th string of a "reg-names" string array
names the i-th memory resource of the device, in "reg" order on Device
Tree and in _CRS order on ACPI.  Only memory resources count toward
the index; descriptors of other types in _CRS, e.g. interrupts, are
skipped.  platform_get_resource_byname() and
devm_platform_ioremap_resource_byname() consult the property when a
named memory resource is requested, and fall back to the named-resource
lookup, so drivers shared between Device Tree and ACPI work unchanged
on both.

For ACPI devices the property is provided in the Device Properties UUID
package of _DSD.  This is the natural thing to do for devices described
by a Device Tree binding, i.e. those enumerated through the PRP0001 DT
namespace link: firmware following such a binding provides the binding's
properties in _DSD.  Resources created from _CRS carry no names, so
without the property by-name lookups fail on ACPI.

Note that IRQ resources are handled analogously through
fwnode_irq_get_byname() (see "Named Interrupts" in
Documentation/firmware-guide/acpi/enumeration.rst).

Example - a PRP0001 device for a Bosch M_CAN controller, following
Documentation/devicetree/bindings/net/can/bosch,m_can.yaml::

	Device (CAN0)
	{
		Name (_HID, "PRP0001")
		Name (_DSD, Package () {
			ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
			Package () {
				Package () { "compatible", "bosch,m_can" },
				Package () { "reg-names", Package () {
					"m_can", "message_ram"
				} },
			}
		})
		Method (_CRS, 0, Serialized)
		{
			Name (RBUF, ResourceTemplate ()
			{
				QWordMemory (ResourceConsumer, PosDecode,
					     MinFixed, MaxFixed, NonCacheable,
					     ReadWrite,
					     0x0, 0xF0000000, 0xF0000FFF,
					     0x0, 0x1000)
				QWordMemory (ResourceConsumer, PosDecode,
					     MinFixed, MaxFixed, NonCacheable,
					     ReadWrite,
					     0x0, 0xF0001000, 0xF0001FFF,
					     0x0, 0x1000)
			})
			Return (RBUF)
		}
	}

Here "m_can" names the first and "message_ram" the second MEM resource
in _CRS.
