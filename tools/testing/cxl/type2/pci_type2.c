#include <linux/module.h>
#include <linux/pci.h>
#include <linux/cxl.h>
#include <linux/cxlpci.h>
#include <linux/cxlmem.h>

struct cxl_dev_state *cxlds;

#define CXL_TYPE2_MEM_SIZE   (1024*1024*256)

static int type2_pci_probe(struct pci_dev *pci_dev,
			   const struct pci_device_id *entry)

{
	u16 dvsec;

	dvsec = pci_find_dvsec_capability(pci_dev, PCI_DVSEC_VENDOR_ID_CXL, CXL_DVSEC_PCIE_DEVICE);

	if (!dvsec) {
		pci_info(pci_dev, "No CXL capability (vendor: %x\n", pci_dev->vendor);
		return 0;
	} else {
		pci_info(pci_dev, "CXL CXL_DVSEC_PCIE_DEVICE capability found");
	}

	cxlds = cxl_accel_state_create(&pci_dev->dev);
	if (IS_ERR(cxlds))
		return PTR_ERR(cxlds);

	pci_info(pci_dev, "Initializing cxlds...");
	cxlds->cxl_dvsec = dvsec;
	cxlds->serial = pci_dev->dev.id;

	/* Should not this be based on DVSEC range size registers */
	cxlds->dpa_res = DEFINE_RES_MEM(0, CXL_TYPE2_MEM_SIZE);
	cxlds->ram_res = DEFINE_RES_MEM_NAMED(0, CXL_TYPE2_MEM_SIZE, "ram");

	return 0;
}

static void type2_pci_remove(struct pci_dev *pci_dev)
{

}

/* PCI device ID table */
static const struct pci_device_id type2_pci_table[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_AMD, 0xbabe)},
	{0}                     /* end of list */
};

static struct pci_driver type2_pci_driver = {
	.name           = KBUILD_MODNAME,
	.id_table       = type2_pci_table,
	.probe          = type2_pci_probe,
	.remove         = type2_pci_remove,
};

static int __init type2_cxl_init(void)
{
	int rc;

	rc = pci_register_driver(&type2_pci_driver);

	return rc;
}

static void __exit type2_cxl_exit(void)
{
	pci_unregister_driver(&type2_pci_driver);
}

module_init(type2_cxl_init);
module_exit(type2_cxl_exit);

MODULE_AUTHOR("Alejadro Lucero <alucerop@amd.com>");
MODULE_DESCRIPTION("CXL Type2 device support, driver test");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(CXL);
MODULE_DEVICE_TABLE(pci, type2_pci_table);
