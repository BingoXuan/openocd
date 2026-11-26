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

static int64_t agrv2k_now_ms(void *ctx)
{
	return timeval_ms();
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
	if (address > UINT32_MAX)
		return ERROR_COMMAND_ARGUMENT_INVALID;
	return agrv2k_result(common, agrv2k_dm_write_memory(&common->dm, address, size, count, buffer));
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
	.read_phys_memory = agrv2k_read_memory,
	.write_phys_memory = agrv2k_write_memory,
	.get_gdb_arch = agrv2k_gdb_arch,
	.get_gdb_reg_list = agrv2k_gdb_regs,
	.get_gdb_reg_list_noread = agrv2k_gdb_regs,
};
