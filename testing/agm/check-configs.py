#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check target option parsing without opening any USB device or calling init."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openocd", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    binary = args.openocd.resolve()
    cases = {
        "cmsis-dap-swd": ["-f", "board/agm/agm32_vf407-cmsis-dap.cfg"],
        "ch347-swd": ["-f", "board/agm/agm32_vf407-ch347.cfg"],
        "legacy-riscv-jtag": [
            "-c", "adapter driver ch347", "-c", "transport select jtag",
            "-c", "jtag newtap rv cpu -irlen 5",
            "-c", "target create rv.cpu riscv -tap rv.cpu",
        ],
        "zynq-taps-only": ["-f", "board/agm/ch347-zynq7010.cfg"],
    }
    results = []
    for name, config in cases.items():
        query = []
        if name.endswith("swd"):
            query = ["-c", "echo [target names]", "-c", "echo [agrv2k.cpu cget -ap-num]"]
        proc = subprocess.run(
            [str(binary), "-s", str(root / "tcl"), *config, *query, "-c", "shutdown"],
            text=True, capture_output=True, timeout=15, check=False,
        )
        output = proc.stdout + proc.stderr
        passed = proc.returncode == 0 and "Error:" not in output
        if name.endswith("swd"):
            passed = passed and "agrv2k.cpu" in output and "\n0\n" in output
        results.append({"case": name, "passed": passed, "returncode": proc.returncode, "output": output})
    proc = subprocess.run(
        [str(binary), "-c", "adapter driver cmsis-dap", "-c", "transport select swd",
         "-c", "target create rv.cpu riscv -dap definitely_missing", "-c", "shutdown"],
        text=True, capture_output=True, timeout=15, check=False,
    )
    output = proc.stdout + proc.stderr
    results.append({"case": "invalid-dap-rejected", "passed": proc.returncode != 0 and
                    "DAP 'definitely_missing' not found" in output,
                    "returncode": proc.returncode, "output": output})
    print(json.dumps(results, indent=2))
    return 0 if all(case["passed"] for case in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
