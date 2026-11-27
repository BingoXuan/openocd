// SPDX-License-Identifier: GPL-2.0-or-later
/* AGRV2K main Flash only. Option bytes and CPLD JTAG are deliberately absent. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <target/algorithm.h>
#include <helper/time_support.h>
#include <helper/binarybuffer.h>

#define AGRV2K_FLASH_BASE UINT32_C(0x80000000)
#define AGRV2K_FLASH_SIZE UINT32_C(0x100000)
#define AGRV2K_SECTOR_SIZE UINT32_C(4096)
#define AGRV2K_FMC UINT32_C(0x40001000)
#define AGRV2K_SR (AGRV2K_FMC + 0x0c)
#define AGRV2K_CR (AGRV2K_FMC + 0x10)
/* Continuous memory writer used by the AGM FLASH_EnableFastProgram API. */
#define AGRV2K_FAST_PROGRAM UINT32_C(0x8211)
#define AGRV2K_FAST_STOP UINT32_C(0x200)

struct agrv2k_flash_bank {
	bool probed;
};

static int agrv2k_wait(struct target *target, unsigned int timeout, bool done)
{
	int64_t deadline = timeval_ms() + timeout;
	for (;;) {
		uint32_t status;
		int ret = target_read_u32(target, AGRV2K_SR, &status);
		if (ret != ERROR_OK)
			return ret;
		if (!(status & 1) && (!done || (status & 0x34))) {
			if (status & 0x14) {
				LOG_TARGET_ERROR(target, "AGRV2K Flash status 0x%08" PRIx32, status);
				return ERROR_FLASH_OPERATION_FAILED;
			}
			return ERROR_OK;
		}
		if (timeval_ms() >= deadline)
			return ERROR_TARGET_TIMEOUT;
		alive_sleep(1);
	}
}

static int agrv2k_lock(struct target *target, uint32_t idle)
{
	/* Never change CR while a timed-out operation is still busy. Error status
	 * does not prevent relocking after the controller becomes idle. */
	int64_t deadline = timeval_ms() + 1000;
	uint32_t status;
	int ret;
	do {
		ret = target_read_u32(target, AGRV2K_SR, &status);
		if (ret != ERROR_OK)
			return ret;
		if (!(status & 1)) {
			int clear = target_write_u32(target, AGRV2K_SR, 0x34);
			int lock = target_write_u32(target, AGRV2K_CR, idle | 0x80);
			return clear != ERROR_OK ? clear : lock;
		}
		alive_sleep(1);
	} while (timeval_ms() < deadline);
	return ERROR_TARGET_TIMEOUT;
}

static int agrv2k_prepare(struct target *target, uint32_t *idle)
{
	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;
	int ret = agrv2k_wait(target, 1000, false);
	uint32_t control;
	if (ret == ERROR_OK)
		ret = target_read_u32(target, AGRV2K_CR, &control);
	if (ret != ERROR_OK)
		return ret;
	*idle = control & ~UINT32_C(0xd6ff);
	if (!(control & 0x80))
		return ERROR_OK;
	ret = target_write_u32(target, AGRV2K_FMC + 4, 0x45670123);
	if (ret == ERROR_OK)
		ret = target_write_u32(target, AGRV2K_FMC + 4, 0xcdef89ab);
	if (ret == ERROR_OK)
		ret = target_read_u32(target, AGRV2K_CR, &control);
	if (ret == ERROR_OK && (control & 0x80))
		ret = ERROR_FLASH_OPERATION_FAILED;
	if (ret != ERROR_OK)
		agrv2k_lock(target, *idle);
	return ret;
}

FLASH_BANK_COMMAND_HANDLER(agrv2k_flash_bank_command)
{
	if (CMD_ARGC != 6 || strcmp(target_type_name(bank->target), "agrv2k") ||
		bank->base != AGRV2K_FLASH_BASE || !bank->size || bank->size > AGRV2K_FLASH_SIZE ||
		(bank->size % AGRV2K_SECTOR_SIZE)) {
		LOG_ERROR("agrv2k requires its isolated target, main Flash base and a sector-aligned bounded size");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}
	bank->driver_priv = calloc(1, sizeof(struct agrv2k_flash_bank));
	if (!bank->driver_priv)
		return ERROR_FAIL;
	bank->write_start_alignment = 4;
	bank->write_end_alignment = 4;
	return ERROR_OK;
}

