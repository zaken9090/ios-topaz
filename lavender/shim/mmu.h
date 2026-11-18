#pragma once
#include <stdint.h>

#define MMU_FB_PHYS	0x9d400000UL	/* splash_region, lavender */

struct mmu_info {
	uintptr_t img_lo, img_hi, ttbr;
	int n_l3;
	uint64_t sctlr_before, sctlr_after;
};

int mmu_enable(struct mmu_info *info);
uint64_t mmu_probe(uintptr_t va);
