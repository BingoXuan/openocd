// SPDX-License-Identifier: GPL-2.0-or-later
/* Isolated single-hart RV32 target for AGM's MEM-AP debug-module window.
 * No generic RISC-V target symbols or mutable state are used here. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "agrv2k_dm.h"
#include "target/target.h"
#include "target/target_type.h"
#include "target/arm_adi_v5.h"
#include "target/register.h"
#include "target/algorithm.h"
#include <helper/binarybuffer.h>
#include <helper/time_support.h>

#define AGRV2K_CORE_REGS 33
#define AGRV2K_TOTAL_REGS 35

struct agrv2k_common;

struct agrv2k_reg {
	struct agrv2k_common *common;
	uint32_t dm_regno;
};

struct agrv2k_common {
	struct target *target;
	struct adiv5_ap *ap;
	int last_io_error;
	struct agrv2k_dm dm;
	struct reg_cache cache;
	struct reg regs[AGRV2K_TOTAL_REGS];
	struct agrv2k_reg reg_info[AGRV2K_TOTAL_REGS];
	uint8_t reg_values[AGRV2K_TOTAL_REGS][4];
	bool algorithm_active;
	uint32_t saved_gpr[32];
	uint32_t saved_pc, saved_dcsr, saved_mstatus;
};

static int agrv2k_ap_read(void *ctx, uint32_t reg, uint32_t *value)
{
	struct agrv2k_common *common = ctx;
	target_addr_t address = (target_addr_t)common->target->dbgbase + 4ULL * reg;
	int ret = mem_ap_read_atomic_u32(common->ap, address, value);
	if (ret != ERROR_OK)
		common->last_io_error = ret;
	return ret;
}

static int agrv2k_ap_write(void *ctx, uint32_t reg, uint32_t value)
{
	struct agrv2k_common *common = ctx;
	target_addr_t address = (target_addr_t)common->target->dbgbase + 4ULL * reg;
	int ret = mem_ap_write_atomic_u32(common->ap, address, value);
	if (ret != ERROR_OK)
		common->last_io_error = ret;
	return ret;
}

static int agrv2k_ap_read_many(void *ctx, const uint32_t *regs,
		uint32_t *values, unsigned int count)
{
	struct agrv2k_common *common = ctx;
	int queued = ERROR_OK;
	for (unsigned int i = 0; i < count && queued == ERROR_OK; ++i) {
		target_addr_t address = (target_addr_t)common->target->dbgbase + 4ULL * regs[i];
		queued = mem_ap_read_u32(common->ap, address, &values[i]);
	}
	/* Flush even after queue failure: destinations must outlive queued reads. */
	int ret = dap_run(common->ap->dap);
	if (queued != ERROR_OK)
		ret = queued;
	if (ret != ERROR_OK)
		common->last_io_error = ret;
	return ret;
}

static int64_t agrv2k_now_ms(void *ctx)
{
	return timeval_ms();
}

static int agrv2k_ap_write_many(void *ctx, uint32_t reg,
		const uint32_t *values, unsigned int count)
{
	struct agrv2k_common *common = ctx;
	target_addr_t address = (target_addr_t)common->target->dbgbase + 4ULL * reg;
	int queued = ERROR_OK;
	for (unsigned int i = 0; i < count && queued == ERROR_OK; ++i)
		queued = mem_ap_write_u32(common->ap, address, values[i]);
	int ret = dap_run(common->ap->dap);
	if (queued != ERROR_OK)
		ret = queued;
	if (ret != ERROR_OK)
		common->last_io_error = ret;
	return ret;
}

static void agrv2k_sleep_ms(void *ctx, unsigned int ms)
{
	alive_sleep(ms);
}

