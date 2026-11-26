// SPDX-License-Identifier: GPL-2.0-or-later
/* Host model: DMI-visible behavior, GPR/CSR state, and bounded SRAM.
 * The production DM module is compiled directly, without OpenOCD or USB. */
#include "target/agm/agrv2k_dm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SRAM_BASE UINT32_C(0x20000000)
#define SRAM_SIZE 256

struct model {
	uint32_t dmi[128];
	uint32_t gpr[32];
	uint32_t csr[4096];
	uint8_t memory[SRAM_SIZE];
	int64_t time;
	unsigned int reads, writes, memory_reads, memory_writes, commands, fences, batches;
	bool running, reset, resume_ack, have_reset;
	bool busy_abstract, busy_sba, stall_halt, reject_csr, progbuf_exception;
	bool error_after_store;
	bool batch_busy_once;
	int fail_read, fail_write;
};

static uint32_t load(struct model *m, uint32_t address, unsigned int size)
{
	assert(address >= SRAM_BASE && (uint64_t)address + size <= SRAM_BASE + SRAM_SIZE);
	m->memory_reads++;
	uint32_t result = 0;
	for (unsigned int i = 0; i < size; ++i)
		result |= (uint32_t)m->memory[address - SRAM_BASE + i] << (8 * i);
	return result;
}

static void store(struct model *m, uint32_t address, unsigned int size, uint32_t value)
{
	assert(address >= SRAM_BASE && (uint64_t)address + size <= SRAM_BASE + SRAM_SIZE);
	for (unsigned int i = 0; i < size; ++i)
		m->memory[address - SRAM_BASE + i] = value >> (8 * i);
	m->memory_writes++;
}

static uint32_t status(struct model *m)
{
	uint32_t value = m->dmi[AGRV2K_DMSTATUS];
	value |= m->reset ? AGRV2K_ANYUNAVAIL :
		m->running ? AGRV2K_ALLRUNNING : AGRV2K_ALLHALTED;
	if (m->resume_ack)
		value |= AGRV2K_ALLRESUMEACK;
	if (m->have_reset)
		value |= AGRV2K_ANYHAVERESET;
	return value;
}

static int model_read(void *ctx, uint32_t reg, uint32_t *value)
{
	struct model *m = ctx;
	assert(reg < 128);
	m->reads++;
	if ((int)reg == m->fail_read)
		return -1;
	*value = reg == AGRV2K_DMSTATUS ? status(m) : m->dmi[reg];
	if (reg == AGRV2K_ABSTRACTCS && m->busy_abstract)
		*value |= AGRV2K_ABSTRACT_BUSY;
	if (reg == AGRV2K_SBCS && m->busy_sba)
		*value |= AGRV2K_SBBUSY;
	if (reg == AGRV2K_SBDATA0 && (m->dmi[AGRV2K_SBCS] & AGRV2K_SBREADONDATA)) {
		unsigned int size = 1U << ((m->dmi[AGRV2K_SBCS] >> 17) & 7);
		m->dmi[AGRV2K_SBDATA0] = load(m, m->dmi[AGRV2K_SBADDRESS0], size);
		if (m->dmi[AGRV2K_SBCS] & AGRV2K_SBAUTOINCREMENT)
			m->dmi[AGRV2K_SBADDRESS0] += size;
	}
	return 0;
}

static int model_read_many(void *ctx, const uint32_t *regs,
		uint32_t *values, unsigned int count)
{
	struct model *m = ctx;
	m->batches++;
	for (unsigned int i = 0; i < count; ++i) {
		if (model_read(ctx, regs[i], &values[i]) != 0)
			return -1;
		if (m->batch_busy_once && regs[i] == AGRV2K_SBCS) {
			values[i] |= AGRV2K_SBBUSY;
			m->batch_busy_once = false;
		}
	}
	return 0;
}