static int agrv2k_probe(struct flash_bank *bank)
{
	struct agrv2k_flash_bank *info = bank->driver_priv;
	if (info->probed)
		return ERROR_OK;
	uint32_t id;
	int ret = target_read_u32(bank->target, 0x03000100, &id);
	if (ret != ERROR_OK)
		return ret;
	if (id != 0x40200001) {
		LOG_ERROR("Unsupported AGRV2K DEVICE_ID=0x%08" PRIx32, id);
		return ERROR_FLASH_OPERATION_FAILED;
	}
	bank->num_sectors = bank->size / AGRV2K_SECTOR_SIZE;
	bank->sectors = alloc_block_array(0, AGRV2K_SECTOR_SIZE, bank->num_sectors);
	if (!bank->sectors)
		return ERROR_FAIL;
	info->probed = true;
	return ERROR_OK;
}

static int agrv2k_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
	if (first > last || last >= bank->num_sectors)
		return ERROR_FLASH_SECTOR_INVALID;
	struct target *target = bank->target;
	uint32_t idle;
	int ret = agrv2k_prepare(target, &idle);
	if (ret != ERROR_OK)
		return ret;
	for (unsigned int sector = first; ret == ERROR_OK && sector <= last; ++sector) {
		ret = target_write_u32(target, AGRV2K_SR, 0x34);
		if (ret == ERROR_OK)
			ret = target_write_u32(target, AGRV2K_FMC + 0x14, bank->base + sector * AGRV2K_SECTOR_SIZE);
		if (ret == ERROR_OK)
			ret = target_write_u32(target, AGRV2K_CR, idle | 2);
		if (ret == ERROR_OK) {
			alive_sleep(1);
			ret = target_write_u32(target, AGRV2K_CR, idle | 0x42);
		}
		if (ret == ERROR_OK)
			ret = agrv2k_wait(target, 1000, true);
		bank->sectors[sector].is_erased = ret == ERROR_OK ? 1 : -1;
	}
	int cleanup = agrv2k_lock(target, idle);
	return ret != ERROR_OK ? ret : cleanup;
}

static int agrv2k_write_blocks(struct target *target, const uint8_t *buffer, uint32_t bytes,
		struct working_area *fifo, struct working_area *code, uint32_t code_size,
		struct reg_param *params, unsigned int num_params)
{
	uint32_t begin = fifo->address + 8;
	uint32_t address = buf_get_u32(params[4].value, 0, 32);
	uint32_t capacity = fifo->size - 12;
	unsigned int transfers = 0;
	while (bytes) {
		uint32_t chunk = MIN(bytes, capacity);
		/* End intermediate transfers on a page boundary. Only the final
		 * transfer needs the controller's partial-page commit. */
		if (bytes > chunk && chunk >= 256)
			chunk -= (address + chunk) & 255;
		/* Stage while halted, then execute without SRAM accesses from the
		 * debug master competing with the hart's Flash controller traffic. */
		int ret = target_write_buffer(target, begin, chunk, buffer);
		if (ret == ERROR_OK)
			ret = target_write_u32(target, fifo->address, begin + chunk);
		if (ret == ERROR_OK)
			ret = target_write_u32(target, fifo->address + 4, begin);
		uint32_t idle;
		if (ret == ERROR_OK)
			ret = agrv2k_prepare(target, &idle);
		if (ret == ERROR_OK)
			ret = target_write_u32(target, AGRV2K_SR, 0x34);
		if (ret == ERROR_OK)
			ret = target_write_u32(target, AGRV2K_CR, idle | AGRV2K_FAST_PROGRAM);
		if (ret != ERROR_OK)
			return ret;
		buf_set_u32(params[0].value, 0, 32, AGRV2K_FMC);
		buf_set_u32(params[1].value, 0, 32, chunk / 4);
		buf_set_u32(params[4].value, 0, 32, address);
		buf_set_u32(params[6].value, 0, 32, idle | AGRV2K_FAST_STOP);
		ret = target_run_algorithm(target, 0, NULL, num_params, params,
			code->address, code->address + code_size - 4, 10000, NULL);
		if (ret != ERROR_OK)
			return ret;
		uint32_t status = buf_get_u32(params[0].value, 0, 32);
		if (status || buf_get_u32(params[4].value, 0, 32) != address + chunk) {
			LOG_ERROR("AGRV2K block loader failed at 0x%08" PRIx32 ", status=0x%08" PRIx32,
				buf_get_u32(params[4].value, 0, 32), status);
			return ERROR_FLASH_OPERATION_FAILED;
		}
		bytes -= chunk;
		buffer += chunk;
		address += chunk;
		transfers++;
		keep_alive();
	}
	LOG_DEBUG("AGRV2K SRAM uploads: %u blocks", transfers);
	return ERROR_OK;
}

