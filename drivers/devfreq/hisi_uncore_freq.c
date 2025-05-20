// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon uncore frequency scaling driver
 *
 * Copyright (c) 2025 HiSilicon Co., Ltd
 */

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/devfreq.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/property.h>
#include <linux/topology.h>
#include <linux/units.h>
#include <acpi/pcc.h>

#include "governor.h"

struct hisi_uncore_pcc_data {
	u16 status;
	u16 resv;
	u32 data;
};

struct hisi_uncore_pcc_shmem {
	struct acpi_pcct_shared_memory head;
	struct hisi_uncore_pcc_data pcc_data;
};

enum hisi_uncore_pcc_cmd_type {
	HUCF_PCC_CMD_GET_CAP = 0,
	HUCF_PCC_CMD_GET_FREQ,
	HUCF_PCC_CMD_SET_FREQ,
	HUCF_PCC_CMD_GET_MODE,
	HUCF_PCC_CMD_SET_MODE,
	HUCF_PCC_CMD_GET_PLAT_FREQ_NUM,
	HUCF_PCC_CMD_GET_PLAT_FREQ_BY_IDX,
	HUCF_PCC_CMD_MAX = 256,
};

static int hisi_platform_gov_usage;
static DEFINE_MUTEX(hisi_platform_gov_usage_lock);

enum hisi_uncore_freq_mode {
	HUCF_MODE_PLATFORM = 0,
	HUCF_MODE_OS,
	HUCF_MODE_MAX,
};

#define HUCF_CAP_PLATFORM_CTRL	BIT(0)

/**
 * struct hisi_uncore_freq - hisi uncore frequency scaling device data
 * @dev:		device of this frequency scaling driver
 * @cl:			mailbox client object
 * @pchan:		PCC mailbox channel
 * @chan_id:		PCC channel ID
 * @last_cmd_cmpl_time:	timestamp of the last completed PCC command
 * @pcc_lock:		PCC channel lock
 * @devfreq:		devfreq data of this hisi_uncore_freq device
 * @related_cpus:	CPUs whose performance is majorly affected by this
 *			uncore frequency domain
 * @cap:		capabililty flag
 */
struct hisi_uncore_freq {
	struct device *dev;
	struct mbox_client cl;
	struct pcc_mbox_chan *pchan;
	int chan_id;
	ktime_t last_cmd_cmpl_time;
	struct mutex pcc_lock;
	struct devfreq *devfreq;
	struct cpumask related_cpus;
	u32 cap;
};

/* PCC channel timeout = PCC nominal latency * NUM */
#define HUCF_PCC_POLL_TIMEOUT_NUM	1000
#define HUCF_PCC_POLL_INTERVAL_US	5

/* Default polling interval in ms for devfreq governors*/
#define DEFAULT_POLLING_MS 100

static int hisi_uncore_request_pcc_chan(struct hisi_uncore_freq *uncore)
{
	struct pcc_mbox_chan *pcc_chan;
	int rc;

	uncore->cl = (struct mbox_client) {
		.dev = uncore->dev,
		.tx_block = false,
		.knows_txdone = true,
	};

	pcc_chan = pcc_mbox_request_channel(&uncore->cl, uncore->chan_id);
	if (IS_ERR(pcc_chan)) {
		dev_err(uncore->dev, "Failed to request PCC channel %u\n",
			uncore->chan_id);
		return PTR_ERR(pcc_chan);
	}

	if (!pcc_chan->shmem_base_addr) {
		dev_err(uncore->dev, "Invalid PCC shared memory address\n");
		rc = -EINVAL;
		goto err_pcc_chan_free;
	}

	if (pcc_chan->shmem_size < sizeof(struct hisi_uncore_pcc_shmem)) {
		dev_err(uncore->dev, "Invalid PCC shared memory size (%lluB)\n",
			pcc_chan->shmem_size);
		rc = -EINVAL;
		goto err_pcc_chan_free;
	}

	mutex_init(&uncore->pcc_lock);
	uncore->pchan = pcc_chan;

	return 0;

err_pcc_chan_free:
	pcc_mbox_free_channel(pcc_chan);

	return rc;
}

static void hisi_uncore_free_pcc_chan(struct hisi_uncore_freq *uncore)
{
	if (uncore->pchan) {
		guard(mutex)(&uncore->pcc_lock);
		pcc_mbox_free_channel(uncore->pchan);
		uncore->pchan = NULL;
	}
}

static void devm_hisi_uncore_free_pcc_chan(void *data)
{
	hisi_uncore_free_pcc_chan(data);
}

