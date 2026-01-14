// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <unistd.h>

#include <linux/scmi.h>

#define SLEEP_MS	3000
#define DEF_TLM_ROOT	"/sys/fs/arm_telemetry/"

#define IOCTL_ERR_STR(_ioctl)	"IOCTL:" #_ioctl

struct tlm_de {
	struct scmi_tlm_de_info *info;
	struct scmi_tlm_de_config cfg;
	struct scmi_tlm_de_sample sample;
};

struct tlm_group {
	int fd;
	struct scmi_tlm_grp_info *info;
	struct scmi_tlm_grp_desc *desc;
	struct scmi_tlm_intervals *ivs;
};

struct tlm_state {
	int dfd;
	int fd;
	int g_dfd;
	const char *path;
	struct scmi_tlm_base_info info;
	struct scmi_tlm_config cfg;
	struct scmi_tlm_intervals *ivs;
	unsigned int num_des;
	struct tlm_de *des;
	unsigned int num_groups;
	struct tlm_group *grps;
};

static inline void dump_state(struct tlm_state *st)
{
	uint32_t *uuid32 = st->info.de_impl_version;
	uint16_t *uuid16 = (uint16_t *)&st->info.de_impl_version[1];

	fprintf(stdout, "- SYSTEM TELEMETRY @instance: %s\n\n", st->path);
	fprintf(stdout, "+ Version: 0x%08X\n", st->info.version);
	fprintf(stdout, "+ DEs#: %d\n", st->info.num_des);
	fprintf(stdout, "+ GRPS#: %d\n", st->info.num_groups);
	fprintf(stdout, "+ INTRV#: %d\n", st->info.num_intervals);

	fprintf(stdout, "+ UUID: ");
	fprintf(stdout, "%X-", uuid32[0]);
	fprintf(stdout, "%X-", uuid16[0]);
	fprintf(stdout, "%X-", uuid16[1]);
	fprintf(stdout, "%X", uuid16[2]);
	fprintf(stdout, "%X\n", uuid32[3]);

	fprintf(stdout, "\n+ TLM_ENABLED: %d\n", st->cfg.enable);
	fprintf(stdout, "+ CURRENT_UPDATE_INTERVAL: %d\n",
		st->cfg.current_update_interval);

	fprintf(stdout, "+ Found #%u Global Update Intervals\n",
		st->info.num_intervals);
	for (int i = 0; i < st->ivs->num; i++)
		fprintf(stdout, "\t[%d]::%u\n", i, st->ivs->update_intervals[i]);

	if (st->info.num_des != st->num_des) {
		fprintf(stdout, "\n++++++ DES NOT FULLY_ENUMERATED ++++++\n");
		fprintf(stdout, "+++ DECLARED:%u  ENUMERATED:%u +++\n",
			st->info.num_des, st->num_des);
	}

	fprintf(stdout, "\n+ Found #%d DEs:\n", st->num_des);
	for (int i = 0; i < st->num_des; i++)
		fprintf(stdout, "\t0x%08X %s %s -- TS:%16llu %016llX\n",
			st->des[i].info->id,
			st->des[i].cfg.enable ? "ON" : "--",
			st->des[i].cfg.t_enable ? "TS_ON" : "-----",
			st->des[i].sample.tstamp, st->des[i].sample.val);
	fprintf(stdout, "\n");

	fprintf(stdout, "+ Found %d GRPs: ", st->num_groups);
	for (int i = 0; i < st->num_groups; i++) {
		fprintf(stdout, "\n\tGRP_ID:%d  DES#:%d  INTRVS#:%d\n",
			st->grps[i].info->id, st->grps[i].info->num_des,
			st->grps[i].info->num_intervals);

		fprintf(stdout, "\tCOMPOSING_DES:");
		for (int j = 0; j < st->grps[i].desc->num_des; j++)
			fprintf(stdout, "0x%08X ",
				st->grps[i].desc->composing_des[j]);
		fprintf(stdout, "\n");
	}
}

static int discover_base_info(int fd, struct scmi_tlm_base_info *info)
{
	int ret;

	ret = ioctl(fd, SCMI_TLM_GET_INFO, info);
	if (ret) {
		perror(IOCTL_ERR_STR(SCMI_TLM_GET_INFO));
		return ret;
	}

	return ret;
}

