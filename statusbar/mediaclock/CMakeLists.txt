# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  mediaclock
  SOURCES
  mediaclock_timebase.cpp
  TESTS
  mediaclock_ptp_clock_test.cpp
  mediaclock_timebase_test.cpp)
