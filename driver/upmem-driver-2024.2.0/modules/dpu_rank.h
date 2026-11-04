/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2020 UPMEM. All rights reserved. */
#ifndef DPU_RANK_INCLUDE_H
#define DPU_RANK_INCLUDE_H

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/sizes.h>
#include <linux/types.h>

#include "dpu_region_address_translation.h"
#include "dpu_types.h"
#include <dpu_vpd_structures.h>

#define DPU_RANK_NAME "dpu_rank"
#define DPU_RANK_PATH DPU_RANK_NAME "%d"

#define DPU_RANK_MCU_VERSION_LEN 128
#define DPU_DIMM_PART_NUMBER_LEN 20
#define DPU_DIMM_SERIAL_NUMBER_LEN 10
#define DPU_RANK_INVALID_INDEX 255

/* Size in bytes of one rank of a DPU DIMM */
#define DPU_RANK_SIZE (8ULL * SZ_1G)

// Common with backends
struct dpu_configuration_slice_info_t {
	uint64_t byte_order;
	uint64_t structure_value;
	struct dpu_slice_target slice_target;
	dpu_bitfield_t host_mux_mram_state;
	dpu_selected_mask_t dpus_per_group[DPU_MAX_NR_GROUPS];
	dpu_selected_mask_t enabled_dpus;
	bool all_dpus_are_enabled;
};

// Common with backends
struct dpu_control_interface_context {
	dpu_ci_bitfield_t fault_decode;
	dpu_ci_bitfield_t fault_collide;

	dpu_ci_bitfield_t color;
	struct dpu_configuration_slice_info_t slice_info
		[DPU_MAX_NR_CIS]; // Used for the current application to hold slice info
};

struct dpu_run_context_t {
	dpu_bitfield_t dpu_running[DPU_MAX_NR_CIS];
	dpu_bitfield_t dpu_in_fault[DPU_MAX_NR_CIS];
	uint8_t nb_dpu_running;
};

struct dpu_runtime_state_t {
	struct dpu_control_interface_context control_interface;
	struct dpu_run_context_t run_context;
};

struct dpu_rank_owner {
	uint8_t is_owned;
	unsigned int usage_count;
};

struct dpu_t {
	struct dpu_rank_t *rank;
	dpu_slice_id_t slice_id;
	dpu_member_id_t dpu_id;
	bool enabled;
};

struct xfer_page {
	struct page **pages;
	unsigned long nb_pages;
	/* Because user allocation through malloc
	 * can be unaligned to page size, we must
	 * know the offset within the first page of
	 * the buffer.
	 */
	int off_first_page;
};

/* Forward declaration */
struct dpu_region;

struct dpu_rank_t {
	struct list_head list;

	struct dpu_region *region;

	struct cdev cdev;
	struct device dev;

	struct dpu_rank_owner owner;
	struct dpu_runtime_state_t runtime;
	struct dpu_bit_config bit_config;

	struct dpu_t *dpus;

	uint8_t id;
	uint8_t channel_id;
	uint8_t slot_index;

	uint8_t debug_mode;

	uint64_t control_interface[DPU_MAX_NR_CIS];
	uint64_t data[DPU_MAX_NR_CIS];

	/* Preallocates a huge array of struct page *
	* pointers for get_user_pages and xferp.
	*/
	struct page **xfer_dpu_page_array;
	struct xfer_page xfer_pg[DPU_MAX_NR_DPUS];

	/* Information requested from the MCU */
	uint8_t rank_index;
	uint8_t rank_count;
	char mcu_version[DPU_RANK_MCU_VERSION_LEN];
	char part_number[DPU_DIMM_PART_NUMBER_LEN]; /* e.g UPMEM-E19 */
	char serial_number[DPU_DIMM_SERIAL_NUMBER_LEN];
	struct dpu_vpd vpd;
	uint8_t mailbox_dpu;
	uint8_t mailbox_ci;
};

extern struct class *dpu_rank_class;

int dpu_rank_init_device(struct device *dev, struct dpu_region *region,
			 bool must_init_mram);
void dpu_rank_release_device(struct dpu_region *region);
uint32_t dpu_rank_get(struct dpu_rank_t *rank);
void dpu_rank_put(struct dpu_rank_t *rank);
void dpu_rank_dmi_find_channel(struct dpu_rank_t *rank);
void dpu_rank_dmi_init(void);
void dpu_rank_dmi_exit(void);

int dpu_rank_copy_to_rank(struct dpu_rank_t *rank,
			  struct dpu_transfer_mram *transfer_matrix);
int dpu_rank_copy_from_rank(struct dpu_rank_t *rank,
			    struct dpu_transfer_mram *transfer_matrix);

extern const struct attribute_group *dpu_rank_attrs_groups[];

#endif /* DPU_RANK_INCLUDE_H */