static void run_progbuf(struct model *m)
{
	if (m->progbuf_exception) {
		m->dmi[AGRV2K_ABSTRACTCS] |= 3U << 8;
		return;
	}
	assert(!m->running);
	if ((m->dmi[AGRV2K_ABSTRACTCS] >> 24) >= 2)
		assert(m->dmi[AGRV2K_PROGBUF0 + 1] == UINT32_C(0x00100073));
	else
		assert(m->dmi[AGRV2K_DMSTATUS] & AGRV2K_IMPEBREAK);
	uint32_t insn = m->dmi[AGRV2K_PROGBUF0];
	unsigned int opcode = insn & 127;
	unsigned int rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
	unsigned int rd = (insn >> 7) & 31, funct3 = (insn >> 12) & 7;
	if (opcode == 3) {
		unsigned int size = 1U << (funct3 & 3);
		m->gpr[rd] = load(m, m->gpr[rs1], size);
	} else if (opcode == 0x23) {
		store(m, m->gpr[rs1], 1U << funct3, m->gpr[rs2]);
	} else if (opcode == 0x73) {
		unsigned int csr = insn >> 20;
		uint32_t old = m->csr[csr];
		if (funct3 == 1)
			m->csr[csr] = m->gpr[rs1];
		else
			assert(funct3 == 2 && rs1 == 0);
		if (rd)
			m->gpr[rd] = old;
	} else {
		assert(insn == UINT32_C(0x0330000f) || insn == UINT32_C(0x0000100f));
		m->fences++;
	}
	m->gpr[0] = 0;
}

static void abstract_command(struct model *m, uint32_t command)
{
	m->commands++;
	assert((command >> 24) == 0 && ((command >> 20) & 7) == 2);
	assert(!m->running);
	if (command & (1U << 17)) {
		unsigned int reg = command & 0xffff;
		if (reg < 4096 && m->reject_csr) {
			m->dmi[AGRV2K_ABSTRACTCS] |= 2U << 8;
			return;
		}
		assert(reg < 4096 || (reg >= 0x1000 && reg < 0x1020));
		uint32_t *value = reg < 4096 ? &m->csr[reg] : &m->gpr[reg - 0x1000];
		if (command & (1U << 16))
			*value = m->dmi[AGRV2K_DATA0];
		else
			m->dmi[AGRV2K_DATA0] = *value;
		m->gpr[0] = 0;
	}
	if (command & (1U << 18))
		run_progbuf(m);
}

static int model_write(void *ctx, uint32_t reg, uint32_t value)
{
	struct model *m = ctx;
	assert(reg < 128);
	m->writes++;
	if ((int)reg == m->fail_write)
		return -1;
	if (reg == AGRV2K_DMCONTROL) {
		bool deassert = m->reset && !(value & AGRV2K_NDMRESET);
		m->reset = value & AGRV2K_NDMRESET;
		if (deassert) {
			m->have_reset = true;
			m->running = !(value & AGRV2K_HALTREQ);
		}
		if (value & AGRV2K_ACKHAVERESET)
			m->have_reset = false;
		if ((value & AGRV2K_HALTREQ) && !m->stall_halt) {
			m->running = false;
			m->csr[AGRV2K_CSR_DCSR] = 3U << 6;
		}
		m->resume_ack = value & AGRV2K_RESUMEREQ;
		if (m->resume_ack) {
			m->running = !(m->csr[AGRV2K_CSR_DCSR] & 4);
			if (!m->running) {
				m->csr[AGRV2K_CSR_DPC] += 4;
				m->csr[AGRV2K_CSR_DCSR] = (m->csr[AGRV2K_CSR_DCSR] & ~0x1c0U) | (4U << 6);
			}
		}
	} else if (reg == AGRV2K_ABSTRACTCS) {
		m->dmi[reg] &= ~(value & AGRV2K_CMDERR_MASK);
		return 0;
	} else if (reg == AGRV2K_COMMAND) {
		abstract_command(m, value);
	} else if (reg == AGRV2K_SBCS) {
		assert(!m->busy_sba);
		uint32_t fixed = m->dmi[reg] & UINT32_C(0xe0000fff);
		uint32_t errors = m->dmi[reg] & (AGRV2K_SBERROR_MASK | AGRV2K_SBBUSYERROR);
		m->dmi[reg] = fixed | (errors & ~value) | (value & UINT32_C(0x1f8000));
		return 0;
	} else if (reg == AGRV2K_SBADDRESS0 && (m->dmi[AGRV2K_SBCS] & AGRV2K_SBREADONADDR)) {
		unsigned int size = 1U << ((m->dmi[AGRV2K_SBCS] >> 17) & 7);
		m->dmi[AGRV2K_SBDATA0] = load(m, value, size);
		m->dmi[reg] = value + ((m->dmi[AGRV2K_SBCS] & AGRV2K_SBAUTOINCREMENT) ? size : 0);
		return 0;
	} else if (reg == AGRV2K_SBDATA0) {
		store(m, m->dmi[AGRV2K_SBADDRESS0], 1U << ((m->dmi[AGRV2K_SBCS] >> 17) & 7), value);
		if (m->error_after_store)
			m->dmi[AGRV2K_SBCS] |= 2U << 12;
	}
	m->dmi[reg] = value;
	return 0;
}