static acpi_status hisi_uncore_pcc_reg_scan(struct acpi_resource *res,
					    void *ctx)
{
	struct acpi_resource_generic_register *reg;
	struct hisi_uncore_freq *uncore;

	if (!res || res->type != ACPI_RESOURCE_TYPE_GENERIC_REGISTER)
		return AE_OK;

	reg = &res->data.generic_reg;
	if (reg->space_id != ACPI_ADR_SPACE_PLATFORM_COMM)
		return AE_OK;

	if (!ctx)
		return AE_ERROR;

	uncore = ctx;
	/* PCC subspace ID stored in Access Size */
	uncore->chan_id = reg->access_size;

	return AE_CTRL_TERMINATE;
}

static int hisi_uncore_init_pcc_chan(struct hisi_uncore_freq *uncore)
{
	acpi_handle handle = ACPI_HANDLE(uncore->dev);
	acpi_status status;
	int rc;

	uncore->chan_id = -1;
	status = acpi_walk_resources(handle, METHOD_NAME__CRS,
				     hisi_uncore_pcc_reg_scan, uncore);
	if (ACPI_FAILURE(status) || uncore->chan_id < 0) {
		dev_err(uncore->dev, "Failed to get a PCC channel\n");
		return -ENODEV;
	}

	rc = hisi_uncore_request_pcc_chan(uncore);
	if (rc)
		return rc;

	return devm_add_action_or_reset(uncore->dev,
					devm_hisi_uncore_free_pcc_chan,
					uncore);
}

static int hisi_uncore_cmd_send(struct hisi_uncore_freq *uncore,
				u8 cmd, u32 *data)
{
	struct hisi_uncore_pcc_shmem __iomem *addr;
	struct hisi_uncore_pcc_shmem shmem;
	struct pcc_mbox_chan *pchan;
	unsigned int mrtt;
	s64 time_delta;
	u16 status;
	int rc;

	guard(mutex)(&uncore->pcc_lock);

	pchan = uncore->pchan;
	if (!pchan)
		return -ENODEV;

	addr = (struct hisi_uncore_pcc_shmem __iomem *)pchan->shmem;
	if (!addr)
		return -EINVAL;

	/* Handle the Minimum Request Turnaround Time (MRTT) */
	mrtt = pchan->min_turnaround_time;
	time_delta = ktime_us_delta(ktime_get(),
				    uncore->last_cmd_cmpl_time);
	if (mrtt > time_delta)
		udelay(mrtt - time_delta);

	/* Copy data */
	shmem.head = (struct acpi_pcct_shared_memory) {
		.signature = PCC_SIGNATURE | uncore->chan_id,
		.command = cmd,
		.status = 0,
	};
	shmem.pcc_data.data = *data;
	memcpy_toio(addr, &shmem, sizeof(shmem));

	/* Ring doorbell */
	rc = mbox_send_message(pchan->mchan, &cmd);
	if (rc < 0) {
		dev_err(uncore->dev, "Failed to send mbox message, %d\n", rc);
		return rc;
	}

	/* Wait status */
	rc = readw_poll_timeout(&addr->head.status, status,
				status & (PCC_STATUS_CMD_COMPLETE |
					  PCC_STATUS_ERROR),
				HUCF_PCC_POLL_INTERVAL_US,
				pchan->latency * HUCF_PCC_POLL_TIMEOUT_NUM);
	if (rc) {
		dev_err(uncore->dev, "PCC channel response timeout, cmd=%u\n", cmd);
		goto exit;
	}

	if (status & PCC_STATUS_ERROR) {
		dev_err(uncore->dev, "PCC cmd error, cmd=%u\n", cmd);
		rc = -EIO;
		goto exit;
	}

exit:
	uncore->last_cmd_cmpl_time = ktime_get();

	/* Copy data back */
	memcpy_fromio(data, &addr->pcc_data.data, sizeof(*data));

	/* Clear mailbox active req */
	mbox_client_txdone(pchan->mchan, rc);

	return rc;
}

static int hisi_uncore_target(struct device *dev, unsigned long *freq,
			      u32 flags)
{
	struct hisi_uncore_freq *uncore = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	u32 data;

	if (WARN_ON(!uncore || !uncore->pchan))
		return -ENODEV;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp)) {
		dev_err(dev, "Failed to get opp for freq %lu hz\n", *freq);
		return PTR_ERR(opp);
	}
	dev_pm_opp_put(opp);

	data = (u32)(dev_pm_opp_get_freq(opp) / HZ_PER_MHZ);

	return hisi_uncore_cmd_send(uncore, HUCF_PCC_CMD_SET_FREQ, &data);
}

