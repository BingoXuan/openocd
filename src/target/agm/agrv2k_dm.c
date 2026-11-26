// SPDX-License-Identifier: GPL-2.0-or-later

#include "agrv2k_dm.h"

int agrv2k_dm_read(struct agrv2k_dm *dm, uint32_t reg, uint32_t *value)
{
	return dm->io.read(dm->io.ctx, reg, value) == 0 ? AGRV2K_DM_OK : AGRV2K_DM_IO;
}

int agrv2k_dm_write(struct agrv2k_dm *dm, uint32_t reg, uint32_t value)
{
	return dm->io.write(dm->io.ctx, reg, value) == 0 ? AGRV2K_DM_OK : AGRV2K_DM_IO;
}

static int wait_bits(struct agrv2k_dm *dm, uint32_t reg, uint32_t mask,
		uint32_t expected, uint32_t *last)
{
	int64_t deadline = dm->io.now_ms(dm->io.ctx) + dm->timeout_ms;
	for (;;) {
		int ret = agrv2k_dm_read(dm, reg, last);
		if (ret != AGRV2K_DM_OK)
			return ret;
		if ((*last & mask) == expected)
			return AGRV2K_DM_OK;
		if (dm->io.now_ms(dm->io.ctx) >= deadline)
			return AGRV2K_DM_TIMEOUT;
		dm->io.sleep_ms(dm->io.ctx, 1);
	}
}

static int require_halted(struct agrv2k_dm *dm)
{
	uint32_t status;
	int ret = agrv2k_dm_read(dm, AGRV2K_DMSTATUS, &status);
	if (ret != AGRV2K_DM_OK)
		return ret;
	return status & AGRV2K_ALLHALTED ? AGRV2K_DM_OK : AGRV2K_DM_NOT_HALTED;
}

static int abstract_ready(struct agrv2k_dm *dm, uint32_t *status)
{
	return wait_bits(dm, AGRV2K_ABSTRACTCS, AGRV2K_ABSTRACT_BUSY, 0, status);
}

static int execute_abstract(struct agrv2k_dm *dm, uint32_t command)
{
	uint32_t status;
	int ret = abstract_ready(dm, &status);
	if (ret != AGRV2K_DM_OK)
		return ret;
	if (status & AGRV2K_CMDERR_MASK) {
		ret = agrv2k_dm_write(dm, AGRV2K_ABSTRACTCS, AGRV2K_CMDERR_MASK);
		if (ret != AGRV2K_DM_OK)
			return ret;
	}
	ret = agrv2k_dm_write(dm, AGRV2K_COMMAND, command);
	if (ret != AGRV2K_DM_OK)
		return ret;
	ret = abstract_ready(dm, &status);
	if (ret != AGRV2K_DM_OK)
		return ret;
	dm->last_cmderr = (status >> 8) & 7;
	if (!dm->last_cmderr)
		return AGRV2K_DM_OK;
	ret = agrv2k_dm_write(dm, AGRV2K_ABSTRACTCS, AGRV2K_CMDERR_MASK);
	if (ret != AGRV2K_DM_OK)
		return ret;
	return dm->last_cmderr == 2 ? AGRV2K_DM_UNSUPPORTED : AGRV2K_DM_COMMAND_ERROR;
}

static int access_register(struct agrv2k_dm *dm, uint32_t regno,
		bool write, uint32_t *value)
{
	/* Access Register: RV32 aarsize=2, transfer=1; register number is 16 bits. */
	uint32_t status;
	int ret = abstract_ready(dm, &status);
	if (ret != AGRV2K_DM_OK)
		return ret;
	if (write) {
		ret = agrv2k_dm_write(dm, AGRV2K_DATA0, *value);
		if (ret != AGRV2K_DM_OK)
			return ret;
	}
	ret = execute_abstract(dm, (2U << 20) | (1U << 17) |
		(write ? (1U << 16) : 0) | regno);
	if (ret == AGRV2K_DM_OK && !write)
		ret = agrv2k_dm_read(dm, AGRV2K_DATA0, value);
	return ret;
}

static bool has_progbuf(const struct agrv2k_dm *dm)
{
	return dm->progbuf_words >= 2 || (dm->progbuf_words == 1 && dm->impebreak);
}

static int execute_progbuf(struct agrv2k_dm *dm, uint32_t instruction)
{
	if (!has_progbuf(dm))
		return AGRV2K_DM_UNSUPPORTED;
	uint32_t status;
	int ret = abstract_ready(dm, &status);
	if (ret != AGRV2K_DM_OK)
		return ret;
	ret = agrv2k_dm_write(dm, AGRV2K_PROGBUF0, instruction);
	if (ret != AGRV2K_DM_OK)
		return ret;
	if (dm->progbuf_words >= 2) {
		ret = agrv2k_dm_write(dm, AGRV2K_PROGBUF0 + 1, UINT32_C(0x00100073));
		if (ret != AGRV2K_DM_OK)
			return ret;
	}
	/* postexec=1, transfer=0: execute without modifying a GPR. */
	return execute_abstract(dm, (2U << 20) | (1U << 18));
}