static int64_t model_now(void *ctx)
{
	return ((struct model *)ctx)->time;
}

static void model_sleep(void *ctx, unsigned int ms)
{
	((struct model *)ctx)->time += ms;
}

static struct agrv2k_dm make_dm(struct model *m)
{
	memset(m, 0, sizeof(*m));
	m->fail_read = -1;
	m->fail_write = -1;
	m->dmi[AGRV2K_DMSTATUS] = 2 | AGRV2K_AUTHENTICATED | AGRV2K_IMPEBREAK;
	m->dmi[AGRV2K_ABSTRACTCS] = (8U << 24) | 1;
	m->dmi[AGRV2K_SBCS] = (1U << 29) | (32U << 5) | 7;
	m->csr[0x301] = UINT32_C(0x40801125);
	for (unsigned int i = 1; i < 32; ++i)
		m->gpr[i] = UINT32_C(0x12340000) + i;
	return (struct agrv2k_dm) {
		.io = {.read = model_read, .write = model_write, .now_ms = model_now,
			.sleep_ms = model_sleep, .ctx = m, .read_many = model_read_many},
		.timeout_ms = 10,
	};
}

static void check_probe(void)
{
	struct model m;
	struct agrv2k_dm dm = make_dm(&m);
	m.running = true;
	assert(agrv2k_dm_examine(&dm) == AGRV2K_DM_OK);
	assert(m.running && !m.memory_writes);
	assert(dm.version == 2 && dm.progbuf_words == 8);
	m.dmi[AGRV2K_DMSTATUS] = 3 | AGRV2K_AUTHENTICATED;
	assert(agrv2k_dm_examine(&dm) == AGRV2K_DM_OK && dm.version == 3);
	for (unsigned int v = 0; v < 16; ++v) {
		if (v == 2 || v == 3)
			continue;
		m.dmi[AGRV2K_DMSTATUS] = v | AGRV2K_AUTHENTICATED;
		assert(agrv2k_dm_examine(&dm) == AGRV2K_DM_UNSUPPORTED);
	}
	m.dmi[AGRV2K_DMSTATUS] = 2;
	assert(agrv2k_dm_examine(&dm) == AGRV2K_DM_UNSUPPORTED);
}

static void check_registers_and_run(void)
{
	struct model m;
	struct agrv2k_dm dm = make_dm(&m);
	assert(agrv2k_dm_examine(&dm) == 0);
	uint32_t value;
	unsigned int commands = m.commands;
	assert(agrv2k_dm_reg_write(&dm, 0x10000, 123) == AGRV2K_DM_UNSUPPORTED);
	assert(m.commands == commands);
	assert(agrv2k_dm_reg_write(&dm, 0x1001, 0xabcdef01) == 0);
	assert(agrv2k_dm_reg_read(&dm, 0x1001, &value) == 0 && value == 0xabcdef01);
	assert(agrv2k_dm_reg_write(&dm, 0x1000, UINT32_MAX) == 0 && m.gpr[0] == 0);
	m.reject_csr = true;
	uint32_t saved_s0 = m.gpr[8];
	assert(agrv2k_dm_reg_read(&dm, 0x301, &value) == 0 && value == 0x40801125);
	assert(m.gpr[8] == saved_s0);
	assert(agrv2k_dm_reg_write(&dm, AGRV2K_CSR_DPC, SRAM_BASE) == 0);
	assert(m.csr[AGRV2K_CSR_DPC] == SRAM_BASE && m.gpr[8] == saved_s0);
	assert(agrv2k_dm_resume(&dm, true) == 0 && !m.running);
	assert(m.csr[AGRV2K_CSR_DPC] == SRAM_BASE + 4 && m.fences == 2);
	assert(agrv2k_dm_resume(&dm, false) == 0 && m.running);
	assert(!(m.csr[AGRV2K_CSR_DCSR] & 4) && m.fences == 4);
	assert(agrv2k_dm_reg_read(&dm, 0x1001, &value) == AGRV2K_DM_NOT_HALTED);
	assert(agrv2k_dm_halt(&dm) == 0 && !m.running);
	assert(!(m.dmi[AGRV2K_DMCONTROL] & AGRV2K_HALTREQ));
}

