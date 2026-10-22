# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  rtp
  SOURCES
  rtp_header.cpp
  DEPS
  statusbar::buffer
  statusbar::ieee
  TESTS
  rtp_test.cpp)