static int hisi_uncore_get_dev_status(struct device *dev,
				      struct devfreq_dev_status *stat)
{
	/* Not used */
	return 0;
}

static int hisi_uncore_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct hisi_uncore_freq *uncore = dev_get_drvdata(dev);
	u32 data = 0;
	int rc;

	if (WARN_ON(!uncore || !uncore->pchan))
		return -ENODEV;

	rc = hisi_uncore_cmd_send(uncore, HUCF_PCC_CMD_GET_FREQ, &data);

	/*
	 * Upon a failure, 'data' remains 0 and 'freq' is set to 0 rather than a
	 * random value.  devfreq shouldn't use 'freq' in that case though.
	 */
	*freq = data * HZ_PER_MHZ;

	return rc;
}

static void devm_hisi_uncore_remove_opp(void *data)
{
	struct hisi_uncore_freq *uncore = data;

	dev_pm_opp_remove_all_dynamic(uncore->dev);
}

static int hisi_uncore_init_opp(struct hisi_uncore_freq *uncore)
{
	unsigned long freq_mhz;
	u32 data = 0, num, index;
	int rc;

	rc = hisi_uncore_cmd_send(uncore, HUCF_PCC_CMD_GET_PLAT_FREQ_NUM,
				  &data);
	if (rc) {
		dev_err(uncore->dev, "Failed to get plat freq num\n");
		return rc;
	}
	num = data;

	for (index = 0; index < num; index++) {
		data = index;
		rc = hisi_uncore_cmd_send(uncore,
					  HUCF_PCC_CMD_GET_PLAT_FREQ_BY_IDX,
					  &data);
		if (rc) {
			dev_err(uncore->dev, "Failed to get plat freq at index %u\n", index);
			dev_pm_opp_remove_all_dynamic(uncore->dev);
			return rc;
		}
		freq_mhz = data;

		/* Don't care OPP votlage, take 1V as default */
		rc = dev_pm_opp_add(uncore->dev,
				    freq_mhz * HZ_PER_MHZ, 1000000);
		if (rc) {
			dev_err(uncore->dev, "Add OPP %lu failed (%d)\n",
				freq_mhz, rc);
			dev_pm_opp_remove_all_dynamic(uncore->dev);
			return rc;
		}
	}

	return devm_add_action_or_reset(uncore->dev,
					devm_hisi_uncore_remove_opp, uncore);
}

static int hisi_platform_gov_func(struct devfreq *df, unsigned long *freq)
{
	/*
	 * Platform-controlled mode doesn't care the frequency issued from
	 * devfreq, so just pick the max freq.
	 */
	*freq = DEVFREQ_MAX_FREQ;

	return 0;
}

static int hisi_platform_gov_handler(struct devfreq *df, unsigned int event,
				     void *val)
{
	struct hisi_uncore_freq *uncore = dev_get_drvdata(df->dev.parent);
	int rc = 0;
	u32 data;

	if (WARN_ON(!uncore || !uncore->pchan))
		return -ENODEV;

	switch (event) {
	case DEVFREQ_GOV_START:
		data = HUCF_MODE_PLATFORM;
		rc = hisi_uncore_cmd_send(uncore, HUCF_PCC_CMD_SET_MODE, &data);
		break;
	case DEVFREQ_GOV_STOP:
		data = HUCF_MODE_OS;
		rc = hisi_uncore_cmd_send(uncore, HUCF_PCC_CMD_SET_MODE, &data);
		break;
	default:
		break;
	}

	if (rc)
		dev_err(uncore->dev, "Failed to set operate mode (%d)\n", rc);

	return rc;
}

/*
 * In the platform-controlled mode, the platform decides the uncore frequency
 * and ignores the frequency issued from the driver.
 * Thus, create a pseudo 'hisi_platform' governor that stops devfreq monitor
 * from working so as to save meaningless overhead.
 */
static struct devfreq_governor hisi_platform_governor = {
	.name = "hisi_platform",
	/*
	 * Set interrupt_driven to skip the devfreq monitor mechanism, though
	 * this governor not interrupt-driven.
	 */
	.flags = DEVFREQ_GOV_FLAG_IRQ_DRIVEN,
	.get_target_freq = hisi_platform_gov_func,
	.event_handler = hisi_platform_gov_handler,
};

