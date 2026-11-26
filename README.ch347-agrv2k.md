# CH347 + VF407 / AGRV2K OpenOCD fork

This branch builds on official OpenOCD commit
`983467b0ca00bc56445445587b67cc0c75bc08fc`. That upstream revision already
contains the CH347 JTAG/SWD adapter driver. The changes here add the RISC-V
Debug Module access path needed by AGM AGRV2K and configurations for VF407.
This is an experimental hardware enablement branch, not an AGM vendor release.

Validation on **2026-11-26**, macOS arm64, Apple Clang 21:

| Check | Result |
| --- | --- |
| Native build with CH347 + CMSIS-DAP | Passed |
| Upstream configured `make check` tests | 3/3 passed |
| Offline target/configuration checks | 5/5 passed |
| Mock flash bounds, byte order, errors and cleanup | Passed |
| CH347T 5.44, firmware 0x45, direct Zynq JTAG scan | 2/2 passed at 469 kHz |
| Observed JTAG IDs | XC7Z010 `0x13722093`, ARM DAP `0x4ba00477` |
| VF407 SWD/debug/SRAM/flash operations | Not physically tested |
| FPGA bitstream or persistent boot-flash writes | Not tested |

## What was adapted

AGRV2K exposes its RISC-V Debug Module through **AP0 of an ARM ADIv5 DAP**.
Ordinary RISC-V JTAG DTM scans cannot access that module over SWD. This fork
accepts `target create ... riscv -dap ... -ap-num 0`, translates DMI register
accesses into MEM-AP accesses, and retains the ordinary RISC-V JTAG path.