int agrv2k_dm_examine(struct agrv2k_dm *dm)
{
	uint32_t status;
	int ret = agrv2k_dm_write(dm, AGRV2K_DMCONTROL, AGRV2K_DMACTIVE);
	if (ret != AGRV2K_DM_OK)
		return ret;
	ret = agrv2k_dm_read(dm, AGRV2K_DMSTATUS, &status);
	if (ret != AGRV2K_DM_OK)
		return ret;
	dm->version = status & 15;
	if ((dm->version != 2 && dm->version != 3) ||
		!(status & AGRV2K_AUTHENTICATED) || (status & AGRV2K_ANYNONEXISTENT))
		return AGRV2K_DM_UNSUPPORTED;
	dm->impebreak = status & AGRV2K_IMPEBREAK;
	ret = abstract_ready(dm, &status);
	if (ret != AGRV2K_DM_OK)
		return ret;
	if (!(status & 15))
		return AGRV2K_DM_UNSUPPORTED;
	dm->progbuf_words = (status >> 24) & 31;
	ret = agrv2k_dm_write(dm, AGRV2K_ABSTRACTAUTO, 0);
	if (ret != AGRV2K_DM_OK)
		return ret;
	return agrv2k_dm_read(dm, AGRV2K_SBCS, &dm->sbcs);
}

int agrv2k_dm_halt(struct agrv2k_dm *dm)
{
	int ret = agrv2k_dm_write(dm, AGRV2K_DMCONTROL, AGRV2K_DMACTIVE | AGRV2K_HALTREQ);
	if (ret != AGRV2K_DM_OK)
		return ret;
	uint32_t status;
	ret = wait_bits(dm, AGRV2K_DMSTATUS, AGRV2K_ALLHALTED, AGRV2K_ALLHALTED, &status);
	int cleanup = agrv2k_dm_write(dm, AGRV2K_DMCONTROL, AGRV2K_DMACTIVE);
	return ret != AGRV2K_DM_OK ? ret : cleanup;
}

static int csr_via_progbuf(struct agrv2k_dm *dm, uint32_t csr,
		bool write, uint32_t *value)
{
	if (!has_progbuf(dm) || csr >= 0x1000)
		return AGRV2K_DM_UNSUPPORTED;
	uint32_t saved;
	int ret = access_register(dm, AGRV2K_GPR_BASE + 8, false, &saved);
	if (ret != AGRV2K_DM_OK)
		return ret;
	if (write) {
		ret = access_register(dm, AGRV2K_GPR_BASE + 8, true, value);
		if (ret == AGRV2K_DM_OK)
			/* csrrw x0, csr, x8 */
			ret = execute_progbuf(dm, (csr << 20) | (8U << 15) | (1U << 12) | 0x73);
	} else {
		/* csrrs x8, csr, x0 */
		ret = execute_progbuf(dm, (csr << 20) | (2U << 12) | (8U << 7) | 0x73);
		if (ret == AGRV2K_DM_OK)
			ret = access_register(dm, AGRV2K_GPR_BASE + 8, false, value);
	}
	int cleanup = access_register(dm, AGRV2K_GPR_BASE + 8, true, &saved);
	return ret != AGRV2K_DM_OK ? ret : cleanup;
}

int agrv2k_dm_reg_read(struct agrv2k_dm *dm, uint32_t regno, uint32_t *value)
{
	if (regno > AGRV2K_GPR_BASE + 31)
		return AGRV2K_DM_UNSUPPORTED;
	int ret = require_halted(dm);
	if (ret != AGRV2K_DM_OK)
		return ret;
	ret = access_register(dm, regno, false, value);
	if (ret == AGRV2K_DM_UNSUPPORTED && regno < AGRV2K_GPR_BASE)
		ret = csr_via_progbuf(dm, regno, false, value);
	return ret;
}

int agrv2k_dm_reg_write(struct agrv2k_dm *dm, uint32_t regno, uint32_t value)
{
	if (regno > AGRV2K_GPR_BASE + 31)
		return AGRV2K_DM_UNSUPPORTED;
	int ret = require_halted(dm);
	if (ret != AGRV2K_DM_OK)
		return ret;
	ret = access_register(dm, regno, true, &value);
	if (ret == AGRV2K_DM_UNSUPPORTED && regno < AGRV2K_GPR_BASE)
		ret = csr_via_progbuf(dm, regno, true, &value);
	return ret;
}