static void check_memory(bool sba)
{
	struct model m;
	struct agrv2k_dm dm = make_dm(&m);
	if (!sba)
		m.dmi[AGRV2K_SBCS] = 0;
	assert(agrv2k_dm_examine(&dm) == 0);
	uint8_t input[] = {0xc2, 0xa9, 0x00, 0xff, 0x55, 0x66, 0x80, 0x7f};
	uint8_t output[sizeof(input)];
	uint32_t saved_s0 = m.gpr[8], saved_s1 = m.gpr[9];
	for (unsigned int size = 1; size <= 4; size *= 2) {
		memset(m.memory, 0xcc, sizeof(m.memory));
		assert(agrv2k_dm_write_memory(&dm, SRAM_BASE + 8, size, sizeof(input) / size, input) == 0);
		assert(agrv2k_dm_read_memory(&dm, SRAM_BASE + 8, size, sizeof(input) / size, output) == 0);
		assert(!memcmp(input, output, sizeof(input)));
		assert(m.memory[7] == 0xcc && m.memory[16] == 0xcc);
		assert(m.gpr[8] == saved_s0 && m.gpr[9] == saved_s1);
	}
	unsigned int writes = m.writes;
	assert(agrv2k_dm_write_memory(&dm, SRAM_BASE + 1, 4, 1, input) == AGRV2K_DM_UNSUPPORTED);
	assert(agrv2k_dm_write_memory(&dm, UINT32_C(0xfffffffc), 4, 2, input) == AGRV2K_DM_UNSUPPORTED);
	assert(agrv2k_dm_write_memory(&dm, SRAM_BASE, 0, 1, input) == AGRV2K_DM_UNSUPPORTED);
	assert(m.writes == writes);
	m.running = true;
	if (sba)
		assert(agrv2k_dm_read_memory(&dm, SRAM_BASE, 4, 1, output) == 0);
	else
		assert(agrv2k_dm_write_memory(&dm, SRAM_BASE, 4, 1, input) == AGRV2K_DM_NOT_HALTED);
}

static void check_failures(void)
{
	struct model m;
	struct agrv2k_dm dm = make_dm(&m);
	assert(agrv2k_dm_examine(&dm) == 0);
	uint8_t bytes[4] = {1, 2, 3, 4};
	m.error_after_store = true;
	assert(agrv2k_dm_write_memory(&dm, SRAM_BASE, 4, 1, bytes) == AGRV2K_DM_COMMAND_ERROR);
	assert(m.memory_writes == 1 && m.commands == 0);
	assert(!(m.dmi[AGRV2K_SBCS] & AGRV2K_SBERROR_MASK));
	m.error_after_store = false;
	m.busy_sba = true;
	unsigned int writes = m.writes;
	assert(agrv2k_dm_write_memory(&dm, SRAM_BASE, 4, 1, bytes) == AGRV2K_DM_TIMEOUT);
	assert(m.writes == writes && m.time == 10);
	m.busy_sba = false;
	m.busy_abstract = true;
	uint32_t value;
	assert(agrv2k_dm_reg_read(&dm, 0x1001, &value) == AGRV2K_DM_TIMEOUT);
	assert(m.writes == writes && m.time == 20);
	m.busy_abstract = false;
	m.fail_read = AGRV2K_DATA0;
	assert(agrv2k_dm_reg_read(&dm, 0x1001, &value) == AGRV2K_DM_IO);
	m.fail_read = -1;
	m.fail_write = AGRV2K_COMMAND;
	assert(agrv2k_dm_reg_write(&dm, 0x1001, 0x777) == AGRV2K_DM_IO);
	assert(m.gpr[1] == 0x12340001);
}

