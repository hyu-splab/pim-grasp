/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2020 UPMEM. All rights reserved. */
#include <dpu_power_management.h>

#include <linux/kernel.h>

#include <dpu_config.h>
#include <dpu_runner.h>
#include <dpu_rank_mcu.h>
#include <dpu_region.h>
#include <dpu_types.h>
#include <ufi/ufi_ci.h>
#include <ufi/ufi.h>
#include "ufi_rank_utils.h"

#define DPU_CLOCK_DIV_POWER_SAVING_MODE DPU_CLOCK_DIV8

void dpu_power_rank_enter_saving_mode(struct dpu_rank_t *rank)
{
	struct dpu_run_context_t *run_context = &rank->runtime.run_context;
	int ret;
	uint8_t nr_cis, each_ci;
	uint8_t ci_mask = ALL_CIS;

	// TODO: WIP, return here to disable in a weird way
	// investigation must be done to check for removal
	return;
	ufi_select_cis(rank, &ci_mask);
	/*
	 * We must make sure no DPU is in fault before sending a soft reset, otherwise
	 * that would not allow debugging to work.
	 * Note that in the meantime, leaving a rank with a DPU in fault does not allow to
	 * reduce power consumption of this rank.
	 */
	ret = ci_get_color(rank, NULL, ci_mask);
	if (ret != 0) {
		dev_warn(
			&rank->dev,
			"failed to get rank context, not entering power saving mode");
		return;
	}

	/*
	 * Restore the group configuration from the reset stage.
	 */
	ret = dpu_set_groups_from_slice_info(rank);
	if (ret != 0) {
		dev_warn(
			&rank->dev,
			"failed to configure groups, not entering power saving mode");
		return;
	}

	ret = dpu_poll_rank(rank, NULL);
	if (ret != 0) {
		dev_warn(&rank->dev,
			 "failed to poll rank, not entering power saving mode");
		return;
	}

	nr_cis = rank->region->addr_translate.desc.topology
			 .nr_of_control_interfaces;
	for (each_ci = 0; each_ci < nr_cis; ++each_ci) {
		if (run_context->dpu_in_fault[each_ci]) {
			dev_warn(
				&rank->dev,
				"dpu in fault, not entering power saving mode");
			return;
		}
	}
	// TODO this is not more needed in the 1.4
	if (!is_chip_v1_4(rank)) {
		ret = dpu_soft_reset(rank, DPU_CLOCK_DIV_POWER_SAVING_MODE);
		if (ret != 0) {
			dev_warn(
				&rank->dev,
				"failed to set power saving mode clock division");
			return;
		}
	} else {
		//TODO implement the new 1.4 clock gating +
		// and modify the alloc in mdd and sdk to implement the minimum operations
		// and avoid the rank_reset
	}
}

void dpu_power_rank_exit_saving_mode(struct dpu_rank_t *rank)
{
}
