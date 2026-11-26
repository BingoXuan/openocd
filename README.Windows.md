# OpenOCD for Windows

This README contains instructions that are specific to Windows.

## Building

You can build OpenOCD for Windows natively with either MinGW-w64/MSYS
or Cygwin (plain MinGW might work with `--disable-werror` but is not
recommended as it doesn't provide enough C99 compatibility).
Alternatively, one can cross-compile it using MinGW-w64 on a *nix
host. See [README](README.md) for the generic instructions.

Also, the MSYS2 project provides both ready-made binaries and an easy
way to self-compile from their software repository out of the box.

## GitHub Actions builds for this fork

The **Windows build** workflow runs on pushes, pull requests, and manual
`workflow_dispatch` requests. It uses a Windows x64 runner and MSYS2 UCRT64
to build the isolated AGRV2K target, CH347, CMSIS-DAP v1/v2, and FTDI MPSSE
support. JimTcl comes from the repository's pinned submodule.

Open **Actions → Windows build → a successful run → Artifacts** and download
`openocd-windows-x64.zip`. Extract it and run:

```powershell
.\openocd-windows-x64\bin\openocd.exe --version
.\openocd-windows-x64\bin\openocd.exe -f board/agm/agm32_vf407-cmsis-dap.cfg -c shutdown
```

The ZIP includes the executable, its transitive runtime DLLs, installed Tcl
scripts, licenses, `build-info.json`, and `SHA256SUMS`. It targets Windows 10
or later and does not require an MSYS2 installation to run. The workflow
checks configuration parsing, generic RISC-V source isolation, DM contracts,
and upstream tests, then tests the extracted package with MSYS2 removed
from `PATH`. These checks do not access a USB adapter.

`windows-build-logs` contains configuration results, dependency versions and
the package checksum. The workflow uploads build artifacts; it does not
create or replace GitHub Releases.

## USB adapters

For the adapters that use a HID-based protocol, e.g. CMSIS-DAP, you do
not need to perform any additional configuration.

For all the others you usually need to have WinUSB.sys (or
libusbK.sys) driver installed. Some vendor software (e.g. for
ST-LINKv2) does it on its own. For the other cases the easiest way to
assign WinUSB to a device is to use the latest Zadig installer: <https://zadig.akeo.ie>

When using a composite USB device, it's often necessary to assign
WinUSB.sys to the composite parent instead of the specific
interface. To do that one needs to activate an advanced option in the
Zadig installer.

If you need to use the same adapter with other applications that may
require another driver, a solution for Windows Vista and above is to
activate the `IgnoreHWSerNum` registry setting for the USB device.

That setting forces Windows to associate the driver per port instead of
per serial number, the same behaviour as when the device does not contain
a serial number. So different drivers can be installed for the adapter on
different ports and you just need to plug the adapter into the correct
port depending on which application to use.

For more information, see:

- <https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-device-specific-registry-settings>
- <https://www.ftdichip.com/Support/Knowledgebase/index.html?ignorehardwareserialnumber.htm>