static void check_progbuf_failure(void)
{
	struct model m;
	struct agrv2k_dm dm = make_dm(&m);
	m.dmi[AGRV2K_SBCS] = 0;
	assert(agrv2k_dm_examine(&dm) == 0);
	uint32_t s0 = m.gpr[8], s1 = m.gpr[9];
	m.progbuf_exception = true;
	uint8_t bytes[4] = {1, 2, 3, 4};
	assert(agrv2k_dm_write_memory(&dm, SRAM_BASE, 4, 1, bytes) == AGRV2K_DM_COMMAND_ERROR);
	assert(m.gpr[8] == s0 && m.gpr[9] == s1 && m.memory_writes == 0);
	m.progbuf_exception = false;
	dm.progbuf_words = 1;
	dm.impebreak = false;
	assert(agrv2k_dm_write_memory(&dm, SRAM_BASE, 4, 1, bytes) == AGRV2K_DM_UNSUPPORTED);
	dm.impebreak = true;
	m.dmi[AGRV2K_ABSTRACTCS] = (1U << 24) | 1;
	assert(agrv2k_dm_write_memory(&dm, SRAM_BASE, 4, 1, bytes) == 0);
	assert(load(&m, SRAM_BASE, 4) == 0x04030201);
}

static void check_reset_and_contexts(void)
{
	struct model a, b;
	struct agrv2k_dm da = make_dm(&a), db = make_dm(&b);
	assert(agrv2k_dm_examine(&da) == 0 && agrv2k_dm_examine(&db) == 0);
	assert(agrv2k_dm_reset_assert(&da, true) == 0 && a.reset && !b.reset);
	assert(agrv2k_dm_reset_deassert(&da, true) == 0 && !a.reset && !a.running);
	assert(agrv2k_dm_reset_assert(&da, false) == 0);
	assert(agrv2k_dm_reset_deassert(&da, false) == 0 && a.running);
	assert(!b.running && !b.memory_writes);
	a.stall_halt = true;
	assert(agrv2k_dm_halt(&da) == AGRV2K_DM_TIMEOUT && a.running);
	uint32_t value;
	assert(agrv2k_dm_reg_read(&db, 0x1001, &value) == 0 && value == 0x12340001);
}

static void check_burst(void)
{
	struct model m;
	struct agrv2k_dm dm = make_dm(&m);
	assert(agrv2k_dm_examine(&dm) == 0);
	for (unsigned int i = 0; i < SRAM_SIZE; ++i)
		m.memory[i] = (i * 37) ^ 0xa5;
	uint8_t output[SRAM_SIZE];
	assert(agrv2k_dm_read_memory(&dm, SRAM_BASE, 4, SRAM_SIZE / 4, output) == 0);
	assert(!memcmp(output, m.memory, sizeof(output)));
	assert(m.memory_reads == SRAM_SIZE / 4 && m.batches == 2);
	assert(!(m.dmi[AGRV2K_SBCS] & AGRV2K_SBREADONDATA));
	/* A read at the last two words must not prefetch outside bounded SRAM. */
	assert(agrv2k_dm_read_memory(&dm, SRAM_BASE + SRAM_SIZE - 8, 4, 2, output) == 0);
	assert(!memcmp(output, m.memory + SRAM_SIZE - 8, 8));
	m.batch_busy_once = true;
	unsigned int before = m.memory_reads;
	assert(agrv2k_dm_read_memory(&dm, SRAM_BASE, 4, 2, output) == AGRV2K_DM_COMMAND_ERROR);
	assert(m.memory_reads - before == 2 && m.commands == 0);
	assert(!(m.dmi[AGRV2K_SBCS] & AGRV2K_SBREADONDATA));
	assert(agrv2k_dm_read_memory(&dm, SRAM_BASE, 4, 2, output) == 0);
	m.fail_read = AGRV2K_SBDATA0;
	assert(agrv2k_dm_read_memory(&dm, SRAM_BASE, 4, 2, output) == AGRV2K_DM_IO);
	assert(!(m.dmi[AGRV2K_SBCS] & AGRV2K_SBREADONDATA));
}

int main(void)
{
	check_probe();
	check_registers_and_run();
	check_memory(true);
	check_memory(false);
	check_failures();
	check_progbuf_failure();
	check_reset_and_contexts();
	check_burst();
	puts("AGRV2K_DM_CONTRACTS_PASS groups=8");
	return 0;
}
