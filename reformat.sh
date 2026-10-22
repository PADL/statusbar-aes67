#! /bin/sh
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT

# Run clang-format / cmake-format / ruff on this package.
#   default:  rewrite files in place.
#   --check:  report files needing reformatting and exit non-zero if any
#             would change. No files modified. Used by the pre-commit
#             hook and for manual or CI verification.

set -e

check_mode=0
case "${1:-}" in
    --check|--dry-run) check_mode=1 ;;
    "") ;;
    *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

# Pinned clang-format version. Keep this in lockstep across the host, the dev
# container, and CI (see requirements-format.txt) — clang-format changes its
# line breaking/alignment between major versions, so a drifting version silently
# reformats unrelated files. 19.1.7 matches the trixie/arm64 build clang.
CLANG_FORMAT_VERSION=19.1.7
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "error: '$CLANG_FORMAT' not found. Install the pinned formatter:" >&2
    echo "    pipx install clang-format==$CLANG_FORMAT_VERSION" >&2
    exit 1
fi

actual_cf_version=$("$CLANG_FORMAT" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p')
if [ "$actual_cf_version" != "$CLANG_FORMAT_VERSION" ] && [ "${CLANG_FORMAT_SKIP_VERSION_CHECK:-0}" != "1" ]; then
    echo "error: clang-format $actual_cf_version found, but this repo pins $CLANG_FORMAT_VERSION." >&2
    echo "Different versions reformat differently and churn unrelated files across build contexts." >&2
    echo "Fix: pipx install clang-format==$CLANG_FORMAT_VERSION (or set CLANG_FORMAT=/path/to/clang-format)." >&2
    echo "Override (not recommended): CLANG_FORMAT_SKIP_VERSION_CHECK=1 $0" >&2
    exit 1
fi

if [ "$check_mode" -eq 1 ]; then
    clang_args="--dry-run --Werror"
    cmake_args="--check"
    ruff_args="--check"
else
    clang_args="-i"
    cmake_args="-i"
    ruff_args=""
fi

# C/C++ sources
find statusbar \
    \( -name '*.cpp' -or -name '*.cppm' -or -name '*.hpp' -or -name '*.c' -or -name '*.h' \) \
    -print0 | xargs -0 "$CLANG_FORMAT" $clang_args

# CMake files
if command -v cmake-format >/dev/null 2>&1; then
    cmake-format $cmake_args CMakeLists.txt
    find statusbar -name CMakeLists.txt -print0 2>/dev/null | xargs -0 cmake-format $cmake_args
    find cmake -name '*.cmake' -print0 2>/dev/null | xargs -0 cmake-format $cmake_args
fi

# Python (ruff format walks the cwd recursively when no paths are given)
if command -v ruff >/dev/null 2>&1; then
    ruff format $ruff_args .
fi
