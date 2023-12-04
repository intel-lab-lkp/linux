// SPDX-License-Identifier: GPL-2.0
#include "pre.h"
#include "drivers/cxl/core/region.c"
#include "post.h"
#include "../stub.c"

#include <CUnit/Basic.h>
#include <stdlib.h>

DECLARE_RWSEM(cxl_dpa_rwsem);

static void
uuid_show_test(void)
{
	struct cxl_region region = { .dev.type = &cxl_region_type };
	char buf[128];

	region.mode = CXL_DECODER_RAM;
	CU_ASSERT(uuid_show(&region.dev, NULL, buf) == 1);
}

static void
is_cxl_pmem_region_test(void)
{
	struct device dev;

	dev.type = &cxl_pmem_region_type;
	CU_ASSERT(is_cxl_pmem_region(&dev) == true);

	dev.type = &cxl_dax_region_type;
	CU_ASSERT(is_cxl_pmem_region(&dev) == false);
}

const struct device_type dummy_region_type = {
	.name = "dummy"
};

static void
is_dup_test(void)
{
	struct device dummy = { .type = &dummy_region_type };
	struct cxl_region region = { .dev.type = &cxl_region_type };
	uuid_t uuid = { 0 };

	/* non-region devices should always return 0 */
	CU_ASSERT(is_dup(&dummy, NULL) == 0);

	/*
	 * uuid matches, indicates the specified uuid duplicates
	 * the uuid for an existing region
	 * return -EBUSY
	 */
	CU_ASSERT(is_dup(&region.dev, &uuid) == -EBUSY);

	/*
	 * uuid does not match
	 */
	uuid.b[0] = 1;
	CU_ASSERT(is_dup(&region.dev, &uuid) == 0);
}

static void
interleave_ways_store_test(void)
{
	struct cxl_region *region = calloc(1, sizeof(*region));
	const char *str0 = "0";
	const char *str1 = "1";
	const char *str16 = "16";
	char buf[32];
	struct cxl_root_decoder *root_decoder = calloc(1, sizeof(*root_decoder));

	region->dev.type = &cxl_region_type;
	region->dev.parent = &root_decoder->cxlsd.cxld.dev;
	root_decoder->cxlsd.cxld.interleave_ways = 1;
	region->params.interleave_ways = 0xFF;

	CU_ASSERT(interleave_ways_store(&region->dev, NULL, str0, strlen(str0)) < 0);
	CU_ASSERT(region->params.interleave_ways == 0xFF);

	CU_ASSERT(interleave_ways_store(&region->dev, NULL, str1, strlen(str1)) == strlen(str1));
	CU_ASSERT(region->params.interleave_ways == 1);
	/* interleave_ways_show appends a newline to the value string */
	CU_ASSERT(interleave_ways_show(&region->dev, NULL, buf) == strlen(str1) + 1);
	CU_ASSERT(strncmp(buf, str1, strlen(str1)) == 0);

	region->params.interleave_ways = 0xFF;
	CU_ASSERT(interleave_ways_store(&region->dev, NULL, str16, strlen(str16)) == strlen(str16));
	CU_ASSERT(region->params.interleave_ways == 16);
	/* interleave_ways_show appends a newline to the value string */
	CU_ASSERT(interleave_ways_show(&region->dev, NULL, buf) == strlen(str16) + 1);
	CU_ASSERT(strncmp(buf, str16, strlen(str16)) == 0);

	free(root_decoder);
	free(region);
}

