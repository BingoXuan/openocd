#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build into a caller-selected prefix; never replace the system OpenOCD.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <install-prefix> [build-directory]" >&2
    exit 2
fi

source_dir=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$1"
prefix=$(cd "$1" && pwd)
build_arg=${2:-"$source_dir/../build-ch347-agrv2k"}
mkdir -p "$build_arg"
build_dir=$(cd "$build_arg" && pwd)
jobs=${JOBS:-4}

cd "$source_dir"
git submodule update --init --depth 1 jimtcl
./bootstrap nosubmodule
cd "$build_dir"
"$source_dir/configure" --prefix="$prefix" \
    --enable-agrv2k --enable-ch347 --enable-cmsis-dap --enable-internal-jimtcl \
    --disable-jlink --without-capstone --disable-werror \
    --disable-doxygen-html --disable-doxygen-pdf MAKEINFO=true
make -j"$jobs"
make install
"$prefix/bin/openocd" --version
