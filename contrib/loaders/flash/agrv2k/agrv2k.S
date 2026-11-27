/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stackless RV32I buffered FastProgram writer. The host unlocks Flash.
 * a0: controller -> result (0 success, 1 timeout, SR error bits, 0x80000000 abort)
 * a1: word count; a2: buffer header; a3: buffer end; a4: destination;
 * a5: completion poll budget; a6: idle/lock control for closing FastProgram.
 * FIFO head/tail follow target_run_flash_async_algorithm(). */
	.section .text
	.option norvc
	.option norelax
	.global _start
_start:
	mv t6, a0
	lw t0, 4(a2)
next:
	beqz a1, success
wait_data:
	lw t1, 0(a2)
	beqz t1, aborted
	beq t0, t1, wait_data
	fence rw, rw
	lw t2, 0(t0)
	sw t2, 0(a4)
	fence iorw, iorw
	lw t2, 12(t6)
	andi a0, t2, 0x14
	bnez a0, close
	addi a4, a4, 4
	addi a1, a1, -1
	addi t0, t0, 4
	bne t0, a3, publish
	addi t0, a2, 8
publish:
	fence rw, rw
	sw t0, 4(a2)
	j next
aborted:
	li a0, 0x80000000
	j close
success:
	li a0, 0
close:
	/* Closing the continuous writer commits its final partial page. */
	sw a6, 16(t6)
	fence iorw, iorw
	mv t4, a5
wait_flash:
	lw t2, 12(t6)
	andi t3, t2, 1
	beqz t3, idle
	addi t4, t4, -1
	bnez t4, wait_flash
	li a0, 1
	j failed
idle:
	bnez a0, failed
	andi a0, t2, 0x14
	beqz a0, done
failed:
	fence rw, rw
	sw zero, 4(a2)
done:
	ebreak
