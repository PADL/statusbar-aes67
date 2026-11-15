// Copyright 2026 PADL Software Pty Ltd
// SPDX-License-Identifier: MIT

/// The ASan options of a fuzzer built without ENABLE_ASAN, linked into each one by add_aes67_fuzzer().

/// No container-overflow checks: the libraries are uninstrumented, so they false-positive on libc++ strings.
extern "C" auto __asan_default_options() -> char const*  // NOLINT(bugprone-reserved-identifier): ASan's hook
{
    return "detect_container_overflow=0";
}