The implementation ports Tomas Vanek's
[Gerrit change 9590, patchset 2](https://review.openocd.org/c/openocd/+/9590/2)
and the nested `riscv_private_config.adi_pc` correction described in
[AGaMEMnon's OpenOCD tools](https://github.com/bbenchoff/AGaMEMnon/tree/065f7fd716f54ba8e4b9099f45169df03f4e7be2/tools/openocd).
The AGaMEMnon reference uses OpenOCD base
`a17c5f5a6dac6625cd5b01dfc3234f57cb58f1f3`; its L48/256-KiB acceptance results
do not qualify VF407. The port preserves the current target-list API and DTM
checks, keys shared DMs by DAP/AP, and propagates queued AP errors.

No proprietary OS-Q executable or vendor flash driver is included. The
experimental Tcl flash procedures use documented register operations; they
do not register a native OpenOCD `flash bank ... agrv` driver.

## Build

Required: C compiler, make, Git, autoconf, automake, GNU libtool, pkg-config,
libusb and HIDAPI. JimTcl is fetched at the commit pinned by this repository's
submodule (`d5243a25c488dfe751ef218828f13516e04ea2ba`). J-Link and Capstone
are disabled by the helper. Runtime libraries are dynamically linked; the
result is a local build, not a self-contained release archive.

On Apple Silicon, install missing prerequisites without automatic cleanup:

```sh
HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 \
  brew install autoconf automake libtool pkg-config libusb hidapi
export PATH="$(brew --prefix libtool)/libexec/gnubin:$PATH"
```

On Debian/Ubuntu, the corresponding packages are `build-essential autoconf
automake libtool pkg-config libusb-1.0-0-dev libhidapi-dev git`.

```sh
git clone --branch ch347-agrv2k https://github.com/BingoXuan/openocd.git
cd openocd
bash contrib/build-ch347-agrv2k.sh "$PWD/../openocd-prefix"
export OPENOCD="$PWD/../openocd-prefix/bin/openocd"
"$OPENOCD" -c 'adapter list' -c shutdown
```

## CH347 with the current Zynq-7010 board

The following profile creates only the FPGA TAP, ARM DAP TAP, and FPGA PLD.
It does not attach to, halt, or reset an ARM CPU. `init` resets the **JTAG TAP
state machine**, so this is a scan procedure, not a claim of zero pin activity.

```sh
"$OPENOCD" -f board/agm/ch347-zynq7010.cfg \
  -c 'gdb port disabled' -c 'tcl port disabled' -c 'telnet port disabled' \
  -c init -c scan_chain -c shutdown
```

Expected FPGA ID is `0x13722093` (XC7Z010, version ignored by the profile),
followed by ARM DAP ID `0x4ba00477`. The profile requests 500 kHz.

To configure the PL with a bitstream built for this exact FPGA/board, OpenOCD
provides `pld load zynq_pl.pld /absolute/path/design.bit` after `init`. This
updates volatile FPGA configuration, not QSPI boot flash. This branch has not
qualified bitstream loading, QSPI writes, or running a C program on Zynq.

## VF407 / AGRV2K over SWD

Choose the adapter that is actually connected. The board-only profile
`board/agm/agm32_vf407.cfg` can also follow another compatible SWD interface.

```sh
# Existing CMSIS-DAP hardware:
"$OPENOCD" -f board/agm/agm32_vf407-cmsis-dap.cfg \
  -c init -c agrv2k_info -c shutdown

# CH347 with SWD-capable firmware and correct SWCLK/SWDIO wiring:
"$OPENOCD" -f board/agm/agm32_vf407-ch347.cfg \
  -c init -c agrv2k_info -c shutdown
```

The target uses AP0, little-endian accesses, a 64-KiB work area at
`0x20000000`, and `riscv mem_access sysbus progbuf`. `agrv2k_info` reads
DEVICE_ID at `0x03000100`. Target examination may temporarily halt a hart;
these commands are not a non-intrusive observation of running firmware.

CH347 firmware support matters: upstream's driver documentation reports no
SWD in version 2.41 and SWD defects in some later versions. See
[`doc/openocd.texi`, CH347 interface](doc/openocd.texi). A successful CH347
**JTAG** scan on Zynq does not establish **SWD** compatibility with VF407.
The CMSIS-DAP route is available independently of CH347 firmware.

## Experimental VF407 main-flash procedures

The VF407 profile defines main flash at `0x80000000`, size **1 MiB**, sector
size **4096 bytes**, word size **4 bytes**, and controller `0x40001000`.
These values are VF407-specific, not the reference L48's 256-KiB geometry.

Procedures are defined when the profile is sourced and run only on explicit
calls. The target must already be halted. `agrv2k_flash_erase ADDRESS LENGTH`
requires complete sectors. `agrv2k_flash_program FILE.bin ADDRESS` writes
words, pads a final partial word with `0xff`, and runs `verify_image` after
successful programming. It does **not** erase first, change option bytes,
set CPLD boot addresses, or accept `.bit` files.

The current Leego VF407 layout reserves the tail from offset `0xe7000` for
CPLD startup data. The default writable limit is therefore `0xe7000` bytes;
range checks include file padding. Actual board contents/layout must be read
and backed up before choosing a writable sector. An existing variable
`AGRV2K_FLASH_WRITABLE_SIZE` can set a smaller limit before sourcing the
profile. Increasing it is an explicit board-layout decision, not an
automatically detected capability.

The helpers poll busy/error/EOP status with deadlines and attempt to relock
the controller on success or failure. If the controller stays busy or the
transport fails, cleanup cannot promise that the hardware is locked. They
are host-driven and slow. VF407 SRAM execution, flash erase/program/restore,
and preservation of existing CPLD data still need physical acceptance.

OpenOCD loads/debugs compiled firmware; it is not a C compiler. An AGRV2K
application still needs the board's RISC-V toolchain, linker script, and
startup code. AGM FPGA/CPLD configuration is a separate feature and is not
implemented by this RISC-V-over-DAP port.

## Offline validation

These commands never open USB or call target `init`:

```sh
python3 testing/agm/check-configs.py --openocd "$OPENOCD"
mkdir -p ../agm-test-temp
../build-ch347-agrv2k/jimtcl/jimsh testing/agm/flash-contracts.tcl \
  tcl/target/agm/agrv2k-flash.tcl ../agm-test-temp
```

The configuration checks exercise CMSIS-DAP SWD, CH347 SWD, ordinary RISC-V
JTAG, a TAP-only Zynq profile, and invalid-DAP rejection. The flash test
replaces all target I/O with mocks and checks bounds, binary byte order,
word padding, running-target rejection, error propagation, timeout, and
controller cleanup. Passing them proves host logic and parser compatibility,
not real-device programming.

All changes are distributed under OpenOCD's GPL-2.0-or-later terms. The
original Gerrit patch is credited to Tomas Vanek; AGaMEMnon is credited for
the configuration fix and public AG32 integration reference.