static int agrv2k_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	static const uint8_t loader[] = {
#include "../../../contrib/loaders/flash/agrv2k/agrv2k.inc"
	};
	struct target *target = bank->target;
	if (target->state != TARGET_HALTED)
		return ERROR_TARGET_NOT_HALTED;
	if ((offset & 3) || offset > bank->size || count > bank->size - offset ||
		((count + 3) & ~3U) > bank->size - offset)
		return ERROR_FLASH_DST_OUT_OF_BANK;
	if (!count)
		return ERROR_OK;
	uint32_t bytes = (count + 3) & ~3U;
	uint8_t *padded = NULL;
	if (bytes != count) {
		padded = malloc(bytes);
		if (!padded)
			return ERROR_FAIL;
		memcpy(padded, buffer, count);
		memset(padded + count, 0xff, bytes - count);
		buffer = padded;
	}
	struct working_area *code = NULL, *fifo = NULL;
	int ret = target_alloc_working_area(target, sizeof(loader), &code);
	if (ret == ERROR_OK)
		ret = target_write_buffer(target, code->address, sizeof(loader), loader);
	if (ret == ERROR_OK) {
		uint32_t available = target_get_working_area_avail(target);
		ret = ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		for (unsigned int size = available > 8 ? MIN(MAX(256U, bytes + 4), MIN(65536U, available - 8)) & ~3U : 0;
			size >= 256; size = (size / 2) & ~3U) {
			ret = target_alloc_working_area_try(target, size + 8, &fifo);
			if (ret == ERROR_OK)
				break;
		}
	}
	if (ret != ERROR_OK)
		goto release;
	uint32_t idle;
	ret = agrv2k_prepare(target, &idle);
	if (ret != ERROR_OK)
		goto release;
	if (ret == ERROR_OK) {
		struct reg_param params[7];
		const char * const names[] = {"a0", "a1", "a2", "a3", "a4", "a5", "a6"};
		const uint32_t values[] = {AGRV2K_FMC, bytes / 4, fifo->address,
			fifo->address + fifo->size, bank->base + offset, 1000000, idle | AGRV2K_FAST_STOP};
		for (unsigned int i = 0; i < ARRAY_SIZE(params); ++i) {
			init_reg_param(&params[i], names[i], 32, i == 0 || i == 4 ? PARAM_IN_OUT : PARAM_OUT);
			buf_set_u32(params[i].value, 0, 32, values[i]);
		}
		LOG_INFO("AGRV2K SRAM block loader: %" PRIu32 " bytes, %" PRIu32
			" byte buffer at 0x%08" PRIx32, count, fifo->size - 12, (uint32_t)fifo->address);
		ret = agrv2k_write_blocks(target, buffer, bytes, fifo, code, sizeof(loader), params, ARRAY_SIZE(params));
		for (unsigned int i = 0; i < ARRAY_SIZE(params); ++i)
			destroy_reg_param(&params[i]);
	}
	if (target->state != TARGET_HALTED) {
		LOG_TARGET_ERROR(target, "Loader not halted; SRAM left intact. Reset and reconnect before retrying.");
		/* Do not restore code beneath a running hart. Drop host backups and
		 * allocator handles without writing SRAM; their user pointers refer
		 * to this stack frame and must not escape on the failure path. */
		free(fifo->backup);
		fifo->backup = NULL;
		target_free_working_area(target, fifo);
		free(code->backup);
		code->backup = NULL;
		target_free_working_area(target, code);
		free(padded);
		return ret == ERROR_OK ? ERROR_FAIL : ret;
	}
	int cleanup = agrv2k_lock(target, idle);
	if (ret == ERROR_OK)
		ret = cleanup;
release:
	if (fifo) {
		int result = target_free_working_area(target, fifo);
		if (ret == ERROR_OK)
			ret = result;
	}
	if (code) {
		int result = target_free_working_area(target, code);
		if (ret == ERROR_OK)
			ret = result;
	}
	free(padded);
	return ret;
}

const struct flash_driver agrv2k_flash = {
	.name = "agrv2k",
	.flash_bank_command = agrv2k_flash_bank_command,
	.erase = agrv2k_erase,
	.write = agrv2k_write,
	.read = default_flash_read,
	.probe = agrv2k_probe,
	.auto_probe = agrv2k_probe,
	.erase_check = default_flash_blank_check,
	.free_driver_priv = default_flash_free_driver_priv,
};