int agrv2k_dm_resume(struct agrv2k_dm *dm, bool step)
{
	uint32_t dcsr;
	int ret = agrv2k_dm_reg_read(dm, AGRV2K_CSR_DCSR, &dcsr);
	if (ret != AGRV2K_DM_OK)
		return ret;
	/* Enable debugger ebreak handling; preserve all unrelated DCSR fields. */
	dcsr = (dcsr & ~4U) | (step ? 4U : 0) | (1U << 15) | (1U << 13) | (1U << 12);
	ret = agrv2k_dm_reg_write(dm, AGRV2K_CSR_DCSR, dcsr);
	if (ret != AGRV2K_DM_OK)
		return ret;
	/* Synchronize data and instruction fetch before running freshly loaded SRAM. */
	ret = execute_progbuf(dm, UINT32_C(0x0330000f));
	if (ret == AGRV2K_DM_OK)
		ret = execute_progbuf(dm, UINT32_C(0x0000100f));
	if (ret != AGRV2K_DM_OK)
		return ret;
	ret = agrv2k_dm_write(dm, AGRV2K_DMCONTROL, AGRV2K_DMACTIVE | AGRV2K_RESUMEREQ);
	if (ret != AGRV2K_DM_OK)
		return ret;
	uint32_t status;
	ret = wait_bits(dm, AGRV2K_DMSTATUS, AGRV2K_ALLRESUMEACK, AGRV2K_ALLRESUMEACK, &status);
	int cleanup = agrv2k_dm_write(dm, AGRV2K_DMCONTROL, AGRV2K_DMACTIVE);
	if (ret != AGRV2K_DM_OK)
		return ret;
	if (cleanup != AGRV2K_DM_OK)
		return cleanup;
	if (step)
		return wait_bits(dm, AGRV2K_DMSTATUS, AGRV2K_ALLHALTED, AGRV2K_ALLHALTED, &status);
	return AGRV2K_DM_OK;
}

int agrv2k_dm_reset_assert(struct agrv2k_dm *dm, bool halt)
{
	return agrv2k_dm_write(dm, AGRV2K_DMCONTROL,
		AGRV2K_DMACTIVE | AGRV2K_NDMRESET | (halt ? AGRV2K_HALTREQ : 0));
}

int agrv2k_dm_reset_deassert(struct agrv2k_dm *dm, bool halt)
{
	int ret = agrv2k_dm_write(dm, AGRV2K_DMCONTROL,
		AGRV2K_DMACTIVE | AGRV2K_ACKHAVERESET | (halt ? AGRV2K_HALTREQ : 0));
	if (ret != AGRV2K_DM_OK)
		return ret;
	uint32_t status;
	uint32_t wanted = halt ? AGRV2K_ALLHALTED : AGRV2K_ALLRUNNING;
	ret = wait_bits(dm, AGRV2K_DMSTATUS, wanted, wanted, &status);
	int cleanup = agrv2k_dm_write(dm, AGRV2K_DMCONTROL, AGRV2K_DMACTIVE);
	return ret != AGRV2K_DM_OK ? ret : cleanup;
}

static bool has_sba(const struct agrv2k_dm *dm, unsigned int size)
{
	/* SBA v1 semantics only. Other implementations use the program buffer. */
	unsigned int encoding = size == 4 ? 2 : size == 2 ? 1 : 0;
	return (dm->sbcs >> 29) == 1 && ((dm->sbcs >> 5) & 127) >= 32 &&
		(dm->sbcs & (1U << encoding));
}

static int sba_wait(struct agrv2k_dm *dm)
{
	uint32_t status;
	int ret = wait_bits(dm, AGRV2K_SBCS, AGRV2K_SBBUSY, 0, &status);
	if (ret != AGRV2K_DM_OK)
		return ret;
	if (status & (AGRV2K_SBERROR_MASK | AGRV2K_SBBUSYERROR))
		return AGRV2K_DM_COMMAND_ERROR;
	return AGRV2K_DM_OK;
}