static void hisi_uncore_remove_platform_gov(struct hisi_uncore_freq *uncore)
{
	u32 data = HUCF_MODE_PLATFORM;
	int rc;

	if (!(uncore->cap & HUCF_CAP_PLATFORM_CTRL))
		return;

	guard(mutex)(&hisi_platform_gov_usage_lock);

	if (--hisi_platform_gov_usage == 0) {
		rc = devfreq_remove_governor(&hisi_platform_governor);
		if (rc)
			dev_err(uncore->dev, "Failed to remove hisi_platform gov (%d)\n", rc);
	}

	/*
	 * Set to the platform-controlled mode, if supported, so as to have a
	 * certain behaviour when the driver is detached.
	 */
	rc = hisi_uncore_cmd_send(uncore, HUCF_PCC_CMD_SET_MODE, &data);
	if (rc)
		dev_err(uncore->dev, "Failed to set platform mode on exit (%d)\n", rc);
}

static void devm_hisi_uncore_remove_platform_gov(void *data)
{
	hisi_uncore_remove_platform_gov(data);
}

static int hisi_uncore_add_platform_gov(struct hisi_uncore_freq *uncore)
{
	int rc = 0;

	if (!(uncore->cap & HUCF_CAP_PLATFORM_CTRL))
		return 0;

	guard(mutex)(&hisi_platform_gov_usage_lock);

	if (hisi_platform_gov_usage == 0) {
		rc = devfreq_add_governor(&hisi_platform_governor);
		if (rc)
			return rc;
	}
	hisi_platform_gov_usage++;

	return devm_add_action_or_reset(uncore->dev,
					devm_hisi_uncore_remove_platform_gov,
					uncore);
}

static int hisi_uncore_devfreq_register(struct hisi_uncore_freq *uncore)
{
	struct devfreq_dev_profile *profile;
	unsigned long freq;
	u32 data;
	int rc;

	rc = hisi_uncore_get_cur_freq(uncore->dev, &freq);
	if (rc) {
		dev_err(uncore->dev, "Failed to get plat init freq (%d)\n", rc);
		return rc;
	}

	profile = devm_kzalloc(uncore->dev, sizeof(*profile), GFP_KERNEL);
	if (!profile)
		return -ENOMEM;

	profile->initial_freq = freq;
	profile->polling_ms = DEFAULT_POLLING_MS;
	profile->timer = DEVFREQ_TIMER_DELAYED;
	profile->target = hisi_uncore_target;
	profile->get_dev_status = hisi_uncore_get_dev_status;
	profile->get_cur_freq = hisi_uncore_get_cur_freq;

	rc = hisi_uncore_cmd_send(uncore, HUCF_PCC_CMD_GET_MODE, &data);
	if (rc) {
		dev_err(uncore->dev, "Failed to get operate mode (%d)\n", rc);
		return rc;
	}

	if (data == HUCF_MODE_PLATFORM)
		uncore->devfreq = devm_devfreq_add_device(uncore->dev, profile,
					  hisi_platform_governor.name, NULL);
	else
		uncore->devfreq = devm_devfreq_add_device(uncore->dev, profile,
					  DEVFREQ_GOV_PERFORMANCE, NULL);
	if (IS_ERR(uncore->devfreq)) {
		dev_err(uncore->dev, "Failed to add devfreq device\n");
		return PTR_ERR(uncore->devfreq);
	}

	return 0;
}

static int hisi_uncore_mark_related_cpus(struct hisi_uncore_freq *uncore,
				 char *property, int (*get_topo_id)(int cpu),
				 const struct cpumask *(*get_cpumask)(int cpu))
{
	unsigned int i, cpu;
	size_t len;
	u32 *num;
	int rc;

	rc = device_property_count_u32(uncore->dev, property);
	if (rc < 0)
		return rc;
	if (rc == 0)
		return -EINVAL;

	len = rc;
	num = kcalloc(len, sizeof(*num), GFP_KERNEL);
	if (!num)
		return -ENOMEM;

	rc = device_property_read_u32_array(uncore->dev, property, num, len);
	if (rc)
		goto out;

	for (i = 0; i < len; i++) {
		for_each_possible_cpu(cpu) {
			if (get_topo_id(cpu) == num[i]) {
				cpumask_or(&uncore->related_cpus,
					   &uncore->related_cpus,
					   get_cpumask(cpu));
				break;
			}
		}
	}

out:
	kfree(num);

	return rc;
}

static int get_package_id(int cpu)
{
	return topology_physical_package_id(cpu);
}

static const struct cpumask *get_package_cpumask(int cpu)
{
	return topology_core_cpumask(cpu);
}

static int get_cluster_id(int cpu)
{
	return topology_cluster_id(cpu);
}

static const struct cpumask *get_cluster_cpumask(int cpu)
{
	return topology_cluster_cpumask(cpu);
}

