# Isolated AGRV2K target and upstream CH347

This branch starts directly from official OpenOCD commit
`983467b0ca00bc56445445587b67cc0c75bc08fc`. **Every file under
`src/target/riscv/` is unchanged from that upstream commit.**

The branch does not apply or contain the commits from the prototype
`ch347-agrv2k` branch, Gerrit 9590, or AGaMEMnon's
`0001-target-riscv-DM-access-on-a-DAP.patch`. The generic `riscv` target
retains its upstream option parser, JTAG DTM, batches, register handling,
reset, and memory/flash-loader behavior. In particular, it still rejects
`target create ... riscv -dap ...`.

AGM support is selected separately:

```tcl
target create agrv2k.cpu agrv2k -dap agrv2k.dap -ap-num 0
```

The new `src/target/agm/` code has its own DM state, register cache, and
RV32 access implementation. It does not call, wrap, rename, copy or rebuild
the generic RISC-V target with alternate macros. It registers no global
`riscv` commands. Shared code is limited to existing OpenOCD target services
and the existing ADIv5 MEM-AP implementation. Target registration is guarded
by `--enable-agrv2k`, which defaults to **disabled**.

## Hardware scope

The hardware topology is based on
[AGaMEMnon's open AGRV2K configuration](https://github.com/bbenchoff/AGaMEMnon/blob/065f7fd716f54ba8e4b9099f45169df03f4e7be2/agamemnon/openocd/agrv2k.cfg):
SWD reaches an ADIv5 DAP, whose AP0 exposes a window of RISC-V DM registers.
This is a hardware reference, not a port of its generic-RISC-V patch.
DM commands follow the
[RISC-V Debug Specification](https://docs.riscv.org/reference/debug/v1.0/debug_module.html).
The actual VF407 AP mapping and behavior still require physical acceptance.

DMI word indices are translated to MEM-AP byte addresses as
`-dbgbase + 4 * dmi_register`; `-dbgbase` defaults to zero. The target reads
`dmstatus.version` and accepts DM 0.13 / 1.0. It never invents or scans a JTAG
DTMCS version.

The initial implementation is deliberately limited to a single little-endian
RV32 hart (hart 0). It implements halt, resume, step, reset, x0-x31/PC,
MISA/DCSR register access, SBA v1 memory transfers, and program-buffer
fallback for memory/CSR accesses. Scratch GPRs are restored after program
buffer operations; resume issues data/instruction fences before execution.
There is no automatic retry through a second method after an issued SBA
transaction fails.

**This is not a replacement for the full upstream RISC-V target.** Hardware
triggers, software breakpoints, FPU registers, SMP, virtual memory,
semihosting are not implemented. SRAM algorithm execution is available for
the isolated Flash and CRC loaders. GDB interoperability and board-specific acceptance are separate from the
source and model checks documented below.

## Build

Required: C compiler, make, Git, autoconf, automake, GNU libtool, pkg-config,
libusb and HIDAPI. The pinned internal JimTcl is used; J-Link and Capstone
are disabled by the dedicated build helper. The binary dynamically links
local runtime libraries; it is not a portable release bundle.

On macOS, install missing prerequisites without automatic cleanup:

```sh
HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 \
  brew install autoconf automake libtool pkg-config libusb hidapi
export PATH="$(brew --prefix libtool)/libexec/gnubin:$PATH"
```

On Debian/Ubuntu, use `build-essential autoconf automake libtool pkg-config
libusb-1.0-0-dev libhidapi-dev git`.

```sh
git clone --branch feat-agrv2k-support https://github.com/BingoXuan/openocd.git
cd openocd
bash contrib/build-ch347-agrv2k.sh "$PWD/../openocd-prefix"
export OPENOCD="$PWD/../openocd-prefix/bin/openocd"
```

The dedicated helper explicitly passes `--enable-agrv2k`. A normal configure
without that option excludes the AGM objects and target registration.
Shared integration is limited to the explicit target/Flash registries and
Automake/configure; the generic RISC-V implementation remains unchanged.

## VF407 / AGRV2K

Select the interface actually connected to the board:

```sh
"$OPENOCD" -f board/agm/agm32_vf407-cmsis-dap.cfg \
  -c init -c halt -c agrv2k_info -c shutdown

# Alternative: CH347 with working SWD firmware and correct wiring.
"$OPENOCD" -f board/agm/agm32_vf407-ch347.cfg \
  -c init -c halt -c agrv2k_info -c shutdown
```

These commands explicitly halt the CPU and leave it halted. `agrv2k_info`
reads DEVICE_ID at `0x03000100`. The board-only profile can follow a different
compatible SWD adapter. CH347 JTAG success on a Zynq board does not qualify
CH347 SWD on VF407; firmware support and wiring must be checked independently.

The profile describes 1 MiB main Flash at `0x80000000`, 4096-byte sectors,
4-byte words, and controller `0x40001000`. A native `agrv2k` NOR driver
programs through an SRAM loader. The explicit Tcl procedures
`agrv2k_flash_erase ADDRESS LENGTH` and `agrv2k_flash_program FILE.bin ADDRESS`
remain available; program does not erase first and verifies successful writes.
No option bytes are written. See [AGRV2K main Flash](README.agrv2k-flash.md)
for native commands, working-area requirements and validation interfaces.

The writable prefix defaults to 0xe7000 bytes to preserve the current Leego
CPLD startup tail. Actual board contents/layout must be read and backed up
before choosing a writable sector. A preexisting
`AGRV2K_FLASH_WRITABLE_SIZE` variable can set a smaller bound. Increasing the
bound is an explicit layout choice, not automatic discovery.

AGM FPGA/CPLD configuration is a separate implementation and is not provided
by this MCU target. OpenOCD loads compiled firmware; it does not compile C.

## CH347 and Zynq-7010

CH347 support comes from the unmodified upstream driver. The TAP/PLD-only
profile scans PL and ARM DAP without creating an ARM CPU target:

```sh
"$OPENOCD" -f board/agm/ch347-zynq7010.cfg \
  -c 'gdb port disabled' -c 'tcl port disabled' -c 'telnet port disabled' \
  -c init -c scan_chain -c shutdown
```

Expected IDs are XC7Z010 `0x13722093` and ARM DAP `0x4ba00477`. The requested
clock is 500 kHz. TAP initialization toggles JTAG state; it is not a claim of
zero pin activity. Bitstream loading and QSPI writes remain unqualified.

## Validation without USB

Validated on 2026-11-26, macOS arm64:

| Check | Result |
| --- | --- |
| AGM enabled / default disabled builds | Both passed |
| Upstream configured tests | 3/3 in each build |
| Configuration checks / isolation checks | 6/6 each |
| Generic RISC-V object machine code | All 11 text sections identical between builds |
| Independent DM model with ASan/UBSan | 7 groups passed |
| Mock flash bounds/error/cleanup contracts | Passed |
| Physical VF407 and GDB acceptance | Not performed |

```sh
python3 testing/agm/check-configs.py --openocd "$OPENOCD"
python3 testing/agm/check-isolation.py --openocd "$OPENOCD"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g -Isrc \
  testing/agm/test-agrv2k-dm.c src/target/agm/agrv2k_dm.c -o ../test-agrv2k-dm
../test-agrv2k-dm

mkdir -p ../agm-test-temp
../build-ch347-agrv2k/jimtcl/jimsh testing/agm/flash-contracts.tcl \
  tcl/target/agm/agrv2k-flash.tcl ../agm-test-temp
```

`check-isolation.py` checks the entire generic-RISC-V source subtree against
the pinned upstream, verifies that the prototype patch is not in branch
history, and confirms the generic target still rejects `-dap`. Pass
`--disabled-openocd PATH` to also verify a build made without
`--enable-agrv2k`: it must still expose `riscv` and must not expose `agrv2k`.

The C model checks DM version/authentication, registers/CSR fallback,
byte/halfword/word memory access, scratch restoration, halt/resume/step,
fences, reset, timeout, I/O errors, and separate target contexts. The Tcl
flash contract test mocks all target I/O. Neither test opens USB or performs
real flash writes. The configured upstream `make check` tests remain available.

All new files use GPL-2.0-or-later. The original prototype remains on its own
branch for history; use `feat-agrv2k-support` for this implementation.
