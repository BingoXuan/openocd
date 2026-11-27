/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef OPENOCD_TARGET_AGM_AGRV2K_DM_H
#define OPENOCD_TARGET_AGM_AGRV2K_DM_H

#include <stdbool.h>
#include <stdint.h>

/* This module has no dependency on the generic OpenOCD RISC-V target. */
enum agrv2k_dm_result {
	AGRV2K_DM_OK = 0,
	AGRV2K_DM_IO = -1,
	AGRV2K_DM_TIMEOUT = -2,
	AGRV2K_DM_UNSUPPORTED = -3,
	AGRV2K_DM_NOT_HALTED = -4,
	AGRV2K_DM_COMMAND_ERROR = -5,
};

enum agrv2k_dm_register {
	AGRV2K_DATA0 = 0x04,
	AGRV2K_DMCONTROL = 0x10,
	AGRV2K_DMSTATUS = 0x11,
	AGRV2K_ABSTRACTCS = 0x16,
	AGRV2K_COMMAND = 0x17,
	AGRV2K_ABSTRACTAUTO = 0x18,
	AGRV2K_PROGBUF0 = 0x20,
	AGRV2K_SBCS = 0x38,
	AGRV2K_SBADDRESS0 = 0x39,
	AGRV2K_SBDATA0 = 0x3c,
};

#define AGRV2K_DMACTIVE UINT32_C(1)
#define AGRV2K_NDMRESET UINT32_C(2)
#define AGRV2K_ACKHAVERESET (UINT32_C(1) << 28)
#define AGRV2K_RESUMEREQ (UINT32_C(1) << 30)
#define AGRV2K_HALTREQ (UINT32_C(1) << 31)
#define AGRV2K_AUTHENTICATED (UINT32_C(1) << 7)
#define AGRV2K_ALLHALTED (UINT32_C(1) << 9)
#define AGRV2K_ALLRUNNING (UINT32_C(1) << 11)
#define AGRV2K_ANYUNAVAIL (UINT32_C(1) << 12)
#define AGRV2K_ANYNONEXISTENT (UINT32_C(1) << 14)
#define AGRV2K_ALLRESUMEACK (UINT32_C(1) << 17)
#define AGRV2K_ANYHAVERESET (UINT32_C(1) << 18)
#define AGRV2K_IMPEBREAK (UINT32_C(1) << 22)
#define AGRV2K_ABSTRACT_BUSY (UINT32_C(1) << 12)
#define AGRV2K_CMDERR_MASK (UINT32_C(7) << 8)
#define AGRV2K_SBERROR_MASK (UINT32_C(7) << 12)
#define AGRV2K_SBBUSY (UINT32_C(1) << 21)
#define AGRV2K_SBBUSYERROR (UINT32_C(1) << 22)
#define AGRV2K_SBREADONADDR (UINT32_C(1) << 20)
#define AGRV2K_SBAUTOINCREMENT (UINT32_C(1) << 16)
#define AGRV2K_SBREADONDATA (UINT32_C(1) << 15)
#define AGRV2K_CSR_DCSR 0x7b0
#define AGRV2K_CSR_DPC 0x7b1
#define AGRV2K_GPR_BASE 0x1000

struct agrv2k_dm_io {
	/* DMI register indices, not CPU addresses or AP byte addresses. */
	int (*read)(void *ctx, uint32_t reg, uint32_t *value);
	int (*write)(void *ctx, uint32_t reg, uint32_t value);
	int64_t (*now_ms)(void *ctx);
	void (*sleep_ms)(void *ctx, unsigned int ms);
	void *ctx;
	/* Optional ordered batch of DMI reads, completed before returning. */
	int (*read_many)(void *ctx, const uint32_t *regs, uint32_t *values, unsigned int count);
	/* Optional ordered writes, completed before returning; SRAM staging only. */
	int (*write_many)(void *ctx, uint32_t reg, const uint32_t *values, unsigned int count);
};

struct agrv2k_dm {
	struct agrv2k_dm_io io;
	unsigned int timeout_ms;
	unsigned int version;
	unsigned int progbuf_words;
	unsigned int last_cmderr;
	bool impebreak;
	uint32_t sbcs;
};

int agrv2k_dm_read(struct agrv2k_dm *dm, uint32_t reg, uint32_t *value);
int agrv2k_dm_write(struct agrv2k_dm *dm, uint32_t reg, uint32_t value);
int agrv2k_dm_examine(struct agrv2k_dm *dm);
int agrv2k_dm_halt(struct agrv2k_dm *dm);
int agrv2k_dm_resume(struct agrv2k_dm *dm, bool step);
int agrv2k_dm_reset_assert(struct agrv2k_dm *dm, bool halt);
int agrv2k_dm_reset_deassert(struct agrv2k_dm *dm, bool halt);
int agrv2k_dm_reg_read(struct agrv2k_dm *dm, uint32_t regno, uint32_t *value);
int agrv2k_dm_reg_write(struct agrv2k_dm *dm, uint32_t regno, uint32_t value);
int agrv2k_dm_read_memory(struct agrv2k_dm *dm, uint32_t address,
		unsigned int size, uint32_t count, uint8_t *buffer);
int agrv2k_dm_write_memory(struct agrv2k_dm *dm, uint32_t address,
		unsigned int size, uint32_t count, const uint8_t *buffer);

#endif /* OPENOCD_TARGET_AGM_AGRV2K_DM_H */