static int agrv2k_result(struct agrv2k_common *common, int result)
{
	if (result == AGRV2K_DM_OK)
		return ERROR_OK;
	register_cache_invalidate(&common->cache);
	LOG_TARGET_ERROR(common->target, "AGRV2K DM operation failed (%d, cmderr=%u)",
		result, common->dm.last_cmderr);
	if (result == AGRV2K_DM_NOT_HALTED)
		return ERROR_TARGET_NOT_HALTED;
	if (result == AGRV2K_DM_TIMEOUT)
		return ERROR_TARGET_TIMEOUT;
	if (result == AGRV2K_DM_IO && common->last_io_error != ERROR_OK)
		return common->last_io_error;
	return ERROR_FAIL;
}

static int agrv2k_reg_get(struct reg *reg)
{
	struct agrv2k_reg *info = reg->arch_info;
	struct agrv2k_common *common = info->common;
	if (common->target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;
	uint32_t value;
	int ret = agrv2k_dm_reg_read(&common->dm, info->dm_regno, &value);
	if (ret != AGRV2K_DM_OK)
		return agrv2k_result(common, ret);
	buf_set_u32(reg->value, 0, 32, value);
	reg->valid = true;
	reg->dirty = false;
	return ERROR_OK;
}

static int agrv2k_reg_set(struct reg *reg, uint8_t *buffer)
{
	struct agrv2k_reg *info = reg->arch_info;
	struct agrv2k_common *common = info->common;
	if (common->target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;
	uint32_t value = buf_get_u32(buffer, 0, 32);
	int ret = agrv2k_dm_reg_write(&common->dm, info->dm_regno, value);
	if (ret != AGRV2K_DM_OK)
		return agrv2k_result(common, ret);
	buf_set_u32(reg->value, 0, 32, reg->number == 0 ? 0 : value);
	reg->valid = true;
	reg->dirty = false;
	return ERROR_OK;
}

static const struct reg_arch_type agrv2k_reg_type = {
	.get = agrv2k_reg_get,
	.set = agrv2k_reg_set,
};

static struct reg_feature agrv2k_cpu_feature = { .name = "org.gnu.gdb.riscv.cpu" };
static struct reg_feature agrv2k_csr_feature = { .name = "org.gnu.gdb.riscv.csr" };
static struct reg_data_type agrv2k_uint32_type = { .type = REG_TYPE_UINT32 };
static const char * const agrv2k_reg_names[] = {
	"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
	"fp", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
	"a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
	"s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6", "pc", "misa", "dcsr",
};

static int agrv2k_create(struct target *target)
{
	struct adiv5_private_config *pc = target->private_config;
	if (!pc || !pc->dap || target->endianness != TARGET_LITTLE_ENDIAN ||
		target->coreid != 0 || (target->dbgbase & 3)) {
		LOG_TARGET_ERROR(target, "AGRV2K requires -dap, hart 0, little endian and aligned DM base");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}
	struct agrv2k_common *common = calloc(1, sizeof(*common));
	if (!common)
		return ERROR_FAIL;
	common->target = target;
	common->dm.io = (struct agrv2k_dm_io) {
		.read = agrv2k_ap_read, .write = agrv2k_ap_write,
		.now_ms = agrv2k_now_ms, .sleep_ms = agrv2k_sleep_ms, .ctx = common,
		.read_many = agrv2k_ap_read_many,
		.write_many = agrv2k_ap_write_many,
	};
	common->dm.timeout_ms = 1000;
	common->cache.name = "agrv2k-rv32";
	common->cache.num_regs = AGRV2K_TOTAL_REGS;
	common->cache.reg_list = common->regs;
	for (unsigned int i = 0; i < AGRV2K_TOTAL_REGS; ++i) {
		struct reg *reg = &common->regs[i];
		reg->name = agrv2k_reg_names[i];
		reg->number = i;
		reg->size = 32;
		reg->exist = true;
		reg->caller_save = i > 0;
		reg->group = i < AGRV2K_CORE_REGS ? "general" : "csr";
		reg->feature = i < AGRV2K_CORE_REGS ? &agrv2k_cpu_feature : &agrv2k_csr_feature;
		reg->reg_data_type = &agrv2k_uint32_type;
		reg->value = common->reg_values[i];
		reg->type = &agrv2k_reg_type;
		reg->arch_info = &common->reg_info[i];
		common->reg_info[i].common = common;
		common->reg_info[i].dm_regno = i < 32 ? AGRV2K_GPR_BASE + i :
			i == 32 ? AGRV2K_CSR_DPC : i == 33 ? 0x301 : AGRV2K_CSR_DCSR;
		if (i >= AGRV2K_CORE_REGS)
			reg->number = 65 + common->reg_info[i].dm_regno;
	}
	target->arch_info = common;
	target->reg_cache = &common->cache;
	return ERROR_OK;
}

static int agrv2k_init(struct command_context *cmd_ctx, struct target *target)
{
	target->state = TARGET_UNKNOWN;
	target->debug_reason = DBG_REASON_UNDEFINED;
	return ERROR_OK;
}

static void agrv2k_deinit(struct target *target)
{
	struct agrv2k_common *common = target->arch_info;
	if (common) {
		if (common->ap)
			dap_put_ap(common->ap);
		register_unlink_cache(&target->reg_cache, &common->cache);
		free(common);
	}
	free(target->private_config);
	target->private_config = NULL;
	target->arch_info = NULL;
}

static int agrv2k_poll(struct target *target)
{
	struct agrv2k_common *common = target->arch_info;
	uint32_t status;
	int ret = agrv2k_dm_read(&common->dm, AGRV2K_DMSTATUS, &status);
	if (ret != AGRV2K_DM_OK)
		return agrv2k_result(common, ret);
	enum target_state previous = target->state;
	if (status & AGRV2K_ANYNONEXISTENT)
		return ERROR_TARGET_FAILURE;
	if (status & AGRV2K_ANYUNAVAIL) {
		target->state = TARGET_UNKNOWN;
		register_cache_invalidate(&common->cache);
		return ERROR_OK;
	}
	if (status & AGRV2K_ANYHAVERESET) {
		register_cache_invalidate(&common->cache);
		ret = agrv2k_dm_write(&common->dm, AGRV2K_DMCONTROL,
			AGRV2K_DMACTIVE | AGRV2K_ACKHAVERESET);
		if (ret != AGRV2K_DM_OK)
			return agrv2k_result(common, ret);
	}
	if (status & AGRV2K_ALLHALTED) {
		target->state = TARGET_HALTED;
		if (previous != TARGET_HALTED) {
			register_cache_invalidate(&common->cache);
			uint32_t dcsr;
			ret = agrv2k_dm_reg_read(&common->dm, AGRV2K_CSR_DCSR, &dcsr);
			unsigned int cause = ret == AGRV2K_DM_OK ? (dcsr >> 6) & 7 : 0;
			target->debug_reason = cause == 4 ? DBG_REASON_SINGLESTEP :
				cause == 1 || cause == 2 ? DBG_REASON_BREAKPOINT : DBG_REASON_DBGRQ;
			target_call_event_callbacks(target, previous == TARGET_DEBUG_RUNNING ?
				TARGET_EVENT_DEBUG_HALTED : TARGET_EVENT_HALTED);
		}
	} else if (status & AGRV2K_ALLRUNNING) {
		if (previous != TARGET_DEBUG_RUNNING)
			target->state = TARGET_RUNNING;
		register_cache_invalidate(&common->cache);
	}
	return ERROR_OK;
}

static int agrv2k_examine(struct target *target)
{
	struct agrv2k_common *common = target->arch_info;
	struct adiv5_private_config *pc = target->private_config;
	if (!common->ap) {
		common->ap = dap_get_ap(pc->dap, pc->ap_num);
		if (!common->ap)
			return ERROR_FAIL;
	}
	int ret = agrv2k_result(common, agrv2k_dm_examine(&common->dm));
	if (ret != ERROR_OK)
		return ret;
	LOG_TARGET_INFO(target, "Isolated RV32 DM version %u, AP 0x%" PRIx64 ", progbuf %u words",
		common->dm.version, pc->ap_num, common->dm.progbuf_words);
	LOG_TARGET_INFO(target, "SBCS=0x%08" PRIx32, common->dm.sbcs);
	target_set_examined(target);
	return agrv2k_poll(target);
}

static int agrv2k_halt(struct target *target)
{
	struct agrv2k_common *common = target->arch_info;
	int ret = agrv2k_result(common, agrv2k_dm_halt(&common->dm));
	return ret == ERROR_OK ? agrv2k_poll(target) : ret;
}

static int agrv2k_resume(struct target *target, bool current, target_addr_t address,
		bool handle_breakpoints, bool debug_execution)
{
	struct agrv2k_common *common = target->arch_info;
	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;
	if (!current) {
		if (address > UINT32_MAX || (address & 1))
			return ERROR_COMMAND_ARGUMENT_INVALID;
		int ret = agrv2k_result(common, agrv2k_dm_reg_write(&common->dm, AGRV2K_CSR_DPC, address));
		if (ret != ERROR_OK)
			return ret;
	}
	int ret = agrv2k_result(common, agrv2k_dm_resume(&common->dm, false));
	if (ret != ERROR_OK)
		return ret;
	register_cache_invalidate(&common->cache);
	target->state = debug_execution ? TARGET_DEBUG_RUNNING : TARGET_RUNNING;
	target->debug_reason = DBG_REASON_NOTHALTED;
	target_call_event_callbacks(target, debug_execution ? TARGET_EVENT_DEBUG_RESUMED : TARGET_EVENT_RESUMED);
	return agrv2k_poll(target);
}

static int agrv2k_step(struct target *target, bool current, target_addr_t address, bool handle_breakpoints)
{
	struct agrv2k_common *common = target->arch_info;
	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;
	if (!current) {
		if (address > UINT32_MAX || (address & 1))
			return ERROR_COMMAND_ARGUMENT_INVALID;
		int ret = agrv2k_result(common, agrv2k_dm_reg_write(&common->dm, AGRV2K_CSR_DPC, address));
		if (ret != ERROR_OK)
			return ret;
	}
	int ret = agrv2k_result(common, agrv2k_dm_resume(&common->dm, true));
	if (ret != ERROR_OK)
		return ret;
	register_cache_invalidate(&common->cache);
	target->state = TARGET_RUNNING;
	target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
	return agrv2k_poll(target);
}

static int agrv2k_reset_assert(struct target *target)
{
	struct agrv2k_common *common = target->arch_info;
	int ret = agrv2k_result(common, agrv2k_dm_reset_assert(&common->dm, target->reset_halt));
	if (ret == ERROR_OK) {
		register_cache_invalidate(&common->cache);
		common->algorithm_active = false;
		target->running_alg = false;
		target->state = TARGET_RESET;
	}
	return ret;
}

static int agrv2k_reset_deassert(struct target *target)
{
	struct agrv2k_common *common = target->arch_info;
	int ret = agrv2k_result(common, agrv2k_dm_reset_deassert(&common->dm, target->reset_halt));
	return ret == ERROR_OK ? agrv2k_poll(target) : ret;
}

static int agrv2k_read_memory(struct target *target, target_addr_t address,
		uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct agrv2k_common *common = target->arch_info;
	if (address > UINT32_MAX)
		return ERROR_COMMAND_ARGUMENT_INVALID;
	return agrv2k_result(common, agrv2k_dm_read_memory(&common->dm, address, size, count, buffer));
}

static int agrv2k_write_memory(struct target *target, target_addr_t address,
		uint32_t size, uint32_t count, const uint8_t *buffer)
{
	struct agrv2k_common *common = target->arch_info;
	LOG_TARGET_DEBUG(target, "AGRV2K memory write: address=0x%08" PRIx64
		", size=%" PRIu32 ", count=%" PRIu32 ", SBA=0x%08" PRIx32 ", batch=%u",
		(uint64_t)address, size, count, common->dm.sbcs, common->dm.io.write_many != NULL);
	if (address > UINT32_MAX)
		return ERROR_COMMAND_ARGUMENT_INVALID;
	return agrv2k_result(common, agrv2k_dm_write_memory(&common->dm, address, size, count, buffer));
}

static bool agrv2k_direction_valid(enum param_direction direction)
{
	return direction == PARAM_IN || direction == PARAM_OUT || direction == PARAM_IN_OUT;
}

static struct reg *agrv2k_algorithm_reg(struct target *target, const struct reg_param *param)
{
	struct reg *reg = register_get_by_name(target->reg_cache, param->reg_name, false);
	if (!reg || reg->number < 1 || reg->number > 31 || param->size != 32 ||
		!agrv2k_direction_valid(param->direction))
		return NULL;
	return reg;
}

static int agrv2k_restore_algorithm(struct target *target)
{
	struct agrv2k_common *common = target->arch_info;
	/* A failed resume may have issued RESUMEREQ before losing the link.
	 * Never trust the cached state before restoring registers or SRAM. */
	int halted = agrv2k_halt(target);
	if (halted != ERROR_OK) {
		target->state = TARGET_UNKNOWN;
		LOG_TARGET_ERROR(target, "Cannot halt SRAM algorithm; reset and reconnect required");
		return halted;
	}
	int result = AGRV2K_DM_OK;
	for (unsigned int i = 1; i < 32; ++i) {
		int ret = agrv2k_dm_reg_write(&common->dm, AGRV2K_GPR_BASE + i, common->saved_gpr[i]);
		if (result == AGRV2K_DM_OK)
			result = ret;
	}
	const uint32_t regs[] = {AGRV2K_CSR_DPC, AGRV2K_CSR_DCSR, 0x300};
	const uint32_t values[] = {common->saved_pc, common->saved_dcsr, common->saved_mstatus};
	for (unsigned int i = 0; i < ARRAY_SIZE(regs); ++i) {
		int ret = agrv2k_dm_reg_write(&common->dm, regs[i], values[i]);
		if (result == AGRV2K_DM_OK)
			result = ret;
	}
	register_cache_invalidate(&common->cache);
	if (result == AGRV2K_DM_OK) {
		common->algorithm_active = false;
		target->running_alg = false;
	}
	return agrv2k_result(common, result);
}

static int agrv2k_start_algorithm(struct target *target,
		int num_mem_params, struct mem_param *mem_params,
		int num_reg_params, struct reg_param *reg_params,
		target_addr_t entry_point, target_addr_t exit_point, void *arch_info)
{
	struct agrv2k_common *common = target->arch_info;
	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;
	if (common->algorithm_active || entry_point < 0x20000000 || entry_point >= 0x20020000 ||
		(entry_point & 1) || exit_point > UINT32_MAX || num_mem_params < 0 || num_reg_params < 0)
		return ERROR_COMMAND_ARGUMENT_INVALID;
	for (int i = 0; i < num_reg_params; ++i) {
		if (!agrv2k_algorithm_reg(target, &reg_params[i]))
			return ERROR_COMMAND_ARGUMENT_INVALID;
	}
	for (int i = 0; i < num_mem_params; ++i) {
		if (!agrv2k_direction_valid(mem_params[i].direction))
			return ERROR_COMMAND_ARGUMENT_INVALID;
	}
	/* Save every GPR, not just argument registers: loaders may use temporaries
	 * without exposing those as arguments. Never borrow the application's stack. */
	for (unsigned int i = 1; i < 32; ++i) {
		int ret = agrv2k_dm_reg_read(&common->dm, AGRV2K_GPR_BASE + i, &common->saved_gpr[i]);
		if (ret != AGRV2K_DM_OK)
			return agrv2k_result(common, ret);
	}
	int ret = agrv2k_dm_reg_read(&common->dm, AGRV2K_CSR_DPC, &common->saved_pc);
	if (ret == AGRV2K_DM_OK)
		ret = agrv2k_dm_reg_read(&common->dm, AGRV2K_CSR_DCSR, &common->saved_dcsr);
	if (ret == AGRV2K_DM_OK)
		ret = agrv2k_dm_reg_read(&common->dm, 0x300, &common->saved_mstatus);
	if (ret != AGRV2K_DM_OK)
		return agrv2k_result(common, ret);
	common->algorithm_active = true;
	ret = agrv2k_result(common, agrv2k_dm_reg_write(&common->dm, 0x300,
		common->saved_mstatus & ~UINT32_C(0xb)));
	for (int i = 0; ret == ERROR_OK && i < num_mem_params; ++i) {
		if (mem_params[i].direction != PARAM_IN)
			ret = target_write_buffer(target, mem_params[i].address, mem_params[i].size, mem_params[i].value);
	}
	for (int i = 0; ret == ERROR_OK && i < num_reg_params; ++i) {
		if (reg_params[i].direction != PARAM_IN) {
			struct reg *reg = agrv2k_algorithm_reg(target, &reg_params[i]);
			ret = reg->type->set(reg, reg_params[i].value);
		}
	}
	if (ret == ERROR_OK)
		ret = agrv2k_resume(target, false, entry_point, false, true);
	if (ret != ERROR_OK) {
		int cleanup = agrv2k_restore_algorithm(target);
		if (cleanup != ERROR_OK)
			return cleanup;
	}
	return ret;
}

static int agrv2k_wait_algorithm(struct target *target,
		int num_mem_params, struct mem_param *mem_params,
		int num_reg_params, struct reg_param *reg_params,
		target_addr_t exit_point, unsigned int timeout_ms, void *arch_info)
{
	struct agrv2k_common *common = target->arch_info;
	if (!common->algorithm_active)
		return ERROR_FAIL;
	int64_t deadline = timeval_ms() + timeout_ms;
	int ret = ERROR_OK;
	while (target->state != TARGET_HALTED) {
		ret = agrv2k_poll(target);
		if (ret != ERROR_OK || target->state == TARGET_HALTED)
			break;
		if (timeval_ms() >= deadline) {
			ret = ERROR_TARGET_TIMEOUT;
			break;
		}
		alive_sleep(1);
	}
	if (ret == ERROR_OK) {
		uint32_t pc;
		ret = agrv2k_result(common, agrv2k_dm_reg_read(&common->dm, AGRV2K_CSR_DPC, &pc));
		if (ret == ERROR_OK && ((exit_point && pc != exit_point) ||
			target->debug_reason != DBG_REASON_BREAKPOINT)) {
			LOG_TARGET_ERROR(target, "SRAM algorithm stopped at unexpected PC 0x%08" PRIx32, pc);
			ret = ERROR_TARGET_ALGO_EXIT;
		}
	}
	for (int i = 0; ret == ERROR_OK && i < num_reg_params; ++i) {
		if (reg_params[i].direction != PARAM_OUT) {
			struct reg *reg = agrv2k_algorithm_reg(target, &reg_params[i]);
			if (!reg)
				ret = ERROR_COMMAND_ARGUMENT_INVALID;
			else {
				ret = reg->type->get(reg);
				if (ret == ERROR_OK)
					buf_cpy(reg->value, reg_params[i].value, 32);
			}
		}
	}
	for (int i = 0; ret == ERROR_OK && i < num_mem_params; ++i) {
		if (mem_params[i].direction != PARAM_OUT)
			ret = target_read_buffer(target, mem_params[i].address, mem_params[i].size, mem_params[i].value);
	}
	/* Restore on timeout and unexpected exit as well as on success. */
	int cleanup = agrv2k_restore_algorithm(target);
	return cleanup != ERROR_OK ? cleanup : ret;
}

static int agrv2k_checksum_memory(struct target *target, target_addr_t address,
		uint32_t count, uint32_t *checksum)
{
	static const uint8_t code[] = {
#include "../../../contrib/loaders/checksum/riscv32_crc.inc"
	};
	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;
	if (address > UINT32_MAX || (uint64_t)count > UINT64_C(0x100000000) - address)
		return ERROR_COMMAND_ARGUMENT_INVALID;
	/* A CRC loader must not overwrite the SRAM image it is verifying. The
	 * common checksum service falls back to a read when scratch overlaps. */
	if (count && address < target->working_area_phys + target->working_area_size &&
		address + count > target->working_area_phys)
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	struct working_area *area;
	int ret = target_alloc_working_area(target, sizeof(code), &area);
	if (ret != ERROR_OK)
		return ret;
	ret = target_write_buffer(target, area->address, sizeof(code), code);
	if (ret == ERROR_OK) {
		struct reg_param params[2];
		init_reg_param(&params[0], "a0", 32, PARAM_IN_OUT);
		init_reg_param(&params[1], "a1", 32, PARAM_OUT);
		buf_set_u32(params[0].value, 0, 32, address);
		buf_set_u32(params[1].value, 0, 32, count);
		ret = target_run_algorithm(target, 0, NULL, ARRAY_SIZE(params), params,
			area->address, 0, 2000 + count / 50, NULL);
		if (ret == ERROR_OK)
			*checksum = buf_get_u32(params[0].value, 0, 32);
		for (unsigned int i = 0; i < ARRAY_SIZE(params); ++i)
			destroy_reg_param(&params[i]);
	}
	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "CRC loader not halted; SRAM left intact, reset and reconnect required");
		free(area->backup);
		area->backup = NULL;
		target_free_working_area(target, area);
		return ret == ERROR_OK ? ERROR_FAIL : ret;
	}
	int cleanup = target_free_working_area(target, area);
	return ret != ERROR_OK ? ret : cleanup;
}

