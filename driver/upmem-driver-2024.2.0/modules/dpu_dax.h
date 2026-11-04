/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2020 UPMEM. All rights reserved. */
#ifndef DPU_DAX_H
#define DPU_DAX_H

#include <linux/completion.h>
#include <linux/memremap.h>
#include <linux/percpu-refcount.h>
#include <linux/platform_device.h>
#include <linux/version.h>

struct dpu_dax_device {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
	struct percpu_ref ref;
#endif
	struct dev_pagemap pgmap;
	struct completion cmp;
};

extern struct class *dpu_dax_class;
extern const struct attribute_group *dpu_dax_region_attrs_groups[];

/* Forward declaration */
struct dpu_region;

int dpu_dax_init_device(struct platform_device *pdev,
			struct dpu_region *region);
void dpu_dax_release_device(struct dpu_region *region);

#endif /* DPU_DAX_H */
