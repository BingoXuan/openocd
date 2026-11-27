/* SPDX-License-Identifier: GPL-2.0-or-later */
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
	.text 0 : { *(.text) }
	/DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