static const char *agrv2k_gdb_arch(const struct target *target)
{
	return "riscv:rv32";
}

static int agrv2k_gdb_regs(struct target *target, struct reg **reg_list[],
		int *reg_list_size, enum target_register_class reg_class)
{
	struct agrv2k_common *common = target->arch_info;
	unsigned int count = reg_class == REG_CLASS_ALL ? AGRV2K_TOTAL_REGS : AGRV2K_CORE_REGS;
	*reg_list = malloc(count * sizeof(**reg_list));
	if (!*reg_list)
		return ERROR_FAIL;
	for (unsigned int i = 0; i < count; ++i)
		(*reg_list)[i] = &common->regs[i];
	*reg_list_size = count;
	return ERROR_OK;
}

static int agrv2k_arch_state(struct target *target)
{
	LOG_TARGET_USER(target, "RV32 hart 0: %s", target_state_name(target));
	return ERROR_OK;
}

struct target_type agrv2k_target = {
	.name = "agrv2k",
	.target_create = agrv2k_create,
	.target_jim_configure = adiv5_jim_configure,
	.init_target = agrv2k_init,
	.deinit_target = agrv2k_deinit,
	.examine = agrv2k_examine,
	.poll = agrv2k_poll,
	.arch_state = agrv2k_arch_state,
	.halt = agrv2k_halt,
	.resume = agrv2k_resume,
	.step = agrv2k_step,
	.assert_reset = agrv2k_reset_assert,
	.deassert_reset = agrv2k_reset_deassert,
	.read_memory = agrv2k_read_memory,
	.write_memory = agrv2k_write_memory,
	.start_algorithm = agrv2k_start_algorithm,
	.wait_algorithm = agrv2k_wait_algorithm,
	.checksum_memory = agrv2k_checksum_memory,
	.get_gdb_arch = agrv2k_gdb_arch,
	.get_gdb_reg_list = agrv2k_gdb_regs,
	.get_gdb_reg_list_noread = agrv2k_gdb_regs,
};
