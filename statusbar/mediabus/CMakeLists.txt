# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  mediabus
  SOURCES
  mediabus_frame_ring.cpp
  DEPS
  statusbar::status
  TESTS
  mediabus_frame_ring_test.cpp
  mediabus_ptp_clock_test.cpp)
