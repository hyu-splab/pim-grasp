/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2020 UPMEM. All rights reserved. */
#ifndef DPU_REGION_INCLUDE_H
#define DPU_REGION_INCLUDE_H

#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/idr.h>
#include <linux/version.h>
#include <linux/device.h>

#include <dpu_dax.h>
#include <dpu_rank.h>
#include <dpu_region_address_translation.h>
#include <dpu_region_constants.h>

#include <dpu_fpga_kc705_device.h>

#define DPU_REGION_NAME "dpu_region"
#define DPU_REGION_PATH DPU_REGION_NAME "%d"

struct dpu_region {
	struct dpu_region_address_translation addr_translate;
	uint8_t mode;
	struct mutex lock;

	struct dpu_rank_t rank;

	/* Memory driver */
	struct dpu_dax_device dpu_dax_dev;
	struct cdev cdev_dax;
	struct device dev_dax;
	void *base; /* linear address corresponding to the region resource */
	uint64_t size;

	/* Pci fpga kc705 driver */
	struct pci_device_fpga dpu_fpga_kc705_dev;
	uint8_t activate_ila;
	uint8_t activate_filtering_ila;
	uint8_t activate_mram_bypass;
	uint8_t spi_mode_enabled;
	uint32_t mram_refresh_emulation_period;
	struct dentry *iladump;
	struct dentry *dpu_debugfs;

	/* Pci fpga aws driver */
	struct xdma_dev *dpu_fpga_aws_dev;
};

extern struct ida dpu_region_ida;

void dpu_region_lock(struct dpu_region *region);
void dpu_region_unlock(struct dpu_region *region);

int dpu_region_dev_probe(void);
int dpu_region_srat_probe(void);

int dpu_region_mem_add(u64 addr, u64 size, u32 pxm, int index);

#endif /* DPU_REGION_INCLUDE_H */