static int hisi_uncore_mark_related_cpus_wrap(struct hisi_uncore_freq *uncore)
{
	int rc;

	cpumask_clear(&uncore->related_cpus);

	rc = hisi_uncore_mark_related_cpus(uncore, "related-package",
					   get_package_id,
					   get_package_cpumask);
	if (rc == 0)
		return rc;

	return hisi_uncore_mark_related_cpus(uncore, "related-cluster",
					     get_cluster_id,
					     get_cluster_cpumask);
}

static ssize_t related_cpus_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct hisi_uncore_freq *uncore = dev_get_drvdata(dev->parent);

	return cpumap_print_to_pagebuf(true, buf, &uncore->related_cpus);
}

DEVICE_ATTR_RO(related_cpus);

static struct device_attribute *attr_group[] = {
	&dev_attr_related_cpus,
	NULL,
};

static void hisi_uncore_remove_dev_interface(struct hisi_uncore_freq *uncore)
{
	struct device_attribute **attr = attr_group;

	while (attr && *attr) {
		device_remove_file(&uncore->devfreq->dev, *attr);
		attr++;
	}
}

static void devm_hisi_uncore_remove_dev_interface(void *data)
{
	hisi_uncore_remove_dev_interface(data);
}

static int hisi_uncore_init_dev_interface(struct hisi_uncore_freq *uncore)
{
	struct device_attribute **attr = attr_group;
	int rc;

	rc = hisi_uncore_mark_related_cpus_wrap(uncore);
	if (rc) {
		dev_err(uncore->dev, "Failed to mark related cpus (%d)\n", rc);
		return rc;
	}

	while (attr && *attr) {
		rc = device_create_file(&uncore->devfreq->dev, *attr);
		if (rc) {
			hisi_uncore_remove_dev_interface(uncore);
			return rc;
		}
		attr++;
	}

	return devm_add_action_or_reset(uncore->dev,
					devm_hisi_uncore_remove_dev_interface,
					uncore);
}

static int hisi_uncore_freq_probe(struct platform_device *pdev)
{
	struct hisi_uncore_freq *uncore;
	u32 cap;
	int rc;

	uncore = devm_kzalloc(&pdev->dev, sizeof(*uncore), GFP_KERNEL);
	if (!uncore)
		return -ENOMEM;

	uncore->dev = &pdev->dev;
	platform_set_drvdata(pdev, uncore);

	rc = hisi_uncore_init_pcc_chan(uncore);
	if (rc) {
		dev_err(uncore->dev, "Failed to init PCC channel (%d)", rc);
		return rc;
	}

	rc = hisi_uncore_init_opp(uncore);
	if (rc) {
		dev_err(uncore->dev, "Failed to init OPP (%d)\n", rc);
		goto err_free_pcc_chan;
	}

	rc = hisi_uncore_cmd_send(uncore, HUCF_PCC_CMD_GET_CAP, &cap);
	if (rc) {
		dev_err(uncore->dev, "Failed to get capability (%d)\n", rc);
		goto err_free_opp;
	}
	uncore->cap = cap;

	rc = hisi_uncore_add_platform_gov(uncore);
	if (rc) {
		dev_err(uncore->dev, "Failed to add hisi_platform governor (%d)\n", rc);
		goto err_free_opp;
	}

	rc = hisi_uncore_devfreq_register(uncore);
	if (rc) {
		dev_err(uncore->dev, "Failed to register devfreq (%d)\n", rc);
		goto err_remove_platform_gov;
	}

	rc = hisi_uncore_init_dev_interface(uncore);
	if (rc) {
		dev_err(uncore->dev, "Failed to init custom device interfaces (%d)\n", rc);
		goto err_remove_platform_gov;
	}

	return 0;

err_remove_platform_gov:
	hisi_uncore_remove_platform_gov(uncore);

err_free_opp:
	dev_pm_opp_remove_all_dynamic(uncore->dev);

err_free_pcc_chan:
	hisi_uncore_free_pcc_chan(uncore);

	return rc;
}

static const struct acpi_device_id hisi_uncore_freq_acpi_match[] = {
	{ "HISI04F1", },
	{ },
};
MODULE_DEVICE_TABLE(acpi, hisi_uncore_freq_acpi_match);

static struct platform_driver hisi_uncore_freq_drv = {
	.probe	= hisi_uncore_freq_probe,
	.driver = {
		.name	= "hisi_uncore_freq",
		.acpi_match_table = hisi_uncore_freq_acpi_match,
	},
};
module_platform_driver(hisi_uncore_freq_drv);

MODULE_DESCRIPTION("HiSilicon uncore frequency scaling driver");
MODULE_AUTHOR("Jie Zhan <zhanjie9@hisilicon.com>");
MODULE_LICENSE("GPL");