static struct scmi_tlm_des_list *scmi_get_des_list(int fd, int num_des)
{
	struct scmi_tlm_des_list *dsl;
	size_t size = sizeof(*dsl) + num_des * sizeof(dsl->des[0]);
	int ret;

	dsl = malloc(size);
	if (!dsl)
		return NULL;

	bzero(dsl, size);
	dsl->num_des = num_des;
	ret = ioctl(fd, SCMI_TLM_GET_DE_LIST, dsl);
	if (ret) {
		perror(IOCTL_ERR_STR(SCMI_TLM_GET_DE_LIST));
		return NULL;
	}

	return dsl;
}

static struct tlm_de *enumerate_des(struct tlm_state *st)
{
	struct scmi_tlm_des_list *dsl;
	struct tlm_de *des;

	dsl = scmi_get_des_list(st->fd, st->info.num_des);
	if (!dsl)
		return NULL;

	st->num_des = dsl->num_des;
	des = malloc(sizeof(*des) * st->num_des);
	if (!des)
		return NULL;

	bzero(des, sizeof(*des) * st->num_des);
	for (int i = 0; i < st->num_des; i++) {
		struct tlm_de *de = &des[i];
		int ret;

		de->info = &dsl->des[i];
		de->cfg.id = de->info->id;
		ret = ioctl(st->fd, SCMI_TLM_GET_DE_CFG, &de->cfg);
		if (ret) {
			perror(IOCTL_ERR_STR(SCMI_TLM_GET_DE_CFG));
			continue;
		}

		if (!de->cfg.enable)
			continue;

		/* Collect initial sample */
		de->sample.id = de->info->id;
		ret = ioctl(st->fd, SCMI_TLM_GET_DE_VALUE, &de->sample);
		if (ret) {
			perror(IOCTL_ERR_STR(SCMI_TLM_GET_DE_VALUE));
			continue;
		}
	}

	return des;
}

static int get_current_config(int fd, struct scmi_tlm_config *cfg)
{
	int ret;

	ret = ioctl(fd, SCMI_TLM_GET_CFG, cfg);
	if (ret) {
		perror(IOCTL_ERR_STR(SCMI_TLM_GET_CFG));
		return ret;
	}

	return ret;
}

static struct scmi_tlm_grps_list *scmi_get_grps_list(int fd, int num_groups)
{
	struct scmi_tlm_grps_list *gsl;
	size_t size = sizeof(*gsl) + num_groups * sizeof(gsl->grps[0]);
	int ret;

	gsl = malloc(size);
	if (!gsl)
		return NULL;

	bzero(gsl, size);
	gsl->num_grps = num_groups;
	ret = ioctl(fd, SCMI_TLM_GET_GRP_LIST, gsl);
	if (ret) {
		perror(IOCTL_ERR_STR(SCMI_TLM_GET_GRP_LIST));
		return NULL;
	}

	return gsl;
}

static struct scmi_tlm_intervals *enumerate_intervals(int fd, int num_intervals)
{
	struct scmi_tlm_intervals *ivs;
	size_t sz;
	int ret;

	sz = sizeof(*ivs) + sizeof(*ivs->update_intervals) * num_intervals;
	ivs = malloc(sz);
	if (!ivs)
		return NULL;

	memset(ivs, 0, sz);

	ivs->num = num_intervals;
	ret = ioctl(fd, SCMI_TLM_GET_INTRVS, ivs);
	if (ret) {
		perror(IOCTL_ERR_STR(SCMI_TLM_GET_INTRVS));
		free(ivs);
		return NULL;
	}

	return ivs;
}

static struct tlm_group *enumerate_groups(struct tlm_state *st)
{
	struct scmi_tlm_grps_list *gsl;
	struct tlm_group *grps;

	gsl = scmi_get_grps_list(st->fd, st->info.num_groups);
	if (!gsl)
		return NULL;

	st->g_dfd = openat(st->dfd, "groups", O_RDONLY);
	if (st->g_dfd < 0)
		return NULL;

	st->num_groups = gsl->num_grps;
	grps = malloc(sizeof(*grps) * st->num_groups);
	if (!grps)
		return NULL;