static int memory_sba(struct agrv2k_dm *dm, uint32_t address,
		unsigned int size, bool write, uint32_t *value)
{
	int ret = sba_wait(dm);
	if (ret != AGRV2K_DM_OK) {
		if (ret == AGRV2K_DM_COMMAND_ERROR)
			agrv2k_dm_write(dm, AGRV2K_SBCS, AGRV2K_SBERROR_MASK | AGRV2K_SBBUSYERROR);
		return ret;
	}
	unsigned int encoding = size == 4 ? 2 : size == 2 ? 1 : 0;
	uint32_t control = (encoding << 17) | (write ? 0 : AGRV2K_SBREADONADDR);
	ret = agrv2k_dm_write(dm, AGRV2K_SBCS, control);
	if (ret == AGRV2K_DM_OK)
		ret = agrv2k_dm_write(dm, AGRV2K_SBADDRESS0, address);
	if (ret == AGRV2K_DM_OK && write)
		ret = agrv2k_dm_write(dm, AGRV2K_SBDATA0, *value);
	if (ret == AGRV2K_DM_OK)
		ret = sba_wait(dm);
	if (ret == AGRV2K_DM_OK && !write)
		ret = agrv2k_dm_read(dm, AGRV2K_SBDATA0, value);
	if (ret == AGRV2K_DM_COMMAND_ERROR)
		agrv2k_dm_write(dm, AGRV2K_SBCS, AGRV2K_SBERROR_MASK | AGRV2K_SBBUSYERROR);
	/* Do not retry an issued SBA transaction through another access method. */
	return ret;
}

static int memory_progbuf(struct agrv2k_dm *dm, uint32_t address,
		unsigned int size, bool write, uint32_t *value)
{
	if (!has_progbuf(dm))
		return AGRV2K_DM_UNSUPPORTED;
	int ret = require_halted(dm);
	if (ret != AGRV2K_DM_OK)
		return ret;
	uint32_t saved_s0, saved_s1;
	ret = access_register(dm, AGRV2K_GPR_BASE + 8, false, &saved_s0);
	if (ret != AGRV2K_DM_OK)
		return ret;
	ret = access_register(dm, AGRV2K_GPR_BASE + 9, false, &saved_s1);
	if (ret != AGRV2K_DM_OK)
		return ret;
	ret = access_register(dm, AGRV2K_GPR_BASE + 8, true, &address);
	if (ret == AGRV2K_DM_OK && write)
		ret = access_register(dm, AGRV2K_GPR_BASE + 9, true, value);
	unsigned int encoding = size == 4 ? 2 : size == 2 ? 1 : 0;
	if (ret == AGRV2K_DM_OK) {
		uint32_t instruction;
		if (write)
			instruction = (9U << 20) | (8U << 15) | (encoding << 12) | 0x23;
		else
			instruction = (8U << 15) | ((encoding + (size < 4 ? 4 : 0)) << 12) |
				(9U << 7) | 0x03;
		ret = execute_progbuf(dm, instruction);
	}
	if (ret == AGRV2K_DM_OK && !write)
		ret = access_register(dm, AGRV2K_GPR_BASE + 9, false, value);
	int cleanup_s0 = access_register(dm, AGRV2K_GPR_BASE + 8, true, &saved_s0);
	int cleanup_s1 = access_register(dm, AGRV2K_GPR_BASE + 9, true, &saved_s1);
	if (ret != AGRV2K_DM_OK)
		return ret;
	return cleanup_s0 != AGRV2K_DM_OK ? cleanup_s0 : cleanup_s1;
}

static int memory_check(uint32_t address, unsigned int size, uint32_t count)
{
	if ((size != 1 && size != 2 && size != 4) || (address & (size - 1)) ||
		(uint64_t)size * count > UINT64_C(0x100000000) - address)
		return AGRV2K_DM_UNSUPPORTED;
	return AGRV2K_DM_OK;
}

int agrv2k_dm_read_memory(struct agrv2k_dm *dm, uint32_t address,
		unsigned int size, uint32_t count, uint8_t *buffer)
{
	int ret = memory_check(address, size, count);
	if (ret != AGRV2K_DM_OK)
		return ret;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t value = 0;
		ret = has_sba(dm, size) ? memory_sba(dm, address, size, false, &value) :
			memory_progbuf(dm, address, size, false, &value);
		if (ret != AGRV2K_DM_OK)
			return ret;
		for (unsigned int j = 0; j < size; ++j)
			*buffer++ = value >> (8 * j);
		address += size;
	}
	return AGRV2K_DM_OK;
}

int agrv2k_dm_write_memory(struct agrv2k_dm *dm, uint32_t address,
		unsigned int size, uint32_t count, const uint8_t *buffer)
{
	int ret = memory_check(address, size, count);
	if (ret != AGRV2K_DM_OK)
		return ret;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t value = 0;
		for (unsigned int j = 0; j < size; ++j)
			value |= (uint32_t)*buffer++ << (8 * j);
		ret = has_sba(dm, size) ? memory_sba(dm, address, size, true, &value) :
			memory_progbuf(dm, address, size, true, &value);
		if (ret != AGRV2K_DM_OK)
			return ret;
		address += size;
	}
	return AGRV2K_DM_OK;
}
