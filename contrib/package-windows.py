#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bundle the MinGW executable and its transitive non-system DLL imports."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    prefix, runtime = args.prefix.resolve(), args.runtime.resolve()
    binary = prefix / "bin/openocd.exe"
    if not binary.is_file():
        raise FileNotFoundError(binary)
    system = Path(os.environ["SystemRoot"]) / "System32"
    libraries = {path.name.lower(): path for path in (runtime / "bin").glob("*.dll")}
    pending, checked = [binary], set()
    while pending:
        path = pending.pop()
        if path.name.lower() in checked:
            continue
        checked.add(path.name.lower())
        headers = subprocess.check_output(["objdump", "-p", str(path)], text=True, errors="replace")
        if "file format pei-x86-64" not in headers:
            raise ValueError(f"Not a Windows x64 binary: {path}")
        for name in re.findall(r"DLL Name:\s*(\S+)", headers):
            key = name.lower()
            if key in {"msys-2.0.dll", "cygwin1.dll"}:
                raise ValueError(f"Package unexpectedly depends on {name}")
            if key.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            target = prefix / "bin" / name
            if not target.is_file():
                source = libraries.get(key)
                if source is None:
                    if (system / name).is_file():
                        continue
                    raise FileNotFoundError(f"Unresolved DLL import {name} in {path.name}")
                shutil.copy2(source, target)
            pending.append(target)

    source = Path(__file__).resolve().parents[1]
    for name in ("COPYING", "AUTHORS", "README.Windows.md"):
        shutil.copy2(source / name, prefix / name)
    licenses = runtime / "share/licenses"
    if licenses.is_dir():
        shutil.copytree(licenses, prefix / "share/licenses/runtime", dirs_exist_ok=True)
    for name in ("LICENSE", "tcl.license.terms"):
        jim_license = source / "jimtcl" / name
        if jim_license.is_file():
            destination = prefix / "share/licenses/jimtcl"
            destination.mkdir(parents=True, exist_ok=True)
            shutil.copy2(jim_license, destination / name)
    metadata = {
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
        "platform": "windows-x64-ucrt",
        "features": ["agrv2k", "ch347", "cmsis-dap", "cmsis-dap-v2", "ftdi"],
        "dlls": sorted(name for name in checked if name.endswith(".dll")),
    }
    (prefix / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    files = sorted(path for path in prefix.rglob("*") if path.is_file())
    sums = [hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.relative_to(prefix).as_posix()
            for path in files]
    (prefix / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(prefix.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(prefix.parent).as_posix())
    print(f"Created {args.output}; bundled {len(metadata['dlls'])} runtime DLLs")


if __name__ == "__main__":
    main()
