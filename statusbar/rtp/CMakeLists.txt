# Copyright 2026 PADL Software Pty Ltd
# SPDX-License-Identifier: MIT

statusbar_add_module(
  NAME
  rtp
  SOURCES
  rtp_frame.cpp
  rtp_header.cpp
  DEPS
  statusbar::buffer
  statusbar::ieee
  statusbar::ip
  TESTS
  rtp_frame_test.cpp
  rtp_test.cpp)
