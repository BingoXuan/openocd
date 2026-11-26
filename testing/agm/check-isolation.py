#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove source isolation and opt-in registration; no target init or USB I/O."""
import argparse
import json
from pathlib import Path
import subprocess

BASE = "983467b0ca00bc56445445587b67cc0c75bc08fc"
PROTOTYPE = "b8a5ad745d8d7d4fbe24314ec839f2be057dde76"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openocd", required=True, type=Path)
    parser.add_argument("--disabled-openocd", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]

    def git(*cmd):
        return subprocess.run(["git", *cmd], cwd=root, capture_output=True, text=True, check=False)

    diff = git("diff", "--name-only", BASE, "--", "src/target/riscv")
    other = git("ls-files", "--others", "--exclude-standard", "--", "src/target/riscv")
    base_tree = git("rev-parse", f"{BASE}:src/target/riscv")
    changed, extra = diff.stdout.splitlines(), other.stdout.splitlines()
    valid = diff.returncode == other.returncode == base_tree.returncode == 0
    results = [{"case": "generic-riscv-source-identical", "passed": valid and not changed and not extra,
                "base_tree": base_tree.stdout.strip(),
                "changed": changed, "extra": extra}]
    ancestry = git("merge-base", "--is-ancestor", BASE, "HEAD")
    history = git("rev-list", f"{BASE}..HEAD")
    clean_history = ancestry.returncode == history.returncode == 0 and PROTOTYPE not in history.stdout.splitlines()
    results.append({"case": "prototype-patch-not-in-history", "passed": clean_history})
    for label, path in [("enabled", args.openocd), ("disabled", args.disabled_openocd)]:
        if path is None:
            continue
        binary = str(path.resolve())
        proc = subprocess.run([binary, "-c", "target types", "-c", "shutdown"],
                              capture_output=True, text=True, timeout=15, check=False)
        output = proc.stdout + proc.stderr
        tokens = output.split()
        passed = proc.returncode == 0 and "riscv" in tokens and (("agrv2k" in tokens) == (label == "enabled"))
        results.append({"case": f"{label}-target-registration", "passed": passed, "output": output})
        proc = subprocess.run([binary, "-c", "adapter driver cmsis-dap", "-c", "transport select swd",
                               "-c", "target create rv.cpu riscv -dap missing", "-c", "shutdown"],
                              capture_output=True, text=True, timeout=15, check=False)
        output = proc.stdout + proc.stderr
        results.append({"case": f"{label}-generic-riscv-dap-still-rejected",
                        "passed": proc.returncode != 0 and "Unknown param: -dap" in output,
                        "output": output})
    print(json.dumps(results, indent=2))
    return 0 if all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
