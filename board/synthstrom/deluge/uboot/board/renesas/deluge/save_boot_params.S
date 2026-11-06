/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * save_boot_params override for the Synthstrom Deluge (RZ/A1L).
 *
 * U-Boot is launched two ways on this board, and it is built with
 * SKIP_LOWLEVEL_INIT, so cpu_init_cp15 is skipped and U-Boot never touches
 * SCTLR/MMU/caches itself — it inherits whatever the launcher left:
 *
 *   - JTAG (gdb harness): sets SCTLR = 0x00C50078 (MMU + I/D caches OFF)
 *     before branching, so U-Boot starts in a clean caches-off state.
 *   - App-loader (/APPS/*.ELF): hands off with its OWN MMU + L1 I/D caches ON
 *     (flat identity map) and SCTLR ~0x08C5187D. With the MMU + caches still
 *     live, U-Boot relocates and runs against the app-loader's page tables and
 *     caches and corrupts itself (banner prints, then garbage; PC lands in a
 *     poison/exception loop).
 *
 * reset: (arch/arm/cpu/armv7/start.S) branches here first, before any
 * cache/MMU assumption and before relocation. Force the MMU + I/D caches OFF
 * and invalidate, matching the JTAG SCTLR value, so both launch paths are
 * identical. The app-loader uses a flat identity map (VA == PA) and
 * clean-invalidates the caches before jumping, so dropping the MMU here causes
 * no address discontinuity and no dirty-line loss. No stack is used.
 */
#include <linux/linkage.h>

ENTRY(save_boot_params)
	mrc	p15, 0, r0, c1, c0, 0		@ read SCTLR
	bic	r0, r0, #(1 << 0)		@ M = 0  (MMU off)
	bic	r0, r0, #(1 << 2)		@ C = 0  (D-cache off)
	bic	r0, r0, #(1 << 12)		@ I = 0  (I-cache off)
	mcr	p15, 0, r0, c1, c0, 0		@ write SCTLR
	dsb
	isb
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0		@ TLBIALL: invalidate unified TLB
	mcr	p15, 0, r0, c7, c5, 0		@ ICIALLU: invalidate I-cache
	mcr	p15, 0, r0, c7, c5, 6		@ BPIALL: invalidate branch predictor
	dsb
	isb
	b	save_boot_params_ret
ENDPROC(save_boot_params)