	bzero(grps, sizeof(*grps) * st->num_groups);
	for (int i = 0; i < st->num_groups; i++) {
		struct tlm_group *grp = &grps[i];
		char gctrl[32];
		size_t size;
		int ret;

		snprintf(gctrl, 32, "%d/control", i);
		grp->fd = openat(st->g_dfd, gctrl, O_RDWR);
		if (grp->fd < 0)
			return NULL;

		grp->info = &gsl->grps[i];
		size = sizeof(*grp->desc) + sizeof(uint32_t) * grp->info->num_des;
		grp->desc = malloc(size);
		if (!grp->desc)
			return NULL;

		bzero(grp->desc, size);
		grp->desc->num_des = grp->info->num_des;
		ret = ioctl(grp->fd, SCMI_TLM_GET_GRP_DESC, grp->desc);
		if (ret) {
			perror(IOCTL_ERR_STR(SCMI_TLM_GET_GRP_DESC));
			continue;
		}

		grp->ivs = enumerate_intervals(grp->fd, grp->info->num_intervals);
	}

	return grps;
}

static int get_tlm_state(const char *path, struct tlm_state *st)
{
	int ret;

	st->dfd = open(path, O_RDONLY);
	if (st->dfd < 0) {
		perror("open");
		return st->dfd;
	}

	st->fd = openat(st->dfd, "control", O_RDWR);
	if (st->fd < 0) {
		perror("openat");
		return st->fd;
	}

	ret = discover_base_info(st->fd, &st->info);
	if (ret)
		return ret;

	st->ivs = enumerate_intervals(st->fd, st->info.num_intervals);
	if (!st->ivs)
		return -1;

	ret = get_current_config(st->fd, &st->cfg);
	if (ret)
		return ret;

	if (st->info.num_des)
		st->des = enumerate_des(st);

	if (st->info.num_groups)
		st->grps = enumerate_groups(st);

	st->path = path;

	return 0;
}

int main(int argc, char **argv)
{
	const char *tlm_root_instance = DEF_TLM_ROOT "tlm_0/";
	struct scmi_tlm_data_read *bulk;
	struct scmi_tlm_de_config de_cfg = {};
	struct tlm_state st = {};
	size_t bulk_sz;
	int ret;

	ret = get_tlm_state(tlm_root_instance, &st);
	if (ret)
		return ret;

	dump_state(&st);

	bulk_sz = sizeof(*bulk) + sizeof(bulk->samples[0]) * st.info.num_des;
	bulk = malloc(bulk_sz);
	if (!bulk)
		return -1;

	bzero(bulk, bulk_sz);
	bulk->num_samples = st.info.num_des;
	ret = ioctl(st.fd, SCMI_TLM_SINGLE_SAMPLE, bulk);
	if (ret) {
		perror(IOCTL_ERR_STR(SCMI_TLM_SINGLE_SAMPLE));
		return -1;
	}

	fprintf(stdout, "\n--- Enabling ALL DEs with timestamp...\n");
	de_cfg.enable = 1;
	de_cfg.t_enable = 1;
	ret = ioctl(st.fd, SCMI_TLM_SET_ALL_CFG, &de_cfg);
	if (ret) {
		perror(IOCTL_ERR_STR(SCMI_TLM_SET_ALL_CFG));
		return ret;
	}

	fprintf(stdout, "\n- Single ASYNC read -\n-------------------\n");
	for (int i = 0; i < bulk->num_samples; i++)
		fprintf(stdout, "0x%08X %016llu %016llX\n",
			bulk->samples[i].id, bulk->samples[i].tstamp,
			bulk->samples[i].val);

	bzero(bulk, bulk_sz);
	bulk->num_samples = st.info.num_des;
	ret = ioctl(st.fd, SCMI_TLM_BULK_READ, bulk);
	if (ret) {
		perror(IOCTL_ERR_STR(SCMI_TLM_BULK_READ));
		return -1;
	}

	fprintf(stdout, "\n- BULK read -\n-------------------\n");
	for (int i = 0; i < bulk->num_samples; i++)
		fprintf(stdout, "0x%08X %016llu %016llX\n",
			bulk->samples[i].id, bulk->samples[i].tstamp,
			bulk->samples[i].val);

	return 0;
}
