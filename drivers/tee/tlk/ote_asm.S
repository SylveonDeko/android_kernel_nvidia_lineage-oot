/*
 * Copyright (c) 2014-2016, NVIDIA Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <asm/asm-offsets.h>
#include <asm/ptrace.h>
#include <linux/linkage.h>
#include <linux/init.h>

#ifdef CONFIG_ARM64

/* uint32_t tlk_generic_smc(uint32_t arg0, uint32_t arg1, uint32_t arg2) */
SYM_FUNC_START(_tlk_generic_smc)
	smc	#0
	ret
SYM_FUNC_END(_tlk_generic_smc)

SYM_FUNC_START(_tlk_get_mair)
	mrs	x0, mair_el1
	ret
SYM_FUNC_END(_tlk_get_mair)

#else

	.arch_extension sec
SYM_FUNC_START(_tlk_generic_smc)
	smc	#0
	mov	pc, lr
SYM_FUNC_END(_tlk_generic_smc)

SYM_FUNC_START(_tlk_get_mair)
	mrc	p15, 0, r0, c10, c2, 0
	mrc	p15, 0, r1, c10, c2, 1
	mov	pc, lr
SYM_FUNC_END(_tlk_get_mair)

#endif