static void
cxl_port_setup_targets_test(void)
{
	struct cxl_port			port;
	struct cxl_region		region;
	struct cxl_endpoint_decoder	ep_decoder, ep_decoder2;
	struct cxl_port			ep_decoder_port, ep_decoder_port2;
	struct cxl_region_ref		region_ref;
	struct cxl_port			parent_port;
	struct cxl_switch_decoder	switch_decoder;
	struct cxl_root_decoder		root_decoder;
	struct resource			resource;
	struct cxl_memdev		memdev, memdev2;
	struct cxl_ep			ep, ep2;
	struct cxl_dport		dport;

	ep_decoder.cxld.dev.parent = &ep_decoder_port.dev;
	ep_decoder.pos = 0;
	xa_init(&port.regions);
	radix_tree_init();
	CU_ASSERT(xa_insert(&port.regions, (unsigned long)&region, &region_ref, GFP_KERNEL) == 0);
	region_ref.nr_targets = 2;
	port.dev.parent = &parent_port.dev;
	region_ref.region = &region;
	region_ref.port = &port;
	region_ref.decoder = &switch_decoder.cxld;
	region.dev.parent = &root_decoder.cxlsd.cxld.dev;
	region.params.interleave_granularity = 256;
	root_decoder.cxlsd.cxld.interleave_ways = 2;
	switch_decoder.nr_targets = 2;
	resource.start = 0x1000000000;
	resource.end =   0x2000000000;
	region.params.res = &resource;
	region.params.nr_targets = 2;
	region.params.targets[0] = &ep_decoder;
	region.params.targets[1] = &ep_decoder2;
	ep_decoder_port.uport_dev = &memdev.dev;
	xa_init(&port.endpoints);
	ep.dport = &dport;
	CU_ASSERT(xa_insert(&port.endpoints, (unsigned long)&memdev, &ep, GFP_KERNEL) == 0);
	CU_ASSERT(cxl_port_setup_targets(&port, &region, &ep_decoder) == 0);
	CU_ASSERT(region_ref.nr_targets_set == 1);
	CU_ASSERT(switch_decoder.target[0] == &dport);

	ep_decoder2.pos = 1;
	ep_decoder2.cxld.dev.parent = &ep_decoder_port2.dev;
	ep_decoder_port2.uport_dev = &memdev2.dev;
	ep2.dport = &dport;
	CU_ASSERT(xa_insert(&port.endpoints, (unsigned long)&memdev2, &ep2, GFP_KERNEL) == 0);
	CU_ASSERT(cxl_port_setup_targets(&port, &region, &ep_decoder2) == 0);
	CU_ASSERT(region_ref.nr_targets_set == 1);
	CU_ASSERT(switch_decoder.target[0] == &dport);
	CU_ASSERT(switch_decoder.target[1] == NULL);
}

static void
cxl_region_setup_targets_test(void)
{
	struct cxl_port			port;
	struct cxl_region		region;
	struct cxl_endpoint_decoder	ep_decoder, ep_decoder2;
	struct cxl_port			ep_decoder_port, ep_decoder_port2;
	struct cxl_region_ref		region_ref;
	struct cxl_port			parent_port;
	struct cxl_switch_decoder	switch_decoder;
	struct cxl_root_decoder		root_decoder;
	struct resource			resource;
	struct cxl_memdev		memdev, memdev2;
	struct cxl_ep			ep, ep2;
	struct cxl_dport		dport;
	struct cxl_dev_state		dev_state, dev_state2;

	ep_decoder.cxld.dev.parent = &ep_decoder_port.dev;
	ep_decoder.pos = 0;
	xa_init(&port.regions);
	radix_tree_init();
	CU_ASSERT(xa_insert(&port.regions, (unsigned long)&region, &region_ref, GFP_KERNEL) == 0);
	region_ref.nr_targets = 2;
	port.dev.parent = &parent_port.dev;
	region_ref.region = &region;
	region_ref.port = &port;
	region_ref.decoder = &switch_decoder.cxld;
	region.dev.parent = &root_decoder.cxlsd.cxld.dev;
	region.params.interleave_granularity = 256;
	root_decoder.cxlsd.cxld.interleave_ways = 2;
	switch_decoder.nr_targets = 2;
	resource.start = 0x1000000000;
	resource.end =   0x2000000000;
	region.params.res = &resource;
	region.params.nr_targets = 2;
	region.params.targets[0] = &ep_decoder;
	region.params.targets[1] = &ep_decoder2;
	ep_decoder_port.uport_dev = &memdev.dev;
	ep_decoder_port.dev.parent = &port.dev;
	ep_decoder_port2.dev.parent = &port.dev;
	memdev.cxlds = &dev_state;
	memdev2.cxlds = &dev_state2;
	dev_state.rcd = 0;
	dev_state2.rcd = 0;
	xa_init(&port.endpoints);
	ep.dport = &dport;

	ep_decoder2.pos = 1;
	ep_decoder2.cxld.dev.parent = &ep_decoder_port2.dev;
	ep_decoder_port2.uport_dev = &memdev2.dev;
	ep2.dport = &dport;

	CU_ASSERT(xa_insert(&port.endpoints, (unsigned long)&memdev, &ep, GFP_KERNEL) == 0);
	CU_ASSERT(xa_insert(&port.endpoints, (unsigned long)&memdev2, &ep2, GFP_KERNEL) == 0);

	CU_ASSERT(cxl_region_setup_targets(&region) == 0);

	CU_ASSERT(region_ref.nr_targets_set == 1);
	CU_ASSERT(switch_decoder.target[0] == &dport);
	CU_ASSERT(switch_decoder.target[1] == NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("app_suite", NULL, NULL);
	CU_ADD_TEST(suite, uuid_show_test);
	CU_ADD_TEST(suite, is_dup_test);
	CU_ADD_TEST(suite, is_cxl_pmem_region_test);
	CU_ADD_TEST(suite, interleave_ways_store_test);
	CU_ADD_TEST(suite, cxl_port_setup_targets_test);
	CU_ADD_TEST(suite, cxl_region_setup_targets_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
