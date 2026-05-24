// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 *
 * NMBM initialization routine
 */

#include <env.h>
#include <mtd.h>
#include <linux/mtd/mtd.h>

#include <nmbm/nmbm.h>
#include <nmbm/nmbm-mtd.h>

#include "mtd_helper.h"

int board_nmbm_init(void)
{
	struct mtd_info *lower, *upper;
	int ret;

	/*
	 * Try to load the persistent env so mtd_nmbm_enabled() below can honor
	 * a user-saved nmbm_enable value. On boards where env lives on UBI
	 * rooted on nmbm0 this call fails (no nmbm0 yet); the env layer falls
	 * back to defaults and we fall back to CONFIG_ENABLE_NAND_NMBM.
	 */
	env_load();

	if (!mtd_nmbm_enabled()) {
		printf("\n");
		printf("NMBM disabled by env (nmbm_enable=0)\n");
		return 0;
	}

	printf("\n");
	printf("Initializing NMBM ...\n");

	mtd_probe_devices();

	lower = get_mtd_device_nm(CONFIG_NMBM_LOWER_MTD);
	if (IS_ERR(lower) || !lower) {
		printf("Lower MTD device '%s' not found\n",
		       CONFIG_NMBM_LOWER_MTD);
		return 0;
	}

	ret = nmbm_attach_mtd(lower,
			      NMBM_F_CREATE | NMBM_F_EMPTY_PAGE_ECC_OK,
			      CONFIG_NMBM_MAX_RATIO,
			      CONFIG_NMBM_MAX_BLOCKS, &upper);

	printf("\n");

	if (ret)
		return 0;

	add_mtd_device(upper);

	/*
	 * The first mtd_probe_devices() call above may have parsed mtdparts
	 * before the NMBM upper MTD (nmbm0) exists. Probe again after
	 * registering the upper device so partitions defined for nmbm0 are
	 * created immediately.
	 */
	mtd_probe_devices();

	/*
	 * Refresh the env now that nmbm0 (and its UBI-hosted env volume on
	 * env-on-NMBM boards) is reachable. The hashtable is replaced
	 * atomically with the persistent values; on env-on-non-NMBM boards
	 * this is a harmless re-load.
	 */
	env_load();

	return 0;
}
